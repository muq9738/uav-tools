package main

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log"
	"net/http"
	"net/url"
	"os"
	"runtime"
	"strings"
	"sync"
	"time"

	"github.com/egoist/mygo/netproxy"
)

const (
	updateGitHubRepo         = "muq9738/uav-tools"
	updateProxyEnvVar        = "OSGB_TILES_UPDATE_PROXY"
	maxManifestBytes         = 1 << 20
	chinaDetectTimeout       = 2500 * time.Millisecond
	chinaDetectCacheTTL      = 30 * time.Minute
	proxyManifestRaceTimeout = 10 * time.Second
)

// defaultChinaGitHubProxies 为实测支持公开 Release 清单、压缩包与 .delta 增量下载的国内加速代理列表。
// 运行时会在检查更新时并发竞速并优先复用最快可用节点，全部失败时自动回退 GitHub 直连。
var defaultChinaGitHubProxies = []string{
	"https://gh-proxy.com/",
	"https://ghfast.top/",
	"https://gh-proxy.org/",
	"https://ghfile.geekertao.top/",
	"https://edgeone.gh-proxy.com/",
	"https://git.yylx.win/",
}

// updateGitHubToken 在发布构建时通过 MYGO_LDFLAGS="-X main.updateGitHubToken=..." 注入，
// 为仅对该仓库具备 Contents 只读权限的细粒度 PAT；源码中保持为空。
var updateGitHubToken string

type githubReleaseAsset struct {
	Name               string `json:"name"`
	URL                string `json:"url"`
	BrowserDownloadURL string `json:"browser_download_url"`
}

type githubRelease struct {
	TagName    string               `json:"tag_name"`
	Draft      bool                 `json:"draft"`
	Prerelease bool                 `json:"prerelease"`
	Assets     []githubReleaseAsset `json:"assets"`
}

type manifestProbe struct {
	Version   string `json:"version"`
	URL       string `json:"url"`
	Signature string `json:"signature"`
}

// githubTokenUpdateSource 管理桌面端自动更新源：
// - 在中国大陆网络环境下自动通过多节点竞速选用可用的 GitHub 代理加速（绝不向第三方代理发送令牌）；
// - 支持通过 OSGB_TILES_UPDATE_PROXY 显式指定自定义代理前缀或设为 off 禁用代理；
// - 代理不可用或处于海外环境时自动直连 GitHub（带嵌入式只读令牌兼容受限访问）。
type githubTokenUpdateSource struct {
	repo          string
	token         string
	apiBase       string
	downloadBase  string
	platform      string
	client        *http.Client
	probeClient   *http.Client
	proxies       []string
	proxyForced   bool
	proxyDisabled bool
	detectChina   func(context.Context) bool

	// 可在测试中替换的地区探测端点。
	ipipProbeURL    string
	cfTraceProbeURL string
	cn204ProbeURL   string
	intl204ProbeURL string

	mu             sync.Mutex
	byDownloadURL  map[string]string
	byTagAsset     map[string]string
	activeProxy    string
	chinaChecked   bool
	chinaDetected  bool
	chinaCheckedAt time.Time
}

