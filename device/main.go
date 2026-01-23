package device

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"os"
	"strings"
	"sync"
	"time"

	"go.bug.st/serial"
	"go.bug.st/serial/enumerator"
)

const (
	defaultVID         = 0x0403
	defaultPID         = 0x6001
	defaultBaudRate    = 115200
	usbbridgePacketLen = 5
	defaultWriteQueue  = 1
	maxLogLineBytes    = 16384
)

const (
	usbbridgeTypeKeyboard = 0x00
	usbbridgeTypeConsumer = 0x01
	usbbridgeTypeVendor   = 0x02
	usbbridgeReleaseFlag  = 0x80
)

var errDeviceNotFound = errors.New("usbbridge device not found")

type Manager struct {
	mu       sync.Mutex
	writeMu  sync.Mutex
	port     serial.Port
	portName string
	stopCh   chan struct{}
	wg       sync.WaitGroup
	logger   *slog.Logger
	vid      uint16
	pid      uint16

	writeCh chan [usbbridgePacketLen]byte
}

type Config struct {
	Logger *slog.Logger
	VID    uint16
	PID    uint16
}

func NewManager(config Config) *Manager {
	manager := &Manager{
		stopCh:  make(chan struct{}),
		writeCh: make(chan [usbbridgePacketLen]byte, defaultWriteQueue),
	}
	if config.Logger != nil {
		manager.logger = config.Logger
	} else {
		manager.logger = slog.New(slog.NewTextHandler(os.Stdout, &slog.HandlerOptions{}))
	}
	manager.logger = manager.logger.With("component", "usbbridge")
	manager.vid = config.VID
	manager.pid = config.PID
	if manager.vid == 0 {
		manager.vid = defaultVID
	}
	if manager.pid == 0 {
		manager.pid = defaultPID
	}
	manager.wg.Go(manager.reconnectLoop)
	manager.wg.Go(manager.deviceLogReadLoop)
	manager.wg.Go(manager.writeWorker)
	return manager
}

func (m *Manager) SendKeyboard(ctx context.Context, keyCode uint16, modifier byte, flags byte, release bool) error {
	if m.currentPort() == nil {
		return fmt.Errorf("usbbridge not connected")
	}
	if ctx == nil {
		ctx = context.Background()
	}
	typeByte := byte(usbbridgeTypeKeyboard)
	if release {
		typeByte |= usbbridgeReleaseFlag
	}
	packet := buildPacket(typeByte, keyCode, modifier, flags)
	return m.enqueuePacket(ctx, packet)
}

func (m *Manager) SendConsumer(ctx context.Context, usage uint16, release bool) error {
	if m.currentPort() == nil {
		return fmt.Errorf("usbbridge not connected")
	}
	if ctx == nil {
		ctx = context.Background()
	}
	typeByte := byte(usbbridgeTypeConsumer)
	if release {
		typeByte |= usbbridgeReleaseFlag
	}
	packet := buildPacket(typeByte, usage, 0, 0)
	return m.enqueuePacket(ctx, packet)
}

func (m *Manager) SendVendor(ctx context.Context, usage uint16, release bool) error {
	if m.currentPort() == nil {
		return fmt.Errorf("usbbridge not connected")
	}
	if ctx == nil {
		ctx = context.Background()
	}
	typeByte := byte(usbbridgeTypeVendor)
	if release {
		typeByte |= usbbridgeReleaseFlag
	}
	packet := buildPacket(typeByte, usage, 0, 0)
	return m.enqueuePacket(ctx, packet)
}

func (m *Manager) enqueuePacket(ctx context.Context, packet [usbbridgePacketLen]byte) error {
	select {
	case m.writeCh <- packet:
		return nil
	case <-m.stopCh:
		return fmt.Errorf("usbbridge closed")
	case <-ctx.Done():
		return fmt.Errorf("usbbridge send canceled: %w", ctx.Err())
	}
}

func buildPacket(typeByte byte, code uint16, modifier byte, flags byte) [usbbridgePacketLen]byte {
	return [usbbridgePacketLen]byte{
		typeByte,
		byte(code & 0xFF),
		byte((code >> 8) & 0xFF),
		modifier,
		flags,
	}
}

