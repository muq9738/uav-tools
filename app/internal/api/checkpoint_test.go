package api

import (
	"encoding/json"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/muq9738/uav-tools/app/internal/jobs"
	"github.com/muq9738/uav-tools/app/internal/testnative"
)

func TestPauseAndResumeDoNotAcknowledgeFailedPersistence(t *testing.T) {
	gate := testnative.NewGate(t)
	dataDir := t.TempDir()
	manager, _ := nativeManager(t, dataDir)
	job, err := manager.Submit(jobs.Spec{Input: testnative.Input(t, testnative.Config{GateURL: gate.URL}), Output: filepath.Join(t.TempDir(), "out")})
	if err != nil {
		t.Fatal(err)
	}
	boundary := gate.Next(t)
	tmp := filepath.Join(dataDir, "jobs.json.tmp")
	if err := os.Mkdir(tmp, 0o700); err != nil {
		t.Fatal(err)
	}
	if err := manager.Pause(job.ID); err == nil {
		t.Fatal("pause acknowledged a failed index write")
	}
	if current, _ := manager.Get(job.ID); current.State != jobs.StateRunning {
		t.Fatalf("failed pause changed state: %+v", current)
	}
	if _, err := os.Stat(boundary.Options.PauseFile); !os.IsNotExist(err) {
		t.Fatalf("failed pause sent a token: %v", err)
	}
	if err := os.Remove(tmp); err != nil {
		t.Fatal(err)
	}
	if err := manager.Pause(job.ID); err != nil {
		t.Fatal(err)
	}
	boundary.Release()
	paused := waitManagerState(t, manager, job.ID, jobs.StatePaused)
	if err := os.Mkdir(tmp, 0o700); err != nil {
		t.Fatal(err)
	}
	if err := manager.Resume(job.ID); err == nil {
		t.Fatal("resume acknowledged a failed index write")
	}
	if current, _ := manager.Get(job.ID); current.State != jobs.StatePaused || current.CheckpointDir != paused.CheckpointDir || !current.ResumeRequired {
		t.Fatalf("failed resume lost paused state: %+v", current)
	}
	if err := os.Remove(tmp); err != nil {
		t.Fatal(err)
	}
	if err := manager.Resume(job.ID); err != nil {
		t.Fatal(err)
	}
	resumed := gate.Next(t)
	if !resumed.Options.Resume {
		t.Fatal("retry after persistence failure did not resume")
	}
	resumed.Release()
	waitManagerState(t, manager, job.ID, jobs.StateSucceeded)
}

func TestRestartRetainsBothPauseKindsWithoutCurrentCapability(t *testing.T) {
	gate := testnative.NewGate(t)
	dataDir := t.TempDir()
	manager, _ := nativeManager(t, dataDir)
	first, err := manager.Submit(jobs.Spec{Input: testnative.Input(t, testnative.Config{GateURL: gate.URL}), Output: filepath.Join(t.TempDir(), "first")})
	if err != nil {
		t.Fatal(err)
	}
	boundary := gate.Next(t)
	second, err := manager.Submit(jobs.Spec{Input: testnative.Input(t, testnative.Config{GateURL: gate.URL}), Output: filepath.Join(t.TempDir(), "second")})
	if err != nil {
		t.Fatal(err)
	}
	if err := manager.Pause(second.ID); err != nil {
		t.Fatal(err)
	}
	if err := manager.Pause(first.ID); err != nil {
		t.Fatal(err)
	}
	boundary.Release()
	waitManagerState(t, manager, first.ID, jobs.StatePaused)
	manager.Close()

	t.Setenv("OSGB_TEST_NO_PAUSE", "1")
	restored, e := nativeManager(t, dataDir)
	server, _ := newTestServer(t, Options{Manager: restored, Runner: e})
	for _, id := range []string{first.ID, second.ID} {
		if job, _ := restored.Get(id); job.State != jobs.StatePaused {
			t.Fatalf("capability change discarded pause history: %+v", job)
		}
		if status, _ := postJSON(t, server.URL+"/api/jobs/"+id+"/resume", nil); status != http.StatusNotImplemented {
			t.Fatalf("unsupported resume must return 501: %d", status)
		}
	}
	queued, _ := restored.Get(second.ID)
	if queued.CheckpointDir != "" || queued.ResumeRequired || queued.StartedAt != nil {
		t.Fatalf("restart manufactured a queued checkpoint: %+v", queued)
	}
	marker, err := restored.Submit(jobs.Spec{Input: testnative.Input(t, testnative.Config{}), Output: filepath.Join(t.TempDir(), "marker")})
	if err != nil {
		t.Fatal(err)
	}
	waitManagerState(t, restored, marker.ID, jobs.StateSucceeded)
	server.Close()
	restored.Close()

	t.Setenv("OSGB_TEST_NO_PAUSE", "0")
	again, _ := nativeManager(t, dataDir)
	if err := again.Resume(second.ID); err != nil {
		t.Fatal(err)
	}
	started := gate.Next(t)
	if started.Options.Resume {
		t.Fatal("never-started task acquired --resume across restarts")
	}
	started.Release()
	waitManagerState(t, again, second.ID, jobs.StateSucceeded)
	if first, _ := again.Get(first.ID); first.State != jobs.StatePaused {
		t.Fatalf("another resume automatically started paused history: %+v", first)
	}
}

