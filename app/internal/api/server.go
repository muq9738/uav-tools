// Package api 暴露本机 HTTP 接口与内置 Web 界面。
//
// 桌面窗口与浏览器打开同一个页面、使用同一套接口，因此两种形态的行为一致。
package api

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io/fs"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"runtime"
	"strconv"
	"strings"
	"time"

	"github.com/muq9738/uav-tools/app/internal/jobs"
	"github.com/muq9738/uav-tools/app/internal/runner"
)

// Options 配置 Server。
type Options struct {
	Manager     *jobs.Manager
	Runner      runnerFeatures
	Web         fs.FS
	Version     string
	Token       string
	DataDir     string
	MaxParallel int
	Desktop     bool
	// Picker 只在桌面模式下提供，用于调起系统目录选择器。
	Picker func(ctx context.Context) (string, error)
	// Reveal 只在桌面模式下提供，用于在文件管理器中显示输出目录。
	Reveal func(path string) error
}

type runnerFeatures interface {
	Features() runner.Features
}

// Server 提供 HTTP 接口与静态页面。
type Server struct {
	options Options
	started time.Time
}

// New 构建 Server。
func New(options Options) (*Server, error) {
	if options.Manager == nil {
		return nil, errors.New("api: 需要任务管理器")
	}
	if options.Version == "" {
		options.Version = "0.1.0"
	}
	return &Server{options: options, started: time.Now()}, nil
}

// Handler 返回完整路由。
func (s *Server) Handler() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("GET /api/health", s.handleHealth)
	mux.HandleFunc("GET /api/config", s.handleConfig)
	mux.HandleFunc("GET /api/doctor", s.handleDoctor)
	mux.HandleFunc("POST /api/pick-directory", s.handlePickDirectory)
	mux.HandleFunc("POST /api/jobs/{id}/reveal", s.handleReveal)
	mux.HandleFunc("GET /api/jobs", s.handleListJobs)
	mux.HandleFunc("POST /api/jobs", s.handleCreateJob)
	mux.HandleFunc("GET /api/jobs/{id}", s.handleGetJob)
	mux.HandleFunc("POST /api/jobs/{id}/cancel", s.handleCancelJob)
	mux.HandleFunc("POST /api/jobs/{id}/pause", s.handlePauseJob)
	mux.HandleFunc("POST /api/jobs/{id}/resume", s.handleResumeJob)
	mux.HandleFunc("GET /api/jobs/{id}/logs", s.handleLogs)
	mux.HandleFunc("GET /api/jobs/{id}/events", s.handleEvents)
	mux.Handle("GET /", s.staticHandler())
	return s.authenticate(mux)
}

// authenticate 在设置了令牌时校验 Bearer 头或 token 查询参数（供 EventSource 使用）。
func (s *Server) authenticate(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if s.options.Token != "" {
			presented := strings.TrimPrefix(r.Header.Get("Authorization"), "Bearer ")
			if presented == "" {
				presented = r.URL.Query().Get("token")
			}
			if presented != s.options.Token {
				writeError(w, http.StatusUnauthorized, "缺少或错误的访问令牌")
				return
			}
		}
		w.Header().Set("X-Content-Type-Options", "nosniff")
		w.Header().Set("Referrer-Policy", "no-referrer")
		next.ServeHTTP(w, r)
	})
}

func (s *Server) staticHandler() http.Handler {
	web := s.options.Web
	files := http.FileServerFS(web)
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		path := strings.TrimPrefix(r.URL.Path, "/")
		if path == "" {
			path = "index.html"
		}
		if _, err := fs.Stat(web, path); err != nil {
			// 单页界面：未知路径回落到首页，便于带状态的前端路由。
			r.URL.Path = "/"
			w.Header().Set("Cache-Control", "no-store")
			files.ServeHTTP(w, r)
			return
		}
		// 界面与逻辑同源下发：禁止缓存，避免窗口在更新后仍用旧脚本或样式。
		w.Header().Set("Cache-Control", "no-store")
		files.ServeHTTP(w, r)
	})
}

func (s *Server) handleHealth(w http.ResponseWriter, _ *http.Request) {
	features := s.options.Runner.Features()
	writeJSON(w, http.StatusOK, map[string]any{
		"status":           "ok",
		"version":          s.options.Version,
		"uptimeSeconds":    int(time.Since(s.started).Seconds()),
		"sidecarAvailable": features.Available,
		"desktop":          s.options.Desktop,
	})
}

func (s *Server) handleConfig(w http.ResponseWriter, _ *http.Request) {
	writeJSON(w, http.StatusOK, map[string]any{
		"version":         s.options.Version,
		"desktop":         s.options.Desktop,
		"pickerAvailable": s.options.Picker != nil,
		"revealAvailable": s.options.Reveal != nil,
		"maxParallel":     s.options.MaxParallel,
		"dataDir":         s.options.DataDir,
		"sidecar":         s.options.Runner.Features(),
	})
}

