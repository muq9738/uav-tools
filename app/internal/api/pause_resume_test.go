package api

import (
	"bufio"
	"context"
	"encoding/json"
	"net/http"
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"testing"
	"time"

	"github.com/muq9738/uav-tools/app/internal/jobs"
	"github.com/muq9738/uav-tools/app/internal/runner"
	"github.com/muq9738/uav-tools/app/internal/testnative"
)

func TestMain(m *testing.M) {
	if code, handled := testnative.Run(os.Args[1:]); handled {
		os.Exit(code)
	}
	os.Exit(m.Run())
}

func nativeManager(t *testing.T, dataDir string) (*jobs.Manager, *runner.Exec) {
	t.Helper()
	e := &runner.Exec{Binary: testnative.Binary(t)}
	manager, err := jobs.New(jobs.Config{Runner: e, DataDir: dataDir, MaxParallel: 1})
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(manager.Close)
	return manager, e
}

func waitManagerState(t *testing.T, manager *jobs.Manager, id string, want jobs.State) jobs.Job {
	t.Helper()
	events, unsubscribe, err := manager.Subscribe(id)
	if err != nil {
		t.Fatal(err)
	}
	defer unsubscribe()
	timeout := time.NewTimer(10 * time.Second)
	defer timeout.Stop()
	for {
		job, err := manager.Get(id)
		if err != nil {
			t.Fatal(err)
		}
		if job.State == want {
			return job
		}
		if job.State.Terminal() {
			t.Fatalf("wanted %s, got %s: %s", want, job.State, job.Error)
		}
		select {
		case <-events:
		case <-timeout.C:
			t.Fatalf("waiting for %s: %+v", want, job)
		}
	}
}

func waitManagerLog(t *testing.T, manager *jobs.Manager, id, text string) {
	t.Helper()
	events, unsubscribe, err := manager.Subscribe(id)
	if err != nil {
		t.Fatal(err)
	}
	defer unsubscribe()
	timeout := time.NewTimer(5 * time.Second)
	defer timeout.Stop()
	for {
		lines, _, err := manager.Logs(id, 0, 5000)
		if err != nil {
			t.Fatal(err)
		}
		for _, line := range lines {
			if strings.Contains(line.Text, text) {
				return
			}
		}
		select {
		case <-events:
		case <-timeout.C:
			t.Fatalf("missing log %q", text)
		}
	}
}

func eventStream(t *testing.T, base, id string) (<-chan jobs.Notification, context.CancelFunc) {
	t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 20*time.Second)
	t.Cleanup(cancel)
	request, err := http.NewRequestWithContext(ctx, http.MethodGet, base+"/api/jobs/"+id+"/events", nil)
	if err != nil {
		t.Fatal(err)
	}
	response, err := http.DefaultClient.Do(request)
	if err != nil {
		t.Fatal(err)
	}
	if response.StatusCode != http.StatusOK {
		response.Body.Close()
		t.Fatalf("SSE status: %d", response.StatusCode)
	}
	events := make(chan jobs.Notification, 128)
	go func() {
		defer response.Body.Close()
		defer close(events)
		scanner := bufio.NewScanner(response.Body)
		for scanner.Scan() {
			line := scanner.Text()
			if !strings.HasPrefix(line, "data: ") {
				continue
			}
			var event jobs.Notification
			if json.Unmarshal([]byte(strings.TrimPrefix(line, "data: ")), &event) != nil {
				continue
			}
			select {
			case events <- event:
			case <-ctx.Done():
				return
			}
		}
	}()
	return events, cancel
}

func waitEventState(t *testing.T, events <-chan jobs.Notification, want jobs.State) {
	t.Helper()
	timeout := time.NewTimer(10 * time.Second)
	defer timeout.Stop()
	for {
		select {
		case event, ok := <-events:
			if !ok {
				t.Fatalf("SSE closed before %s", want)
			}
			if event.Kind == "state" && event.Job.State == want {
				return
			}
		case <-timeout.C:
			t.Fatalf("SSE missed %s", want)
		}
	}
}

