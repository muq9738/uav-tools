package main

import (
	"context"
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
)

func TestGitHubTokenUpdateSourceManifestAndOpen(t *testing.T) {
	const secretToken = "test-readonly-pat-value"
	const archivePayload = "signed-archive-bytes"
	const manifestPayload = `{"version":"0.6.0","url":"https://github.invalid/muq9738/uav-tools/releases/download/v0.6.0/osgb-tiles-0.6.0-windows-amd64.tar.gz","size":20,"signature":"sig"}`

	var cdnSawAuth bool
	var apiServer *httptest.Server
	cdnServer := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Header.Get("Authorization") != "" {
			cdnSawAuth = true
		}
		io.WriteString(w, archivePayload)
	}))
	defer cdnServer.Close()

	var tagQueries int
	apiServer = httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if got := r.Header.Get("Authorization"); got != "Bearer "+secretToken {
			t.Errorf("API 请求缺少 Bearer 令牌：%q (%s)", got, r.URL.Path)
			http.Error(w, "unauthorized", http.StatusUnauthorized)
			return
		}
		switch r.URL.Path {
		case "/repos/muq9738/uav-tools/releases/latest":
			json.NewEncoder(w).Encode(githubRelease{
				TagName: "v0.6.0",
				Assets: []githubReleaseAsset{
					{
						Name:               "update-windows-amd64.json",
						URL:                apiServer.URL + "/repos/muq9738/uav-tools/releases/assets/101",
						BrowserDownloadURL: "https://github.invalid/muq9738/uav-tools/releases/download/v0.6.0/update-windows-amd64.json",
					},
					{
						Name:               "osgb-tiles-0.6.0-windows-amd64.tar.gz",
						URL:                apiServer.URL + "/repos/muq9738/uav-tools/releases/assets/102",
						BrowserDownloadURL: "https://github.invalid/muq9738/uav-tools/releases/download/v0.6.0/osgb-tiles-0.6.0-windows-amd64.tar.gz",
					},
				},
			})
		case "/repos/muq9738/uav-tools/releases/tags/v0.5.0":
			tagQueries++
			json.NewEncoder(w).Encode(githubRelease{
				TagName: "v0.5.0",
				Assets: []githubReleaseAsset{
					{
						Name:               "delta-v0.5.0.delta",
						URL:                apiServer.URL + "/repos/muq9738/uav-tools/releases/assets/99",
						BrowserDownloadURL: "https://github.invalid/muq9738/uav-tools/releases/download/v0.5.0/delta-v0.5.0.delta",
					},
				},
			})
		case "/repos/muq9738/uav-tools/releases/assets/101":
			if got := r.Header.Get("Accept"); got != "application/octet-stream" {
				t.Errorf("Manifest 资产请求 Accept 应为 application/octet-stream，实际为 %q", got)
			}
			io.WriteString(w, manifestPayload)
		case "/repos/muq9738/uav-tools/releases/assets/102", "/repos/muq9738/uav-tools/releases/assets/99":
			if got := r.Header.Get("Accept"); got != "application/octet-stream" {
				t.Errorf("二进制资产请求 Accept 应为 application/octet-stream，实际为 %q", got)
			}
			http.Redirect(w, r, cdnServer.URL+"/blob/"+strings.TrimPrefix(r.URL.Path, "/"), http.StatusFound)
		default:
			http.NotFound(w, r)
		}
	}))
	defer apiServer.Close()

	src := newGitHubTokenSource("muq9738/uav-tools", secretToken)
	src.apiBase = apiServer.URL
	src.downloadBase = "https://github.invalid"
	src.platform = "windows-amd64"

	body, err := src.Manifest(context.Background(), false)
	if err != nil {
		t.Fatalf("Manifest 失败：%v", err)
	}
	rawManifest, err := io.ReadAll(body)
	body.Close()
	if err != nil || string(rawManifest) != manifestPayload {
		t.Fatalf("Manifest 内容不符：err=%v body=%q", err, string(rawManifest))
	}

	// 已缓存下载 URL 的压缩包读取 + 重定向鉴权剥离验证
	stream, err := src.Open(context.Background(), "https://github.invalid/muq9738/uav-tools/releases/download/v0.6.0/osgb-tiles-0.6.0-windows-amd64.tar.gz")
	if err != nil {
		t.Fatalf("Open 压缩包失败：%v", err)
	}
	gotArchive, _ := io.ReadAll(stream)
	stream.Close()
	if string(gotArchive) != archivePayload {
		t.Fatalf("压缩包内容不符：%q", string(gotArchive))
	}
	if cdnSawAuth {
		t.Fatal("重定向到 CDN 主机时必须剥离 Authorization 头")
	}

	// 未在 latest 缓存中的历史版本增量包，应按 tag 查询并解析资产 API URL
	deltaStream, err := src.Open(context.Background(), "https://github.invalid/muq9738/uav-tools/releases/download/v0.5.0/delta-v0.5.0.delta")
	if err != nil {
		t.Fatalf("Open 历史 tag 资产失败：%v", err)
	}
	gotDelta, _ := io.ReadAll(deltaStream)
	deltaStream.Close()
	if string(gotDelta) != archivePayload || tagQueries != 1 {
		t.Fatalf("历史 tag 解析失败：tagQueries=%d data=%q", tagQueries, string(gotDelta))
	}
}