func newGitHubTokenSource(repo, token string) *githubTokenUpdateSource {
	tr := netproxy.Transport().Clone()
	tr.ResponseHeaderTimeout = 15 * time.Second

	probeTr := netproxy.Transport().Clone()
	probeTr.ResponseHeaderTimeout = chinaDetectTimeout

	mode, customProxies := parseUpdateProxyEnv(os.Getenv(updateProxyEnvVar))
	proxies := append([]string(nil), defaultChinaGitHubProxies...)
	proxyForced := false
	proxyDisabled := false
	switch mode {
	case "off":
		proxyDisabled = true
		proxies = nil
	case "custom":
		proxyForced = true
		proxies = customProxies
	}

	s := &githubTokenUpdateSource{
		repo:            repo,
		token:           token,
		apiBase:         "https://api.github.com",
		downloadBase:    "https://github.com",
		platform:        runtime.GOOS + "-" + runtime.GOARCH,
		proxies:         proxies,
		proxyForced:     proxyForced,
		proxyDisabled:   proxyDisabled,
		ipipProbeURL:    "https://myip.ipip.net/json",
		cfTraceProbeURL: "https://www.cloudflare.com/cdn-cgi/trace",
		cn204ProbeURL:   "https://connect.rom.miui.com/generate_204",
		intl204ProbeURL: "https://connectivitycheck.gstatic.com/generate_204",
		byDownloadURL:   map[string]string{},
		byTagAsset:      map[string]string{},
	}
	s.detectChina = s.defaultDetectChina
	s.client = &http.Client{
		Transport:     tr,
		Timeout:       10 * time.Minute,
		CheckRedirect: s.checkRedirect,
	}
	s.probeClient = &http.Client{
		Transport: probeTr,
		Timeout:   chinaDetectTimeout,
	}
	return s
}

func parseUpdateProxyEnv(raw string) (string, []string) {
	trimmed := strings.TrimSpace(raw)
	if trimmed == "" {
		return "auto", nil
	}
	switch strings.ToLower(trimmed) {
	case "off", "none", "direct", "false", "0":
		return "off", nil
	}
	var out []string
	seen := map[string]struct{}{}
	for _, part := range strings.Split(trimmed, ",") {
		normalized := normalizeProxyPrefix(part)
		if normalized == "" {
			continue
		}
		if _, dup := seen[normalized]; dup {
			continue
		}
		seen[normalized] = struct{}{}
		out = append(out, normalized)
	}
	if len(out) == 0 {
		return "auto", nil
	}
	return "custom", out
}

func normalizeProxyPrefix(raw string) string {
	s := strings.TrimSpace(raw)
	if s == "" {
		return ""
	}
	if !strings.Contains(s, "://") {
		s = "https://" + s
	}
	parsed, err := url.Parse(s)
	if err != nil || (parsed.Scheme != "http" && parsed.Scheme != "https") || parsed.Host == "" {
		return ""
	}
	return strings.TrimRight(parsed.String(), "/") + "/"
}

// ActiveProxy 返回当前选中的国内加速代理前缀；直连时返回空字符串。
func (s *githubTokenUpdateSource) ActiveProxy() string {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.activeProxy
}

// ChinaMirrorActive 报告当前是否处于国内代理加速模式。
func (s *githubTokenUpdateSource) ChinaMirrorActive() bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.activeProxy != "" || (s.chinaChecked && s.chinaDetected && !s.proxyDisabled)
}

func (s *githubTokenUpdateSource) setActiveProxy(proxyPrefix string) {
	s.mu.Lock()
	changed := s.activeProxy != proxyPrefix
	s.activeProxy = proxyPrefix
	s.mu.Unlock()
	if changed && proxyPrefix != "" {
		log.Printf("自动更新已启用国内加速代理：%s", proxyPrefix)
	}
}

func (s *githubTokenUpdateSource) shouldUseChinaProxy(ctx context.Context) bool {
	if s.proxyDisabled || len(s.proxies) == 0 {
		return false
	}
	if s.proxyForced {
		return true
	}
	s.mu.Lock()
	if s.chinaChecked && time.Since(s.chinaCheckedAt) < chinaDetectCacheTTL {
		res := s.chinaDetected
		s.mu.Unlock()
		return res
	}
	s.mu.Unlock()

	detected := false
	if s.detectChina != nil {
		detected = s.detectChina(ctx)
	}
	s.mu.Lock()
	s.chinaChecked = true
	s.chinaDetected = detected
	s.chinaCheckedAt = time.Now()
	s.mu.Unlock()
	return detected
}