func TestPauseResumeRunningEndpointSurvivesRestart(t *testing.T) {
	gate := testnative.NewGate(t)
	input := testnative.Input(t, testnative.Config{GateURL: gate.URL})
	dataDir := t.TempDir()
	manager, e := nativeManager(t, dataDir)
	server, _ := newTestServer(t, Options{Manager: manager, Runner: e, DataDir: dataDir})
	spec := jobs.Spec{Input: input, Output: filepath.Join(t.TempDir(), "out"), Geometry: "raw",
		Threads: 3, MemoryBudgetMiB: 512, TexturePixels: true, SRS: "EPSG:32651", Origin: "500000,0,10"}
	status, created := postJSON(t, server.URL+"/api/jobs", spec)
	if status != http.StatusCreated {
		t.Fatalf("submit: %d %v", status, created)
	}
	id := created["id"].(string)
	boundary := gate.Next(t)
	running := waitManagerState(t, manager, id, jobs.StateRunning)
	events, closeStream := eventStream(t, server.URL, id)
	waitEventState(t, events, jobs.StateRunning)
	status, pausedRequest := postJSON(t, server.URL+"/api/jobs/"+id+"/pause", nil)
	if status != http.StatusAccepted || pausedRequest["state"] != "pausing" {
		t.Fatalf("pause: %d %v", status, pausedRequest)
	}
	waitEventState(t, events, jobs.StatePausing)
	if status, _ := postJSON(t, server.URL+"/api/jobs/"+id+"/pause", nil); status != http.StatusAccepted {
		t.Fatalf("pause must be idempotent: %d", status)
	}
	if status, _ := postJSON(t, server.URL+"/api/jobs/"+id+"/resume", nil); status != http.StatusConflict {
		t.Fatalf("pausing cannot resume before process exit: %d", status)
	}
	if _, err := os.Stat(boundary.Options.CancelFile); !os.IsNotExist(err) {
		t.Fatalf("pause canceled child: %v", err)
	}
	boundary.Release()
	paused := waitManagerState(t, manager, id, jobs.StatePaused)
	waitEventState(t, events, jobs.StatePaused)
	if !paused.ResumeRequired || paused.CheckpointDir != filepath.Join(dataDir, "checkpoints", id) || paused.PausedAt == nil ||
		paused.FinishedAt != nil || paused.ExitCode == nil || *paused.ExitCode != 75 || paused.Progress.Total != 3 || len(paused.Report) == 0 {
		t.Fatalf("invalid paused snapshot: %+v", paused)
	}
	if _, err := os.Stat(filepath.Join(spec.Output, "tileset.json")); !os.IsNotExist(err) {
		t.Fatalf("paused task published output: %v", err)
	}
	before, sequence, err := manager.Logs(id, 0, 100)
	if err != nil || len(before) < 5 {
		t.Fatalf("missing pause logs: %v %v", before, err)
	}
	closeStream()
	server.Close()
	manager.Close()

	restored, e2 := nativeManager(t, dataDir)
	server2, _ := newTestServer(t, Options{Manager: restored, Runner: e2, DataDir: dataDir})
	saved, err := restored.Get(id)
	var beforeReport, afterReport map[string]any
	if json.Unmarshal(paused.Report, &beforeReport) != nil || json.Unmarshal(saved.Report, &afterReport) != nil {
		t.Fatal("invalid persisted pause report")
	}
	if err != nil || saved.State != jobs.StatePaused || saved.CheckpointDir != paused.CheckpointDir || !saved.ResumeRequired || !reflect.DeepEqual(beforeReport, afterReport) {
		t.Fatalf("restart lost paused task: %+v %v", saved, err)
	}
	lines, restoredSequence, err := restored.Logs(id, 0, 100)
	if err != nil || len(lines) != len(before) || restoredSequence < sequence {
		t.Fatalf("restart lost logs: %+v %v", lines, err)
	}
	// A completed independent task is a scheduler barrier: auto-resuming the paused
	// task would block at gate and prevent this single-slot task from finishing.
	marker, err := restored.Submit(jobs.Spec{Input: testnative.Input(t, testnative.Config{}), Output: filepath.Join(t.TempDir(), "marker")})
	if err != nil {
		t.Fatal(err)
	}
	waitManagerState(t, restored, marker.ID, jobs.StateSucceeded)
	if saved, _ := restored.Get(id); saved.State != jobs.StatePaused {
		t.Fatalf("restart automatically executed paused task: %+v", saved)
	}
	events2, closeStream2 := eventStream(t, server2.URL, id)
	defer closeStream2()
	waitEventState(t, events2, jobs.StatePaused)
	status, resumed := postJSON(t, server2.URL+"/api/jobs/"+id+"/resume", nil)
	if status != http.StatusAccepted || resumed["report"] != nil || resumed["exitCode"] != nil {
		t.Fatalf("resume: %d %v", status, resumed)
	}
	second := gate.Next(t)
	if !second.Options.Resume || second.Options.CheckpointDir != paused.CheckpointDir || second.Options.CancelFile == boundary.Options.CancelFile ||
		second.Options.SRS != spec.SRS || second.Options.Origin != spec.Origin || second.Options.Threads != 3 || second.Options.Budget != 512 || !second.Options.TexturePixels {
		t.Fatalf("incorrect resume options: %+v", second.Options)
	}
	if _, err := os.Stat(second.Options.PauseFile); !os.IsNotExist(err) {
		t.Fatalf("stale pause file on resume: %v", err)
	}
	second.Release()
	done := waitManagerState(t, restored, id, jobs.StateSucceeded)
	waitEventState(t, events2, jobs.StateSucceeded)
	if done.StartedAt == nil || !done.StartedAt.Equal(*running.StartedAt) || done.FinishedAt == nil || done.ResumeRequired || done.Error != "" || done.Spec != running.Spec {
		t.Fatalf("resume lost task identity: %+v", done)
	}
	newLines, finalSequence, err := restored.Logs(id, sequence, 100)
	if err != nil || len(newLines) < 3 || finalSequence <= sequence {
		t.Fatalf("missing continuation logs: %+v %v", newLines, err)
	}
	previous := sequence
	for _, line := range newLines {
		if line.Seq <= previous {
			t.Fatal("log sequence reset on resume")
		}
		previous = line.Seq
	}
	if status, _ := postJSON(t, server2.URL+"/api/jobs/"+id+"/resume", nil); status != http.StatusConflict {
		t.Fatalf("completed task resumed: %d", status)
	}
	for _, action := range []string{"pause", "resume"} {
		if status, _ := postJSON(t, server2.URL+"/api/jobs/missing/"+action, nil); status != http.StatusNotFound {
			t.Fatalf("unknown %s: %d", action, status)
		}
	}
}

