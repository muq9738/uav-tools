// Package runner 调用随 App 打包的唯一 native 转换器。
package runner

import (
	"bufio"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"sync"
	"time"

	"github.com/muq9738/uav-tools/app/internal/jobs"
)

type Exec struct {
	Binary   string
	once     sync.Once
	features Features
}

type Defaults struct {
	Threads         int     `json:"threads"`
	Geometry        string  `json:"geometry"`
	MemoryBudgetMiB uint64  `json:"memoryBudgetMiB"`
	TexturePixels   bool    `json:"texturePixels"`
	Preview         bool    `json:"preview"`
	RebuildTop      bool    `json:"rebuildTop"`
	TopSealDistance float64 `json:"topSealDistance"`
	TextureFormat   string  `json:"textureFormat"`
	DracoSpeed      *int    `json:"dracoSpeed,omitempty"`
}

type Features struct {
	Path            string   `json:"path"`
	Available       bool     `json:"available"`
	JSONEvents      bool     `json:"jsonEvents"`
	CancelFile      bool     `json:"cancelFile"`
	PauseResume     bool     `json:"pauseResume"`
	Preview         bool     `json:"preview"`
	RebuildTop      bool     `json:"rebuildTop"`
	TopSealDistance bool     `json:"topSealDistance"`
	GeometryFormats []string `json:"geometryFormats"`
	TextureFormats  []string `json:"textureFormats"`
	ProtocolVersion int      `json:"protocolVersion"`
	Version         string   `json:"version"`
	Defaults        Defaults `json:"defaults"`
	Detail          string   `json:"detail,omitempty"`
}

// Resolve 显式覆盖用于开发；正式包只在自身资源中查找，不回退到开发机 PATH。
func Resolve(explicit string) (string, error) {
	name := "osgb-tiles"
	if runtime.GOOS == "windows" {
		name += ".exe"
	}
	if explicit == "" {
		explicit = os.Getenv("OSGB_TILES_BIN")
	}
	if explicit != "" {
		absolute, err := filepath.Abs(explicit)
		if err != nil {
			return "", err
		}
		if isExecutableFile(absolute) {
			return absolute, nil
		}
		return "", fmt.Errorf("指定的 native 不可用：%s", absolute)
	}
	executable, err := os.Executable()
	if err != nil {
		return "", err
	}
	dir := filepath.Dir(executable)
	for _, candidate := range []string{
		filepath.Join(dir, "..", "Resources", "bin", name),
		filepath.Join(dir, "bin", name),
	} {
		if isExecutableFile(candidate) {
			if _, err := os.Stat(filepath.Join(filepath.Dir(candidate), "native-runtime.json")); err == nil {
				return filepath.Clean(candidate), nil
			}
		}
	}
	return "", errors.New("缺少随包 native：请重新构建完整 App；开发时可用 --sidecar 指定同一版本的转换器")
}

func isExecutableFile(path string) bool {
	info, err := os.Stat(path)
	return err == nil && info.Mode().IsRegular() && (runtime.GOOS == "windows" || info.Mode()&0o111 != 0)
}

func (e *Exec) Features() Features {
	e.once.Do(func() {
		path, err := Resolve(e.Binary)
		if err != nil {
			e.features.Detail = err.Error()
			return
		}
		e.features.Path = path
		ctx, cancel := context.WithTimeout(context.Background(), 8*time.Second)
		defer cancel()
		output, err := exec.CommandContext(ctx, path, "--capabilities").Output()
		if err != nil {
			detail := err.Error()
			var exitErr *exec.ExitError
			if errors.As(err, &exitErr) {
				if stderr := strings.TrimSpace(string(exitErr.Stderr)); stderr != "" {
					detail += "（" + stderr + "）"
				}
			}
			e.features.Detail = "无法读取 native 契约：" + detail
			return
		}
		var capability Features
		if err := json.Unmarshal(output, &capability); err != nil {
			e.features.Detail = "native 契约不是有效 JSON"
			return
		}
		if capability.ProtocolVersion != 1 || !capability.JSONEvents || !capability.CancelFile || capability.Defaults.Geometry != "meshopt" || capability.Defaults.Threads != 2 || capability.Defaults.MemoryBudgetMiB != 0 || capability.Defaults.TexturePixels || capability.Defaults.Preview || capability.Defaults.RebuildTop || (capability.Defaults.TextureFormat != "" && capability.Defaults.TextureFormat != "original") || (capability.Defaults.DracoSpeed != nil && *capability.Defaults.DracoSpeed != 5) {
			e.features.Detail = "native 契约与 App 不一致，请重新构建完整包"
			return
		}
		capability.Path = path
		capability.Available = true
		e.features = capability
	})
	return e.features
}

