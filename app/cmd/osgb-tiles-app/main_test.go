package main

import (
	"bytes"
	"context"
	"crypto/tls"
	"encoding/json"
	"io"
	"net/http"
	"os"
	"path/filepath"
	"testing"
	"time"

	"github.com/muq9738/uav-tools/app/internal/api"
	"github.com/muq9738/uav-tools/app/internal/jobs"
	"github.com/muq9738/uav-tools/app/internal/runner"
	"github.com/muq9738/uav-tools/app/web"
)

type noopRunner struct{}

func (noopRunner) Convert(context.Context, jobs.Spec, jobs.Control, jobs.Sink) (jobs.Result, error) {
	return jobs.Result{ExitCode: 0, Report: json.RawMessage(`{"files":1}`)}, nil
}

func TestResolveCompatHTTPSAddr(t *testing.T) {
	cases := []struct {
		name        string
		httpsAddr   string
		primaryAddr string
		headless    bool
		primaryTLS  bool
		want        string
	}{
		{name: "desktop default enables 42225", httpsAddr: "", primaryAddr: "127.0.0.1:0", headless: false, primaryTLS: false, want: "0.0.0.0:42225"},
		{name: "headless default stays off", httpsAddr: "", primaryAddr: "127.0.0.1:0", headless: true, primaryTLS: false, want: ""},
		{name: "headless explicit https-addr", httpsAddr: "0.0.0.0:42225", primaryAddr: "127.0.0.1:0", headless: true, primaryTLS: false, want: "0.0.0.0:42225"},
		{name: "desktop disabled with off", httpsAddr: "off", primaryAddr: "127.0.0.1:0", headless: false, primaryTLS: false, want: ""},
		{name: "desktop disabled with none", httpsAddr: "NONE", primaryAddr: "127.0.0.1:0", headless: false, primaryTLS: false, want: ""},
		{name: "primary tls skips compat", httpsAddr: "", primaryAddr: "127.0.0.1:0", headless: false, primaryTLS: true, want: ""},
		{name: "custom primary addr skips auto compat", httpsAddr: "", primaryAddr: "0.0.0.0:8990", headless: false, primaryTLS: false, want: ""},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			if got := resolveCompatHTTPSAddr(tc.httpsAddr, tc.primaryAddr, tc.headless, tc.primaryTLS); got != tc.want {
				t.Fatalf("resolveCompatHTTPSAddr(%q, %q, %v, %v) = %q, want %q",
					tc.httpsAddr, tc.primaryAddr, tc.headless, tc.primaryTLS, got, tc.want)
			}
		})
	}
}