func (s *Server) handleDoctor(w http.ResponseWriter, _ *http.Request) {
	features := s.options.Runner.Features()
	writable := false
	if s.options.DataDir != "" {
		if info, err := os.Stat(s.options.DataDir); err == nil && info.IsDir() {
			if file, err := os.CreateTemp(s.options.DataDir, ".doctor-*"); err == nil {
				file.Close()
				os.Remove(file.Name())
				writable = true
			}
		}
	}
	writeJSON(w, http.StatusOK, map[string]any{
		"version":         s.options.Version,
		"platform":        runtime.GOOS + "/" + runtime.GOARCH,
		"dataDir":         s.options.DataDir,
		"dataDirWritable": writable,
		"sidecar":         features,
		"note":            "转换器为按平台打包的独立二进制；App 与命令行共用随包 native 和 SDK 运行时。",
	})
}

func (s *Server) handlePickDirectory(w http.ResponseWriter, r *http.Request) {
	if s.options.Picker == nil {
		writeError(w, http.StatusNotImplemented, "当前模式不支持系统目录选择，请手动填写路径")
		return
	}
	path, err := s.options.Picker(r.Context())
	if err != nil {
		writeError(w, http.StatusInternalServerError, "无法打开目录选择器："+err.Error())
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"path": path})
}

type createRequest = jobs.Spec

// handleReveal 在文件管理器中显示任务输出目录，仅限桌面模式。
func (s *Server) handleReveal(w http.ResponseWriter, r *http.Request) {
	if s.options.Reveal == nil {
		writeError(w, http.StatusNotImplemented, "当前模式不支持打开文件管理器")
		return
	}
	job, err := s.options.Manager.Get(r.PathValue("id"))
	if err != nil {
		writeJobError(w, err)
		return
	}
	if _, err := os.Stat(job.Spec.Output); err != nil {
		writeError(w, http.StatusConflict, "输出目录尚不存在")
		return
	}
	if err := s.options.Reveal(job.Spec.Output); err != nil {
		writeError(w, http.StatusInternalServerError, "无法打开文件管理器："+err.Error())
		return
	}
	writeJSON(w, http.StatusOK, map[string]string{"path": job.Spec.Output})
}

func (s *Server) handleCreateJob(w http.ResponseWriter, r *http.Request) {
	var request createRequest
	decoder := json.NewDecoder(http.MaxBytesReader(w, r.Body, 1<<20))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(&request); err != nil {
		writeError(w, http.StatusBadRequest, "请求体不是有效的 JSON："+err.Error())
		return
	}
	job, err := s.options.Manager.Submit(request)
	if err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	writeJSON(w, http.StatusCreated, job)
}

func (s *Server) handleListJobs(w http.ResponseWriter, _ *http.Request) {
	writeJSON(w, http.StatusOK, map[string]any{"jobs": s.options.Manager.List()})
}

func (s *Server) handleGetJob(w http.ResponseWriter, r *http.Request) {
	job, err := s.options.Manager.Get(r.PathValue("id"))
	if err != nil {
		writeJobError(w, err)
		return
	}
	writeJSON(w, http.StatusOK, job)
}

func (s *Server) handleCancelJob(w http.ResponseWriter, r *http.Request) {
	if err := s.options.Manager.Cancel(r.PathValue("id")); err != nil {
		if errors.Is(err, jobs.ErrNotFound) {
			writeJobError(w, err)
			return
		}
		writeError(w, http.StatusConflict, err.Error())
		return
	}
	job, err := s.options.Manager.Get(r.PathValue("id"))
	if err != nil {
		writeJobError(w, err)
		return
	}
	writeJSON(w, http.StatusAccepted, job)
}

func (s *Server) handlePauseJob(w http.ResponseWriter, r *http.Request) {
	s.handlePauseResume(w, r, s.options.Manager.Pause)
}

func (s *Server) handleResumeJob(w http.ResponseWriter, r *http.Request) {
	s.handlePauseResume(w, r, s.options.Manager.Resume)
}

func (s *Server) handlePauseResume(w http.ResponseWriter, r *http.Request, action func(string) error) {
	id := r.PathValue("id")
	if _, err := s.options.Manager.Get(id); err != nil {
		writeJobError(w, err)
		return
	}
	features := s.options.Runner.Features()
	if !features.Available || !features.PauseResume || !s.options.Manager.SupportsPauseResume() {
		writeError(w, http.StatusNotImplemented, jobs.ErrPauseUnsupported.Error())
		return
	}
	if err := action(id); err != nil {
		writeError(w, http.StatusConflict, err.Error())
		return
	}
	job, err := s.options.Manager.Get(id)
	if err != nil {
		writeJobError(w, err)
		return
	}
	writeJSON(w, http.StatusAccepted, job)
}

func (s *Server) handleLogs(w http.ResponseWriter, r *http.Request) {
	after := parseInt64(r.URL.Query().Get("after"))
	limit := parseInt(r.URL.Query().Get("limit"), 500)
	lines, sequence, err := s.options.Manager.Logs(r.PathValue("id"), after, limit)
	if err != nil {
		writeJobError(w, err)
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"lines": lines, "sequence": sequence})
}