func TestClosedManagerCannotMutatePausedHistory(t *testing.T) {
	gate := testnative.NewGate(t)
	manager, _ := nativeManager(t, t.TempDir())
	job, err := manager.Submit(jobs.Spec{Input: testnative.Input(t, testnative.Config{GateURL: gate.URL}), Output: filepath.Join(t.TempDir(), "out")})
	if err != nil {
		t.Fatal(err)
	}
	boundary := gate.Next(t)
	if err := manager.Pause(job.ID); err != nil {
		t.Fatal(err)
	}
	boundary.Release()
	paused := waitManagerState(t, manager, job.ID, jobs.StatePaused)
	manager.Close()
	for name, action := range map[string]func(string) error{"pause": manager.Pause, "resume": manager.Resume, "cancel": manager.Cancel} {
		if err := action(job.ID); err == nil {
			t.Fatalf("closed manager accepted %s", name)
		}
	}
	current, err := manager.Get(job.ID)
	if err != nil || current.State != jobs.StatePaused || current.Sequence != paused.Sequence {
		t.Fatalf("closed manager changed paused history: %+v %v", current, err)
	}
}

func TestRestoredCheckpointCannotBelongToAnotherTask(t *testing.T) {
	gate := testnative.NewGate(t)
	dataDir := t.TempDir()
	manager, _ := nativeManager(t, dataDir)
	job, err := manager.Submit(jobs.Spec{Input: testnative.Input(t, testnative.Config{GateURL: gate.URL}), Output: filepath.Join(t.TempDir(), "out")})
	if err != nil {
		t.Fatal(err)
	}
	boundary := gate.Next(t)
	if err := manager.Pause(job.ID); err != nil {
		t.Fatal(err)
	}
	boundary.Release()
	waitManagerState(t, manager, job.ID, jobs.StatePaused)
	manager.Close()

	path := filepath.Join(dataDir, "jobs.json")
	raw, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	var stored struct {
		Jobs []jobs.Job `json:"jobs"`
	}
	if err := json.Unmarshal(raw, &stored); err != nil {
		t.Fatal(err)
	}
	outside := t.TempDir()
	marker := filepath.Join(outside, "keep.txt")
	if err := os.WriteFile(marker, []byte("other task data"), 0o600); err != nil {
		t.Fatal(err)
	}
	stored.Jobs[0].CheckpointDir = outside
	raw, _ = json.Marshal(stored)
	if err := os.WriteFile(path, raw, 0o600); err != nil {
		t.Fatal(err)
	}
	restored, _ := nativeManager(t, dataDir)
	failed, err := restored.Get(job.ID)
	if err != nil || failed.State != jobs.StateFailed || !strings.Contains(failed.Error, "不属于本任务") {
		t.Fatalf("foreign checkpoint was accepted: %+v %v", failed, err)
	}
	if err := restored.Resume(job.ID); err == nil {
		t.Fatal("foreign checkpoint was resumed")
	}
	if content, err := os.ReadFile(marker); err != nil || string(content) != "other task data" {
		t.Fatalf("foreign task files were touched: %q %v", content, err)
	}
}