// defaultDetectChina 并发执行低延迟网络探测以判断是否处于中国大陆网络环境，
// 优先采用国内可直连的 GeoIP 与 Cloudflare trace，并在所有网络探测均失败时回退时区/语言启发式判断。
func (s *githubTokenUpdateSource) defaultDetectChina(ctx context.Context) bool {
	probeCtx, cancel := context.WithTimeout(ctx, chinaDetectTimeout)
	defer cancel()

	type verdict struct {
		known bool
		china bool
	}

	ipipCh := make(chan verdict, 1)
	cfCh := make(chan verdict, 1)
	connCh := make(chan verdict, 1)

	go func() {
		ipipCh <- verdictFromBool(s.probeIPIP(probeCtx))
	}()
	go func() {
		cfCh <- verdictFromBool(s.probeCloudflareTrace(probeCtx))
	}()
	go func() {
		connCh <- verdictFromBool(s.probeDomesticVsIntl(probeCtx))
	}()

	var ipipRes, cfRes, connRes verdict
	received := 0
	for received < 3 {
		select {
		case v := <-ipipCh:
			ipipRes = v
			received++
			if v.known && v.china {
				return true
			}
		case v := <-cfCh:
			cfRes = v
			received++
			if v.known && v.china {
				return true
			}
		case v := <-connCh:
			connRes = v
			received++
			if v.known && v.china {
				return true
			}
		case <-probeCtx.Done():
			received = 3
		}
		// 若 ipip 与 cf-trace 均已明确返回非中国大陆，可立即结束，无需再等待 204 探测。
		if ipipRes.known && !ipipRes.china && cfRes.known && !cfRes.china {
			return false
		}
	}

	if ipipRes.known {
		return ipipRes.china
	}
	if cfRes.known {
		return cfRes.china
	}
	if connRes.known {
		return connRes.china
	}
	return isChinaTimezoneAndLocale()
}

func verdictFromBool(known, china bool) struct {
	known bool
	china bool
} {
	return struct {
		known bool
		china bool
	}{known: known, china: china}
}

func (s *githubTokenUpdateSource) probeIPIP(ctx context.Context) (known bool, china bool) {
	if s.ipipProbeURL == "" {
		return false, false
	}
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, s.ipipProbeURL, nil)
	if err != nil {
		return false, false
	}
	req.Header.Set("User-Agent", "mygo-updater")
	resp, err := s.probeClient.Do(req)
	if err != nil {
		return false, false
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return false, false
	}
	var payload struct {
		Ret  string `json:"ret"`
		Data struct {
			Location []string `json:"location"`
		} `json:"data"`
	}
	if err := json.NewDecoder(io.LimitReader(resp.Body, 4096)).Decode(&payload); err != nil {
		return false, false
	}
	if payload.Ret != "ok" || len(payload.Data.Location) == 0 {
		return false, false
	}
	country := strings.TrimSpace(payload.Data.Location[0])
	if country == "" {
		return false, false
	}
	if country != "中国" {
		return true, false
	}
	if len(payload.Data.Location) > 1 {
		region := strings.TrimSpace(payload.Data.Location[1])
		if region == "香港" || region == "澳门" || region == "台湾" {
			return true, false
		}
	}
	return true, true
}

func (s *githubTokenUpdateSource) probeCloudflareTrace(ctx context.Context) (known bool, china bool) {
	if s.cfTraceProbeURL == "" {
		return false, false
	}
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, s.cfTraceProbeURL, nil)
	if err != nil {
		return false, false
	}
	req.Header.Set("User-Agent", "mygo-updater")
	resp, err := s.probeClient.Do(req)
	if err != nil {
		return false, false
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		return false, false
	}
	raw, err := io.ReadAll(io.LimitReader(resp.Body, 4096))
	if err != nil {
		return false, false
	}
	for _, line := range strings.Split(string(raw), "\n") {
		if loc, ok := strings.CutPrefix(strings.TrimSpace(line), "loc="); ok && loc != "" {
			return true, strings.EqualFold(loc, "CN")
		}
	}
	return false, false
}