func TestQueuedPauseSkipsCheckpointAndFirstResumeFlag(t *testing.T) {
	gate := testnative.NewGate(t)
	manager, e := nativeManager(t, t.TempDir())
	server, _ := newTestServer(t, Options{Manager: manager, Runner: e})
	first, err := manager.Submit(jobs.Spec{Input: testnative.Input(t, testnative.Config{GateURL: gate.URL}), Output: filepath.Join(t.TempDir(), "first")})
	if err != nil {
		t.Fatal(err)
	}
	boundary := gate.Next(t)
	second, err := manager.Submit(jobs.Spec{Input: testnative.Input(t, testnative.Config{GateURL: gate.URL}), Output: filepath.Join(t.TempDir(), "second")})
	if err != nil {
		t.Fatal(err)
	}
	status, snapshot := postJSON(t, server.URL+"/api/jobs/"+second.ID+"/pause", nil)
	if status != http.StatusAccepted || snapshot["state"] != "paused" || snapshot["checkpointDir"] != nil || snapshot["resumeRequired"] == true || snapshot["startedAt"] != nil {
		t.Fatalf("queued pause incorrectly started conversion: %d %v", status, snapshot)
	}
	if second, _ := manager.Get(second.ID); second.State != jobs.StatePaused || second.State.Terminal() {
		t.Fatalf("paused must be resumable: %+v", second)
	}
	if err := manager.Cancel(first.ID); err != nil {
		t.Fatal(err)
	}
	waitManagerLog(t, manager, first.ID, "已向转换器发送取消请求")
	boundary.Release()
	waitManagerState(t, manager, first.ID, jobs.StateCanceled)
	if status, _ := postJSON(t, server.URL+"/api/jobs/"+second.ID+"/resume", nil); status != http.StatusAccepted {
		t.Fatalf("queued resume: %d", status)
	}
	firstStart := gate.Next(t)
	if firstStart.Options.Resume || firstStart.Options.CheckpointDir == "" {
		t.Fatalf("never-started task got --resume: %+v", firstStart.Options)
	}
	firstStart.Release()
	waitManagerState(t, manager, second.ID, jobs.StateSucceeded)
}

