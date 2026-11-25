// Package jobs 管理 OSGB → 3D Tiles 转换任务：排队、执行、暂停、续转、取消、进度与日志。
//
// 任务状态与日志是唯一事实来源：HTTP 服务、桌面窗口和命令行都只读这份状态，
// 便于同一个转换器在服务器与桌面两种形态下表现一致。
package jobs

import (
	"context"
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"runtime"
	"sort"
	"strings"
	"sync"
	"time"
)

// State 是任务的对外状态。
type State string

// 暂停不是终态；运行中的暂停必须等待转换器确认。
const (
	StateQueued    State = "queued"
	StateRunning   State = "running"
	StatePausing   State = "pausing"
	StatePaused    State = "paused"
	StateSucceeded State = "succeeded"
	StateFailed    State = "failed"
	StateCanceled  State = "canceled"
)

// Terminal 报告该状态是否为终态。
func (s State) Terminal() bool {
	return s == StateSucceeded || s == StateFailed || s == StateCanceled
}

// Spec 是一次转换请求的参数；与转换器命令行一一对应。
type Spec struct {
	Name            string   `json:"name,omitempty"`
	Input           string   `json:"input"`
	Output          string   `json:"output"`
	Threads         int      `json:"threads"`
	SRS             string   `json:"srs,omitempty"`
	Origin          string   `json:"origin,omitempty"`
	Geometry        string   `json:"geometry"`
	MemoryBudgetMiB uint64   `json:"memoryBudgetMiB"`
	TexturePixels   bool     `json:"texturePixels,omitempty"`
	EmbedTextures   bool     `json:"embedTextures,omitempty"`
	Preview         bool     `json:"preview,omitempty"`
	RebuildTop      bool     `json:"rebuildTop,omitempty"`
	TopSealDistance *float64 `json:"topSealDistance,omitempty"`
	TextureFormat   string   `json:"textureFormat,omitempty"`
	DracoSpeed      *int     `json:"dracoSpeed,omitempty"`
	Overwrite       bool     `json:"overwrite,omitempty"`
}

// Progress 是转换进度的快照。Total 为 0 表示尚无法估算总量。
type Progress struct {
	Stage  string `json:"stage,omitempty"`
	Done   int    `json:"done"`
	Total  int    `json:"total"`
	Unit   string `json:"unit,omitempty"`
	Source string `json:"source,omitempty"`
}

// Line 是一行转换日志。Seq 在单个任务内单调递增，用于断点续查。
type Line struct {
	Seq   int64     `json:"seq"`
	At    time.Time `json:"at"`
	Level string    `json:"level"`
	Text  string    `json:"text"`
}

// Job 是对外可见的任务。
type Job struct {
	ID         string          `json:"id"`
	Spec       Spec            `json:"spec"`
	State      State           `json:"state"`
	CreatedAt  time.Time       `json:"createdAt"`
	StartedAt  *time.Time      `json:"startedAt,omitempty"`
	FinishedAt *time.Time      `json:"finishedAt,omitempty"`
	Progress   Progress        `json:"progress"`
	Error      string          `json:"error,omitempty"`
	ExitCode   *int            `json:"exitCode,omitempty"`
	Report     json.RawMessage `json:"report,omitempty"`
	// CheckpointDir 由任务管理器拥有；用户 Spec 不能指定续转控制。
	CheckpointDir  string     `json:"checkpointDir,omitempty"`
	ResumeRequired bool       `json:"resumeRequired,omitempty"`
	PausedAt       *time.Time `json:"pausedAt,omitempty"`
	// Sequence 是该任务最近一次状态或日志变更的序号，可用于日志断点续查。
	Sequence int64 `json:"sequence"`
}

// Result 是转换器退出后的结果。
type Result struct {
	ExitCode int
	Report   json.RawMessage
	Paused   bool
}

// Control 是本次执行的任务私有路径，不属于可提交的转换参数。
type Control struct {
	CheckpointDir string
	PauseFile     string
	Resume        bool
}

// Sink 接收转换过程中产生的进度与日志；实现必须可被并发调用。
type Sink interface {
	Progress(Progress)
	Log(level, text string)
}

// Runner 执行一次转换。ctx 取消时应尽快终止子进程并以 ctx 的错误返回。
type Runner interface {
	Convert(ctx context.Context, spec Spec, control Control, sink Sink) (Result, error)
}

// pauseResumeRunner 只有实际支持合作暂停的 Runner 才实现该能力。
type pauseResumeRunner interface {
	SupportsPauseResume() bool
}

// Notification 是一条可推送给订阅者的变更。
type Notification struct {
	Kind     string `json:"kind"` // state | progress | log
	Sequence int64  `json:"sequence"`
	Job      Job    `json:"job"`
	Line     *Line  `json:"line,omitempty"`
}

// Config 配置 Manager。
type Config struct {
	// DataDir 保存任务索引与日志；为空时不做持久化。
	DataDir string
	// MaxParallel limits concurrent conversions. Zero selects the host default.
	MaxParallel int
	Runner      Runner
	Now         func() time.Time
}

