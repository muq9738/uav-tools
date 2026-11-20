//go:build unix

package runner

import (
	"context"
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"

	"github.com/muq9738/uav-tools/app/internal/jobs"
)

type recorder struct {
	mu       sync.Mutex
	logs     []string
	levels   []string
	progress []jobs.Progress
}

func (r *recorder) Progress(p jobs.Progress) {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.progress = append(r.progress, p)
}
func (r *recorder) Log(level, text string) {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.logs = append(r.logs, text)
	r.levels = append(r.levels, level)
}
func writeScript(t *testing.T, body string) string {
	t.Helper()
	p := filepath.Join(t.TempDir(), "osgb-tiles")
	if err := os.WriteFile(p, []byte("#!/bin/sh\n"+body), 0o755); err != nil {
		t.Fatal(err)
	}
	return p
}

const capabilityScript = `if [ "$1" = "--capabilities" ]; then
 echo '{"protocolVersion":1,"jsonEvents":true,"cancelFile":true,"rebuildTop":true,"topSealDistance":true,"defaults":{"geometry":"meshopt","threads":2,"topSealDistance":2}}'; exit 0
fi
`

func prepareDirs(t *testing.T) (string, string) {
	t.Helper()
	return t.TempDir(), filepath.Join(t.TempDir(), "out")
}

func TestFeaturesRejectsMismatchedNative(t *testing.T) {
	good := (&Exec{Binary: writeScript(t, capabilityScript)}).Features()
	if !good.Available || !good.JSONEvents || good.Defaults.Geometry != "meshopt" {
		t.Fatal(good)
	}
	bad := (&Exec{Binary: writeScript(t, `echo '{"protocolVersion":0}'`)}).Features()
	if bad.Available {
		t.Fatal("旧版本 native 不应可用")
	}
	missing := (&Exec{Binary: filepath.Join(t.TempDir(), "missing")}).Features()
	if missing.Available {
		t.Fatal("指定路径缺失不能回退其他转换器")
	}
	failed := (&Exec{Binary: writeScript(t, "echo 'UTF-8 路径诊断失败' >&2\nexit 1\n")}).Features()
	if failed.Available || !strings.Contains(failed.Detail, "UTF-8 路径诊断失败") {
		t.Fatalf("应透传 --capabilities 失败的 stderr：%v", failed)
	}
}

func TestConvertPassesOptionsAndUsesNativeProgress(t *testing.T) {
	script := capabilityScript + `
input=$1; output=$2; shift 2
threads=2; geometry=meshopt; budget=0; preserve=true
while [ "$#" -gt 0 ]; do
 case "$1" in
 --threads) threads=$2; shift 2;;
 --geometry) geometry=$2; shift 2;;
 --memory-budget) budget=$2; shift 2;;
 --json-events) shift;;
 --cancel-file) shift 2;;
 --srs|--origin) shift 2;;
 --texture-pixels) preserve=false; shift;;
 *) exit 9;;
 esac
done
mkdir -p "$output"; echo '{}' > "$output/tileset.json"
echo '读取源文件' >&2
echo '{"type":"progress","stage":"convert","done":1,"total":0}'
if [ "$geometry" = meshopt ]; then geometry=EXT_meshopt_compression; fi
printf '{"type":"result","report":{"status":"complete","geometryEncoding":"%s","memoryBudgetBytes":%s,"requestedThreads":%s,"preserveEncodedTextures":%s}}\n' "$geometry" "$((budget*1048576))" "$threads" "$preserve"
`
	for _, mode := range []string{"raw", "meshopt"} {
		t.Run(mode, func(t *testing.T) {
			input, output := prepareDirs(t)
			sink := &recorder{}
			result, err := (&Exec{Binary: writeScript(t, script)}).Convert(context.Background(), jobs.Spec{Input: input, Output: output, Geometry: mode, Threads: 3, MemoryBudgetMiB: 512, TexturePixels: true}, jobs.Control{}, sink)
			if err != nil {
				t.Fatal(err)
			}
			var report map[string]any
			if json.Unmarshal(result.Report, &report) != nil || report["memoryBudgetBytes"] != float64(512<<20) {
				t.Fatal(string(result.Report))
			}
			if len(sink.progress) != 1 || sink.progress[0].Source != "converter" || sink.progress[0].Done != 1 {
				t.Fatal(sink.progress)
			}
		})
	}
}

func TestConvertPlumbsTopSealDistance(t *testing.T) {
	script := capabilityScript + `
input=$1; output=$2; shift 2
seal=2
while [ "$#" -gt 0 ]; do
 case "$1" in
 --threads|--geometry|--memory-budget|--srs|--origin|--cancel-file) shift 2;;
 --json-events|--rebuild-top) shift;;
 --top-seal-distance) seal=$2; shift 2;;
 *) exit 9;;
 esac
done
mkdir -p "$output"; echo '{}' > "$output/tileset.json"
printf '{"type":"result","report":{"status":"complete","geometryEncoding":"EXT_meshopt_compression","memoryBudgetBytes":0,"requestedThreads":2,"preserveEncodedTextures":true,"rebuildTop":true,"topSealDistance":%s}}\n' "$seal"
`
	seal := 1.5
	input, output := prepareDirs(t)
	if _, err := (&Exec{Binary: writeScript(t, script)}).Convert(context.Background(), jobs.Spec{Input: input, Output: output, Geometry: "meshopt", RebuildTop: true, TopSealDistance: &seal}, jobs.Control{}, &recorder{}); err != nil {
		t.Fatal(err)
	}
	// A report that disagrees with the requested distance must be rejected.
	script = strings.Replace(script, `--json-events|--rebuild-top) shift;;`, `--json-events|--rebuild-top) shift;;`, 1)
	mismatch := strings.Replace(script, `%s}}\n' "$seal"`, `%s}}\n' "2.0"`, 1)
	input, output = prepareDirs(t)
	if _, err := (&Exec{Binary: writeScript(t, mismatch)}).Convert(context.Background(), jobs.Spec{Input: input, Output: output, Geometry: "meshopt", RebuildTop: true, TopSealDistance: &seal}, jobs.Control{}, &recorder{}); err == nil || !strings.Contains(err.Error(), "报告") {
		t.Fatalf("焊缝距离不一致必须拒绝: %v", err)
	}
}

func TestConvertRejectsMissingOrWrongReport(t *testing.T) {
	for _, report := range []string{"", `echo '{"type":"result","report":{"status":"complete","geometryEncoding":"raw","requestedThreads":2}}'`} {
		input, output := prepareDirs(t)
		_, err := (&Exec{Binary: writeScript(t, capabilityScript+report)}).Convert(context.Background(), jobs.Spec{Input: input, Output: output}, jobs.Control{}, &recorder{})
		if err == nil || !strings.Contains(err.Error(), "报告") {
			t.Fatalf("应拒绝缺失或不匹配报告: %v", err)
		}
	}
}

func TestConvertReportsFailureDetail(t *testing.T) {
	input, output := prepareDirs(t)
	_, err := (&Exec{Binary: writeScript(t, capabilityScript+"echo '缺失 OSGB 分页引用' >&2\nexit 3\n")}).Convert(context.Background(), jobs.Spec{Input: input, Output: output}, jobs.Control{}, &recorder{})
	if err == nil || !strings.Contains(err.Error(), "退出码 3") || !strings.Contains(err.Error(), "缺失 OSGB") {
		t.Fatal(err)
	}
}