func TestPausingKeepsSlotAndRequeuedPauseRetainsCheckpoint(t *testing.T) {
	gate := testnative.NewGate(t)
	manager, e := nativeManager(t, t.TempDir())
	server, _ := newTestServer(t, Options{Manager: manager, Runner: e})
	first, err := manager.Submit(jobs.Spec{Input: testnative.Input(t, testnative.Config{GateURL: gate.URL}), Output: filepath.Join(t.TempDir(), "first")})
	if err != nil {
		t.Fatal(err)
	}
	boundary := gate.Next(t)
	if err := manager.Pause(first.ID); err != nil {
		t.Fatal(err)
	}
	second, err := manager.Submit(jobs.Spec{Input: testnative.Input(t, testnative.Config{GateURL: gate.URL}), Output: filepath.Join(t.TempDir(), "second")})
	if err != nil {
		t.Fatal(err)
	}
	if queued, _ := manager.Get(second.ID); queued.State != jobs.StateQueued {
		t.Fatalf("pausing prematurely released scheduler slot: %+v", queued)
	}
	boundary.Release()
	paused := waitManagerState(t, manager, first.ID, jobs.StatePaused)
	other := gate.Next(t)
	if err := manager.Resume(first.ID); err != nil {
		t.Fatal(err)
	}
	if err := manager.Pause(first.ID); err != nil {
		t.Fatal(err)
	}
	repaused := waitManagerState(t, manager, first.ID, jobs.StatePaused)
	if !repaused.ResumeRequired || repaused.CheckpointDir != paused.CheckpointDir {
		t.Fatalf("pausing a queued continuation lost checkpoint: %+v", repaused)
	}
	other.Release()
	waitManagerState(t, manager, second.ID, jobs.StateSucceeded)
	if status, _ := postJSON(t, server.URL+"/api/jobs/"+first.ID+"/cancel", nil); status != http.StatusAccepted {
		t.Fatalf("cannot cancel paused job: %d", status)
	}
	canceled := waitManagerState(t, manager, first.ID, jobs.StateCanceled)
	if canceled.FinishedAt == nil {
		t.Fatal("canceling paused job did not end it")
	}
	if status, _ := postJSON(t, server.URL+"/api/jobs/"+first.ID+"/resume", nil); status != http.StatusConflict {
		t.Fatalf("canceled paused job resumed: %d", status)
	}
}

func TestCancelEndpointReallyCancelsRunningAndPausingChildren(t *testing.T) {
	for _, pausing := range []bool{false, true} {
		t.Run(map[bool]string{false: "running", true: "pausing"}[pausing], func(t *testing.T) {
			gate := testnative.NewGate(t)
			manager, e := nativeManager(t, t.TempDir())
			server, _ := newTestServer(t, Options{Manager: manager, Runner: e})
			job, err := manager.Submit(jobs.Spec{Input: testnative.Input(t, testnative.Config{GateURL: gate.URL}), Output: filepath.Join(t.TempDir(), "out")})
			if err != nil {
				t.Fatal(err)
			}
			boundary := gate.Next(t)
			if pausing {
				if err := manager.Pause(job.ID); err != nil {
					t.Fatal(err)
				}
			}
			if status, _ := postJSON(t, server.URL+"/api/jobs/"+job.ID+"/cancel", nil); status != http.StatusAccepted {
				t.Fatalf("cancel endpoint: %d", status)
			}
			waitManagerLog(t, manager, job.ID, "已向转换器发送取消请求")
			if _, err := os.Stat(boundary.Options.CancelFile); err != nil {
				t.Fatal(err)
			}
			boundary.Release()
			canceled := waitManagerState(t, manager, job.ID, jobs.StateCanceled)
			if canceled.ResumeRequired || canceled.FinishedAt == nil {
				t.Fatalf("cancellation became a resumable pause: %+v", canceled)
			}
		})
	}
}

