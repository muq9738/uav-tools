//go:build unix

package runner

import (
	"context"
	"strings"
	"testing"

	"github.com/muq9738/uav-tools/app/internal/jobs"
)

const previewCapabilityScript = `if [ "$1" = "--capabilities" ]; then
 echo '{"protocolVersion":1,"jsonEvents":true,"cancelFile":true,"preview":true,"defaults":{"geometry":"meshopt","threads":2,"preview":false}}'; exit 0
fi
`

const previewSuccessScript = previewCapabilityScript + `
input=$1; output=$2; shift 2
preview=false
while [ "$#" -gt 0 ]; do
 case "$1" in
 --threads|--geometry|--memory-budget|--cancel-file) shift 2;;
 --json-events) shift;;
 --preview) preview=true; shift;;
 *) exit 9;;
 esac
done
[ "$preview" = true ] || exit 8
mkdir -p "$output/preview"
echo '{}' > "$output/tileset.json"
echo '{"asset":{"version":"1.1"}}' > "$output/preview/tileset.json"
echo '{"status":"complete"}' > "$output/preview/preview-report.json"
cat <<'EOF'
{"type":"result","report":{"status":"complete","geometryEncoding":"EXT_meshopt_compression","memoryBudgetBytes":0,"requestedThreads":2,"preserveEncodedTextures":true,"preview":{"status":"complete","entry":"preview/tileset.json","algorithm":"mesh-preview-v1","geometryEncoding":"EXT_meshopt_compression","textureFormat":"uastc"}}}
EOF
`

func TestConvertPreviewPassesFlagAndValidatesSuccess(t *testing.T) {
	exec := &Exec{Binary: writeScript(t, previewSuccessScript)}
	features := exec.Features()
	if !features.Available || !features.Preview || features.Defaults.Preview {
		t.Fatalf("预览能力或默认值不正确：%+v", features)
	}
	input, output := prepareDirs(t)
	result, err := exec.Convert(context.Background(), jobs.Spec{Input: input, Output: output, Preview: true}, jobs.Control{}, &recorder{})
	if err != nil {
		t.Fatal(err)
	}
	if result.ExitCode != 0 || !strings.Contains(string(result.Report), `"algorithm":"mesh-preview-v1"`) {
		t.Fatalf("预览结果未保留：%+v", result)
	}
}

func TestConvertPreviewRejectsMissingOrWrongReportAndOutputs(t *testing.T) {
	tests := []struct {
		name   string
		old    string
		new    string
		script string
	}{
		{name: "missing final report", script: previewCapabilityScript + "exit 0\n"},
		{name: "missing preview report", old: `,"preview":{"status":"complete"`, new: `,"ignored":{"status":"complete"`},
		{name: "wrong status", old: `"preview":{"status":"complete"`, new: `"preview":{"status":"failed"`},
		{name: "wrong entry", old: `"entry":"preview/tileset.json"`, new: `"entry":"tileset.json"`},
		{name: "wrong algorithm", old: `"algorithm":"mesh-preview-v1"`, new: `"algorithm":"other"`},
		{name: "wrong geometry", old: `"algorithm":"mesh-preview-v1","geometryEncoding":"EXT_meshopt_compression"`, new: `"algorithm":"mesh-preview-v1","geometryEncoding":"raw"`},
		{name: "wrong texture", old: `"textureFormat":"uastc"`, new: `"textureFormat":"etc1s"`},
		{name: "missing tileset", old: `echo '{"asset":{"version":"1.1"}}' > "$output/preview/tileset.json"`, new: `:`},
		{name: "invalid tileset", old: `echo '{"asset":{"version":"1.1"}}' > "$output/preview/tileset.json"`, new: `echo 'not-json' > "$output/preview/tileset.json"`},
		{name: "missing preview report file", old: `echo '{"status":"complete"}' > "$output/preview/preview-report.json"`, new: `:`},
		{name: "invalid preview report file", old: `echo '{"status":"complete"}' > "$output/preview/preview-report.json"`, new: `echo 'not-json' > "$output/preview/preview-report.json"`},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			script := test.script
			if script == "" {
				script = strings.Replace(previewSuccessScript, test.old, test.new, 1)
				if script == previewSuccessScript {
					t.Fatal("测试替换未命中")
				}
			}
			input, output := prepareDirs(t)
			result, err := (&Exec{Binary: writeScript(t, script)}).Convert(context.Background(), jobs.Spec{Input: input, Output: output, Preview: true}, jobs.Control{}, &recorder{})
			if err == nil {
				t.Fatalf("无效预览被标记成功：%+v", result)
			}
		})
	}
}

func TestConvertPreviewCapabilityIsOptIn(t *testing.T) {
	legacyScript := capabilityScript + `
output=$2
mkdir -p "$output"
echo '{}' > "$output/tileset.json"
echo '{"type":"result","report":{"status":"complete","geometryEncoding":"EXT_meshopt_compression","memoryBudgetBytes":0,"requestedThreads":2,"preserveEncodedTextures":true}}'
`
	exec := &Exec{Binary: writeScript(t, legacyScript)}
	features := exec.Features()
	if !features.Available || features.Preview || features.Defaults.Preview {
		t.Fatalf("旧 native 应保持可用且预览默认关闭：%+v", features)
	}
	input, output := prepareDirs(t)
	if _, err := exec.Convert(context.Background(), jobs.Spec{Input: input, Output: output}, jobs.Control{}, &recorder{}); err != nil {
		t.Fatalf("非预览任务不应要求 preview 报告：%v", err)
	}
	input, output = prepareDirs(t)
	if _, err := exec.Convert(context.Background(), jobs.Spec{Input: input, Output: output, Preview: true}, jobs.Control{}, &recorder{}); err == nil || !strings.Contains(err.Error(), "轻量预览") {
		t.Fatalf("旧 native 仅在请求预览时应被拒绝：%v", err)
	}
}

func TestFeaturesRejectsPreviewDefaultEnabled(t *testing.T) {
	script := strings.Replace(previewCapabilityScript, `"preview":false`, `"preview":true`, 1) + "exit 0\n"
	if features := (&Exec{Binary: writeScript(t, script)}).Features(); features.Available {
		t.Fatalf("native 默认启用预览应违反 App 契约：%+v", features)
	}
}