// ErrNotFound 表示任务不存在。
var ErrNotFound = errors.New("任务不存在")

// ErrPauseUnsupported 表示当前转换器不具备暂停与续转能力。
var ErrPauseUnsupported = errors.New("当前转换器不支持暂停与续转")

const (
	maxLinesInMemory = 20000
	subscriberBuffer = 128
)

type record struct {
	job    Job
	seq    int64
	logs   []Line
	cancel context.CancelFunc
	log    *os.File
}

// Manager 是任务队列与状态存储。
type Manager struct {
	cfg Config

	mu      sync.Mutex
	records map[string]*record
	order   []string
	queue   []string
	running int
	closed  bool
	wake    chan struct{}
	subs    map[string]map[int]chan Notification
	nextSub int
	wg      sync.WaitGroup
	unlock  func()

	pauseResume bool
	workDir     string
	temporary   bool
}

// New 构建 Manager，并在给出 DataDir 时恢复上次的任务索引。
func New(cfg Config) (*Manager, error) {
	if cfg.Runner == nil {
		return nil, errors.New("jobs: 需要 Runner")
	}
	if cfg.MaxParallel < 0 {
		return nil, errors.New("max-parallel 不能为负数")
	}
	if cfg.MaxParallel == 0 {
		cfg.MaxParallel = 1
		if runtime.GOMAXPROCS(0) >= 4 {
			cfg.MaxParallel = 2
		}
	}
	if cfg.Now == nil {
		cfg.Now = time.Now
	}
	if cfg.DataDir != "" {
		absolute, err := filepath.Abs(cfg.DataDir)
		if err != nil {
			return nil, err
		}
		cfg.DataDir = absolute
	}
	m := &Manager{
		cfg:     cfg,
		records: map[string]*record{},
		wake:    make(chan struct{}, 1),
		subs:    map[string]map[int]chan Notification{},
		workDir: cfg.DataDir,
	}
	if capable, ok := cfg.Runner.(pauseResumeRunner); ok {
		m.pauseResume = capable.SupportsPauseResume()
	}
	if cfg.DataDir == "" && m.pauseResume {
		var err error
		m.workDir, err = os.MkdirTemp("", "osgb-tiles-jobs-")
		if err != nil {
			return nil, err
		}
		m.temporary = true
	}
	if cfg.DataDir != "" {
		if err := os.MkdirAll(filepath.Join(cfg.DataDir, "logs"), 0o755); err != nil {
			return nil, err
		}
		unlock, err := acquireLock(cfg.DataDir)
		if err != nil {
			return nil, err
		}
		m.unlock = unlock
		if err := m.restore(); err != nil {
			unlock()
			return nil, err
		}
	}
	m.wg.Add(1)
	go m.dispatch()
	return m, nil
}

// MaxParallel returns the effective queue limit.
func (m *Manager) MaxParallel() int { return m.cfg.MaxParallel }

// Submit 校验并提交一个任务。校验失败时不产生任务。
func (m *Manager) Submit(spec Spec) (Job, error) {
	normalized, err := normalize(spec)
	if err != nil {
		return Job{}, err
	}
	m.mu.Lock()
	if m.closed {
		m.mu.Unlock()
		return Job{}, errors.New("服务正在关闭")
	}
	for _, existing := range m.records {
		if existing.job.State.Terminal() {
			continue
		}
		if pathsOverlap(existing.job.Spec.Output, normalized.Output) {
			m.mu.Unlock()
			return Job{}, errors.New("输出目录正被进行中的任务占用")
		}
		if normalized.Overwrite && within(normalized.Output, existing.job.Spec.Input) {
			m.mu.Unlock()
			return Job{}, errors.New("输出目录包含进行中任务的输入目录，无法覆盖")
		}
	}
	if normalized.Overwrite {
		if err := clearOutputDir(normalized.Output); err != nil {
			m.mu.Unlock()
			return Job{}, err
		}
	}
	rec := &record{job: Job{
		ID:        newID(),
		Spec:      normalized,
		State:     StateQueued,
		CreatedAt: m.cfg.Now(),
		Progress:  Progress{Stage: "queued", Unit: "files"},
	}}
	m.records[rec.job.ID] = rec
	m.order = append(m.order, rec.job.ID)
	m.queue = append(m.queue, rec.job.ID)
	if err := m.saveLocked(); err != nil {
		delete(m.records, rec.job.ID)
		m.order = m.order[:len(m.order)-1]
		m.queue = m.queue[:len(m.queue)-1]
		m.mu.Unlock()
		return Job{}, err
	}
	job := rec.job
	m.notifyLocked(rec, "state", nil)
	m.mu.Unlock()
	m.signal()
	return job, nil
}

// Get 返回一个任务的快照。
func (m *Manager) Get(id string) (Job, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	rec, ok := m.records[id]
	if !ok {
		return Job{}, ErrNotFound
	}
	return rec.job, nil
}