func (s *githubTokenUpdateSource) probeDomesticVsIntl(ctx context.Context) (known bool, china bool) {
	if s.cn204ProbeURL == "" || s.intl204ProbeURL == "" {
		return false, false
	}
	cnCh := make(chan bool, 1)
	intlCh := make(chan bool, 1)
	go func() { cnCh <- s.probeReachable(ctx, s.cn204ProbeURL) }()
	go func() { intlCh <- s.probeReachable(ctx, s.intl204ProbeURL) }()
	cnOK := <-cnCh
	intlOK := <-intlCh
	if cnOK && !intlOK {
		return true, true
	}
	return false, false
}

func (s *githubTokenUpdateSource) probeReachable(ctx context.Context, target string) bool {
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, target, nil)
	if err != nil {
		return false
	}
	req.Header.Set("User-Agent", "mygo-updater")
	resp, err := s.probeClient.Do(req)
	if err != nil {
		return false
	}
	io.Copy(io.Discard, io.LimitReader(resp.Body, 1024))
	resp.Body.Close()
	return resp.StatusCode >= 200 && resp.StatusCode < 400
}

func isChinaTimezoneAndLocale() bool {
	name, offset := time.Now().Zone()
	if offset != 8*3600 {
		return false
	}
	upperTZ := strings.ToUpper(name)
	if upperTZ == "CST" || strings.Contains(upperTZ, "CHINA") || strings.Contains(name, "中国") {
		return true
	}
	for _, env := range []string{"LC_ALL", "LC_MESSAGES", "LANG"} {
		val := strings.ToLower(os.Getenv(env))
		if strings.Contains(val, "zh_cn") || strings.Contains(val, "zh-cn") || strings.Contains(val, "zh_hans") {
			return true
		}
	}
	return false
}

func (s *githubTokenUpdateSource) checkRedirect(req *http.Request, via []*http.Request) error {
	if len(via) >= 10 {
		return http.ErrUseLastResponse
	}
	apiURL, err := url.Parse(s.apiBase)
	// 重定向到 GitHub 对象存储（objects.githubusercontent.com）或第三方代理域名时必须剥离 Authorization，
	// 既避免签名 URL 返回 400/401，也防止令牌泄露。
	if err == nil && !strings.EqualFold(req.URL.Host, apiURL.Host) {
		req.Header.Del("Authorization")
		req.Header.Del("X-GitHub-Api-Version")
	}
	return nil
}

func (s *githubTokenUpdateSource) publicLatestManifestURL(manifestName string) string {
	return strings.TrimSuffix(s.downloadBase, "/") + "/" + s.repo + "/releases/latest/download/" + manifestName
}

func (s *githubTokenUpdateSource) isRepoReleaseDownloadURL(rawURL string) bool {
	prefix := strings.TrimSuffix(s.downloadBase, "/") + "/" + s.repo + "/releases/download/"
	return strings.HasPrefix(rawURL, prefix)
}

func (s *githubTokenUpdateSource) orderedProxies() []string {
	s.mu.Lock()
	active := s.activeProxy
	s.mu.Unlock()

	out := make([]string, 0, len(s.proxies)+1)
	seen := map[string]struct{}{}
	if active != "" {
		out = append(out, active)
		seen[active] = struct{}{}
	}
	for _, p := range s.proxies {
		if _, dup := seen[p]; dup {
			continue
		}
		seen[p] = struct{}{}
		out = append(out, p)
	}
	return out
}

func (s *githubTokenUpdateSource) Manifest(ctx context.Context, prereleases bool) (io.ReadCloser, error) {
	manifestName := "update-" + s.platform + ".json"
	publicManifestURL := s.publicLatestManifestURL(manifestName)
	useProxy := s.shouldUseChinaProxy(ctx)

	if !prereleases && useProxy {
		if body, err := s.fetchManifestViaProxies(ctx, publicManifestURL); err == nil {
			return body, nil
		}
		// 所有代理失败时自动回退 GitHub 直连。
		return s.manifestDirect(ctx, false, manifestName)
	}

	body, err := s.manifestDirect(ctx, prereleases, manifestName)
	if err == nil {
		return body, nil
	}
	// 直连失败时，若未禁用代理且为正式版通道，尝试国内代理兜底。
	if !prereleases && !s.proxyDisabled && len(s.proxies) > 0 {
		if proxyBody, proxyErr := s.fetchManifestViaProxies(ctx, publicManifestURL); proxyErr == nil {
			return proxyBody, nil
		}
	}
	return nil, err
}

