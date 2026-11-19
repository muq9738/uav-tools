package api

import (
	"bytes"
	"context"
	"crypto/tls"
	"encoding/json"
	"net/http"
	"os"
	"path/filepath"
	"sync"
	"testing"

	"github.com/muq9738/uav-tools/app/internal/jobs"
)

type captureRunner struct {
	mu    sync.Mutex
	specs []jobs.Spec
}

func (c *captureRunner) Convert(_ context.Context, spec jobs.Spec, _ jobs.Control, _ jobs.Sink) (jobs.Result, error) {
	c.mu.Lock()
	c.specs = append(c.specs, spec)
	c.mu.Unlock()
	return jobs.Result{ExitCode: 0, Report: json.RawMessage(`{"files":1}`)}, nil
}

func (c *captureRunner) lastSpec() jobs.Spec {
	c.mu.Lock()
	defer c.mu.Unlock()
	if len(c.specs) == 0 {
		return jobs.Spec{}
	}
	return c.specs[len(c.specs)-1]
}

func TestCompatCreateTaskMapping(t *testing.T) {
	runner := &captureRunner{}
	manager, err := jobs.New(jobs.Config{Runner: runner})
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(manager.Close)
	server, _ := newTestServer(t, Options{Manager: manager})

	input := sampleInput(t)
	output := filepath.Join(input, "out")

	payload := map[string]any{
		"options": map[string]any{
			"name":       "site_001",
			"type":       "TiltModelSlicing",
			"isPublish":  true,
			"inputPath":  filepath.ToSlash(input),
			"outputPath": filepath.ToSlash(output),
			"settings": map[string]any{
				"srs":                    "ENU:30.25,120.15",
				"srsOrigin":              "0,0,0",
				"is3DTiles1_1":           true,
				"noLight":                false,
				"textureFormat":          "default",
				"vertexCompressionLevel": 0,
				"backFaceCulling":        true,
				"isRebuildTop":           true,
				"mandatoryDoubleSide":    false,
				"computedNormal":         false,
				"vertexCompress":         true,
				"maxConcurrency":         32,
				"futureUnknownOption":    "ignored",
			},
		},
	}

	status, created := postJSON(t, server.URL+"/task/create", payload)
	if status != http.StatusCreated {
		t.Fatalf("期望 201 Created，实际 %d %v", status, created)
	}
	id, _ := created["id"].(string)
	if id == "" || created["name"] != "site_001" {
		t.Fatalf("返回体缺少 id 或 name：%v", created)
	}
	waitManagerState(t, manager, id, jobs.StateSucceeded)

	got := runner.lastSpec()
	if got.Input != input || got.Output != output {
		t.Fatalf("路径映射错误：got (%q -> %q), want (%q -> %q)", got.Input, got.Output, input, output)
	}
	if got.SRS != "ENU:30.25,120.15" || got.Origin != "0,0,0" {
		t.Fatalf("坐标映射错误：%q / %q", got.SRS, got.Origin)
	}
	if got.Geometry != "meshopt" || got.TextureFormat != "original" || !got.RebuildTop || got.Threads != 32 {
		t.Fatalf("参数映射错误：%+v", got)
	}
}

func TestCompatCreateTaskFallbackAndValidation(t *testing.T) {
	runner := &captureRunner{}
	manager, err := jobs.New(jobs.Config{Runner: runner})
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(manager.Close)
	server, _ := newTestServer(t, Options{Manager: manager})

	// 不支持的任务类型返回 400。
	status, _ := postJSON(t, server.URL+"/task/create", map[string]any{
		"options": map[string]any{
			"type":       "PointCloudSlicing",
			"inputPath":  sampleInput(t),
			"outputPath": filepath.Join(t.TempDir(), "out"),
		},
	})
	if status != http.StatusBadRequest {
		t.Fatalf("不支持的任务类型应返回 400，实际 %d", status)
	}

	// 无 SRS、关闭顶点压缩、超上限并发裁剪、UASTC 纹理映射。
	input := sampleInput(t)
	output := filepath.Join(t.TempDir(), "out")
	status, created := postJSON(t, server.URL+"/task/create", map[string]any{
		"options": map[string]any{
			"name":       "site_002",
			"type":       "TiltModelSlicing",
			"inputPath":  filepath.ToSlash(input),
			"outputPath": filepath.ToSlash(output),
			"settings": map[string]any{
				"textureFormat":  "uastc",
				"vertexCompress": false,
				"isRebuildTop":   false,
				"maxConcurrency": 128,
			},
		},
	})
	if status != http.StatusCreated {
		t.Fatalf("期望 201，实际 %d %v", status, created)
	}
	id := created["id"].(string)
	waitManagerState(t, manager, id, jobs.StateSucceeded)

	got := runner.lastSpec()
	if got.Geometry != "raw" || got.TextureFormat != "uastc" || got.RebuildTop || got.Threads != 64 || got.SRS != "" {
		t.Fatalf("映射异常：%+v", got)
	}
}

func TestBuiltInSelfSignedTLSServesCompatEndpoint(t *testing.T) {
	dataDir := t.TempDir()
	tlsCfg1, certPath1, err := LoadOrGenerateTLSConfig("", "", dataDir, "127.0.0.1:0")
	if err != nil {
		t.Fatalf("自动生成 TLS 证书失败：%v", err)
	}
	certBytes1, err := os.ReadFile(certPath1)
	if err != nil {
		t.Fatalf("自签证书未持久化：%v", err)
	}
	// 第二次调用应复用已持久化证书。
	_, certPath2, err := LoadOrGenerateTLSConfig("", "", dataDir, "127.0.0.1:0")
	if err != nil || certPath2 != certPath1 {
		t.Fatalf("复用持久化证书失败：%v %q", err, certPath2)
	}
	certBytes2, _ := os.ReadFile(certPath2)
	if !bytes.Equal(certBytes1, certBytes2) {
		t.Fatal("第二次加载重新覆盖了有效证书")
	}

	// 启动真实 HTTPS 监听，校验自签证书下 POST /task/create 可正常返回 201。
	runner := &captureRunner{}
	manager, err := jobs.New(jobs.Config{Runner: runner})
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(manager.Close)
	srv, err := New(Options{Manager: manager, Runner: stubFeatures{}})
	if err != nil {
		t.Fatal(err)
	}
	rawListener, err := Listen("127.0.0.1:0", "")
	if err != nil {
		t.Fatal(err)
	}
	tlsListener := tls.NewListener(rawListener, tlsCfg1)
	httpServer := &http.Server{Handler: srv.Handler()}
	go httpServer.Serve(tlsListener)
	t.Cleanup(func() { httpServer.Close() })

	client := &http.Client{
		Transport: &http.Transport{
			TLSClientConfig: &tls.Config{InsecureSkipVerify: true},
		},
	}
	input := sampleInput(t)
	body, _ := json.Marshal(map[string]any{
		"options": map[string]any{
			"name":       "https_task",
			"type":       "TiltModelSlicing",
			"inputPath":  filepath.ToSlash(input),
			"outputPath": filepath.ToSlash(filepath.Join(input, "out")),
			"settings": map[string]any{
				"is3DTiles1_1":   true,
				"isRebuildTop":   true,
				"vertexCompress": true,
				"maxConcurrency": 32,
			},
		},
	})
	resp, err := client.Post("https://"+tlsListener.Addr().String()+"/task/create", "application/json", bytes.NewReader(body))
	if err != nil {
		t.Fatalf("HTTPS 请求失败：%v", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusCreated {
		t.Fatalf("HTTPS /task/create 期望 201，实际 %d", resp.StatusCode)
	}
}