// List 按提交时间倒序返回全部任务。
func (m *Manager) List() []Job {
	m.mu.Lock()
	defer m.mu.Unlock()
	out := make([]Job, 0, len(m.order))
	for _, id := range m.order {
		out = append(out, m.records[id].job)
	}
	sort.SliceStable(out, func(i, j int) bool { return out[i].CreatedAt.After(out[j].CreatedAt) })
	return out
}

// Logs 返回 seq 大于 after 的日志，至多 limit 行，并给出可继续查询的游标。
func (m *Manager) Logs(id string, after int64, limit int) ([]Line, int64, error) {
	if limit <= 0 || limit > 5000 {
		limit = 500
	}
	m.mu.Lock()
	defer m.mu.Unlock()
	rec, ok := m.records[id]
	if !ok {
		return nil, 0, ErrNotFound
	}
	start := sort.Search(len(rec.logs), func(i int) bool { return rec.logs[i].Seq > after })
	end := start + limit
	if end > len(rec.logs) {
		end = len(rec.logs)
	}
	lines := append([]Line(nil), rec.logs[start:end]...)
	return lines, rec.seq, nil
}

// SupportsPauseResume 报告执行器是否支持真实的暂停与续转。
func (m *Manager) SupportsPauseResume() bool { return m.pauseResume }

// Pause 暂停排队任务，或请求转换器在文件边界合作暂停。它不会取消 context。
func (m *Manager) Pause(id string) error {
	m.mu.Lock()
	rec, ok := m.records[id]
	if !ok {
		m.mu.Unlock()
		return ErrNotFound
	}
	if !m.pauseResume {
		m.mu.Unlock()
		return ErrPauseUnsupported
	}
	if m.closed {
		m.mu.Unlock()
		return errors.New("服务正在关闭")
	}
	previous := rec.job
	text := "已请求暂停，等待转换器完成当前文件"
	switch rec.job.State {
	case StateQueued:
		now := m.cfg.Now()
		rec.job.State = StatePaused
		rec.job.PausedAt = &now
		rec.job.Progress.Stage = "paused"
		text = "任务已暂停（排队中，未启动新的转换进程）"
	case StateRunning:
		rec.job.State = StatePausing
	case StatePausing, StatePaused:
		m.mu.Unlock()
		return nil
	default:
		m.mu.Unlock()
		return fmt.Errorf("任务状态为 %s，无法暂停", rec.job.State)
	}
	// 先保存请求，再创建令牌；持久化失败不向进程发送暂停。
	if err := m.saveLocked(); err != nil {
		rec.job = previous
		m.mu.Unlock()
		return err
	}
	if rec.job.State == StatePausing {
		if err := os.WriteFile(m.pauseFile(id), nil, 0o600); err != nil {
			rec.job = previous
			m.saveLocked()
			m.mu.Unlock()
			return fmt.Errorf("无法建立暂停令牌：%w", err)
		}
	} else {
		m.removeFromQueueLocked(id)
	}
	line := m.appendLocked(rec, "info", text)
	m.saveLocked()
	m.notifyLocked(rec, "log", &line)
	m.notifyLocked(rec, "state", nil)
	m.mu.Unlock()
	m.signal()
	return nil
}

// Resume 把已暂停的任务重新排队；只有已经得到有效暂停结果的任务才续用检查点。
func (m *Manager) Resume(id string) error {
	m.mu.Lock()
	rec, ok := m.records[id]
	if !ok {
		m.mu.Unlock()
		return ErrNotFound
	}
	if !m.pauseResume {
		m.mu.Unlock()
		return ErrPauseUnsupported
	}
	if m.closed {
		m.mu.Unlock()
		return errors.New("服务正在关闭")
	}
	if rec.job.State != StatePaused {
		m.mu.Unlock()
		return fmt.Errorf("任务状态为 %s，无法继续", rec.job.State)
	}
	if err := m.validateCheckpoint(rec.job); err != nil {
		m.mu.Unlock()
		return err
	}
	if err := os.Remove(m.pauseFile(id)); err != nil && !os.IsNotExist(err) {
		m.mu.Unlock()
		return fmt.Errorf("无法移除暂停令牌：%w", err)
	}
	previous := rec.job
	rec.job.State = StateQueued
	rec.job.FinishedAt = nil
	rec.job.ExitCode = nil
	rec.job.Report = nil
	rec.job.Error = ""
	rec.job.Progress.Stage = "queued"
	if err := m.saveLocked(); err != nil {
		rec.job = previous
		m.mu.Unlock()
		return err
	}
	m.queue = append(m.queue, id)
	line := m.appendLocked(rec, "info", "任务继续，已重新排队")
	m.saveLocked()
	m.notifyLocked(rec, "log", &line)
	m.notifyLocked(rec, "state", nil)
	m.mu.Unlock()
	m.signal()
	return nil
}

