package api

import (
	"bufio"
	"context"
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"testing/fstest"
	"time"

	"github.com/muq9738/uav-tools/app/internal/jobs"
	"github.com/muq9738/uav-tools/app/internal/runner"
)

type stubRunner struct {
	release chan struct{}
	fail    error
}

func (s *stubRunner) Convert(ctx context.Context, spec jobs.Spec, control jobs.Control, sink jobs.Sink) (jobs.Result, error) {
	sink.Log("info", "转换中")
	sink.Progress(jobs.Progress{Stage: "convert", Done: 1, Total: 2, Unit: "files", Source: "converter"})
	if s.release != nil {
		select {
		case <-s.release:
		case <-ctx.Done():
			return jobs.Result{}, ctx.Err()
		}
	}
	return jobs.Result{ExitCode: 0, Report: json.RawMessage(`{"files":2,"glbs":1}`)}, s.fail
}

type stubFeatures struct{ features runner.Features }

func (s stubFeatures) Features() runner.Features { return s.features }

func newTestServer(t *testing.T, options Options) (*httptest.Server, *jobs.Manager) {
	t.Helper()
	if options.Manager == nil {
		manager, err := jobs.New(jobs.Config{Runner: &stubRunner{}})
		if err != nil {
			t.Fatal(err)
		}
		t.Cleanup(manager.Close)
		options.Manager = manager
	}
	if options.Runner == nil {
		options.Runner = stubFeatures{features: runner.Features{Path: "/opt/osgb-tiles", Available: true}}
	}
	if options.Web == nil {
		options.Web = fstest.MapFS{
			"index.html": {Data: []byte("<title>OSGB 转 3D Tiles</title>")},
			"app.js":     {Data: []byte("console.log('ok')")},
		}
	}
	server, err := New(options)
	if err != nil {
		t.Fatal(err)
	}
	httpServer := httptest.NewServer(server.Handler())
	t.Cleanup(httpServer.Close)
	return httpServer, options.Manager
}

func getJSON(t *testing.T, url string, headers ...string) (int, map[string]any) {
	t.Helper()
	request, err := http.NewRequest(http.MethodGet, url, nil)
	if err != nil {
		t.Fatal(err)
	}
	for i := 0; i+1 < len(headers); i += 2 {
		request.Header.Set(headers[i], headers[i+1])
	}
	response, err := http.DefaultClient.Do(request)
	if err != nil {
		t.Fatal(err)
	}
	defer response.Body.Close()
	var payload map[string]any
	json.NewDecoder(response.Body).Decode(&payload)
	return response.StatusCode, payload
}

func postJSON(t *testing.T, url string, body any, headers ...string) (int, map[string]any) {
	t.Helper()
	encoded, err := json.Marshal(body)
	if err != nil {
		t.Fatal(err)
	}
	request, err := http.NewRequest(http.MethodPost, url, strings.NewReader(string(encoded)))
	if err != nil {
		t.Fatal(err)
	}
	request.Header.Set("Content-Type", "application/json")
	for i := 0; i+1 < len(headers); i += 2 {
		request.Header.Set(headers[i], headers[i+1])
	}
	response, err := http.DefaultClient.Do(request)
	if err != nil {
		t.Fatal(err)
	}
	defer response.Body.Close()
	var payload map[string]any
	json.NewDecoder(response.Body).Decode(&payload)
	return response.StatusCode, payload
}

func sampleInput(t *testing.T) string {
	t.Helper()
	dir := t.TempDir()
	if err := os.WriteFile(filepath.Join(dir, "metadata.xml"), []byte("<ModelMetadata/>"), 0o644); err != nil {
		t.Fatal(err)
	}
	return dir
}

func TestConfigHealthAndDoctor(t *testing.T) {
	server, _ := newTestServer(t, Options{Version: "9.9.9", DataDir: t.TempDir(), Desktop: true})

	status, health := getJSON(t, server.URL+"/api/health")
	if status != http.StatusOK || health["version"] != "9.9.9" || health["sidecarAvailable"] != true {
		t.Fatalf("健康检查异常：%d %v", status, health)
	}
	status, config := getJSON(t, server.URL+"/api/config")
	if status != http.StatusOK || config["desktop"] != true || config["pickerAvailable"] != false {
		t.Fatalf("配置异常：%d %v", status, config)
	}
	status, doctor := getJSON(t, server.URL+"/api/doctor")
	if status != http.StatusOK || doctor["dataDirWritable"] != true {
		t.Fatalf("诊断异常：%d %v", status, doctor)
	}
}

