package main

import (
	"context"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"log/slog"
	"net"
	"net/http"
	"os"
	"os/signal"
	"strconv"
	"strings"
	"syscall"
	"time"

	usbbridge "github.com/2opremio/picousbkeybridge/client"
	"github.com/2opremio/picousbkeybridge/device"
)

const (
	pusbkbKeyboardFlagAppleFn = 0x01
)

const (
	defaultHost         = "localhost"
	defaultPort         = 8080
	defaultSendTimeoutS = 2
	defaultVID          = "0x0403"
	defaultPID          = "0x6001"
)

func main() {
	host := flag.String("host", defaultHost, "Host to bind the HTTP server to")
	port := flag.Int("port", defaultPort, "Port to bind the HTTP server to")
	sendTimeoutSeconds := flag.Int("send-timeout", defaultSendTimeoutS, "Seconds to wait when queueing an event")
	vidFlag := flag.String("vid", defaultVID, "USB VID of the serial adapter (hex)")
	pidFlag := flag.String("pid", defaultPID, "USB PID of the serial adapter (hex)")
	flag.Parse()

	logger := slog.New(slog.NewTextHandler(os.Stdout, &slog.HandlerOptions{}))
	slog.SetDefault(logger)

  vid, err := parseUSBID(*vidFlag)
  if err != nil {
    logger.Error("invalid VID", "value", *vidFlag, "error", err)
    os.Exit(1)
  }
  pid, err := parseUSBID(*pidFlag)
  if err != nil {
    logger.Error("invalid PID", "value", *pidFlag, "error", err)
    os.Exit(1)
  }

  manager := device.NewManager(device.Config{
    Logger: logger,
    VID:    vid,
    PID:    pid,
  })
	defer manager.Close()

	addr := net.JoinHostPort(*host, strconv.Itoa(*port))
	server := &http.Server{
		Addr:              addr,
		Handler:           newHandler(manager, time.Duration(*sendTimeoutSeconds)*time.Second),
		ReadHeaderTimeout: 5 * time.Second,
	}

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()

	errCh := make(chan error, 1)
	go func() {
		logger.Info("usbbridge server listening", "addr", addr)
		errCh <- server.ListenAndServe()
	}()

	select {
	case <-ctx.Done():
		shutdownCtx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		logger.Info("shutting down")
		if err := server.Shutdown(shutdownCtx); err != nil {
			logger.Error("shutdown error", "error", err)
		}
	case err := <-errCh:
		if err != nil && !errors.Is(err, http.ErrServerClosed) {
			logger.Error("server error", "error", err)
			os.Exit(1)
		}
	}
}

func parseUSBID(value string) (uint16, error) {
  parsed, err := strconv.ParseUint(strings.TrimSpace(value), 0, 16)
  if err != nil {
    return 0, err
  }
  return uint16(parsed), nil
}

type pressReleaseResponse struct {
	Status string `json:"status"`
}

func newHandler(manager *device.Manager, sendTimeout time.Duration) http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("/pressandrelease", func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost {
			http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
			return
		}
		req, err := decodeEventRequest(r)
		if err != nil {
			http.Error(w, err.Error(), http.StatusBadRequest)
			return
		}
		// code=0 is allowed only for modifier-only keyboard events.
		if req.Code == 0 && (strings.TrimSpace(req.Type) != "" && strings.ToLower(strings.TrimSpace(req.Type)) != "keyboard") {
			http.Error(w, "missing code", http.StatusBadRequest)
			return
		}
		if req.Code == 0 && !hasModifiers(req.Modifiers) {
			http.Error(w, "missing code", http.StatusBadRequest)
			return
		}
		sendCtx, cancel := context.WithTimeout(r.Context(), sendTimeout)
		defer cancel()
		if err := sendEvent(sendCtx, manager, req); err != nil {
			http.Error(w, fmt.Sprintf("send failed: %v", err), http.StatusServiceUnavailable)
			return
		}
		w.Header().Set("Content-Type", "application/json")
		_ = json.NewEncoder(w).Encode(pressReleaseResponse{Status: "ok"})
	})

	return mux
}

func decodeEventRequest(r *http.Request) (usbbridge.PressAndReleaseRequest, error) {
	var req usbbridge.PressAndReleaseRequest
	decoder := json.NewDecoder(r.Body)
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(&req); err != nil {
		return req, fmt.Errorf("invalid JSON body")
	}
	if err := decoder.Decode(&struct{}{}); err != io.EOF {
		return req, fmt.Errorf("invalid JSON body")
	}
	if strings.TrimSpace(req.Type) == "" {
		req.Type = "keyboard"
	}
	return req, nil
}

func sendEvent(ctx context.Context, manager *device.Manager, req usbbridge.PressAndReleaseRequest) error {
	switch strings.ToLower(strings.TrimSpace(req.Type)) {
	case "keyboard":
		if req.Code > 0xFF {
			return fmt.Errorf("keyboard code must fit in uint8")
		}
		modifier := modifierMask(req.Modifiers)
		flags := byte(0)
		if appleFnEnabled(req.Modifiers) {
			flags = pusbkbKeyboardFlagAppleFn
		}
		if err := manager.SendKeyboard(ctx, req.Code, modifier, flags, false); err != nil {
			return err
		}
		return manager.SendKeyboard(ctx, req.Code, modifier, flags, true)
	case "consumer":
		if err := manager.SendConsumer(ctx, req.Code, false); err != nil {
			return err
		}
		return manager.SendConsumer(ctx, req.Code, true)
	case "vendor":
		if err := manager.SendVendor(ctx, req.Code, false); err != nil {
			return err
		}
		return manager.SendVendor(ctx, req.Code, true)
	default:
		return fmt.Errorf("invalid type: %s", req.Type)
	}
}

func modifierMask(req *usbbridge.PressAndReleaseModifiers) byte {
	if req == nil {
		return 0
	}
	var mask byte
	if req.LeftCtrl {
		mask |= 0x01
	}
	if req.LeftShift {
		mask |= 0x02
	}
	if req.LeftAlt {
		mask |= 0x04
	}
	if req.LeftGUI {
		mask |= 0x08
	}
	if req.RightCtrl {
		mask |= 0x10
	}
	if req.RightShift {
		mask |= 0x20
	}
	if req.RightAlt {
		mask |= 0x40
	}
	if req.RightGUI {
		mask |= 0x80
	}
	return mask
}

func appleFnEnabled(req *usbbridge.PressAndReleaseModifiers) bool {
	return req != nil && req.AppleFn
}

func hasModifiers(req *usbbridge.PressAndReleaseModifiers) bool {
	if req == nil {
		return false
	}
	return req.LeftCtrl || req.LeftShift || req.LeftAlt || req.LeftGUI ||
		req.RightCtrl || req.RightShift || req.RightAlt || req.RightGUI ||
		req.AppleFn
}