// Cancel 取消排队、运行、正在暂停或已暂停的任务；已结束的任务返回错误。
func (m *Manager) Cancel(id string) error {
	m.mu.Lock()
	rec, ok := m.records[id]
	if !ok {
		m.mu.Unlock()
		return ErrNotFound
	}
	if m.closed {
		m.mu.Unlock()
		return errors.New("服务正在关闭")
	}
	var cleanupDirectory string
	switch rec.job.State {
	case StateQueued, StatePaused:
		previous := rec.job
		if rec.job.ResumeRequired {
			if err := m.validateCheckpoint(rec.job); err != nil {
				m.mu.Unlock()
				return err
			}
			cleanupDirectory = rec.job.CheckpointDir
		}
		m.removeFromQueueLocked(id)
		now := m.cfg.Now()
		rec.job.State = StateCanceled
		rec.job.FinishedAt = &now
		rec.job.ResumeRequired = false
		rec.job.Error = "提交方取消了任务"
		if err := m.saveLocked(); err != nil {
			rec.job = previous
			if previous.State == StateQueued {
				m.queue = append(m.queue, id)
			}
			m.mu.Unlock()
			return err
		}
	case StateRunning, StatePausing:
		if rec.cancel != nil {
			rec.cancel()
		}
	default:
		m.mu.Unlock()
		return fmt.Errorf("任务已结束（%s），无法取消", rec.job.State)
	}
	// 先持久化取消，再清理；保存失败必须仍可继续原检查点。
	var cleanupErr error
	if cleanupDirectory != "" {
		cleanupErr = os.RemoveAll(cleanupDirectory)
		if cleanupErr == nil {
			cleanupErr = os.RemoveAll(filepath.Dir(m.pauseFile(id)))
		}
		if cleanupErr != nil {
			rec.job.Error = "任务已取消，检查点清理失败：" + cleanupErr.Error()
			m.appendLocked(rec, "error", rec.job.Error)
		}
	}
	line := m.appendLocked(rec, "info", "已请求取消任务")
	m.saveLocked()
	m.notifyLocked(rec, "log", &line)
	m.notifyLocked(rec, "state", nil)
	m.mu.Unlock()
	m.signal()
	return cleanupErr
}

// Subscribe 订阅某个任务的变更；返回的函数用于退订。
func (m *Manager) Subscribe(id string) (<-chan Notification, func(), error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	if _, ok := m.records[id]; !ok {
		return nil, nil, ErrNotFound
	}
	m.nextSub++
	key := m.nextSub
	ch := make(chan Notification, subscriberBuffer)
	if m.subs[id] == nil {
		m.subs[id] = map[int]chan Notification{}
	}
	m.subs[id][key] = ch
	return ch, func() {
		m.mu.Lock()
		defer m.mu.Unlock()
		if subs, ok := m.subs[id]; ok {
			delete(subs, key)
			if len(subs) == 0 {
				delete(m.subs, id)
			}
		}
	}, nil
}

// Close 停止接收新任务、取消运行中的转换并关闭日志文件。
func (m *Manager) Close() {
	m.mu.Lock()
	if m.closed {
		m.mu.Unlock()
		return
	}
	m.closed = true
	for _, rec := range m.records {
		if rec.cancel != nil {
			rec.cancel()
		}
	}
	m.mu.Unlock()
	m.signal()
	m.wg.Wait()
	m.mu.Lock()
	for _, rec := range m.records {
		if rec.log != nil {
			rec.log.Close()
			rec.log = nil
		}
	}
	if m.unlock != nil {
		m.unlock()
		m.unlock = nil
	}
	if m.temporary {
		os.RemoveAll(m.workDir)
	}
	m.mu.Unlock()
}

func (m *Manager) signal() {
	select {
	case m.wake <- struct{}{}:
	default:
	}
}

func (m *Manager) dispatch() {
	defer m.wg.Done()
	for {
		m.mu.Lock()
		if m.closed {
			m.mu.Unlock()
			return
		}
		if m.running >= m.cfg.MaxParallel || len(m.queue) == 0 {
			wait := m.wake
			m.mu.Unlock()
			<-wait
			continue
		}
		id := m.queue[0]
		m.queue = m.queue[1:]
		rec := m.records[id]
		if rec == nil || rec.job.State != StateQueued {
			m.mu.Unlock()
			continue
		}
		control, err := m.prepareControl(rec)
		if err != nil {
			now := m.cfg.Now()
			rec.job.State = StateFailed
			rec.job.FinishedAt = &now
			rec.job.Error = err.Error()
			line := m.appendLocked(rec, "error", "无法启动任务："+err.Error())
			m.saveLocked()
			m.notifyLocked(rec, "log", &line)
			m.notifyLocked(rec, "state", nil)
			m.mu.Unlock()
			continue
		}
		ctx, cancel := context.WithCancel(context.Background())
		rec.cancel = cancel
		now := m.cfg.Now()
		rec.job.State = StateRunning
		if rec.job.StartedAt == nil {
			rec.job.StartedAt = &now
		}
		rec.job.Progress.Stage = "starting"
		rec.job.Progress.Unit = "files"
		m.running++
		line := m.appendLocked(rec, "info", "开始执行转换任务")
		m.saveLocked()
		m.notifyLocked(rec, "log", &line)
		m.notifyLocked(rec, "state", nil)
		m.mu.Unlock()
		m.wg.Add(1)
		go m.execute(ctx, rec, control, cancel)
	}
}

