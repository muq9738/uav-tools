package jobs

import (
	"context"
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
	"time"
)

type fakeRunner struct {
	mu           sync.Mutex
	calls        int
	release      chan struct{}
	beforeEvents chan struct{}
	fail         error
	report       json.RawMessage
}

func (f *fakeRunner) Convert(ctx context.Context, spec Spec, control Control, sink Sink) (Result, error) {
	f.mu.Lock()
	f.calls++
	f.mu.Unlock()
	if f.beforeEvents != nil {
		select {
		case <-f.beforeEvents:
		case <-ctx.Done():
			return Result{}, ctx.Err()
		}
	}
	sink.Log("info", "开始转换 "+spec.Input)
	sink.Progress(Progress{Stage: "convert", Done: 1, Total: 2, Unit: "files", Source: "converter"})
	if f.release != nil {
		select {
		case <-f.release:
		case <-ctx.Done():
			return Result{}, ctx.Err()
		}
	}
	sink.Log("info", "完成")
	return Result{ExitCode: 0, Report: f.report}, f.fail
}

func (f *fakeRunner) count() int {
	f.mu.Lock()
	defer f.mu.Unlock()
	return f.calls
}

func newInput(t *testing.T) string {
	t.Helper()
	dir := t.TempDir()
	if err := os.WriteFile(filepath.Join(dir, "metadata.xml"), []byte("<ModelMetadata/>"), 0o644); err != nil {
		t.Fatal(err)
	}
	return dir
}

func waitFor(t *testing.T, manager *Manager, id string, want State) Job {
	t.Helper()
	events, unsubscribe, err := manager.Subscribe(id)
	if err != nil {
		t.Fatal(err)
	}
	defer unsubscribe()
	timeout := time.NewTimer(5 * time.Second)
	defer timeout.Stop()
	for {
		job, err := manager.Get(id)
		if err != nil {
			t.Fatal(err)
		}
		if job.State == want {
			return job
		}
		select {
		case <-events:
		case <-timeout.C:
			t.Fatalf("等待状态 %s 超时，当前为 %s（%s）", want, job.State, job.Error)
		}
	}
}

func TestSubmitValidatesInputs(t *testing.T) {
	input := newInput(t)
	filled := t.TempDir()
	if err := os.WriteFile(filepath.Join(filled, "x.txt"), []byte("x"), 0o644); err != nil {
		t.Fatal(err)
	}
	manager, err := New(Config{Runner: &fakeRunner{}})
	if err != nil {
		t.Fatal(err)
	}
	defer manager.Close()

	cases := []struct {
		name   string
		spec   Spec
		expect string
	}{
		{"缺少输入", Spec{Output: filepath.Join(t.TempDir(), "out")}, "缺少输入目录"},
		{"缺少输出", Spec{Input: input}, "缺少输出目录"},
		{"输入不存在", Spec{Input: filepath.Join(t.TempDir(), "missing"), Output: filepath.Join(t.TempDir(), "out")}, "输入目录不可用"},
		{"输出非空", Spec{Input: input, Output: filled}, "输出目录必须为空"},
		{"输出包含在输入内", Spec{Input: input, Output: filepath.Join(input, "out")}, "不能与输入目录互相包含"},
		{"线程过多", Spec{Input: input, Output: filepath.Join(t.TempDir(), "out"), Threads: 100}, "线程数不能超过"},
	}
	for _, test := range cases {
		_, err := manager.Submit(test.spec)
		if err == nil || !strings.Contains(err.Error(), test.expect) {
			t.Fatalf("%s：期望错误包含 %q，实际 %v", test.name, test.expect, err)
		}
	}
	if len(manager.List()) != 0 {
		t.Fatal("校验失败不应留下任务")
	}
}

