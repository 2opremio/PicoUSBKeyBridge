package usbbridge

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"strings"
)

const defaultHost = "localhost:8080"

type Client struct {
	baseURL string
	http    *http.Client
}

type Config struct {
	Host       string
	HTTPClient *http.Client
}

func New(config Config) *Client {
	httpClient := config.HTTPClient
	if httpClient == nil {
		httpClient = &http.Client{}
	}
	host := strings.TrimSpace(config.Host)
	if host == "" {
		host = defaultHost
	}
	baseURL := "http://" + strings.TrimRight(host, "/")
	return &Client{
		baseURL: baseURL,
		http:    httpClient,
	}
}

type KeypressRequest struct {
	// HIDCode is the USB HID Usage ID (Keyboard/Keypad page).
	HIDCode   uint8 `json:"hid_code"`
	LeftCtrl  bool  `json:"left_ctrl"`
	LeftShift bool  `json:"left_shift"`
	LeftAlt   bool  `json:"left_alt"`
	LeftGUI   bool  `json:"left_gui"`
	RightCtrl  bool `json:"right_ctrl"`
	RightShift bool `json:"right_shift"`
	RightAlt   bool `json:"right_alt"`
	RightGUI   bool `json:"right_gui"`
}

func (r KeypressRequest) ModifierMask() byte {
	var mask byte
	if r.LeftCtrl {
		mask |= 0x01
	}
	if r.LeftShift {
		mask |= 0x02
	}
	if r.LeftAlt {
		mask |= 0x04
	}
	if r.LeftGUI {
		mask |= 0x08
	}
	if r.RightCtrl {
		mask |= 0x10
	}
	if r.RightShift {
		mask |= 0x20
	}
	if r.RightAlt {
		mask |= 0x40
	}
	if r.RightGUI {
		mask |= 0x80
	}
	return mask
}

func (c *Client) SendKeypress(ctx context.Context, req KeypressRequest) error {
	payload, err := json.Marshal(req)
	if err != nil {
		return fmt.Errorf("marshal keypress: %w", err)
	}
	httpReq, err := http.NewRequestWithContext(ctx, http.MethodPost, c.baseURL+"/keypress", bytes.NewReader(payload))
	if err != nil {
		return fmt.Errorf("build keypress request: %w", err)
	}
	httpReq.Header.Set("Content-Type", "application/json")
	resp, err := c.http.Do(httpReq)
	if err != nil {
		return fmt.Errorf("send keypress request: %w", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		body, _ := io.ReadAll(io.LimitReader(resp.Body, 4096))
		return fmt.Errorf("keypress request failed: %s (%s)", resp.Status, string(body))
	}
	return nil
}