func TestPauseEndpointsRejectUnsupportedNativeAndPrivateControls(t *testing.T) {
	t.Setenv("OSGB_TEST_NO_PAUSE", "1")
	gate := testnative.NewGate(t)
	manager, e := nativeManager(t, t.TempDir())
	server, _ := newTestServer(t, Options{Manager: manager, Runner: e})
	input := testnative.Input(t, testnative.Config{GateURL: gate.URL})
	job, err := manager.Submit(jobs.Spec{Input: input, Output: filepath.Join(t.TempDir(), "out")})
	if err != nil {
		t.Fatal(err)
	}
	boundary := gate.Next(t)
	for _, action := range []string{"pause", "resume"} {
		if status, _ := postJSON(t, server.URL+"/api/jobs/"+job.ID+"/"+action, nil); status != http.StatusNotImplemented {
			t.Fatalf("unsupported %s must return 501: %d", action, status)
		}
	}
	_, config := getJSON(t, server.URL+"/api/config")
	if config["sidecar"].(map[string]any)["pauseResume"] != false || boundary.Options.CheckpointDir != "" || boundary.Options.PauseFile != "" || boundary.Options.Resume {
		t.Fatalf("unsupported native pretended to pause: %v %+v", config, boundary.Options)
	}
	for _, field := range []string{"checkpointDir", "pauseFile", "resume", "resumeRequired"} {
		if status, _ := postJSON(t, server.URL+"/api/jobs", map[string]any{
			"input": input, "output": filepath.Join(t.TempDir(), "private"), field: "attacker-supplied",
		}); status != http.StatusBadRequest {
			t.Fatalf("private control %s accepted: %d", field, status)
		}
	}
	boundary.Release()
	waitManagerState(t, manager, job.ID, jobs.StateSucceeded)
}

func TestPauseRacingNormalCompletionStillValidatesSuccess(t *testing.T) {
	gate := testnative.NewGate(t)
	manager, _ := nativeManager(t, t.TempDir())
	job, err := manager.Submit(jobs.Spec{Input: testnative.Input(t, testnative.Config{GateURL: gate.URL, Mode: "complete-on-pause"}), Output: filepath.Join(t.TempDir(), "out")})
	if err != nil {
		t.Fatal(err)
	}
	boundary := gate.Next(t)
	if err := manager.Pause(job.ID); err != nil {
		t.Fatal(err)
	}
	boundary.Release()
	done := waitManagerState(t, manager, job.ID, jobs.StateSucceeded)
	if done.ResumeRequired || done.PausedAt != nil || done.ExitCode == nil || *done.ExitCode != 0 {
		t.Fatalf("normal completion became paused: %+v", done)
	}
}

func TestResumeFailureKeepsNativeRejectionAndOutputProtection(t *testing.T) {
	for _, mode := range []string{"damaged", "source", "nonempty-output"} {
		t.Run(mode, func(t *testing.T) {
			gate := testnative.NewGate(t)
			manager, _ := nativeManager(t, t.TempDir())
			input := testnative.Input(t, testnative.Config{GateURL: gate.URL})
			job, err := manager.Submit(jobs.Spec{Input: input, Output: filepath.Join(t.TempDir(), "out")})
			if err != nil {
				t.Fatal(err)
			}
			boundary := gate.Next(t)
			if err := manager.Pause(job.ID); err != nil {
				t.Fatal(err)
			}
			boundary.Release()
			paused := waitManagerState(t, manager, job.ID, jobs.StatePaused)
			want := "checkpoint damaged"
			switch mode {
			case "damaged":
				err = os.WriteFile(filepath.Join(paused.CheckpointDir, "checkpoint.json"), []byte("damaged"), 0o600)
			case "source":
				want = "checkpoint input or parameters changed"
				err = os.WriteFile(filepath.Join(input, "model.osgb"), []byte("changed"), 0o600)
			case "nonempty-output":
				want = "output is not empty"
				if err = os.MkdirAll(job.Spec.Output, 0o700); err == nil {
					err = os.WriteFile(filepath.Join(job.Spec.Output, "keep.txt"), []byte("existing result"), 0o600)
				}
				if err == nil {
					if _, submitErr := manager.Submit(job.Spec); submitErr == nil {
						t.Fatal("nonempty output was accepted")
					}
				}
			}
			if err != nil {
				t.Fatal(err)
			}
			if err := manager.Resume(job.ID); err != nil {
				t.Fatal(err)
			}
			failed := waitManagerState(t, manager, job.ID, jobs.StateFailed)
			if !strings.Contains(failed.Error, want) || failed.ExitCode == nil || *failed.ExitCode != 3 {
				t.Fatalf("native failure was hidden: %+v", failed)
			}
			if _, err := os.Stat(filepath.Join(job.Spec.Output, "tileset.json")); !os.IsNotExist(err) {
				t.Fatalf("failed resume published output: %v", err)
			}
			if mode == "nonempty-output" {
				data, err := os.ReadFile(filepath.Join(job.Spec.Output, "keep.txt"))
				if err != nil || string(data) != "existing result" {
					t.Fatalf("existing output changed: %q %v", data, err)
				}
			}
		})
	}
}
