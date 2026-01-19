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
	"syscall"
	"time"

	usbbridge "github.com/2opremio/picousbkeybridge/client"
	"github.com/2opremio/picousbkeybridge/device"
)

const (
	defaultHost         = "localhost"
	defaultPort         = 8080
	defaultSendTimeoutS = 2
)

func main() {
	host := flag.String("host", defaultHost, "Host to bind the HTTP server to")
	port := flag.Int("port", defaultPort, "Port to bind the HTTP server to")
	sendTimeoutSeconds := flag.Int("send-timeout", defaultSendTimeoutS, "Seconds to wait when queueing a keypress")
	flag.Parse()

	logger := slog.New(slog.NewTextHandler(os.Stdout, &slog.HandlerOptions{}))
	slog.SetDefault(logger)

	manager := device.NewManager(device.Config{
		Logger: logger,
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

type keypressResponse struct {
	Status string `json:"status"`
}

func newHandler(manager *device.Manager, sendTimeout time.Duration) http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("/keypress", func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost {
			http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
			return
		}
		var req usbbridge.KeypressRequest
		decoder := json.NewDecoder(r.Body)
		decoder.DisallowUnknownFields()
		if err := decoder.Decode(&req); err != nil {
			http.Error(w, "invalid JSON body", http.StatusBadRequest)
			return
		}
		if err := decoder.Decode(&struct{}{}); err != io.EOF {
			http.Error(w, "invalid JSON body", http.StatusBadRequest)
			return
		}

		if req.HIDCode == 0 {
			http.Error(w, "missing hid_code", http.StatusBadRequest)
			return
		}
		sendCtx, cancel := context.WithTimeout(r.Context(), sendTimeout)
		defer cancel()
		if err := manager.Send(sendCtx, req.HIDCode, req.ModifierMask()); err != nil {
			http.Error(w, fmt.Sprintf("send failed: %v", err), http.StatusServiceUnavailable)
			return
		}

		w.Header().Set("Content-Type", "application/json")
		_ = json.NewEncoder(w).Encode(keypressResponse{Status: "ok"})
	})
	return mux
}