func (m *Manager) checkpointDir(id string) string {
	return filepath.Join(m.workDir, "checkpoints", id)
}

func (m *Manager) pauseFile(id string) string {
	return filepath.Join(m.workDir, "controls", id, "pause")
}

func (m *Manager) validateCheckpoint(job Job) error {
	if job.CheckpointDir != "" && job.CheckpointDir != m.checkpointDir(job.ID) {
		return errors.New("检查点路径不属于本任务，拒绝继续")
	}
	if !job.ResumeRequired {
		return nil
	}
	if job.CheckpointDir == "" {
		return errors.New("任务缺少续转检查点")
	}
	info, err := os.Lstat(job.CheckpointDir)
	if err != nil {
		return fmt.Errorf("续转检查点不可用：%w", err)
	}
	if !info.IsDir() {
		return errors.New("续转检查点必须是本任务的目录")
	}
	return nil
}

func (m *Manager) prepareControl(rec *record) (Control, error) {
	if !m.pauseResume {
		if rec.job.ResumeRequired {
			return Control{}, ErrPauseUnsupported
		}
		return Control{}, nil
	}
	if err := m.validateCheckpoint(rec.job); err != nil {
		return Control{}, err
	}
	if err := os.MkdirAll(filepath.Dir(m.checkpointDir(rec.job.ID)), 0o700); err != nil {
		return Control{}, err
	}
	pauseFile := m.pauseFile(rec.job.ID)
	if err := os.MkdirAll(filepath.Dir(pauseFile), 0o700); err != nil {
		return Control{}, err
	}
	if err := os.Remove(pauseFile); err != nil && !os.IsNotExist(err) {
		return Control{}, err
	}
	rec.job.CheckpointDir = m.checkpointDir(rec.job.ID)
	return Control{CheckpointDir: rec.job.CheckpointDir, PauseFile: pauseFile, Resume: rec.job.ResumeRequired}, nil
}

func (m *Manager) execute(ctx context.Context, rec *record, control Control, cancel context.CancelFunc) {
	defer m.wg.Done()
	defer cancel()
	sink := &recordSink{manager: m, rec: rec}
	result, err := m.cfg.Runner.Convert(ctx, rec.job.Spec, control, sink)

	m.mu.Lock()
	m.running--
	now := m.cfg.Now()
	rec.cancel = nil
	rec.job.FinishedAt = &now
	code := result.ExitCode
	switch {
	case rec.job.State == StateCanceled:
		// Cancel 已经写入终态。
	case ctx.Err() != nil:
		rec.job.State = StateCanceled
		rec.job.ResumeRequired = false
		rec.job.Error = "任务被取消"
	case err != nil:
		rec.job.State = StateFailed
		rec.job.Error = err.Error()
		rec.job.ExitCode = &code
	case result.Paused:
		rec.job.State = StatePaused
		rec.job.FinishedAt = nil
		rec.job.PausedAt = &now
		rec.job.ExitCode = &code
		rec.job.Report = result.Report
		rec.job.ResumeRequired = true
		rec.job.Progress.Stage = "paused"
	default:
		rec.job.State = StateSucceeded
		rec.job.ExitCode = &code
		rec.job.ResumeRequired = false
		rec.job.Progress = Progress{Stage: "done", Done: rec.job.Progress.Total, Total: rec.job.Progress.Total, Unit: "files", Source: rec.job.Progress.Source}
		if len(result.Report) > 0 {
			rec.job.Report = result.Report
		}
	}
	line := m.appendLocked(rec, "info", fmt.Sprintf("任务状态：%s", rec.job.State))
	if err := m.saveLocked(); err != nil {
		m.appendLocked(rec, "error", "无法持久化任务状态："+err.Error())
	}
	m.notifyLocked(rec, "log", &line)
	m.notifyLocked(rec, "state", nil)
	m.mu.Unlock()
	m.signal()
}

type recordSink struct {
	manager *Manager
	rec     *record
}

func (s *recordSink) Progress(p Progress) {
	s.manager.mu.Lock()
	if s.rec.job.State != StateRunning && s.rec.job.State != StatePausing {
		s.manager.mu.Unlock()
		return
	}
	s.rec.job.Progress = p
	s.manager.notifyLocked(s.rec, "progress", nil)
	s.manager.mu.Unlock()
}

func (s *recordSink) Log(level, text string) {
	s.manager.log(s.rec, level, text)
}

func (m *Manager) log(rec *record, level, text string) {
	m.mu.Lock()
	rec.seq++
	line := Line{Seq: rec.seq, At: m.cfg.Now(), Level: normalizeLevel(level), Text: text}
	rec.logs = append(rec.logs, line)
	if len(rec.logs) > maxLinesInMemory {
		rec.logs = append([]Line(nil), rec.logs[len(rec.logs)-maxLinesInMemory/2:]...)
	}
	rec.job.Sequence = rec.seq
	m.persistLineLocked(rec, line)
	m.notifyLocked(rec, "log", &line)
	m.mu.Unlock()
}