func TestJobRunsToSuccessAndStreamsLogs(t *testing.T) {
	input := newInput(t)
	report := json.RawMessage(`{"files":2,"glbs":1}`)
	runner := &fakeRunner{report: report}
	manager, err := New(Config{Runner: runner})
	if err != nil {
		t.Fatal(err)
	}
	defer manager.Close()

	job, err := manager.Submit(Spec{Input: input, Output: filepath.Join(t.TempDir(), "out"), Threads: 2})
	if err != nil {
		t.Fatal(err)
	}
	done := waitFor(t, manager, job.ID, StateSucceeded)
	if done.StartedAt == nil || done.FinishedAt == nil {
		t.Fatal("成功任务应记录开始与结束时间")
	}
	if done.ExitCode == nil || *done.ExitCode != 0 {
		t.Fatalf("退出码应为 0，实际 %v", done.ExitCode)
	}
	if string(done.Report) != string(report) {
		t.Fatalf("报告未保存：%s", done.Report)
	}
	if done.Spec.Threads != 2 || !filepath.IsAbs(done.Spec.Input) {
		t.Fatalf("参数未规范化：%+v", done.Spec)
	}

	lines, sequence, err := manager.Logs(job.ID, 0, 100)
	if err != nil {
		t.Fatal(err)
	}
	if len(lines) < 3 {
		t.Fatalf("日志过少：%d", len(lines))
	}
	for i, line := range lines {
		if i > 0 && line.Seq <= lines[i-1].Seq {
			t.Fatal("日志序号必须递增")
		}
	}
	// 只取新行：游标应跳过已有日志。
	rest, _, err := manager.Logs(job.ID, sequence, 100)
	if err != nil {
		t.Fatal(err)
	}
	if len(rest) != 0 {
		t.Fatalf("cursor 之后的日志应为空，实际 %d", len(rest))
	}
}

func TestCancelRunningJob(t *testing.T) {
	input := newInput(t)
	runner := &fakeRunner{release: make(chan struct{})}
	manager, err := New(Config{Runner: runner})
	if err != nil {
		t.Fatal(err)
	}
	defer manager.Close()

	job, err := manager.Submit(Spec{Input: input, Output: filepath.Join(t.TempDir(), "out")})
	if err != nil {
		t.Fatal(err)
	}
	waitFor(t, manager, job.ID, StateRunning)
	if err := manager.Cancel(job.ID); err != nil {
		t.Fatal(err)
	}
	canceled := waitFor(t, manager, job.ID, StateCanceled)
	if canceled.Error == "" {
		t.Fatal("取消后应记录原因")
	}
	if err := manager.Cancel(job.ID); err == nil {
		t.Fatal("已结束任务再次取消应报错")
	}
}

func TestMaxParallelQueuesSecondJob(t *testing.T) {
	input := newInput(t)
	runner := &fakeRunner{release: make(chan struct{})}
	manager, err := New(Config{Runner: runner, MaxParallel: 1})
	if err != nil {
		t.Fatal(err)
	}
	defer manager.Close()

	first, err := manager.Submit(Spec{Input: input, Output: filepath.Join(t.TempDir(), "a")})
	if err != nil {
		t.Fatal(err)
	}
	waitFor(t, manager, first.ID, StateRunning)
	second, err := manager.Submit(Spec{Input: input, Output: filepath.Join(t.TempDir(), "b")})
	if err != nil {
		t.Fatal(err)
	}
	waitFor(t, manager, second.ID, StateQueued)
	close(runner.release)
	waitFor(t, manager, first.ID, StateSucceeded)
	waitFor(t, manager, second.ID, StateSucceeded)
}

func TestQueuedJobCanBeCanceled(t *testing.T) {
	input := newInput(t)
	runner := &fakeRunner{release: make(chan struct{})}
	manager, err := New(Config{Runner: runner, MaxParallel: 1})
	if err != nil {
		t.Fatal(err)
	}
	defer manager.Close()

	first, _ := manager.Submit(Spec{Input: input, Output: filepath.Join(t.TempDir(), "a")})
	waitFor(t, manager, first.ID, StateRunning)
	second, _ := manager.Submit(Spec{Input: input, Output: filepath.Join(t.TempDir(), "b")})
	waitFor(t, manager, second.ID, StateQueued)
	if err := manager.Cancel(second.ID); err != nil {
		t.Fatal(err)
	}
	waitFor(t, manager, second.ID, StateCanceled)
	close(runner.release)
	waitFor(t, manager, first.ID, StateSucceeded)
	if runner.count() != 1 {
		t.Fatalf("取消的排队任务不应执行，实际调用 %d 次", runner.count())
	}
}