func (s *githubTokenUpdateSource) manifestDirect(ctx context.Context, prereleases bool, manifestName string) (io.ReadCloser, error) {
	if s.token == "" && !prereleases {
		return s.getStream(ctx, s.publicLatestManifestURL(manifestName), "", false)
	}
	release, err := s.findRelease(ctx, prereleases, manifestName)
	if err != nil {
		return nil, err
	}
	s.cacheRelease(release)
	for _, asset := range release.Assets {
		if asset.Name == manifestName {
			if prereleases && asset.BrowserDownloadURL != "" && s.shouldUseChinaProxy(ctx) {
				if body, proxyErr := s.fetchManifestViaProxies(ctx, asset.BrowserDownloadURL); proxyErr == nil {
					return body, nil
				}
			}
			if asset.URL != "" {
				return s.getStream(ctx, asset.URL, "application/octet-stream", true)
			}
		}
	}
	return nil, fmt.Errorf("GitHub Release %s 缺少更新清单 %s", release.TagName, manifestName)
}

// fetchManifestViaProxies 并发竞速所有候选代理，选取最快返回合法更新清单 JSON 的节点，
// 自动过滤返回 HTML 限流页或超时卡住的节点。
func (s *githubTokenUpdateSource) fetchManifestViaProxies(ctx context.Context, publicURL string) (io.ReadCloser, error) {
	candidates := s.orderedProxies()
	if len(candidates) == 0 {
		return nil, errors.New("未配置可用的国内加速代理")
	}
	raceCtx, cancel := context.WithTimeout(ctx, proxyManifestRaceTimeout)
	defer cancel()

	type raceResult struct {
		proxy string
		body  []byte
		err   error
	}
	results := make(chan raceResult, len(candidates))
	for _, proxyPrefix := range candidates {
		proxyPrefix := proxyPrefix
		go func() {
			raw, err := s.fetchValidatedManifest(raceCtx, proxyPrefix+publicURL)
			results <- raceResult{proxy: proxyPrefix, body: raw, err: err}
		}()
	}

	var lastErr error
	for range candidates {
		res := <-results
		if res.err == nil {
			cancel()
			s.setActiveProxy(res.proxy)
			return io.NopCloser(bytes.NewReader(res.body)), nil
		}
		lastErr = res.err
	}
	return nil, fmt.Errorf("所有国内加速代理均不可用：%w", lastErr)
}

func (s *githubTokenUpdateSource) fetchValidatedManifest(ctx context.Context, targetURL string) ([]byte, error) {
	// 第三方代理绝不携带仓库令牌。
	stream, err := s.getStream(ctx, targetURL, "application/json", false)
	if err != nil {
		return nil, err
	}
	defer stream.Close()
	raw, err := io.ReadAll(io.LimitReader(stream, maxManifestBytes+1))
	if err != nil {
		return nil, err
	}
	if len(raw) > maxManifestBytes {
		return nil, fmt.Errorf("清单响应超过上限：%s", targetURL)
	}
	var probe manifestProbe
	if err := json.Unmarshal(raw, &probe); err != nil || probe.Version == "" || probe.URL == "" || probe.Signature == "" {
		return nil, fmt.Errorf("代理返回无效清单内容：%s", targetURL)
	}
	return raw, nil
}