func TestDualHTTPUIAndHTTPSCompatListener(t *testing.T) {
	dataDir := t.TempDir()
	manager, err := jobs.New(jobs.Config{DataDir: dataDir, Runner: noopRunner{}})
	if err != nil {
		t.Fatal(err)
	}
	defer manager.Close()

	srv, err := api.New(api.Options{
		Manager:   manager,
		Runner:    &runner.Exec{},
		Web:       web.Files,
		Version:   "0.6.0",
		DataDir:   dataDir,
		HTTPSAddr: "127.0.0.1:0",
	})
	if err != nil {
		t.Fatal(err)
	}

	httpListener, err := api.Listen("127.0.0.1:0", "")
	if err != nil {
		t.Fatal(err)
	}
	httpServer := &http.Server{Handler: srv.Handler(), ReadHeaderTimeout: time.Second}
	go httpServer.Serve(httpListener)
	defer httpServer.Shutdown(context.Background())

	compatServer, compatAddr, certSource, err := startHTTPSCompatServer(srv.Handler(), "127.0.0.1:0", "", "", "", dataDir, nil)
	if err != nil {
		t.Fatal(err)
	}
	defer compatServer.Shutdown(context.Background())
	if certSource == "" {
		t.Fatal("应返回自签证书路径")
	}

	inDir := filepath.Join(t.TempDir(), "osgb-in")
	if err := os.MkdirAll(inDir, 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(inDir, "metadata.xml"), []byte("<ModelMetadata/>"), 0o644); err != nil {
		t.Fatal(err)
	}
	outDir := filepath.Join(t.TempDir(), "out")
	payload, _ := json.Marshal(map[string]any{
		"options": map[string]any{
			"name":       "dual-listener-check",
			"type":       "TiltModelSlicing",
			"inputPath":  inDir,
			"outputPath": outDir,
		},
	})

	httpsClient := &http.Client{
		Transport: &http.Transport{TLSClientConfig: &tls.Config{InsecureSkipVerify: true}},
		Timeout:   5 * time.Second,
	}
	resp, err := httpsClient.Post("https://"+compatAddr.String()+"/task/create", "application/json", bytes.NewReader(payload))
	if err != nil {
		t.Fatalf("HTTPS /task/create 请求失败：%v", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusCreated {
		errBody, _ := io.ReadAll(resp.Body)
		t.Fatalf("HTTPS /task/create 状态码 = %d, want 201 (%s)", resp.StatusCode, string(errBody))
	}
	var created struct {
		ID string `json:"id"`
	}
	if err := json.NewDecoder(resp.Body).Decode(&created); err != nil || created.ID == "" {
		t.Fatalf("解析返回任务 ID 失败：%v", err)
	}

	// 同一个任务立即可从桌面 HTTP UI 的 /api/jobs 列表中看到。
	uiResp, err := http.Get("http://" + httpListener.Addr().String() + "/api/jobs")
	if err != nil {
		t.Fatal(err)
	}
	defer uiResp.Body.Close()
	var list struct {
		Jobs []struct {
			ID string `json:"id"`
		} `json:"jobs"`
	}
	if err := json.NewDecoder(uiResp.Body).Decode(&list); err != nil {
		t.Fatal(err)
	}
	found := false
	for _, job := range list.Jobs {
		if job.ID == created.ID {
			found = true
			break
		}
	}
	if !found {
		t.Fatalf("UI /api/jobs 未同步展示通过 HTTPS /task/create 提交的任务 %s", created.ID)
	}
}

func TestUpdaterStatusInConfigAndDoctor(t *testing.T) {
	dataDir := t.TempDir()
	manager, err := jobs.New(jobs.Config{DataDir: dataDir, Runner: noopRunner{}})
	if err != nil {
		t.Fatal(err)
	}
	defer manager.Close()

	srv, err := api.New(api.Options{
		Manager:      manager,
		Runner:       &runner.Exec{},
		Web:          web.Files,
		Version:      "0.6.1",
		DataDir:      dataDir,
		CheckUpdates: func() {},
		UpdaterStatus: func() api.UpdaterInfo {
			return api.UpdaterInfo{
				Enabled:          true,
				AvailableVersion: "0.6.2",
				TokenConfigured:  true,
			}
		},
	})
	if err != nil {
		t.Fatal(err)
	}

	listener, err := api.Listen("127.0.0.1:0", "")
	if err != nil {
		t.Fatal(err)
	}
	httpServer := &http.Server{Handler: srv.Handler(), ReadHeaderTimeout: time.Second}
	go httpServer.Serve(listener)
	defer httpServer.Shutdown(context.Background())

	for _, endpoint := range []string{"/api/config", "/api/doctor"} {
		resp, err := http.Get("http://" + listener.Addr().String() + endpoint)
		if err != nil {
			t.Fatalf("%s 请求失败：%v", endpoint, err)
		}
		var payload struct {
			Updater *api.UpdaterInfo `json:"updater"`
		}
		if err := json.NewDecoder(resp.Body).Decode(&payload); err != nil {
			resp.Body.Close()
			t.Fatalf("解析 %s 响应失败：%v", endpoint, err)
		}
		resp.Body.Close()
		if payload.Updater == nil || !payload.Updater.Enabled || !payload.Updater.TokenConfigured || payload.Updater.AvailableVersion != "0.6.2" {
			t.Fatalf("%s updater 状态不符合预期：%+v", endpoint, payload.Updater)
		}
	}
}