func (m *Manager) appendLocked(rec *record, level, text string) Line {
	rec.seq++
	line := Line{Seq: rec.seq, At: m.cfg.Now(), Level: level, Text: text}
	rec.logs = append(rec.logs, line)
	rec.job.Sequence = rec.seq
	m.persistLineLocked(rec, line)
	return line
}

func (m *Manager) persistLineLocked(rec *record, line Line) {
	if m.cfg.DataDir == "" {
		return
	}
	if rec.log == nil {
		file, err := os.OpenFile(filepath.Join(m.cfg.DataDir, "logs", rec.job.ID+".log"), os.O_CREATE|os.O_WRONLY|os.O_APPEND, 0o644)
		if err != nil {
			return
		}
		rec.log = file
	}
	payload, err := json.Marshal(line)
	if err != nil {
		return
	}
	rec.log.Write(append(payload, '\n'))
}

// notifyLocked 与变更使用同一把锁，保证并发 stdout/stderr 和状态通知按序发布。
func (m *Manager) notifyLocked(rec *record, kind string, line *Line) {
	for _, ch := range m.subs[rec.job.ID] {
		select {
		case ch <- Notification{Kind: kind, Sequence: rec.seq, Job: rec.job, Line: line}:
		default:
			// 订阅者落后时丢弃，客户端可凭 Sequence 重新查询日志。
		}
	}
}

func (m *Manager) removeFromQueueLocked(id string) {
	for i, queued := range m.queue {
		if queued == id {
			m.queue = append(m.queue[:i], m.queue[i+1:]...)
			return
		}
	}
}

type persisted struct {
	Jobs []Job `json:"jobs"`
}

func (m *Manager) saveLocked() error {
	if m.cfg.DataDir == "" {
		return nil
	}
	all := make([]Job, 0, len(m.order))
	for _, id := range m.order {
		all = append(all, m.records[id].job)
	}
	payload, err := json.MarshalIndent(persisted{Jobs: all}, "", "  ")
	if err != nil {
		return err
	}
	path := filepath.Join(m.cfg.DataDir, "jobs.json")
	tmp := path + ".tmp"
	if err := os.WriteFile(tmp, payload, 0o644); err != nil {
		return fmt.Errorf("无法保存任务索引：%w", err)
	}
	if err := os.Rename(tmp, path); err != nil {
		return fmt.Errorf("无法更新任务索引：%w", err)
	}
	return nil
}

func (m *Manager) restore() error {
	path := filepath.Join(m.cfg.DataDir, "jobs.json")
	bytes, err := os.ReadFile(path)
	if err != nil {
		if os.IsNotExist(err) {
			return nil
		}
		return err
	}
	var stored persisted
	if err := json.Unmarshal(bytes, &stored); err != nil {
		return fmt.Errorf("任务索引损坏：%w", err)
	}
	now := m.cfg.Now()
	for _, job := range stored.Jobs {
		if job.ID == "" || job.ID == "." || job.ID == ".." || filepath.Base(job.ID) != job.ID {
			return errors.New("任务索引含无效任务 ID")
		}
		rec := &record{job: job, seq: job.Sequence}
		if !job.State.Terminal() && job.State != StatePaused {
			rec.job.State = StateFailed
			rec.job.FinishedAt = &now
			rec.job.Error = "服务重启，任务已中断；请重新提交"
			rec.seq++
			rec.job.Sequence = rec.seq
		}
		lines, err := readLogFile(filepath.Join(m.cfg.DataDir, "logs", job.ID+".log"))
		if err == nil {
			rec.logs = lines
			if len(rec.logs) > maxLinesInMemory {
				rec.logs = rec.logs[len(rec.logs)-maxLinesInMemory:]
			}
			if last := len(rec.logs); last > 0 && rec.logs[last-1].Seq > rec.seq {
				rec.seq = rec.logs[last-1].Seq
				rec.job.Sequence = rec.seq
			}
		}
		if job.State == StatePaused {
			if err := m.validateCheckpoint(job); err != nil {
				rec.job.State = StateFailed
				rec.job.FinishedAt = &now
				rec.job.Error = err.Error()
			}
		}
		m.records[job.ID] = rec
		m.order = append(m.order, job.ID)
	}
	return m.saveLocked()
}

func readLogFile(path string) ([]Line, error) {
	bytes, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	lines := make([]Line, 0, 64)
	for _, raw := range strings.Split(string(bytes), "\n") {
		if strings.TrimSpace(raw) == "" {
			continue
		}
		var line Line
		if err := json.Unmarshal([]byte(raw), &line); err != nil {
			continue
		}
		lines = append(lines, line)
	}
	return lines, nil
}

