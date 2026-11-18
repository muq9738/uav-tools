// Package testnative provides a real child-process protocol fixture for App tests.
// It does not validate the actual native conversion engine.
package testnative

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"time"
)

// Options records every effective CLI argument received by the child.
type Options struct {
	Input, Output, Geometry, SRS, Origin string
	Threads                              int
	Budget                               uint64
	TexturePixels, Resume, JSONEvents    bool
	RebuildTop                           bool
	TopSealDistance                      *float64
	CheckpointDir, PauseFile, CancelFile string
}

// Config controls only the fixture's synchronization and malformed-result cases.
type Config struct {
	GateURL string `json:"gateURL"`
	Mode    string `json:"mode,omitempty"`
}

type checkpoint struct {
	Identity string `json:"identity"`
	Done     int    `json:"done"`
}

// Run is called before testing.M.Run in TestMain. A normal test invocation is ignored.
func Run(args []string) (int, bool) {
	if len(args) == 1 && args[0] == "--capabilities" {
		capability := map[string]any{
			"protocolVersion": 1, "jsonEvents": true, "cancelFile": true,
			"pauseResume": os.Getenv("OSGB_TEST_NO_PAUSE") != "1",
			"defaults":    map[string]any{"geometry": "meshopt", "threads": 2, "memoryBudgetMiB": 0, "texturePixels": false},
		}
		json.NewEncoder(os.Stdout).Encode(capability)
		return 0, true
	}
	if len(args) < 2 || strings.HasPrefix(args[0], "-") {
		return 0, false
	}
	code, err := convert(args)
	if err != nil {
		fmt.Fprintln(os.Stderr, "stub:", err)
		return 3, true
	}
	return code, true
}

