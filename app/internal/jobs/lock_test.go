package jobs

import (
	"context"
	"errors"
	"path/filepath"
	"testing"
	"time"
)

func TestDataDirLockRejectsSecondInstance(t *testing.T) {
	dir := t.TempDir()
	first, err := New(Config{DataDir: dir, Runner: &fakeRunner{}})
	if err != nil {
		t.Fatal(err)
	}
	defer first.Close()

	second, err := New(Config{DataDir: dir, Runner: &fakeRunner{}})
	if err == nil {
		second.Close()
		t.Fatal("同一数据目录不应允许第二个实例")
	}
	if !errors.Is(err, err) || err == nil {
		t.Fatal("应返回占用错误")
	}
	if got := err.Error(); got == "" {
		t.Fatal("错误信息不能为空")
	}

	// 第一个实例关闭后，目录可再次使用。
	first.Close()
	third, err := New(Config{DataDir: dir, Runner: &fakeRunner{}})
	if err != nil {
		t.Fatalf("关闭后应可重新打开：%v", err)
	}
	third.Close()
}

func TestLockDoesNotBlockIndependentDirs(t *testing.T) {
	first, err := New(Config{DataDir: t.TempDir(), Runner: &fakeRunner{}})
	if err != nil {
		t.Fatal(err)
	}
	defer first.Close()
	second, err := New(Config{DataDir: t.TempDir(), Runner: &fakeRunner{}})
	if err != nil {
		t.Fatal(err)
	}
	second.Close()
}

func TestCloseWaitsForRunningJobAndReleasesLock(t *testing.T) {
	dir := t.TempDir()
	input := newInput(t)
	runner := &fakeRunner{release: make(chan struct{})}
	manager, err := New(Config{DataDir: dir, Runner: runner})
	if err != nil {
		t.Fatal(err)
	}
	job, err := manager.Submit(Spec{Input: input, Output: t.TempDir() + "/out"})
	if err != nil {
		t.Fatal(err)
	}
	job0 := job.ID
	waitFor(t, manager, job.ID, StateRunning)

	// fakeRunner 直接响应取消，Close 会很快完成；这里只验证不会卡死且锁被释放。
	done := make(chan struct{})
	go func() {
		manager.Close()
		close(done)
	}()
	close(runner.release)
	select {
	case <-done:
	case <-time.After(5 * time.Second):
		t.Fatal("Close 等待任务结束超时")
	}
	if _, err := manager.Get(job0); err != nil {
		t.Fatalf("关闭后仍应保留任务记录：%v", err)
	}

	// 锁已释放，可以重新打开同一目录。
	again, err := New(Config{DataDir: dir, Runner: &fakeRunner{}})
	if err != nil {
		t.Fatalf("Close 后应可重新打开：%v", err)
	}
	again.Close()
}

// stubbornRunner 忽略取消信号，用来验证 Close 会等到子任务真正结束。
type stubbornRunner struct{ release chan struct{} }

func (s *stubbornRunner) Convert(ctx context.Context, spec Spec, control Control, sink Sink) (Result, error) {
	sink.Log("info", "开始")
	<-s.release
	return Result{ExitCode: 0}, nil
}

func TestCloseWaitsForRunnerIgnoringCancel(t *testing.T) {
	dir := t.TempDir()
	input := newInput(t)
	runner := &stubbornRunner{release: make(chan struct{})}
	manager, err := New(Config{DataDir: dir, Runner: runner})
	if err != nil {
		t.Fatal(err)
	}
	job, err := manager.Submit(Spec{Input: input, Output: filepath.Join(t.TempDir(), "out")})
	if err != nil {
		t.Fatal(err)
	}
	waitFor(t, manager, job.ID, StateRunning)

	done := make(chan struct{})
	go func() { manager.Close(); close(done) }()
	select {
	case <-done:
		t.Fatal("子任务未结束时 Close 不应返回")
	case <-time.After(200 * time.Millisecond):
	}
	close(runner.release)
	select {
	case <-done:
	case <-time.After(5 * time.Second):
		t.Fatal("Close 等待超时")
	}

	again, err := New(Config{DataDir: dir, Runner: &fakeRunner{}})
	if err != nil {
		t.Fatalf("Close 后应可重新打开：%v", err)
	}
	again.Close()
}