func TestStaticPageServesIndex(t *testing.T) {
	server, _ := newTestServer(t, Options{})
	for _, path := range []string{"/", "/app.js", "/unknown/route"} {
		response, err := http.Get(server.URL + path)
		if err != nil {
			t.Fatal(err)
		}
		body := make([]byte, 256)
		count, _ := response.Body.Read(body)
		response.Body.Close()
		if response.StatusCode != http.StatusOK {
			t.Fatalf("%s 返回 %d", path, response.StatusCode)
		}
		if count == 0 {
			t.Fatalf("%s 内容为空", path)
		}
	}
}

func TestCreateJobValidationAndLifecycle(t *testing.T) {
	server, manager := newTestServer(t, Options{Runner: stubFeatures{features: runner.Features{Available: true}}, Manager: newManager(t, &stubRunner{})})

	status, payload := postJSON(t, server.URL+"/api/jobs", map[string]any{"input": "", "output": ""})
	if status != http.StatusBadRequest || !strings.Contains(fmt.Sprint(payload["error"]), "输入目录") {
		t.Fatalf("空输入应返回 400：%d %v", status, payload)
	}

	input := sampleInput(t)
	output := filepath.Join(t.TempDir(), "out")
	status, created := postJSON(t, server.URL+"/api/jobs", map[string]any{"input": input, "output": output, "threads": 2})
	if status != http.StatusCreated {
		t.Fatalf("创建任务失败：%d %v", status, created)
	}
	id, _ := created["id"].(string)
	if id == "" {
		t.Fatalf("缺少任务 ID：%v", created)
	}

	waitManagerState(t, manager, id, jobs.StateSucceeded)
	if status, job := getJSON(t, server.URL+"/api/jobs/"+id); status != http.StatusOK || job["state"] != "succeeded" {
		t.Fatalf("任务详情未反映完成状态：%d %v", status, job)
	}

	status, logs := getJSON(t, server.URL+"/api/jobs/"+id+"/logs?after=0")
	if status != http.StatusOK {
		t.Fatalf("日志查询失败：%d", status)
	}
	lines, _ := logs["lines"].([]any)
	if len(lines) == 0 {
		t.Fatal("日志为空")
	}

	_, list := getJSON(t, server.URL+"/api/jobs")
	if jobsList, _ := list["jobs"].([]any); len(jobsList) != 1 {
		t.Fatalf("任务列表应有 1 项：%v", list)
	}

	if status, _ := getJSON(t, server.URL+"/api/jobs/unknown"); status != http.StatusNotFound {
		t.Fatalf("未知任务应 404，实际 %d", status)
	}
	if status, _ := postJSON(t, server.URL+"/api/jobs/"+id+"/cancel", map[string]any{}); status != http.StatusConflict {
		t.Fatalf("已结束任务取消应 409，实际 %d", status)
	}
}

func TestCancelRunningJobEndpoint(t *testing.T) {
	manager := newManager(t, &stubRunner{release: make(chan struct{})})
	server, _ := newTestServer(t, Options{Manager: manager})
	input := sampleInput(t)
	status, created := postJSON(t, server.URL+"/api/jobs", map[string]any{"input": input, "output": filepath.Join(t.TempDir(), "out")})
	if status != http.StatusCreated {
		t.Fatalf("创建失败：%d %v", status, created)
	}
	id := created["id"].(string)
	waitManagerState(t, manager, id, jobs.StateRunning)
	status, canceled := postJSON(t, server.URL+"/api/jobs/"+id+"/cancel", map[string]any{})
	if status != http.StatusAccepted || canceled["id"] != id {
		t.Fatalf("取消失败：%d %v", status, canceled)
	}
	waitManagerState(t, manager, id, jobs.StateCanceled)
}