func (m *Manager) Close() {
	var port serial.Port
	close(m.stopCh)
	m.mu.Lock()
	port = m.port
	m.port = nil
	m.portName = ""
	m.mu.Unlock()

	if port != nil {
		_ = port.Close()
	}
	m.wg.Wait()
}

func (m *Manager) deviceLogReadLoop() {
	for {
		if m.isStopped() {
			return
		}
		port := m.currentPort()
		if port == nil {
			select {
			case <-m.stopCh:
				return
			case <-time.After(500 * time.Millisecond):
			}
			continue
		}
		if err := m.readLogs(port); err != nil {
			if m.isStopped() {
				return
			}
			if err != io.EOF {
				m.logger.Warn("log read error", "error", err)
			}
			m.disconnectWithLog(err)
			select {
			case <-m.stopCh:
				return
			case <-time.After(500 * time.Millisecond):
			}
		}
	}
}

func (m *Manager) writeWorker() {
	for {
		select {
		case <-m.stopCh:
			return
		case packet := <-m.writeCh:
			port := m.currentPort()
			if port == nil {
				continue
			}
			if err := m.writePacket(port, packet[:]); err != nil {
				if !m.isStopped() {
					m.logger.Warn("write failed", "error", err)
				}
			}
		}
	}
}

func (m *Manager) reconnectLoop() {
	var lastErr string
	var loggedNotFound bool
	for {
		if m.isStopped() {
			return
		}
		if m.currentPort() != nil {
			select {
			case <-m.stopCh:
				return
			case <-time.After(500 * time.Millisecond):
			}
			continue
		}
		err := m.connect()
		if err != nil {
			lastErr, loggedNotFound = m.handleConnectError(err, lastErr, loggedNotFound)
			if !m.sleepUntilRetry(1 * time.Second) {
				return
			}
			continue
		}
		lastErr = ""
		loggedNotFound = false
	}
}

func (m *Manager) handleConnectError(err error, lastErr string, loggedNotFound bool) (string, bool) {
	errMsg := err.Error()
	if errors.Is(err, errDeviceNotFound) {
		if !loggedNotFound {
			m.logger.Warn("device not found", "vid", fmt.Sprintf("0x%04X", m.vid), "pid", fmt.Sprintf("0x%04X", m.pid))
			loggedNotFound = true
		}
	} else if errMsg != lastErr {
		m.logger.Warn("connect failed", "error", err)
	}
	return errMsg, loggedNotFound
}

func (m *Manager) sleepUntilRetry(delay time.Duration) bool {
	select {
	case <-m.stopCh:
		return false
	case <-time.After(delay):
		return true
	}
}

func (m *Manager) isStopped() bool {
	select {
	case <-m.stopCh:
		return true
	default:
		return false
	}
}

func (m *Manager) currentPort() serial.Port {
	m.mu.Lock()
	defer m.mu.Unlock()
	return m.port
}

func (m *Manager) readLogs(port serial.Port) error {
	state := logLineState{}
	readBuf := make([]byte, 256)
	for {
		n, err := port.Read(readBuf)
		if n > 0 {
			m.consumeLogBytes(&state, readBuf[:n])
		}
		if err != nil {
			return err
		}
		if n == 0 && m.isStopped() {
			return io.EOF
		}
	}
}

type logLineState struct {
	buffer    bytes.Buffer
	truncated bool
}

func (m *Manager) consumeLogBytes(state *logLineState, data []byte) {
	for len(data) > 0 {
		line, rest, found := splitAtNewline(data)
		m.appendLogBytes(state, line)
		if found {
			m.flushLogLine(state)
			state.truncated = false
			data = rest
			continue
		}
		return
	}
}

func splitAtNewline(data []byte) ([]byte, []byte, bool) {
	index := bytes.IndexByte(data, '\n')
	if index == -1 {
		return data, nil, false
	}
	return data[:index], data[index+1:], true
}

func (m *Manager) appendLogBytes(state *logLineState, data []byte) {
	for len(data) > 0 {
		space := maxLogLineBytes - state.buffer.Len()
		if space == 0 {
			m.flushTruncatedLine(state)
			continue
		}
		toWrite := space
		if len(data) < toWrite {
			toWrite = len(data)
		}
		_, _ = state.buffer.Write(data[:toWrite])
		data = data[toWrite:]
		if state.buffer.Len() == maxLogLineBytes {
			m.flushTruncatedLine(state)
		}
	}
}