// SupportsPauseResume 只有 native 明确声明能力且通过既有协议检查时才为真。
func (e *Exec) SupportsPauseResume() bool {
	features := e.Features()
	return features.Available && features.PauseResume
}

func (e *Exec) Convert(ctx context.Context, spec jobs.Spec, control jobs.Control, sink jobs.Sink) (jobs.Result, error) {
	features := e.Features()
	if !features.Available {
		return jobs.Result{}, errors.New(features.Detail)
	}
	if spec.Preview && !features.Preview {
		return jobs.Result{}, errors.New("当前 native 不支持轻量预览，请更新完整包")
	}
	if spec.RebuildTop && !features.RebuildTop {
		return jobs.Result{}, errors.New("当前 native 不支持重建顶层，请更新完整包")
	}
	controlled := control.CheckpointDir != "" || control.PauseFile != "" || control.Resume
	if controlled && !features.PauseResume {
		return jobs.Result{}, jobs.ErrPauseUnsupported
	}
	if controlled && (control.CheckpointDir == "" || control.PauseFile == "") {
		return jobs.Result{}, errors.New("暂停与续转需要任务拥有的检查点和暂停令牌路径")
	}
	if spec.Geometry == "" {
		spec.Geometry = features.Defaults.Geometry
	}
	if spec.TextureFormat == "" {
		spec.TextureFormat = "original"
	}
	contains := func(values []string, requested string) bool {
		for _, value := range values {
			if value == requested {
				return true
			}
		}
		return false
	}
	if spec.Geometry == "draco" && !contains(features.GeometryFormats, "draco") {
		return jobs.Result{}, errors.New("随包 native 不支持 Draco")
	}
	if spec.TextureFormat != "original" && !contains(features.TextureFormats, spec.TextureFormat) {
		return jobs.Result{}, errors.New("随包 native 不支持所选 KTX2 格式")
	}
	dracoSpeed := 5
	if spec.DracoSpeed != nil {
		dracoSpeed = *spec.DracoSpeed
	}
	if spec.Threads == 0 {
		spec.Threads = features.Defaults.Threads
	}
	cancelDir, err := os.MkdirTemp("", "osgb-tiles-cancel-")
	if err != nil {
		return jobs.Result{}, err
	}
	defer os.RemoveAll(cancelDir)
	cancelFile := filepath.Join(cancelDir, "cancel")
	args := []string{spec.Input, spec.Output, "--threads", fmt.Sprint(spec.Threads), "--geometry", spec.Geometry,
		"--memory-budget", fmt.Sprint(spec.MemoryBudgetMiB), "--json-events", "--cancel-file", cancelFile}
	if controlled {
		args = append(args, "--checkpoint-dir", control.CheckpointDir, "--pause-file", control.PauseFile)
		if control.Resume {
			args = append(args, "--resume")
		}
	}
	if spec.SRS != "" {
		args = append(args, "--srs", spec.SRS)
	}
	if spec.Origin != "" {
		args = append(args, "--origin", spec.Origin)
	}
	if spec.TexturePixels {
		args = append(args, "--texture-pixels")
	}
	if spec.Preview {
		args = append(args, "--preview")
	}
	if spec.RebuildTop {
		args = append(args, "--rebuild-top")
		if spec.TopSealDistance != nil {
			if *spec.TopSealDistance < 0 || *spec.TopSealDistance > 2 {
				return jobs.Result{}, errors.New("焊缝距离必须在 0..2 米之间")
			}
			args = append(args, "--top-seal-distance", fmt.Sprintf("%g", *spec.TopSealDistance))
		}
	}
	if spec.Geometry == "draco" {
		args = append(args, "--draco-speed", fmt.Sprint(dracoSpeed))
	}
	if spec.TextureFormat != "original" {
		args = append(args, "--texture-format", spec.TextureFormat)
	}
	command := exec.CommandContext(ctx, features.Path, args...)
	// 各平台使用同一合作取消协议；超过宽限期才由 os/exec 强制结束。
	command.Cancel = func() error {
		if err := os.WriteFile(cancelFile, nil, 0o600); err != nil {
			return err
		}
		sink.Log("info", "已向转换器发送取消请求")
		return nil
	}
	command.WaitDelay = 5 * time.Second
	stdout, err := command.StdoutPipe()
	if err != nil {
		return jobs.Result{}, err
	}
	stderr, err := command.StderrPipe()
	if err != nil {
		return jobs.Result{}, err
	}
	sink.Log("info", "启动转换："+features.Path+" "+strings.Join(args, " "))
	if err := command.Start(); err != nil {
		return jobs.Result{}, fmt.Errorf("无法启动转换器：%w", err)
	}
	collect := &collector{sink: sink}
	var scans sync.WaitGroup
	scans.Add(2)
	go func() { defer scans.Done(); collect.scanStdout(stdout) }()
	go func() { defer scans.Done(); collect.scanStderr(stderr) }()
	scans.Wait()
	waitErr := command.Wait()
	exitCode := command.ProcessState.ExitCode()
	result := jobs.Result{ExitCode: exitCode, Report: collect.report}
	if ctx.Err() != nil {
		return result, ctx.Err()
	}
	if collect.err != nil {
		return result, collect.err
	}
	if exitCode == 75 {
		if !controlled || !features.PauseResume {
			return result, errors.New("native 未启用任务检查点却返回暂停结果")
		}
		if err := validatePaused(result.Report, spec, control); err != nil {
			return result, err
		}
		result.Paused = true
		return result, nil
	}
	if waitErr != nil {
		detail := strings.Join(collect.tail, " | ")
		if detail == "" {
			detail = waitErr.Error()
		}
		return result, fmt.Errorf("转换器以退出码 %d 结束：%s", exitCode, detail)
	}
	var report struct {
		Status          string          `json:"status"`
		Geometry        string          `json:"geometryEncoding"`
		Budget          *uint64         `json:"memoryBudgetBytes"`
		Threads         *int            `json:"requestedThreads"`
		PreserveEncoded *bool           `json:"preserveEncodedTextures"`
		RebuildTop      bool            `json:"rebuildTop"`
		TopSealDistance *float64        `json:"topSealDistance"`
		TextureFormat   string          `json:"textureFormat"`
		DracoSpeed      int             `json:"dracoSpeed"`
		Preview         json.RawMessage `json:"preview"`
	}
	expected := spec.Geometry
	if expected == "meshopt" {
		expected = "EXT_meshopt_compression"
	} else if expected == "draco" {
		expected = "KHR_draco_mesh_compression"
	}
	if json.Unmarshal(result.Report, &report) != nil || report.Status != "complete" || report.Geometry != expected || report.Budget == nil || *report.Budget != spec.MemoryBudgetMiB*(1<<20) || report.Threads == nil || *report.Threads != spec.Threads || report.PreserveEncoded == nil || *report.PreserveEncoded != (!spec.TexturePixels && spec.TextureFormat == "original") || report.RebuildTop != spec.RebuildTop || !sealMatches(spec.TopSealDistance, report.TopSealDistance) {
		return result, errors.New("native 最终报告缺失或与任务参数不一致，拒绝标记成功")
	}
	if report.TextureFormat == "" {
		report.TextureFormat = "original"
	}
	if report.TextureFormat != spec.TextureFormat || (spec.Geometry == "draco" && report.DracoSpeed != dracoSpeed) {
		return result, errors.New("native 编码报告与任务参数不一致")
	}
	if _, err := os.Stat(filepath.Join(spec.Output, "tileset.json")); err != nil {
		return result, fmt.Errorf("native 未发布 tileset.json：%w", err)
	}
	if spec.Preview {
		var preview struct {
			Status           string `json:"status"`
			Entry            string `json:"entry"`
			Algorithm        string `json:"algorithm"`
			GeometryEncoding string `json:"geometryEncoding"`
			TextureFormat    string `json:"textureFormat"`
		}
		if json.Unmarshal(report.Preview, &preview) != nil || preview.Status != "complete" || preview.Entry != "preview/tileset.json" || preview.Algorithm != "mesh-preview-v2" || preview.GeometryEncoding != "EXT_meshopt_compression+KHR_mesh_quantization" || preview.TextureFormat != "original" {
			return result, fmt.Errorf("native 轻量预览报告缺失或与预览契约不一致，拒绝标记成功（actual: status=%q entry=%q algorithm=%q geometry=%q texture=%q）",
				preview.Status, preview.Entry, preview.Algorithm, preview.GeometryEncoding, preview.TextureFormat)
		}
		if err := validatePreviewOutputs(spec.Output); err != nil {
			return result, err
		}
	}
	return result, nil
}

