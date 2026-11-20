package main

import (
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"runtime"
	"strings"
	"sync"
	"time"

	"github.com/egoist/mygo/netproxy"
)

const updateGitHubRepo = "muq9738/uav-tools"

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

// githubTokenUpdateSource 使用随发布包嵌入的只读 GitHub 令牌读取 Release 清单与更新资产，
// 使私有或受限访问的 GitHub Releases 也能自动检查并下载签名更新（包括增量 delta）。
type githubTokenUpdateSource struct {
	repo         string
	token        string
	apiBase      string
	downloadBase string
	platform     string
	client       *http.Client

	mu            sync.Mutex
	byDownloadURL map[string]string
	byTagAsset    map[string]string
}

func newGitHubTokenSource(repo, token string) *githubTokenUpdateSource {
	s := &githubTokenUpdateSource{
		repo:          repo,
		token:         token,
		apiBase:       "https://api.github.com",
		downloadBase:  "https://github.com",
		platform:      runtime.GOOS + "-" + runtime.GOARCH,
		byDownloadURL: map[string]string{},
		byTagAsset:    map[string]string{},
	}
	s.client = &http.Client{
		Transport:     netproxy.Transport(),
		Timeout:       10 * time.Minute,
		CheckRedirect: s.checkRedirect,
	}
	return s
}

func (s *githubTokenUpdateSource) checkRedirect(req *http.Request, via []*http.Request) error {
	if len(via) >= 10 {
		return http.ErrUseLastResponse
	}
	apiURL, err := url.Parse(s.apiBase)
	// 重定向到 GitHub 对象存储（objects.githubusercontent.com）等外部域名时必须剥离 Authorization，
	// 否则签名 URL 会返回 400/401。
	if err == nil && !strings.EqualFold(req.URL.Host, apiURL.Host) {
		req.Header.Del("Authorization")
	}
	return nil
}

func (s *githubTokenUpdateSource) Manifest(ctx context.Context, prereleases bool) (io.ReadCloser, error) {
	manifestName := "update-" + s.platform + ".json"
	release, err := s.findRelease(ctx, prereleases, manifestName)
	if err != nil {
		return nil, err
	}
	s.cacheRelease(release)
	for _, asset := range release.Assets {
		if asset.Name == manifestName && asset.URL != "" {
			return s.getStream(ctx, asset.URL, "application/octet-stream", true)
		}
	}
	return nil, fmt.Errorf("GitHub Release %s 缺少更新清单 %s", release.TagName, manifestName)
}

func (s *githubTokenUpdateSource) Open(ctx context.Context, rawURL string) (io.ReadCloser, error) {
	if apiURL := s.lookupAssetAPI(ctx, rawURL); apiURL != "" {
		return s.getStream(ctx, apiURL, "application/octet-stream", true)
	}
	parsed, err := url.Parse(rawURL)
	if err != nil {
		return nil, err
	}
	// 非本仓库 GitHub 下载链接直接访问，不携带仓库令牌。
	apiParsed, _ := url.Parse(s.apiBase)
	sendAuth := apiParsed != nil && strings.EqualFold(parsed.Host, apiParsed.Host)
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