// handleEvents 用事件流推送状态、进度与日志；断线后可用 after 续传。
func (s *Server) handleEvents(w http.ResponseWriter, r *http.Request) {
	id := r.PathValue("id")
	// 先订阅再读取快照，避免暂停/继续恰好发生在初始快照与订阅之间而漏事件。
	channel, unsubscribe, err := s.options.Manager.Subscribe(id)
	if err != nil {
		writeJobError(w, err)
		return
	}
	defer unsubscribe()
	job, err := s.options.Manager.Get(id)
	if err != nil {
		writeJobError(w, err)
		return
	}
	flusher, ok := w.(http.Flusher)
	if !ok {
		writeError(w, http.StatusInternalServerError, "当前连接不支持事件流")
		return
	}
	w.Header().Set("Content-Type", "text/event-stream; charset=utf-8")
	w.Header().Set("Cache-Control", "no-store")
	w.Header().Set("Connection", "keep-alive")
	w.WriteHeader(http.StatusOK)

	after := parseInt64(r.URL.Query().Get("after"))
	send := func(kind string, payload jobs.Notification) bool {
		body, err := json.Marshal(payload)
		if err != nil {
			return false
		}
		if _, err := fmt.Fprintf(w, "event: %s\ndata: %s\n\n", kind, body); err != nil {
			return false
		}
		flusher.Flush()
		return true
	}
	lines, _, err := s.options.Manager.Logs(id, after, 5000)
	if err != nil {
		return
	}
	lastLine := after
	for i := range lines {
		line := lines[i]
		if !send("log", jobs.Notification{Kind: "log", Sequence: line.Seq, Job: job, Line: &line}) {
			return
		}
		lastLine = line.Seq
	}
	if !send("state", jobs.Notification{Kind: "state", Sequence: job.Sequence, Job: job}) {
		return
	}
	if job.State.Terminal() {
		return
	}
	heartbeat := time.NewTicker(15 * time.Second)
	defer heartbeat.Stop()
	for {
		select {
		case <-r.Context().Done():
			return
		case <-heartbeat.C:
			if _, err := fmt.Fprint(w, ": heartbeat\n\n"); err != nil {
				return
			}
			flusher.Flush()
		case notification := <-channel:
			// 初始回放期间缓冲的旧状态不应覆盖较新的快照；日志单独按行去重。
			if notification.Kind == "state" && notification.Sequence <= job.Sequence {
				continue
			}
			if notification.Kind == "progress" && notification.Sequence < job.Sequence {
				continue
			}
			if notification.Kind == "log" && notification.Line != nil {
				if notification.Line.Seq <= lastLine {
					continue
				}
				lastLine = notification.Line.Seq
			}
			if !send(notification.Kind, notification) {
				return
			}
			if notification.Kind == "state" {
				job = notification.Job
			}
			if notification.Kind == "state" && notification.Job.State.Terminal() {
				return
			}
		}
	}
}

func writeJSON(w http.ResponseWriter, status int, value any) {
	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.Header().Set("Cache-Control", "no-store")
	w.WriteHeader(status)
	json.NewEncoder(w).Encode(value)
}

func writeError(w http.ResponseWriter, status int, message string) {
	writeJSON(w, status, map[string]string{"error": message})
}

func writeJobError(w http.ResponseWriter, err error) {
	if errors.Is(err, jobs.ErrNotFound) {
		writeError(w, http.StatusNotFound, err.Error())
		return
	}
	writeError(w, http.StatusInternalServerError, err.Error())
}

func parseInt64(raw string) int64 {
	if raw == "" {
		return 0
	}
	value, err := strconv.ParseInt(raw, 10, 64)
	if err != nil || value < 0 {
		return 0
	}
	return value
}

func parseInt(raw string, fallback int) int {
	if raw == "" {
		return fallback
	}
	value, err := strconv.Atoi(raw)
	if err != nil {
		return fallback
	}
	return value
}

// IsLoopback 报告监听地址是否只接受本机连接。
func IsLoopback(addr string) bool {
	host, _, err := net.SplitHostPort(addr)
	if err != nil {
		host = addr
	}
	if host == "" {
		return false
	}
	if strings.EqualFold(host, "localhost") {
		return true
	}
	ip := net.ParseIP(host)
	return ip != nil && ip.IsLoopback()
}

// Listen 在 addr 上启动监听；非本机地址必须设置令牌，避免无鉴权对外暴露。
func Listen(addr, token string) (net.Listener, error) {
	if !IsLoopback(addr) && token == "" {
		return nil, fmt.Errorf("监听 %s 会向本机外暴露接口，必须设置 --token", addr)
	}
	listener, err := net.Listen("tcp", addr)
	if err != nil {
		return nil, err
	}
	return listener, nil
}

// DefaultDataDir 返回各平台的默认数据目录。
func DefaultDataDir() (string, error) {
	base, err := os.UserConfigDir()
	if err != nil {
		return "", err
	}
	return filepath.Join(base, "osgb-tiles-app"), nil
}
