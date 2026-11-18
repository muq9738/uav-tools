package main

import (
	"os"

	"golang.org/x/sys/windows"
)

// GUI 子系统没有控制台；双击启动失败时必须把错误显示给用户。
func showStartupError(err error) {
	// 命令行入口有参数，保留 stderr，避免阻塞 --doctor、服务和自动化。
	if len(os.Args) != 1 {
		return
	}
	message, _ := windows.UTF16PtrFromString(err.Error())
	title, _ := windows.UTF16PtrFromString("OSGB 转 3D Tiles — 启动失败")
	windows.MessageBox(0, message, title, windows.MB_OK|windows.MB_ICONERROR|windows.MB_SETFOREGROUND)
}
