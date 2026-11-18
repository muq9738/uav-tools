package api

import (
	"io/fs"
	"net/http"
	"os"
	"path"
	"strings"

	"github.com/muq9738/uav-tools/app/internal/jobs"
)

var tileContentTypes = map[string]string{
	".glb":  "model/gltf-binary",
	".gltf": "model/gltf+json",
	".json": "application/json; charset=utf-8",
	".ktx2": "image/ktx2",
	".wasm": "application/wasm",
}

// handleTileAsset 只发布已成功任务的输出，且通过 os.Root 限制在输出目录内。
// Cesium 的 Range 请求、相对纹理与外部 tileset 都由同一个入口提供。
func (s *Server) handleTileAsset(w http.ResponseWriter, r *http.Request) {
	job, err := s.options.Manager.Get(r.PathValue("id"))
	if err != nil {
		writeJobError(w, err)
		return
	}
	if job.State != jobs.StateSucceeded {
		writeError(w, http.StatusConflict, "任务尚未成功完成，不能预览")
		return
	}
	name := r.PathValue("path")
	if name == "" || !fs.ValidPath(name) || strings.Contains(name, `\`) {
		writeError(w, http.StatusBadRequest, "瓦片资源路径无效")
		return
	}
	root, err := os.OpenRoot(job.Spec.Output)
	if err != nil {
		writeError(w, http.StatusConflict, "输出目录不可读取")
		return
	}
	defer root.Close()
	file, err := root.Open(name)
	if err != nil {
		writeError(w, http.StatusNotFound, "瓦片资源不存在或不可读取")
		return
	}
	defer file.Close()
	info, err := file.Stat()
	if err != nil || !info.Mode().IsRegular() {
		writeError(w, http.StatusNotFound, "瓦片资源不存在")
		return
	}
	if contentType := tileContentTypes[strings.ToLower(path.Ext(name))]; contentType != "" {
		w.Header().Set("Content-Type", contentType)
	}
	w.Header().Set("Cache-Control", "private, max-age=3600")
	http.ServeContent(w, r, path.Base(name), info.ModTime(), file)
}
