package api

import (
	"encoding/json"
	"net/http"
	"path/filepath"
	"strings"

	"github.com/muq9738/uav-tools/app/internal/jobs"
)

// compatCreateRequest 兼容外部切片任务 POST /task/create 的请求体结构。
// 未声明的扩展字段会被安全忽略，避免上游增加配置项时导致 400。
type compatCreateRequest struct {
	Options compatTaskOptions `json:"options"`
}

type compatTaskOptions struct {
	Name       string             `json:"name"`
	Type       string             `json:"type"`
	IsPublish  bool               `json:"isPublish"`
	InputPath  string             `json:"inputPath"`
	OutputPath string             `json:"outputPath"`
	Settings   compatTaskSettings `json:"settings"`
}

type compatTaskSettings struct {
	SRS                    string `json:"srs"`
	SRSOrigin              string `json:"srsOrigin"`
	Is3DTiles1_1           *bool  `json:"is3DTiles1_1"`
	NoLight                *bool  `json:"noLight"`
	TextureFormat          string `json:"textureFormat"`
	VertexCompressionLevel int    `json:"vertexCompressionLevel"`
	BackFaceCulling        *bool  `json:"backFaceCulling"`
	IsRebuildTop           bool   `json:"isRebuildTop"`
	MandatoryDoubleSide    *bool  `json:"mandatoryDoubleSide"`
	ComputedNormal         *bool  `json:"computedNormal"`
	VertexCompress         *bool  `json:"vertexCompress"`
	MaxConcurrency         int    `json:"maxConcurrency"`
}

// handleCompatCreateTask 接收通用倾斜摄影切片任务请求，映射为内部 jobs.Spec 并返回 201。
func (s *Server) handleCompatCreateTask(w http.ResponseWriter, r *http.Request) {
	var request compatCreateRequest
	decoder := json.NewDecoder(http.MaxBytesReader(w, r.Body, 1<<20))
	if err := decoder.Decode(&request); err != nil {
		writeError(w, http.StatusBadRequest, "请求体不是有效的 JSON："+err.Error())
		return
	}
	opts := request.Options
	if opts.Type != "" && !strings.EqualFold(strings.TrimSpace(opts.Type), "TiltModelSlicing") {
		writeError(w, http.StatusBadRequest, "不支持的任务类型："+opts.Type)
		return
	}
	spec := mapCompatToSpec(opts)
	job, err := s.options.Manager.Submit(spec)
	if err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	writeJSON(w, http.StatusCreated, map[string]any{
		"id":    job.ID,
		"name":  strings.TrimSpace(opts.Name),
		"state": job.State,
		"job":   job,
	})
}

func mapCompatToSpec(opts compatTaskOptions) jobs.Spec {
	settings := opts.Settings

	// 默认采用 Draco 压缩 + GLB 内嵌原始贴图，兼顾低带宽首帧加载与高视觉保真；
	// 仅在显式关闭 vertexCompress 时回退 raw。
	geometry := "draco"
	if settings.VertexCompress != nil && !*settings.VertexCompress {
		geometry = "raw"
	}

	textureFormat := "original"
	switch strings.ToLower(strings.TrimSpace(settings.TextureFormat)) {
	case "uastc", "ktx2", "ktx2_uastc":
		textureFormat = "uastc"
	case "etc1s", "ktx2_etc1s":
		textureFormat = "etc1s"
	default:
		// "default"、"original" 或其他未知值统一走保真原始纹理。
		textureFormat = "original"
	}

	threads := settings.MaxConcurrency
	if threads < 0 {
		threads = 0
	} else if threads > 64 {
		threads = 64
	}

	return jobs.Spec{
		Input:         filepath.FromSlash(strings.TrimSpace(opts.InputPath)),
		Output:        filepath.FromSlash(strings.TrimSpace(opts.OutputPath)),
		Threads:       threads,
		SRS:           strings.TrimSpace(settings.SRS),
		Origin:        strings.TrimSpace(settings.SRSOrigin),
		Geometry:      geometry,
		EmbedTextures: true,
		RebuildTop:    settings.IsRebuildTop,
		TextureFormat: textureFormat,
	}
}
