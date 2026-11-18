package runner

import (
	"context"
	"path/filepath"
	"testing"
	"time"

	"github.com/muq9738/uav-tools/app/internal/jobs"
	"github.com/muq9738/uav-tools/app/internal/testnative"
)

func TestPauseAllowsConsumedTokenAndCanonicalCheckpointReport(t *testing.T) {
	for _, mode := range []string{"consume-token", "canonical-path"} {
		t.Run(mode, func(t *testing.T) {
			gate := testnative.NewGate(t)
			control := newControl(t)
			input := testnative.Input(t, testnative.Config{GateURL: gate.URL, Mode: mode})
			ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
			defer cancel()
			result := convertAsync(t, &Exec{Binary: testnative.Binary(t)}, ctx,
				jobs.Spec{Input: input, Output: filepath.Join(t.TempDir(), "out")}, control, &controlledSink{})
			boundary := gate.Next(t)
			writePause(t, control)
			boundary.Release()
			if done := receiveConversion(t, result); done.err != nil || !done.result.Paused {
				t.Fatalf("valid contract variant rejected: %+v", done)
			}
		})
	}
}

func TestCompletionStillRequiresFullReportAndPublishedEntry(t *testing.T) {
	for _, mode := range []string{"complete-missing-budget", "complete-budget", "complete-threads", "complete-geometry", "complete-textures", "complete-missing-textures", "complete-no-tileset"} {
		t.Run(mode, func(t *testing.T) {
			input := testnative.Input(t, testnative.Config{Mode: mode})
			ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
			defer cancel()
			result, err := (&Exec{Binary: testnative.Binary(t)}).Convert(ctx,
				jobs.Spec{Input: input, Output: filepath.Join(t.TempDir(), "out")}, newControl(t), &controlledSink{})
			if err == nil || result.Paused {
				t.Fatalf("invalid completion accepted (%s): %+v %v", mode, result, err)
			}
		})
	}
}
