package main

import (
	"context"
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"os"
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
	src.proxyDisabled = true
	src.detectChina = func(context.Context) bool { return false }

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

func TestChinaAutoProxyRaceAndFallback(t *testing.T) {
	const secretToken = "must-never-leak-to-proxy"
	const archiveURL = "https://github.invalid/muq9738/uav-tools/releases/download/v0.6.5/osgb-tiles-0.6.5-windows-amd64.tar.gz"
	const manifestPayload = `{"version":"0.6.5","url":"` + archiveURL + `","size":16,"signature":"sig"}`
	const archivePayload = "verified-archive"

	var proxySawAuth bool

	// 代理 1：返回 200 但内容是 HTML 限流页，必须被过滤。
	htmlProxy := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Header.Get("Authorization") != "" {
			proxySawAuth = true
		}
		w.Header().Set("Content-Type", "text/html; charset=utf-8")
		io.WriteString(w, "<html><body>Too Many Requests</body></html>")
	}))
	defer htmlProxy.Close()

	// 代理 2：返回 502。
	errProxy := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Header.Get("Authorization") != "" {
			proxySawAuth = true
		}
		http.Error(w, "bad gateway", http.StatusBadGateway)
	}))
	defer errProxy.Close()

	// 代理 3：Manifest 返回合法 JSON，但下载二进制时模拟返回 HTML 错误页，验证 Open 自动故障切换。
	manifestOnlyProxy := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Header.Get("Authorization") != "" {
			proxySawAuth = true
		}
		if strings.HasSuffix(r.URL.Path, "/update-windows-amd64.json") {
			w.Header().Set("Content-Type", "application/json")
			io.WriteString(w, manifestPayload)
			return
		}
		w.Header().Set("Content-Type", "text/html")
		io.WriteString(w, "<html>blocked</html>")
	}))
	defer manifestOnlyProxy.Close()

	// 代理 4：Manifest 返回 503，但资产下载正常返回二进制。
	assetProxy := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Header.Get("Authorization") != "" {
			proxySawAuth = true
		}
		if strings.HasSuffix(r.URL.Path, ".tar.gz") {
			w.Header().Set("Content-Type", "application/octet-stream")
			io.WriteString(w, archivePayload)
			return
		}
		http.Error(w, "unavailable", http.StatusServiceUnavailable)
	}))
	defer assetProxy.Close()

	src := newGitHubTokenSource("muq9738/uav-tools", secretToken)
	src.downloadBase = "https://github.invalid"
	src.platform = "windows-amd64"
	src.proxyDisabled = false
	src.proxyForced = false
	src.proxies = []string{
		htmlProxy.URL + "/",
		errProxy.URL + "/",
		manifestOnlyProxy.URL + "/",
		assetProxy.URL + "/",
	}
	src.detectChina = func(context.Context) bool { return true }

	body, err := src.Manifest(context.Background(), false)
	if err != nil {
		t.Fatalf("国内代理竞速获取 Manifest 失败：%v", err)
	}
	raw, _ := io.ReadAll(body)
	body.Close()
	if string(raw) != manifestPayload {
		t.Fatalf("Manifest 内容不符：%q", string(raw))
	}
	if got := src.ActiveProxy(); got != manifestOnlyProxy.URL+"/" {
		t.Fatalf("期望选中代理 %s，实际 %q", manifestOnlyProxy.URL+"/", got)
	}

	// 下载资产时 manifestOnlyProxy 返回 text/html，应自动切换到 assetProxy。
	stream, err := src.Open(context.Background(), archiveURL)
	if err != nil {
		t.Fatalf("国内代理故障切换下载资产失败：%v", err)
	}
	gotArchive, _ := io.ReadAll(stream)
	stream.Close()
	if string(gotArchive) != archivePayload {
		t.Fatalf("资产内容不符：%q", string(gotArchive))
	}
	if got := src.ActiveProxy(); got != assetProxy.URL+"/" {
		t.Fatalf("故障切换后应更新 ActiveProxy 为 %s，实际 %q", assetProxy.URL+"/", got)
	}
	if proxySawAuth {
		t.Fatal("安全违规：绝不能向第三方加速代理发送 GitHub Authorization 令牌")
	}
}

