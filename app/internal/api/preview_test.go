package api

import (
	"io"
	"net/http"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"testing"

	"github.com/muq9738/uav-tools/app/internal/jobs"
)

func TestTileAssetsServeOnlyCompletedOutput(t *testing.T) {
	manager := newManager(t, &stubRunner{})
	server, _ := newTestServer(t, Options{Manager: manager, Token: "secret"})
	output := filepath.Join(t.TempDir(), "tiles")
	status, created := postJSON(t, server.URL+"/api/jobs", map[string]any{
		"input": sampleInput(t), "output": output,
	}, "Authorization", "Bearer secret")
	if status != http.StatusCreated {
		t.Fatalf("创建任务失败：%d %v", status, created)
	}
	id := created["id"].(string)
	waitManagerState(t, manager, id, jobs.StateSucceeded)
	if err := os.MkdirAll(filepath.Join(output, "content"), 0o755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(output, "tileset.json"), []byte(`{"asset":{"version":"1.1"}}`), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(output, "content", "tile.glb"), []byte("glTF-test"), 0o644); err != nil {
		t.Fatal(err)
	}

	base := server.URL + "/api/jobs/" + id + "/tiles/"
	response, err := http.Get(base + "tileset.json")
	if err != nil {
		t.Fatal(err)
	}
	response.Body.Close()
	if response.StatusCode != http.StatusUnauthorized {
		t.Fatalf("无令牌的瓦片请求应 401，实际 %d", response.StatusCode)
	}
	request, _ := http.NewRequest(http.MethodGet, base+"content/tile.glb", nil)
	request.Header.Set("Authorization", "Bearer secret")
	request.Header.Set("Range", "bytes=0-3")
	response, err = http.DefaultClient.Do(request)
	if err != nil {
		t.Fatal(err)
	}
	body, _ := io.ReadAll(response.Body)
	response.Body.Close()
	if response.StatusCode != http.StatusPartialContent || string(body) != "glTF" {
		t.Fatalf("GLB Range 读取异常：%d %q", response.StatusCode, body)
	}
	if got := response.Header.Get("Content-Type"); got != "model/gltf-binary" {
		t.Fatalf("GLB Content-Type 错误：%q", got)
	}

	request, _ = http.NewRequest(http.MethodGet, base+"tileset.json", nil)
	request.Header.Set("Authorization", "Bearer secret")
	response, err = http.DefaultClient.Do(request)
	if err != nil {
		t.Fatal(err)
	}
	body, _ = io.ReadAll(response.Body)
	response.Body.Close()
	if response.StatusCode != http.StatusOK || !strings.Contains(string(body), `"version":"1.1"`) {
		t.Fatalf("tileset 读取异常：%d %q", response.StatusCode, body)
	}

	request, _ = http.NewRequest(http.MethodGet, base+"missing.glb", nil)
	request.Header.Set("Authorization", "Bearer secret")
	response, err = http.DefaultClient.Do(request)
	if err != nil {
		t.Fatal(err)
	}
	response.Body.Close()
	if response.StatusCode != http.StatusNotFound {
		t.Fatalf("缺失资源应 404，实际 %d", response.StatusCode)
	}

	if runtime.GOOS != "windows" {
		outside := filepath.Join(t.TempDir(), "secret.txt")
		if err := os.WriteFile(outside, []byte("secret"), 0o600); err != nil {
			t.Fatal(err)
		}
		if err := os.Symlink(outside, filepath.Join(output, "escape.txt")); err != nil {
			t.Fatal(err)
		}
		request, _ = http.NewRequest(http.MethodGet, base+"escape.txt", nil)
		request.Header.Set("Authorization", "Bearer secret")
		response, err = http.DefaultClient.Do(request)
		if err != nil {
			t.Fatal(err)
		}
		response.Body.Close()
		if response.StatusCode != http.StatusNotFound {
			t.Fatalf("符号链接不得逃逸输出目录，实际 %d", response.StatusCode)
		}
	}
}

func TestTileAssetsRejectIncompleteJob(t *testing.T) {
	manager := newManager(t, &stubRunner{release: make(chan struct{})})
	server, _ := newTestServer(t, Options{Manager: manager})
	status, created := postJSON(t, server.URL+"/api/jobs", map[string]any{
		"input": sampleInput(t), "output": filepath.Join(t.TempDir(), "tiles"),
	})
	if status != http.StatusCreated {
		t.Fatalf("创建任务失败：%d %v", status, created)
	}
	id := created["id"].(string)
	waitManagerState(t, manager, id, jobs.StateRunning)
	response, err := http.Get(server.URL + "/api/jobs/" + id + "/tiles/tileset.json")
	if err != nil {
		t.Fatal(err)
	}
	response.Body.Close()
	if response.StatusCode != http.StatusConflict {
		t.Fatalf("未完成任务不得预览，实际 %d", response.StatusCode)
	}
}