func normalize(spec Spec) (Spec, error) {
	if strings.TrimSpace(spec.Input) == "" {
		return Spec{}, errors.New("缺少输入目录")
	}
	if strings.TrimSpace(spec.Output) == "" {
		return Spec{}, errors.New("缺少输出目录")
	}
	input, err := filepath.Abs(spec.Input)
	if err != nil {
		return Spec{}, err
	}
	output, err := filepath.Abs(spec.Output)
	if err != nil {
		return Spec{}, err
	}
	info, err := os.Stat(input)
	if err != nil {
		return Spec{}, fmt.Errorf("输入目录不可用：%w", err)
	}
	if !info.IsDir() && (!info.Mode().IsRegular() || !strings.EqualFold(filepath.Ext(input), ".osgb")) {
		return Spec{}, errors.New("输入必须是 OSGB 目录或索引 .osgb 文件")
	}
	if entries, err := os.ReadDir(input); err == nil && len(entries) == 0 {
		return Spec{}, errors.New("输入目录为空")
	}
	if filepath.Dir(output) == output {
		return Spec{}, errors.New("输出目录不能是根目录")
	}
	if samePath(input, output) {
		return Spec{}, errors.New("输出目录不能与输入目录相同")
	}
	if within(output, input) {
		return Spec{}, errors.New("输出目录不能包含输入目录")
	}
	if stat, err := os.Lstat(output); err == nil {
		if stat.Mode()&os.ModeSymlink != 0 {
			if spec.Overwrite {
				return Spec{}, errors.New("输出路径是符号链接，拒绝覆盖")
			}
			stat, err = os.Stat(output)
			if err != nil {
				return Spec{}, err
			}
		}
		if os.SameFile(info, stat) {
			return Spec{}, errors.New("输出目录不能与输入目录相同")
		}
		if !stat.IsDir() {
			return Spec{}, errors.New("输出路径已存在且不是目录")
		}
		if spec.Overwrite {
			if realInput, err := filepath.EvalSymlinks(input); err == nil {
				if realOutput, err := filepath.EvalSymlinks(output); err == nil {
					if samePath(realInput, realOutput) {
						return Spec{}, errors.New("输出目录不能与输入目录相同")
					}
					if within(realOutput, realInput) {
						return Spec{}, errors.New("输出目录不能包含输入目录")
					}
				}
			}
		}
		entries, err := os.ReadDir(output)
		if err != nil {
			return Spec{}, err
		}
		if len(entries) > 0 && !spec.Overwrite {
			return Spec{}, errors.New("输出目录必须为空或尚不存在，避免覆盖已有成果")
		}
	} else if !os.IsNotExist(err) {
		return Spec{}, err
	}
	if spec.Geometry == "" {
		spec.Geometry = "meshopt"
	}
	if spec.TextureFormat == "" {
		spec.TextureFormat = "original"
	}
	if spec.TextureFormat != "original" && spec.TextureFormat != "uastc" && spec.TextureFormat != "etc1s" {
		return spec, errors.New("textureFormat 必须为 original/uastc/etc1s")
	}
	if spec.DracoSpeed != nil && *spec.DracoSpeed != 0 && *spec.DracoSpeed != 5 && *spec.DracoSpeed != 10 {
		return spec, errors.New("dracoSpeed 必须为 0/5/10")
	}
	if spec.Geometry != "raw" && spec.Geometry != "meshopt" && spec.Geometry != "draco" {
		return Spec{}, errors.New("geometry 必须为 raw/meshopt/draco")
	}
	if spec.MemoryBudgetMiB > ^uint64(0)/(1<<20) {
		return Spec{}, errors.New("内存预算过大")
	}
	if spec.Threads < 0 {
		return Spec{}, errors.New("线程数必须为 1..64")
	}
	if spec.Threads == 0 {
		spec.Threads = 2
	}
	if spec.Threads > 64 {
		return Spec{}, errors.New("线程数不能超过 64")
	}
	spec.Input = input
	spec.Output = output
	spec.Name = strings.TrimSpace(spec.Name)
	if spec.Name == "" || isGenericTaskSegment(spec.Name) {
		if inferred := InferTaskName(input, output); inferred != "" {
			spec.Name = inferred
		}
	}
	spec.SRS = strings.TrimSpace(spec.SRS)
	spec.Origin = strings.TrimSpace(spec.Origin)
	return spec, nil
}

func normalizePathForCompare(path string) string {
	cleaned := filepath.Clean(path)
	if runtime.GOOS == "windows" {
		return strings.ToLower(cleaned)
	}
	return cleaned
}

func samePath(a, b string) bool {
	return normalizePathForCompare(a) == normalizePathForCompare(b)
}

func within(parent, child string) bool {
	relative, err := filepath.Rel(normalizePathForCompare(parent), normalizePathForCompare(child))
	if err != nil {
		return false
	}
	if relative == "." {
		return true
	}
	if relative == ".." || strings.HasPrefix(relative, ".."+string(filepath.Separator)) || filepath.IsAbs(relative) {
		return false
	}
	return true
}

func pathsOverlap(a, b string) bool {
	return samePath(a, b) || within(a, b) || within(b, a)
}