func (s *githubTokenUpdateSource) Open(ctx context.Context, rawURL string) (io.ReadCloser, error) {
	isRepoDownload := s.isRepoReleaseDownloadURL(rawURL)
	if isRepoDownload && !s.proxyDisabled && len(s.proxies) > 0 && (s.ActiveProxy() != "" || s.shouldUseChinaProxy(ctx)) {
		if stream, err := s.openViaProxies(ctx, rawURL); err == nil {
			return stream, nil
		}
		// 全部代理失败时回退 GitHub 直连。
		return s.openDirect(ctx, rawURL)
	}

	stream, err := s.openDirect(ctx, rawURL)
	if err == nil {
		return stream, nil
	}
	if isRepoDownload && !s.proxyDisabled && len(s.proxies) > 0 {
		if proxyStream, proxyErr := s.openViaProxies(ctx, rawURL); proxyErr == nil {
			return proxyStream, nil
		}
	}
	return nil, err
}

func (s *githubTokenUpdateSource) openViaProxies(ctx context.Context, rawURL string) (io.ReadCloser, error) {
	candidates := s.orderedProxies()
	var lastErr error
	for _, proxyPrefix := range candidates {
		// 第三方代理绝不携带仓库令牌。
		stream, err := s.getBinaryStream(ctx, proxyPrefix+rawURL, "", false)
		if err == nil {
			s.setActiveProxy(proxyPrefix)
			return stream, nil
		}
		lastErr = err
	}
	if lastErr == nil {
		lastErr = errors.New("未配置可用的国内加速代理")
	}
	return nil, lastErr
}

func (s *githubTokenUpdateSource) openDirect(ctx context.Context, rawURL string) (io.ReadCloser, error) {
	if s.token != "" {
		if apiURL := s.lookupAssetAPI(ctx, rawURL); apiURL != "" {
			return s.getStream(ctx, apiURL, "application/octet-stream", true)
		}
	}
	parsed, err := url.Parse(rawURL)
	if err != nil {
		return nil, err
	}
	// 非本仓库 GitHub API 链接直接访问，不携带仓库令牌。
	apiParsed, _ := url.Parse(s.apiBase)
	sendAuth := s.token != "" && apiParsed != nil && strings.EqualFold(parsed.Host, apiParsed.Host)
	accept := ""
	if sendAuth {
		accept = "application/octet-stream"
	}
	return s.getStream(ctx, rawURL, accept, sendAuth)
}

func (s *githubTokenUpdateSource) findRelease(ctx context.Context, prereleases bool, manifestName string) (githubRelease, error) {
	if !prereleases {
		return s.fetchRelease(ctx, strings.TrimSuffix(s.apiBase, "/")+"/repos/"+s.repo+"/releases/latest")
	}
	listURL := strings.TrimSuffix(s.apiBase, "/") + "/repos/" + s.repo + "/releases?per_page=30"
	body, err := s.getStream(ctx, listURL, "application/vnd.github+json", true)
	if err != nil {
		return githubRelease{}, err
	}
	defer body.Close()
	var releases []githubRelease
	if err := json.NewDecoder(io.LimitReader(body, 16<<20)).Decode(&releases); err != nil {
		return githubRelease{}, fmt.Errorf("解析 GitHub Release 列表失败：%w", err)
	}
	for _, r := range releases {
		if r.Draft {
			continue
		}
		for _, a := range r.Assets {
			if a.Name == manifestName {
				return r, nil
			}
		}
	}
	return githubRelease{}, fmt.Errorf("未找到包含 %s 的已发布 Release", manifestName)
}

func (s *githubTokenUpdateSource) fetchRelease(ctx context.Context, releaseURL string) (githubRelease, error) {
	body, err := s.getStream(ctx, releaseURL, "application/vnd.github+json", true)
	if err != nil {
		return githubRelease{}, err
	}
	defer body.Close()
	var release githubRelease
	if err := json.NewDecoder(io.LimitReader(body, 4<<20)).Decode(&release); err != nil {
		return githubRelease{}, fmt.Errorf("解析 GitHub Release 元数据失败：%w", err)
	}
	return release, nil
}

