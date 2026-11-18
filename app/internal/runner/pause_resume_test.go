package runner

import (
	"context"
	"errors"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/muq9738/uav-tools/app/internal/jobs"
	"github.com/muq9738/uav-tools/app/internal/testnative"
)

func TestMain(m *testing.M) {
	if code, handled := testnative.Run(os.Args[1:]); handled {
		os.Exit(code)
	}
	os.Exit(m.Run())
}

type controlledSink struct {
	mu       sync.Mutex
	logs     []string
	progress []jobs.Progress
	cancel   chan struct{}
	once     sync.Once
}

func (s *controlledSink) Log(_, text string) {
	s.mu.Lock()
	s.logs = append(s.logs, text)
	s.mu.Unlock()
	if s.cancel != nil && text == "已向转换器发送取消请求" {
		s.once.Do(func() { close(s.cancel) })
	}
}

func (s *controlledSink) Progress(p jobs.Progress) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.progress = append(s.progress, p)
}

type conversion struct {
	result jobs.Result
	err    error
}

func convertAsync(t *testing.T, e *Exec, ctx context.Context, spec jobs.Spec, control jobs.Control, sink jobs.Sink) <-chan conversion {
	t.Helper()
	result := make(chan conversion, 1)
	go func() {
		r, err := e.Convert(ctx, spec, control, sink)
		result <- conversion{r, err}
	}()
	return result
}

func receiveConversion(t *testing.T, result <-chan conversion) conversion {
	t.Helper()
	select {
	case done := <-result:
		return done
	case <-time.After(10 * time.Second):
		t.Fatal("child conversion did not exit")
		return conversion{}
	}
}

func newControl(t *testing.T) jobs.Control {
	t.Helper()
	work := t.TempDir()
	return jobs.Control{CheckpointDir: filepath.Join(work, "checkpoint"), PauseFile: filepath.Join(work, "pause")}
}

func writePause(t *testing.T, control jobs.Control) {
	t.Helper()
	if err := os.WriteFile(control.PauseFile, nil, 0o600); err != nil {
		t.Fatal(err)
	}
}

func TestSubprocessPauseResumeProtocolAndParameters(t *testing.T) {
	gate := testnative.NewGate(t)
	input := testnative.Input(t, testnative.Config{GateURL: gate.URL})
	spec := jobs.Spec{Input: input, Output: filepath.Join(t.TempDir(), "out"), Threads: 3,
		Geometry: "raw", MemoryBudgetMiB: 512, TexturePixels: true, SRS: "EPSG:32651", Origin: "500000,0,10"}
	e := &Exec{Binary: testnative.Binary(t)}
	if !e.SupportsPauseResume() || !e.Features().PauseResume {
		t.Fatal(e.Features())
	}
	control := newControl(t)
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	sink := &controlledSink{}
	result := convertAsync(t, e, ctx, spec, control, sink)
	boundary := gate.Next(t)
	o := boundary.Options
	if o.Resume || o.CheckpointDir != control.CheckpointDir || o.PauseFile != control.PauseFile ||
		o.CancelFile == o.PauseFile || o.Threads != 3 || o.Geometry != "raw" || o.Budget != 512 ||
		!o.TexturePixels || o.SRS != spec.SRS || o.Origin != spec.Origin {
		t.Fatalf("incorrect child arguments: %+v", o)
	}
	writePause(t, control)
	if _, err := os.Stat(o.CancelFile); !os.IsNotExist(err) {
		t.Fatalf("pause created a cancel token: %v", err)
	}
	boundary.Release()
	paused := receiveConversion(t, result)
	if paused.err != nil || !paused.result.Paused || paused.result.ExitCode != 75 || ctx.Err() != nil {
		t.Fatalf("pause must preserve context: %+v ctx=%v", paused, ctx.Err())
	}
	if _, err := os.Stat(filepath.Join(spec.Output, "tileset.json")); !os.IsNotExist(err) {
		t.Fatalf("paused child published output: %v", err)
	}
	if err := os.Remove(control.PauseFile); err != nil {
		t.Fatal(err)
	}
	control.Resume = true
	result = convertAsync(t, e, ctx, spec, control, sink)
	resumed := gate.Next(t)
	if !resumed.Options.Resume || resumed.Options.CheckpointDir != o.CheckpointDir || resumed.Options.CancelFile == o.CancelFile {
		t.Fatalf("resume must reuse checkpoint and get its own cancel token: %+v", resumed.Options)
	}
	resumed.Release()
	done := receiveConversion(t, result)
	if done.err != nil || done.result.Paused || done.result.ExitCode != 0 {
		t.Fatalf("resume failed: %+v", done)
	}
	asset, err := os.ReadFile(filepath.Join(spec.Output, "asset.glb"))
	if err != nil || string(asset) != "fixture geometry and encoded textures" {
		t.Fatalf("child output: %q %v", asset, err)
	}
	if !strings.Contains(strings.Join(sink.logs, "\n"), "stub: paused") || len(sink.progress) < 3 {
		t.Fatalf("lost subprocess logs/progress: %+v", sink)
	}
}

