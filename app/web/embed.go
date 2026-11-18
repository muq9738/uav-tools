// Package web 内嵌界面资源；桌面窗口与浏览器共用同一份前端。
package web

import "embed"

// Files 是界面资源。
//
//go:embed index.html app.css app.js
var Files embed.FS