func validatePreviewOutputs(output string) error {
	for _, name := range []string{"preview/tileset.json", "preview/preview-report.json"} {
		raw, err := os.ReadFile(filepath.Join(output, filepath.FromSlash(name)))
		if err != nil {
			return fmt.Errorf("native 未发布 %s：%w", name, err)
		}
		var value any
		if err := json.Unmarshal(raw, &value); err != nil {
			return fmt.Errorf("native 发布的 %s 不是有效 JSON：%w", name, err)
		}
	}
	return nil
}

// sealMatches 校验 native 实际使用的焊缝距离与任务一致。任务未显式指定时
// 接受缺省（旧 native 不报该字段）或 2 m；显式指定时必须逐值相符。
func sealMatches(requested, reported *float64) bool {
	if requested == nil {
		return reported == nil || *reported == 2.0
	}
	return reported != nil && *reported == *requested
}

func validatePaused(raw json.RawMessage, spec jobs.Spec, control jobs.Control) error {
	var report struct {
		Status         string `json:"status"`
		CheckpointDir  string `json:"checkpointDir"`
		FilesCompleted *int64 `json:"filesCompleted"`
	}
	if json.Unmarshal(raw, &report) != nil || report.Status != "paused" || report.CheckpointDir == "" || report.FilesCompleted == nil || *report.FilesCompleted < 0 {
		return errors.New("native 暂停报告缺失或与任务检查点不一致")
	}
	info, err := os.Lstat(control.CheckpointDir)
	if err != nil || !info.IsDir() {
		return errors.New("native 暂停后未保留有效检查点目录")
	}
	reported, err := os.Stat(report.CheckpointDir)
	if err != nil || !os.SameFile(info, reported) {
		return errors.New("native 暂停报告中的检查点不属于本任务")
	}
	if _, err := os.Lstat(filepath.Join(spec.Output, "tileset.json")); !os.IsNotExist(err) {
		return errors.New("native 暂停时已有发布入口，拒绝标记暂停")
	}
	return nil
}