func (s *githubTokenUpdateSource) cacheRelease(release githubRelease) {
	s.mu.Lock()
	defer s.mu.Unlock()
	for _, asset := range release.Assets {
		if asset.URL == "" {
			continue
		}
		if asset.BrowserDownloadURL != "" {
			s.byDownloadURL[asset.BrowserDownloadURL] = asset.URL
		}
		if release.TagName != "" && asset.Name != "" {
			s.byTagAsset[release.TagName+"/"+asset.Name] = asset.URL
		}
	}
}

func (s *githubTokenUpdateSource) lookupAssetAPI(ctx context.Context, rawURL string) string {
	s.mu.Lock()
	if apiURL := s.byDownloadURL[rawURL]; apiURL != "" {
		s.mu.Unlock()
		return apiURL
	}
	s.mu.Unlock()

	prefix := strings.TrimSuffix(s.downloadBase, "/") + "/" + s.repo + "/releases/download/"
	if !strings.HasPrefix(rawURL, prefix) {
		return ""
	}
	rest := strings.TrimPrefix(rawURL, prefix)
	rawTag, rawName, ok := strings.Cut(rest, "/")
	if !ok || rawTag == "" || rawName == "" || strings.Contains(rawName, "/") {
		return ""
	}
	tag, err1 := url.PathUnescape(rawTag)
	name, err2 := url.PathUnescape(rawName)
	if err1 != nil || err2 != nil {
		tag, name = rawTag, rawName
	}

	s.mu.Lock()
	if apiURL := s.byTagAsset[tag+"/"+name]; apiURL != "" {
		s.byDownloadURL[rawURL] = apiURL
		s.mu.Unlock()
		return apiURL
	}
	s.mu.Unlock()

	release, err := s.fetchRelease(ctx, strings.TrimSuffix(s.apiBase, "/")+"/repos/"+s.repo+"/releases/tags/"+rawTag)
	if err != nil {
		return ""
	}
	s.cacheRelease(release)

	s.mu.Lock()
	defer s.mu.Unlock()
	apiURL := s.byTagAsset[tag+"/"+name]
	if apiURL != "" {
		s.byDownloadURL[rawURL] = apiURL
	}
	return apiURL
}

func (s *githubTokenUpdateSource) getBinaryStream(ctx context.Context, targetURL, accept string, sendAuth bool) (io.ReadCloser, error) {
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, targetURL, nil)
	if err != nil {
		return nil, err
	}
	req.Header.Set("User-Agent", "mygo-updater")
	if accept != "" {
		req.Header.Set("Accept", accept)
	}
	if sendAuth && s.token != "" {
		req.Header.Set("Authorization", "Bearer "+s.token)
		req.Header.Set("X-GitHub-Api-Version", "2022-11-28")
	}
	resp, err := s.client.Do(req)
	if err != nil {
		return nil, err
	}
	if resp.StatusCode != http.StatusOK {
		resp.Body.Close()
		return nil, fmt.Errorf("GET %s: %s", targetURL, resp.Status)
	}
	// 防止代理站点以 HTTP 200 返回 HTML 限流/验证页面。
	if strings.Contains(strings.ToLower(resp.Header.Get("Content-Type")), "text/html") {
		resp.Body.Close()
		return nil, fmt.Errorf("GET %s: unexpected HTML response", targetURL)
	}
	return resp.Body, nil
}

func (s *githubTokenUpdateSource) getStream(ctx context.Context, targetURL, accept string, sendAuth bool) (io.ReadCloser, error) {
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, targetURL, nil)
	if err != nil {
		return nil, err
	}
	req.Header.Set("User-Agent", "mygo-updater")
	if accept != "" {
		req.Header.Set("Accept", accept)
	}
	if sendAuth && s.token != "" {
		req.Header.Set("Authorization", "Bearer "+s.token)
		req.Header.Set("X-GitHub-Api-Version", "2022-11-28")
	}
	resp, err := s.client.Do(req)
	if err != nil {
		return nil, err
	}
	if resp.StatusCode != http.StatusOK {
		resp.Body.Close()
		return nil, fmt.Errorf("GET %s: %s", targetURL, resp.Status)
	}
	return resp.Body, nil
}