func TestChinaProxyAllFailFallsBackToDirectGitHub(t *testing.T) {
	const secretToken = "direct-fallback-token"
	const manifestPayload = `{"version":"0.6.5","url":"https://github.invalid/muq9738/uav-tools/releases/download/v0.6.5/a.tar.gz","size":4,"signature":"sig"}`

	deadProxy := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		http.Error(w, "down", http.StatusBadGateway)
	}))
	defer deadProxy.Close()

	var apiServer *httptest.Server
	apiServer = httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Header.Get("Authorization") != "Bearer "+secretToken {
			http.Error(w, "unauthorized", http.StatusUnauthorized)
			return
		}
		switch r.URL.Path {
		case "/repos/muq9738/uav-tools/releases/latest":
			json.NewEncoder(w).Encode(githubRelease{
				TagName: "v0.6.5",
				Assets: []githubReleaseAsset{{
					Name:               "update-windows-amd64.json",
					URL:                apiServer.URL + "/repos/muq9738/uav-tools/releases/assets/1",
					BrowserDownloadURL: "https://github.invalid/muq9738/uav-tools/releases/download/v0.6.5/update-windows-amd64.json",
				}},
			})
		case "/repos/muq9738/uav-tools/releases/assets/1":
			io.WriteString(w, manifestPayload)
		default:
			http.NotFound(w, r)
		}
	}))
	defer apiServer.Close()

	src := newGitHubTokenSource("muq9738/uav-tools", secretToken)
	src.apiBase = apiServer.URL
	src.downloadBase = "https://github.invalid"
	src.platform = "windows-amd64"
	src.proxies = []string{deadProxy.URL + "/"}
	src.detectChina = func(context.Context) bool { return true }

	body, err := src.Manifest(context.Background(), false)
	if err != nil {
		t.Fatalf("代理全挂后应回退直连 GitHub，但返回错误：%v", err)
	}
	raw, _ := io.ReadAll(body)
	body.Close()
	if string(raw) != manifestPayload {
		t.Fatalf("回退直连清单内容不符：%q", string(raw))
	}
}

func TestDefaultDetectChinaProbes(t *testing.T) {
	cases := []struct {
		name     string
		ipipBody string
		ipipCode int
		cfBody   string
		cfCode   int
		cnCode   int
		intlCode int
		want     bool
	}{
		{
			name:     "中国大陆IP且规则VPN下CF返回US",
			ipipBody: `{"ret":"ok","data":{"ip":"218.75.106.162","location":["中国","浙江","杭州","","电信"]}}`,
			ipipCode: 200,
			cfBody:   "fl=1\nloc=US\n",
			cfCode:   200,
			cnCode:   204,
			intlCode: 204,
			want:     true,
		},
		{
			name:     "香港地区不启用大陆代理",
			ipipBody: `{"ret":"ok","data":{"ip":"1.2.3.4","location":["中国","香港","","",""]}}`,
			ipipCode: 200,
			cfBody:   "loc=HK\n",
			cfCode:   200,
			cnCode:   204,
			intlCode: 204,
			want:     false,
		},
		{
			name:     "ipip不可用但CF返回CN",
			ipipCode: 500,
			cfBody:   "h=www.cloudflare.com\nloc=CN\n",
			cfCode:   200,
			cnCode:   204,
			intlCode: 500,
			want:     true,
		},
		{
			name:     "GeoIP均失败但国内204通且国际204不通",
			ipipCode: 500,
			cfCode:   500,
			cnCode:   204,
			intlCode: 503,
			want:     true,
		},
		{
			name:     "海外用户不启用大陆代理",
			ipipBody: `{"ret":"ok","data":{"ip":"8.8.8.8","location":["美国","加利福尼亚","","",""]}}`,
			ipipCode: 200,
			cfBody:   "loc=US\n",
			cfCode:   200,
			cnCode:   204,
			intlCode: 204,
			want:     false,
		},
	}

	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			srv := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
				switch r.URL.Path {
				case "/ipip":
					w.WriteHeader(tc.ipipCode)
					io.WriteString(w, tc.ipipBody)
				case "/cf":
					w.WriteHeader(tc.cfCode)
					io.WriteString(w, tc.cfBody)
				case "/cn204":
					w.WriteHeader(tc.cnCode)
				case "/intl204":
					w.WriteHeader(tc.intlCode)
				}
			}))
			defer srv.Close()

			src := newGitHubTokenSource("muq9738/uav-tools", "")
			src.ipipProbeURL = srv.URL + "/ipip"
			src.cfTraceProbeURL = srv.URL + "/cf"
			src.cn204ProbeURL = srv.URL + "/cn204"
			src.intl204ProbeURL = srv.URL + "/intl204"

			got := src.defaultDetectChina(context.Background())
			if got != tc.want {
				t.Fatalf("defaultDetectChina() = %v, want %v", got, tc.want)
			}
		})
	}
}