type collector struct {
	sink      jobs.Sink
	report    json.RawMessage
	tail      []string
	err       error
	sawResult bool
}

type protocolEvent struct {
	Type    string          `json:"type"`
	Stage   string          `json:"stage"`
	Done    int             `json:"done"`
	Total   int             `json:"total"`
	Unit    string          `json:"unit"`
	Level   string          `json:"level"`
	Message string          `json:"message"`
	Report  json.RawMessage `json:"report"`
}

func (c *collector) scanStdout(reader io.Reader) {
	scanner := bufio.NewScanner(reader)
	scanner.Buffer(make([]byte, 0, 64*1024), 8*1024*1024)
	for scanner.Scan() {
		if c.sawResult {
			c.err = errors.New("native result 必须是最后一条结构化事件")
			continue
		}
		var event protocolEvent
		if err := json.Unmarshal(scanner.Bytes(), &event); err != nil || event.Type == "" {
			c.err = errors.New("native stdout 含无效结构化事件")
			continue
		}
		switch event.Type {
		case "progress":
			if event.Unit == "" {
				event.Unit = "files"
			}
			c.sink.Progress(jobs.Progress{Stage: event.Stage, Done: event.Done, Total: event.Total, Unit: event.Unit, Source: "converter"})
		case "log":
			c.sink.Log(event.Level, event.Message)
		case "result":
			c.sawResult = true
			c.report = event.Report
		default:
			c.err = fmt.Errorf("未知 native 事件：%s", event.Type)
		}
	}
	if err := scanner.Err(); err != nil {
		c.err = fmt.Errorf("读取 native 事件失败：%w", err)
	}
}

func (c *collector) scanStderr(reader io.Reader) {
	scanner := bufio.NewScanner(reader)
	scanner.Buffer(make([]byte, 0, 64*1024), 1024*1024)
	for scanner.Scan() {
		line := strings.TrimSpace(scanner.Text())
		if line == "" {
			continue
		}
		level := "info"
		lower := strings.ToLower(line)
		if strings.Contains(lower, "error") || strings.Contains(line, "失败") || strings.Contains(line, "错误") {
			level = "error"
		} else if strings.Contains(lower, "warn") {
			level = "warn"
		}
		c.sink.Log(level, line)
		c.tail = append(c.tail, line)
		if len(c.tail) > 40 {
			c.tail = c.tail[len(c.tail)-40:]
		}
	}
	if err := scanner.Err(); err != nil {
		c.sink.Log("warn", "读取 native 日志失败："+err.Error())
	}
}