func TestStateSurvivesRestartAndInterruptedJobsFail(t *testing.T) {
	dataDir := t.TempDir()
	input := newInput(t)
	manager, err := New(Config{DataDir: dataDir, Runner: &fakeRunner{}, Now: time.Now})
	if err != nil {
		t.Fatal(err)
	}
	job, err := manager.Submit(Spec{Input: input, Output: filepath.Join(t.TempDir(), "out")})
	if err != nil {
		t.Fatal(err)
	}
	waitFor(t, manager, job.ID, StateSucceeded)
	manager.Close()

	// 模拟进程异常退出：索引里留下 running，重启后必须标记失败。
	indexPath := filepath.Join(dataDir, "jobs.json")
	bytes, err := os.ReadFile(indexPath)
	if err != nil {
		t.Fatal(err)
	}
	var stored persisted
	if err := json.Unmarshal(bytes, &stored); err != nil {
		t.Fatal(err)
	}
	stored.Jobs = append(stored.Jobs, Job{
		ID: "interrupted", Spec: Spec{Input: input, Output: filepath.Join(t.TempDir(), "w")},
		State: StateRunning, CreatedAt: time.Now(),
	})
	bytes, _ = json.Marshal(stored)
	if err := os.WriteFile(indexPath, bytes, 0o644); err != nil {
		t.Fatal(err)
	}

	restored, err := New(Config{DataDir: dataDir, Runner: &fakeRunner{}})
	if err != nil {
		t.Fatal(err)
	}
	defer restored.Close()
	saved, err := restored.Get(job.ID)
	if err != nil || saved.State != StateSucceeded {
		t.Fatalf("已完成任务应恢复：%+v %v", saved, err)
	}
	lines, _, err := restored.Logs(job.ID, 0, 100)
	if err != nil || len(lines) == 0 {
		t.Fatalf("重启后应能查询历史日志：%v %d", err, len(lines))
	}
	dead, err := restored.Get("interrupted")
	if err != nil || dead.State != StateFailed || !strings.Contains(dead.Error, "服务重启") {
		t.Fatalf("中断任务应标记失败：%+v %v", dead, err)
	}
}

func TestPreviewSpecSurvivesNormalizationAndJobsJSON(t *testing.T) {
	dataDir := t.TempDir()
	input := newInput(t)
	manager, err := New(Config{DataDir: dataDir, Runner: &fakeRunner{}, Now: time.Now})
	if err != nil {
		t.Fatal(err)
	}
	submitted, err := manager.Submit(Spec{Input: input, Output: filepath.Join(t.TempDir(), "out"), Preview: true})
	if err != nil {
		manager.Close()
		t.Fatal(err)
	}
	if !submitted.Spec.Preview {
		t.Fatal("规范化丢失了 preview 参数")
	}
	done := waitFor(t, manager, submitted.ID, StateSucceeded)
	if !done.Spec.Preview {
		t.Fatal("完成状态丢失了 preview 参数")
	}
	raw, err := os.ReadFile(filepath.Join(dataDir, "jobs.json"))
	if err != nil {
		manager.Close()
		t.Fatal(err)
	}
	if !strings.Contains(string(raw), `"preview": true`) {
		manager.Close()
		t.Fatalf("jobs.json 未持久化 preview：%s", raw)
	}
	manager.Close()

	restored, err := New(Config{DataDir: dataDir, Runner: &fakeRunner{}, Now: time.Now})
	if err != nil {
		t.Fatal(err)
	}
	defer restored.Close()
	saved, err := restored.Get(submitted.ID)
	if err != nil || !saved.Spec.Preview {
		t.Fatalf("重启后 preview 参数未恢复：%+v %v", saved, err)
	}
}