// clearOutputDir 在允许覆盖的场景下同步清空已有输出目录，确保返回前旧成果（如 tileset.json）已移除。
func clearOutputDir(output string) error {
	info, err := os.Lstat(output)
	if err != nil {
		if os.IsNotExist(err) {
			return nil
		}
		return fmt.Errorf("无法检查已有输出目录：%w", err)
	}
	if info.Mode()&os.ModeSymlink != 0 {
		return errors.New("输出路径是符号链接，拒绝覆盖")
	}
	if !info.IsDir() {
		return errors.New("输出路径已存在且不是目录")
	}
	entries, err := os.ReadDir(output)
	if err != nil {
		return fmt.Errorf("无法读取已有输出目录：%w", err)
	}
	for _, entry := range entries {
		if err := os.RemoveAll(filepath.Join(output, entry.Name())); err != nil {
			return fmt.Errorf("无法清空已有输出目录：%w", err)
		}
	}
	if err := os.Remove(output); err != nil && !os.IsNotExist(err) {
		// Windows 下若目录本身被资源管理器等短暂占用但内部已清空，保留空目录供转换器直接发布。
		remaining, readErr := os.ReadDir(output)
		if readErr != nil || len(remaining) > 0 {
			return fmt.Errorf("无法移除已有输出目录：%w", err)
		}
	}
	return nil
}

func normalizeLevel(level string) string {
	switch strings.ToLower(strings.TrimSpace(level)) {
	case "error", "err":
		return "error"
	case "warn", "warning":
		return "warn"
	case "debug":
		return "debug"
	default:
		return "info"
	}
}

var genericTaskSegments = map[string]struct{}{
	"out": {}, "output": {}, "outputs": {},
	"3dtiles": {}, "tiles": {}, "tileset": {},
	"b3dm": {}, "b3dms": {}, "glb": {}, "gltf": {},
	"result": {}, "results": {}, "dist": {}, "build": {}, "export": {},
	"terra_osgbs": {}, "terra_osgb": {}, "terra_b3dms": {}, "terra_b3dm": {}, "terra_pcd": {},
	"osgb": {}, "osgbs": {}, "data": {}, "model": {}, "models": {}, "pc": {},
	"block": {}, "blocks": {}, "production": {}, "productions": {},
}

func isAllDigits(s string) bool {
	if s == "" {
		return false
	}
	for i := 0; i < len(s); i++ {
		if s[i] < '0' || s[i] > '9' {
			return false
		}
	}
	return true
}

func isDriveRootSegment(s string) bool {
	return len(s) == 2 && s[1] == ':' && ((s[0] >= 'a' && s[0] <= 'z') || (s[0] >= 'A' && s[0] <= 'Z'))
}

func isGenericTaskSegment(seg string) bool {
	s := strings.TrimSpace(seg)
	if s == "" || s == "." || s == ".." || isDriveRootSegment(s) || isAllDigits(s) {
		return true
	}
	lower := strings.ToLower(s)
	if _, ok := genericTaskSegments[lower]; ok {
		return true
	}
	for _, suffix := range []string{"-3dtiles", "_3dtiles"} {
		if base, ok := strings.CutSuffix(lower, suffix); ok && isGenericTaskSegment(base) {
			return true
		}
	}
	return false
}

func splitPathSegments(p string) []string {
	parts := strings.FieldsFunc(p, func(r rune) bool { return r == '/' || r == '\\' })
	out := make([]string, 0, len(parts))
	for _, part := range parts {
		if trimmed := strings.TrimSpace(part); trimmed != "" {
			out = append(out, trimmed)
		}
	}
	return out
}

func pickProjectSegment(rawPath string, stripOsgbFile bool) string {
	segs := splitPathSegments(rawPath)
	if stripOsgbFile && len(segs) > 0 && strings.HasSuffix(strings.ToLower(segs[len(segs)-1]), ".osgb") {
		segs = segs[:len(segs)-1]
	}
	for i := len(segs) - 1; i >= 0; i-- {
		if !isGenericTaskSegment(segs[i]) {
			return segs[i]
		}
	}
	return ""
}

// InferTaskName 从输出/输入路径中提取有意义的项目名称，跳过诸如 out、terra_osgbs、models、pc、0 等通用层级。
func InferTaskName(input, output string) string {
	outSegs := splitPathSegments(output)
	if len(outSegs) > 0 && !isGenericTaskSegment(outSegs[len(outSegs)-1]) {
		return outSegs[len(outSegs)-1]
	}
	if name := pickProjectSegment(output, false); name != "" {
		return name
	}
	if name := pickProjectSegment(input, true); name != "" {
		return name
	}
	if len(outSegs) > 0 {
		return outSegs[len(outSegs)-1]
	}
	inSegs := splitPathSegments(input)
	if len(inSegs) > 0 {
		return inSegs[len(inSegs)-1]
	}
	return ""
}

func newID() string {
	buf := make([]byte, 8)
	if _, err := rand.Read(buf); err != nil {
		return fmt.Sprintf("job-%d", time.Now().UnixNano())
	}
	return hex.EncodeToString(buf)
}