func (m *Manager) flushTruncatedLine(state *logLineState) {
	m.logDeviceLine(state.buffer.Bytes())
	state.buffer.Reset()
	if !state.truncated {
		m.logger.Warn("usbbridge device log line too long, truncated", "max_bytes", maxLogLineBytes)
		state.truncated = true
	}
}

func (m *Manager) flushLogLine(state *logLineState) {
	m.logDeviceLine(state.buffer.Bytes())
	state.buffer.Reset()
}

func (m *Manager) logDeviceLine(line []byte) {
	if len(line) == 0 {
		return
	}
	text := strings.TrimRight(string(line), "\r")
	if text == "" {
		return
	}
	m.logger.Info("usbbridge device", "line", text)
}

func (m *Manager) writePacketWithTimeout(port serial.Port, packet []byte) error {
	if _, err := port.Write(packet); err != nil {
		m.disconnectWithLog(err)
		return fmt.Errorf("usbbridge write failed: %w", err)
	}
	return nil
}

func (m *Manager) findPort() (string, error) {
	ports, err := enumerator.GetDetailedPortsList()
	if err != nil {
		return "", fmt.Errorf("enumerate serial ports: %w", err)
	}

	expectedVID := fmt.Sprintf("%04X", m.vid)
	expectedPID := fmt.Sprintf("%04X", m.pid)
	for _, port := range ports {
		if port == nil || !port.IsUSB {
			continue
		}
		if !strings.EqualFold(port.VID, expectedVID) || !strings.EqualFold(port.PID, expectedPID) {
			continue
		}
		return port.Name, nil
	}

	return "", fmt.Errorf("%w (vid=0x%04X pid=0x%04X)", errDeviceNotFound, m.vid, m.pid)
}

func (m *Manager) writePacket(port serial.Port, packet []byte) error {
	m.mu.Lock()
	if m.port == nil || m.port != port {
		m.mu.Unlock()
		return fmt.Errorf("usbbridge port not connected")
	}
	m.mu.Unlock()
	if len(packet) != usbbridgePacketLen {
		return fmt.Errorf("invalid usbbridge packet length: %d", len(packet))
	}
	m.writeMu.Lock()
	defer m.writeMu.Unlock()
	return m.writePacketWithTimeout(port, packet)
}

func (m *Manager) connect() error {
	m.mu.Lock()
	if m.port != nil {
		m.mu.Unlock()
		return nil
	}
	m.mu.Unlock()
	if m.isStopped() {
		return nil
	}

	portName, err := m.findPort()
	if err != nil {
		return err
	}
	port, err := m.openPortWithRetry(portName)
	if err != nil {
		return err
	}
	m.setPort(port, portName)
	m.logger.Info("connected", "port", portName)
	return nil
}

func (m *Manager) openPortWithRetry(portName string) (serial.Port, error) {
	const maxAttempts = 5
	delay := 150 * time.Millisecond
	var lastErr error
	for attempt := 1; attempt <= maxAttempts; attempt++ {
		port, err := serial.Open(portName, &serial.Mode{BaudRate: defaultBaudRate})
		if err == nil {
			return port, nil
		}
		lastErr = err
		time.Sleep(delay)
		delay += 150 * time.Millisecond
	}
	return nil, fmt.Errorf("open usbbridge port %q after %d attempts: %w", portName, maxAttempts, lastErr)
}

func (m *Manager) setPort(port serial.Port, name string) {
	m.mu.Lock()
	if m.isStopped() {
		m.mu.Unlock()
		_ = port.Close()
		return
	}
	m.port = port
	m.portName = name
	m.mu.Unlock()
}

func (m *Manager) disconnectWithLog(err error) {
	m.disconnectWithOptions(err, true)
}

func (m *Manager) disconnectWithOptions(err error, logError bool) {
	var port serial.Port
	m.mu.Lock()
	if m.port == nil {
		m.mu.Unlock()
		return
	}
	port = m.port
	m.port = nil
	m.portName = ""
	m.mu.Unlock()

	if port != nil {
		_ = port.Close()
	}
	if logError && !m.isStopped() {
		m.logger.Warn("disconnected", "error", err)
	}
}
