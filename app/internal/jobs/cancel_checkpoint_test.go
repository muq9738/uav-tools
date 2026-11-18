package jobs

import (
	"context"
	"encoding/json"
	"os"
	"path/filepath"
	"testing"
)

type pausedCheckpointRunner struct{}

func (*pausedCheckpointRunner) SupportsPauseResume() bool { return true }
func (*pausedCheckpointRunner) Convert(_ context.Context, _ Spec, control Control, sink Sink) (Result, error) {
	if err := os.MkdirAll(control.CheckpointDir, 0o700); err != nil {
		return Result{}, err
	}
	if err := os.WriteFile(filepath.Join(control.CheckpointDir, "owned-asset"), []byte("owned"), 0o600); err != nil {
		return Result{}, err
	}
	report, _ := json.Marshal(map[string]any{"status": "paused", "checkpointDir": control.CheckpointDir, "filesCompleted": 1})
	sink.Progress(Progress{Stage: "paused", Done: 1, Source: "converter"})
	return Result{Paused: true, ExitCode: 75, Report: report}, nil
}
func checkpointJob(t *testing.T) (*Manager, Job) {
	t.Helper()
	m, err := New(Config{DataDir: t.TempDir(), Runner: &pausedCheckpointRunner{}})
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(m.Close)
	job, err := m.Submit(Spec{Input: newInput(t), Output: filepath.Join(t.TempDir(), "out")})
	if err != nil {
		t.Fatal(err)
	}
	return m, waitFor(t, m, job.ID, StatePaused)
}
func TestCancelPausedRemovesOnlyOwnedCheckpoint(t *testing.T) {
	m, job := checkpointJob(t)
	foreign := filepath.Join(m.workDir, "checkpoints", "unrelated")
	if err := os.MkdirAll(foreign, 0o700); err != nil {
		t.Fatal(err)
	}
	sentinel := filepath.Join(foreign, "keep")
	if err := os.WriteFile(sentinel, []byte("keep"), 0o600); err != nil {
		t.Fatal(err)
	}
	if err := m.Cancel(job.ID); err != nil {
		t.Fatal(err)
	}
	final, _ := m.Get(job.ID)
	if final.State != StateCanceled || final.ResumeRequired {
		t.Fatal(final)
	}
	if _, err := os.Lstat(job.CheckpointDir); !os.IsNotExist(err) {
		t.Fatal("取消后残留检查点", err)
	}
	if _, err := os.Stat(sentinel); err != nil {
		t.Fatal("影响了其他目录", err)
	}
}
func TestCancelPausedRefusesForeignCheckpoint(t *testing.T) {
	m, job := checkpointJob(t)
	foreign := t.TempDir()
	keep := filepath.Join(foreign, "keep")
	if err := os.WriteFile(keep, []byte("keep"), 0o600); err != nil {
		t.Fatal(err)
	}
	m.mu.Lock()
	m.records[job.ID].job.CheckpointDir = foreign
	m.mu.Unlock()
	if err := m.Cancel(job.ID); err == nil {
		t.Fatal("应拒绝非本任务的检查点")
	}
	final, _ := m.Get(job.ID)
	if final.State != StatePaused {
		t.Fatal(final)
	}
	if _, err := os.Stat(keep); err != nil {
		t.Fatal(err)
	}
}
func TestCancelPausedPersistenceFailurePreservesCheckpoint(t *testing.T) {
	m, job := checkpointJob(t)
	index := filepath.Join(m.workDir, "jobs.json")
	saved, err := os.ReadFile(index)
	if err != nil {
		t.Fatal(err)
	}
	if err = os.Remove(index); err != nil {
		t.Fatal(err)
	}
	if err = os.Mkdir(index, 0o700); err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { os.Remove(index); os.WriteFile(index, saved, 0o600) })
	if err = m.Cancel(job.ID); err == nil {
		t.Fatal("保存失败必须返回错误")
	}
	final, _ := m.Get(job.ID)
	if final.State != StatePaused || !final.ResumeRequired {
		t.Fatal(final)
	}
	if _, err = os.Stat(filepath.Join(job.CheckpointDir, "owned-asset")); err != nil {
		t.Fatal("保存失败不能先删检查点", err)
	}
}