func TestSubscribeReceivesStateAndLogs(t *testing.T) {
	input := newInput(t)
	beforeEvents := make(chan struct{})
	manager, err := New(Config{Runner: &fakeRunner{report: json.RawMessage(`{}`), beforeEvents: beforeEvents}})
	if err != nil {
		t.Fatal(err)
	}
	defer manager.Close()

	job, err := manager.Submit(Spec{Input: input, Output: filepath.Join(t.TempDir(), "out")})
	if err != nil {
		t.Fatal(err)
	}
	channel, unsubscribe, err := manager.Subscribe(job.ID)
	if err != nil {
		t.Fatal(err)
	}
	defer unsubscribe()
	// 先订阅再允许 fake runner 发事件，避免快机器上任务早于订阅完成。
	close(beforeEvents)

	kinds := map[string]int{}
	deadline := time.After(5 * time.Second)
	for {
		select {
		case notification := <-channel:
			kinds[notification.Kind]++
			if notification.Kind == "state" && notification.Job.State == StateSucceeded {
				if kinds["log"] == 0 || kinds["progress"] == 0 {
					t.Fatalf("应收到日志与进度，实际 %v", kinds)
				}
				return
			}
		case <-deadline:
			t.Fatalf("等待成功通知超时，已收到 %v", kinds)
		}
	}
}

func TestConcurrentLogsKeepSubscriptionOrder(t *testing.T) {
	manager, err := New(Config{Runner: &fakeRunner{beforeEvents: make(chan struct{})}})
	if err != nil {
		t.Fatal(err)
	}
	defer manager.Close()
	job, err := manager.Submit(Spec{Input: newInput(t), Output: filepath.Join(t.TempDir(), "out")})
	if err != nil {
		t.Fatal(err)
	}
	running := waitFor(t, manager, job.ID, StateRunning)
	events, unsubscribe, err := manager.Subscribe(job.ID)
	if err != nil {
		t.Fatal(err)
	}
	defer unsubscribe()
	manager.mu.Lock()
	rec := manager.records[job.ID]
	manager.mu.Unlock()
	start := make(chan struct{})
	var writes sync.WaitGroup
	for i := 0; i < 32; i++ {
		writes.Add(1)
		go func() {
			defer writes.Done()
			<-start
			manager.log(rec, "info", "concurrent subprocess log")
		}()
	}
	close(start)
	writes.Wait()
	previous := running.Sequence
	for i := 0; i < 32; i++ {
		select {
		case event := <-events:
			if event.Kind != "log" || event.Line == nil || event.Line.Seq != previous+1 {
				t.Fatalf("log notifications arrived out of order: previous=%d event=%+v", previous, event)
			}
			previous = event.Line.Seq
		case <-time.After(5 * time.Second):
			t.Fatal("missing concurrent log notification")
		}
	}
}

func TestNotFound(t *testing.T) {
	manager, err := New(Config{Runner: &fakeRunner{}})
	if err != nil {
		t.Fatal(err)
	}
	defer manager.Close()
	if _, err := manager.Get("nope"); !errors.Is(err, ErrNotFound) {
		t.Fatalf("期望 ErrNotFound，实际 %v", err)
	}
	if _, _, err := manager.Logs("nope", 0, 10); !errors.Is(err, ErrNotFound) {
		t.Fatalf("期望 ErrNotFound，实际 %v", err)
	}
	if err := manager.Cancel("nope"); !errors.Is(err, ErrNotFound) {
		t.Fatalf("期望 ErrNotFound，实际 %v", err)
	}
	if _, _, err := manager.Subscribe("nope"); !errors.Is(err, ErrNotFound) {
		t.Fatalf("期望 ErrNotFound，实际 %v", err)
	}
}