func convert(args []string) (int, error) {
	o := Options{Input: args[0], Output: args[1], Threads: 2, Geometry: "meshopt"}
	for i := 2; i < len(args); i++ {
		switch args[i] {
		case "--json-events":
			o.JSONEvents = true
		case "--resume":
			o.Resume = true
		case "--texture-pixels":
			o.TexturePixels = true
		case "--rebuild-top":
			o.RebuildTop = true
		default:
			if i+1 >= len(args) {
				return 0, fmt.Errorf("missing value: %s", args[i])
			}
			value := args[i+1]
			switch args[i] {
			case "--threads":
				o.Threads, _ = strconv.Atoi(value)
			case "--memory-budget":
				o.Budget, _ = strconv.ParseUint(value, 10, 64)
			case "--geometry":
				o.Geometry = value
			case "--srs":
				o.SRS = value
			case "--origin":
				o.Origin = value
			case "--checkpoint-dir":
				o.CheckpointDir = value
			case "--pause-file":
				o.PauseFile = value
			case "--cancel-file":
				o.CancelFile = value
			case "--top-seal-distance":
				distance, err := strconv.ParseFloat(value, 64)
				if err != nil {
					return 0, fmt.Errorf("invalid seal distance: %s", value)
				}
				o.TopSealDistance = &distance
			default:
				return 0, fmt.Errorf("unknown option: %s", args[i])
			}
			i++
		}
	}
	if !o.JSONEvents || o.CancelFile == "" {
		return 0, fmt.Errorf("missing structured events/cancel protocol")
	}
	var cfg Config
	config, err := os.ReadFile(filepath.Join(o.Input, ".stub.json"))
	if err != nil {
		return 0, err
	}
	if err := json.Unmarshal(config, &cfg); err != nil {
		return 0, err
	}
	source, err := os.ReadFile(filepath.Join(o.Input, "model.osgb"))
	if err != nil {
		return 0, err
	}
	identityOptions := o
	identityOptions.CheckpointDir, identityOptions.PauseFile, identityOptions.CancelFile = "", "", ""
	identityOptions.Resume = false
	parameters, _ := json.Marshal(identityOptions)
	hash := sha256.Sum256(append(parameters, source...))
	identity := hex.EncodeToString(hash[:])
	manifest := filepath.Join(o.CheckpointDir, "checkpoint.json")
	if o.Resume {
		var saved checkpoint
		raw, err := os.ReadFile(manifest)
		if err != nil || json.Unmarshal(raw, &saved) != nil || saved.Done != 1 {
			return 0, fmt.Errorf("checkpoint damaged")
		}
		if saved.Identity != identity {
			return 0, fmt.Errorf("checkpoint input or parameters changed")
		}
		if exists(o.PauseFile) {
			return 0, fmt.Errorf("stale pause token on resume")
		}
	}
	entries, err := os.ReadDir(o.Output)
	if err != nil && !os.IsNotExist(err) {
		return 0, err
	}
	if len(entries) != 0 {
		return 0, fmt.Errorf("output is not empty")
	}
	if o.CheckpointDir != "" {
		if err := os.MkdirAll(o.CheckpointDir, 0o700); err != nil {
			return 0, err
		}
		data, _ := json.Marshal(checkpoint{Identity: identity, Done: 1})
		if err := os.WriteFile(manifest, data, 0o600); err != nil {
			return 0, err
		}
	}
	event := json.NewEncoder(os.Stdout)
	event.Encode(map[string]any{"type": "log", "level": "info", "message": "stub: file boundary"})
	event.Encode(map[string]any{"type": "progress", "stage": "convert", "done": 1, "total": 2, "unit": "files"})
	if cfg.GateURL != "" {
		request, _ := json.Marshal(o)
		client := &http.Client{Timeout: 15 * time.Second}
		response, err := client.Post(cfg.GateURL, "application/json", bytes.NewReader(request))
		if err != nil {
			return 0, err
		}
		io.Copy(io.Discard, response.Body)
		response.Body.Close()
		if response.StatusCode != http.StatusOK {
			return 0, fmt.Errorf("boundary gate returned %d", response.StatusCode)
		}
	}
	// Cancellation wins even when the pause file is also present.
	if exists(o.CancelFile) {
		event.Encode(map[string]any{"type": "log", "level": "info", "message": "stub: canceled"})
		return 1, nil
	}
	if exists(o.PauseFile) && cfg.Mode != "complete-on-pause" {
		event.Encode(map[string]any{"type": "progress", "stage": "convert", "done": 1, "total": 3, "unit": "files"})
		report := map[string]any{"status": "paused", "checkpointDir": o.CheckpointDir, "filesCompleted": 1}
		exit := 75
		switch cfg.Mode {
		case "wrong-status":
			report["status"] = "complete"
		case "wrong-path":
			report["checkpointDir"] = filepath.Dir(o.CheckpointDir)
		case "missing-files":
			delete(report, "filesCompleted")
		case "negative-files":
			report["filesCompleted"] = -1
		case "fractional-files":
			report["filesCompleted"] = 1.5
		case "missing-report":
			report = nil
		case "exit-zero":
			exit = 0
		case "exit-one":
			exit = 1
		case "consume-token":
			os.Remove(o.PauseFile)
		case "canonical-path":
			if canonical, err := filepath.EvalSymlinks(o.CheckpointDir); err == nil {
				report["checkpointDir"] = canonical
			}
		case "missing-checkpoint":
			os.RemoveAll(o.CheckpointDir)
		case "published":
			os.MkdirAll(o.Output, 0o700)
			os.WriteFile(filepath.Join(o.Output, "tileset.json"), []byte("{}"), 0o600)
		case "invalid-event":
			fmt.Fprintln(os.Stdout, "not JSON")
		case "unknown-event":
			event.Encode(map[string]any{"type": "paused"})
		}
		event.Encode(map[string]any{"type": "log", "level": "info", "message": "stub: paused"})
		event.Encode(map[string]any{"type": "result", "report": report})
		if cfg.Mode == "after-result" {
			event.Encode(map[string]any{"type": "progress", "done": 2})
		}
		if cfg.Mode == "duplicate-result" {
			event.Encode(map[string]any{"type": "result", "report": report})
		}
		return exit, nil
	}
	if err := os.MkdirAll(o.Output, 0o700); err != nil {
		return 0, err
	}
	if err := os.WriteFile(filepath.Join(o.Output, "asset.glb"), source, 0o600); err != nil {
		return 0, err
	}
	if err := os.WriteFile(filepath.Join(o.Output, "tileset.json"), []byte(`{"root":{"content":{"uri":"asset.glb"}}}`), 0o600); err != nil {
		return 0, err
	}
	geometry := o.Geometry
	if geometry == "meshopt" {
		geometry = "EXT_meshopt_compression"
	}
	report := map[string]any{
		"status": "complete", "geometryEncoding": geometry, "requestedThreads": o.Threads,
		"memoryBudgetBytes": o.Budget * (1 << 20), "preserveEncodedTextures": !o.TexturePixels,
		"srs": o.SRS, "srsOrigin": o.Origin, "files": 2, "glbs": 1,
		"rebuildTop": o.RebuildTop,
	}
	if o.TopSealDistance != nil {
		report["topSealDistance"] = *o.TopSealDistance
	}
	switch cfg.Mode {
	case "complete-missing-budget":
		delete(report, "memoryBudgetBytes")
	case "complete-budget":
		report["memoryBudgetBytes"] = 123
	case "complete-threads":
		report["requestedThreads"] = 64
	case "complete-geometry":
		report["geometryEncoding"] = "incorrect"
	case "complete-textures":
		report["preserveEncodedTextures"] = o.TexturePixels
	case "complete-missing-textures":
		delete(report, "preserveEncodedTextures")
	case "complete-no-tileset":
		os.Remove(filepath.Join(o.Output, "tileset.json"))
	}
	event.Encode(map[string]any{"type": "progress", "stage": "done", "done": 2, "total": 2, "unit": "files"})
	event.Encode(map[string]any{"type": "result", "report": report})
	return 0, nil
}

func exists(path string) bool {
	if path == "" {
		return false
	}
	_, err := os.Stat(path)
	return err == nil
}
