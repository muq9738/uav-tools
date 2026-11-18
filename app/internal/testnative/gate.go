package testnative

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"sync"
	"testing"
	"time"
)

// Call is a child process stopped at a known file boundary.
type Call struct {
	Options Options
	release chan struct{}
	once    sync.Once
}

func (c *Call) Release() { c.once.Do(func() { close(c.release) }) }

// Gate synchronizes tests with the real child without sleeping for process progress.
type Gate struct {
	URL   string
	calls chan *Call
	done  chan struct{}
}

func NewGate(t *testing.T) *Gate {
	t.Helper()
	g := &Gate{calls: make(chan *Call, 16), done: make(chan struct{})}
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		var options Options
		if err := json.NewDecoder(r.Body).Decode(&options); err != nil {
			t.Errorf("invalid child boundary: %v", err)
			http.Error(w, err.Error(), http.StatusBadRequest)
			return
		}
		call := &Call{Options: options, release: make(chan struct{})}
		select {
		case g.calls <- call:
		case <-g.done:
			return
		case <-r.Context().Done():
			return
		}
		select {
		case <-call.release:
			w.WriteHeader(http.StatusOK)
		case <-g.done:
		case <-r.Context().Done():
		}
	}))
	g.URL = server.URL
	t.Cleanup(func() { close(g.done); server.Close() })
	return g
}

func (g *Gate) Next(t *testing.T) *Call {
	t.Helper()
	select {
	case call := <-g.calls:
		t.Cleanup(call.Release)
		return call
	case <-time.After(10 * time.Second):
		t.Fatal("child did not reach file boundary")
		return nil
	}
}

func Binary(t *testing.T) string {
	t.Helper()
	path, err := os.Executable()
	if err != nil {
		t.Fatal(err)
	}
	return path
}

func Input(t *testing.T, cfg Config) string {
	t.Helper()
	input := t.TempDir()
	if err := os.WriteFile(filepath.Join(input, "model.osgb"), []byte("fixture geometry and encoded textures"), 0o600); err != nil {
		t.Fatal(err)
	}
	Configure(t, input, cfg)
	return input
}

func Configure(t *testing.T, input string, cfg Config) {
	t.Helper()
	payload, err := json.Marshal(cfg)
	if err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(input, ".stub.json"), payload, 0o600); err != nil {
		t.Fatal(err)
	}
}