func TestEventsStreamDeliversStateAndLogs(t *testing.T) {
	manager := newManager(t, &stubRunner{release: make(chan struct{})})
	server, _ := newTestServer(t, Options{Manager: manager})
	input := sampleInput(t)
	_, created := postJSON(t, server.URL+"/api/jobs", map[string]any{"input": input, "output": filepath.Join(t.TempDir(), "out")})
	id := created["id"].(string)
	waitManagerState(t, manager, id, jobs.StateRunning)

	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	request, err := http.NewRequestWithContext(ctx, http.MethodGet, server.URL+"/api/jobs/"+id+"/events?after=0", nil)
	if err != nil {
		t.Fatal(err)
	}
	response, err := http.DefaultClient.Do(request)
	if err != nil {
		t.Fatal(err)
	}
	defer response.Body.Close()
	if contentType := response.Header.Get("Content-Type"); !strings.HasPrefix(contentType, "text/event-stream") {
		t.Fatalf("事件流类型错误：%s", contentType)
	}
	scanner := bufio.NewScanner(response.Body)
	events := []string{}
	var lastData string
	for scanner.Scan() {
		line := scanner.Text()
		if strings.HasPrefix(line, "event: ") {
			events = append(events, strings.TrimPrefix(line, "event: "))
		}
		if strings.HasPrefix(line, "data: ") {
			lastData = strings.TrimPrefix(line, "data: ")
		}
		if len(events) >= 3 {
			break
		}
	}
	if len(events) < 2 {
		t.Fatalf("事件过少：%v", events)
	}
	var notification map[string]any
	if err := json.Unmarshal([]byte(lastData), &notification); err != nil {
		t.Fatalf("事件数据不是 JSON：%q %v", lastData, err)
	}
	job, _ := notification["job"].(map[string]any)
	if job == nil || job["id"] != id {
		t.Fatalf("事件缺少任务快照：%v", notification)
	}
}

func TestTokenProtectsAPI(t *testing.T) {
	server, _ := newTestServer(t, Options{Token: "secret"})
	if status, _ := getJSON(t, server.URL+"/api/health"); status != http.StatusUnauthorized {
		t.Fatalf("无令牌应 401，实际 %d", status)
	}
	if status, _ := getJSON(t, server.URL+"/api/health", "Authorization", "Bearer secret"); status != http.StatusOK {
		t.Fatalf("正确令牌应 200，实际 %d", status)
	}
	if status, _ := getJSON(t, server.URL+"/api/health?token=secret"); status != http.StatusOK {
		t.Fatalf("查询参数令牌应 200，实际 %d", status)
	}
	if status, _ := getJSON(t, server.URL+"/", "Authorization", "Bearer wrong"); status != http.StatusUnauthorized {
		t.Fatalf("错误令牌访问页面应 401，实际 %d", status)
	}
}

func TestPickerEndpoint(t *testing.T) {
	server, _ := newTestServer(t, Options{})
	if status, payload := postJSON(t, server.URL+"/api/pick-directory", map[string]any{}); status != http.StatusNotImplemented || !strings.Contains(fmt.Sprint(payload["error"]), "不支持") {
		t.Fatalf("无选择器应 501：%d %v", status, payload)
	}
	server, _ = newTestServer(t, Options{Picker: func(context.Context) (string, error) { return "/picked/dir", nil }})
	status, payload := postJSON(t, server.URL+"/api/pick-directory", map[string]any{})
	if status != http.StatusOK || payload["path"] != "/picked/dir" {
		t.Fatalf("选择器应返回路径：%d %v", status, payload)
	}
}

func TestListenRequiresTokenWhenExposed(t *testing.T) {
	if _, err := Listen("0.0.0.0:0", ""); err == nil || !strings.Contains(err.Error(), "--token") {
		t.Fatalf("对外监听应要求令牌：%v", err)
	}
	if !IsLoopback("127.0.0.1:0") || !IsLoopback("localhost:8080") || IsLoopback("0.0.0.0:8080") || IsLoopback(":8080") {
		t.Fatal("回环地址判断错误")
	}
	listener, err := Listen("127.0.0.1:0", "")
	if err != nil {
		t.Fatal(err)
	}
	listener.Close()
}

func newManager(t *testing.T, runner *stubRunner) *jobs.Manager {
	t.Helper()
	manager, err := jobs.New(jobs.Config{Runner: runner})
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(manager.Close)
	return manager
}