func TestSubprocessCancelDiffersFromPause(t *testing.T) {
	for _, pausing := range []bool{false, true} {
		t.Run(map[bool]string{false: "running", true: "pausing"}[pausing], func(t *testing.T) {
			gate := testnative.NewGate(t)
			input := testnative.Input(t, testnative.Config{GateURL: gate.URL})
			e := &Exec{Binary: testnative.Binary(t)}
			control := newControl(t)
			ctx, cancel := context.WithCancel(context.Background())
			defer cancel()
			sink := &controlledSink{cancel: make(chan struct{})}
			result := convertAsync(t, e, ctx, jobs.Spec{Input: input, Output: filepath.Join(t.TempDir(), "out")}, control, sink)
			boundary := gate.Next(t)
			if pausing {
				writePause(t, control)
			}
			cancel()
			select {
			case <-sink.cancel: // command.Cancel has actually created the token.
			case <-time.After(5 * time.Second):
				t.Fatal("cancel token was not sent")
			}
			if _, err := os.Stat(boundary.Options.CancelFile); err != nil {
				t.Fatal(err)
			}
			if !pausing {
				if _, err := os.Stat(control.PauseFile); !os.IsNotExist(err) {
					t.Fatalf("cancel created a pause token: %v", err)
				}
			}
			boundary.Release()
			done := receiveConversion(t, result)
			if !errors.Is(done.err, context.Canceled) || done.result.Paused || done.result.ExitCode != 1 {
				t.Fatalf("cancellation became a pause: %+v", done)
			}
		})
	}
}

func TestSubprocessRejectsInvalidPausedResults(t *testing.T) {
	for _, mode := range []string{"wrong-status", "wrong-path", "missing-files", "negative-files", "fractional-files", "missing-report",
		"exit-zero", "exit-one", "missing-checkpoint", "published", "invalid-event", "unknown-event", "after-result", "duplicate-result"} {
		t.Run(mode, func(t *testing.T) {
			gate := testnative.NewGate(t)
			input := testnative.Input(t, testnative.Config{GateURL: gate.URL, Mode: mode})
			control := newControl(t)
			ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
			defer cancel()
			result := convertAsync(t, &Exec{Binary: testnative.Binary(t)}, ctx,
				jobs.Spec{Input: input, Output: filepath.Join(t.TempDir(), "out")}, control, &controlledSink{})
			boundary := gate.Next(t)
			writePause(t, control)
			boundary.Release()
			done := receiveConversion(t, result)
			if done.err == nil || done.result.Paused {
				t.Fatalf("invalid pause accepted (%s): %+v", mode, done)
			}
		})
	}
}

func TestSubprocessLegacyCapabilityAndControls(t *testing.T) {
	t.Setenv("OSGB_TEST_NO_PAUSE", "1")
	e := &Exec{Binary: testnative.Binary(t)}
	if !e.Features().Available || e.SupportsPauseResume() {
		t.Fatal(e.Features())
	}
	gate := testnative.NewGate(t)
	input := testnative.Input(t, testnative.Config{GateURL: gate.URL})
	spec := jobs.Spec{Input: input, Output: filepath.Join(t.TempDir(), "out")}
	if _, err := e.Convert(context.Background(), spec, newControl(t), &controlledSink{}); !errors.Is(err, jobs.ErrPauseUnsupported) {
		t.Fatalf("unsupported controls accepted: %v", err)
	}
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	result := convertAsync(t, e, ctx, spec, jobs.Control{}, &controlledSink{})
	boundary := gate.Next(t)
	if boundary.Options.CheckpointDir != "" || boundary.Options.PauseFile != "" || boundary.Options.Resume {
		t.Fatalf("legacy native received new flags: %+v", boundary.Options)
	}
	boundary.Release()
	if done := receiveConversion(t, result); done.err != nil || done.result.Paused {
		t.Fatalf("legacy conversion failed: %+v", done)
	}
}

func TestSubprocessPropagatesCheckpointRejections(t *testing.T) {
	for _, change := range []string{"source", "parameters", "damaged"} {
		t.Run(change, func(t *testing.T) {
			gate := testnative.NewGate(t)
			input := testnative.Input(t, testnative.Config{GateURL: gate.URL})
			spec := jobs.Spec{Input: input, Output: filepath.Join(t.TempDir(), "out")}
			e := &Exec{Binary: testnative.Binary(t)}
			control := newControl(t)
			ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
			defer cancel()
			result := convertAsync(t, e, ctx, spec, control, &controlledSink{})
			boundary := gate.Next(t)
			writePause(t, control)
			boundary.Release()
			if paused := receiveConversion(t, result); paused.err != nil || !paused.result.Paused {
				t.Fatalf("pause failed: %+v", paused)
			}
			os.Remove(control.PauseFile)
			want := "checkpoint input or parameters changed"
			switch change {
			case "source":
				if err := os.WriteFile(filepath.Join(input, "model.osgb"), []byte("changed input"), 0o600); err != nil {
					t.Fatal(err)
				}
			case "parameters":
				spec.MemoryBudgetMiB = 1
			case "damaged":
				want = "checkpoint damaged"
				if err := os.WriteFile(filepath.Join(control.CheckpointDir, "checkpoint.json"), []byte("invalid"), 0o600); err != nil {
					t.Fatal(err)
				}
			}
			control.Resume = true
			_, err := e.Convert(ctx, spec, control, &controlledSink{})
			if err == nil || !strings.Contains(err.Error(), want) {
				t.Fatalf("native checkpoint rejection lost: %v", err)
			}
		})
	}
}