func TestParseUpdateProxyEnv(t *testing.T) {
	if mode, list := parseUpdateProxyEnv(""); mode != "auto" || len(list) != 0 {
		t.Fatalf("空值应为 auto：%q %v", mode, list)
	}
	for _, off := range []string{"off", "NONE", "direct", "false", "0"} {
		if mode, _ := parseUpdateProxyEnv(off); mode != "off" {
			t.Fatalf("%q 应解析为 off，实际 %q", off, mode)
		}
	}
	mode, list := parseUpdateProxyEnv("gh-proxy.com, https://ghfast.top/, https://gh-proxy.com")
	if mode != "custom" || len(list) != 2 || list[0] != "https://gh-proxy.com/" || list[1] != "https://ghfast.top/" {
		t.Fatalf("自定义代理列表解析异常：%q %v", mode, list)
	}
}

func TestLiveChinaProxyUpdater(t *testing.T) {
	if os.Getenv("OSGB_LIVE_UPDATE_TEST") != "1" {
		t.Skip("set OSGB_LIVE_UPDATE_TEST=1 to run live network verification")
	}
	src := newGitHubTokenSource(updateGitHubRepo, "")
	src.platform = "windows-amd64"

	ctx := context.Background()
	isCN := src.shouldUseChinaProxy(ctx)
	t.Logf("detected Mainland China = %v", isCN)
	if !isCN {
		t.Fatalf("expected Mainland China detection to return true on this host")
	}

	body, err := src.Manifest(ctx, false)
	if err != nil {
		t.Fatalf("live Manifest failed: %v", err)
	}
	raw, err := io.ReadAll(body)
	body.Close()
	if err != nil {
		t.Fatalf("read Manifest failed: %v", err)
	}
	var m manifestProbe
	if err := json.Unmarshal(raw, &m); err != nil || m.Version == "" || m.URL == "" || m.Signature == "" {
		t.Fatalf("invalid live Manifest: err=%v raw=%q", err, string(raw))
	}
	t.Logf("live Manifest OK: version=%s activeProxy=%s url=%s", m.Version, src.ActiveProxy(), m.URL)
	if src.ActiveProxy() == "" {
		t.Fatal("expected ActiveProxy to be selected in China")
	}

	stream, err := src.Open(ctx, m.URL)
	if err != nil {
		t.Fatalf("live Open asset failed: %v", err)
	}
	defer stream.Close()
	buf := make([]byte, 16384)
	n, err := io.ReadFull(stream, buf)
	if err != nil {
		t.Fatalf("read live asset prefix failed: %v", err)
	}
	t.Logf("live Open asset OK: read %d bytes via %s (gzip magic=%x %x)", n, src.ActiveProxy(), buf[0], buf[1])
}
