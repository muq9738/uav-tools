// osgb-tiles-app 是 OSGB → 3D Tiles 转换器的桌面与 HTTP 外壳。
//
// 默认启动桌面窗口；加 --headless 只启动本机 HTTP 服务，供浏览器或自动化调用。
package main

import (
	"context"
	"errors"
	"flag"
	"fmt"
	"log"
	"net"
	"net/http"
	"os"

	"github.com/egoist/mygo"
	"os/signal"
	"runtime"
	"syscall"
	"time"

	"github.com/muq9738/uav-tools/app/internal/api"
	"github.com/muq9738/uav-tools/app/internal/desktop"
	"github.com/muq9738/uav-tools/app/internal/jobs"
	"github.com/muq9738/uav-tools/app/internal/runner"
	"github.com/muq9738/uav-tools/app/web"
)

// version 由构建参数覆盖。
var version = "0.1.0"

func init() {
	// MyGo 要求窗口事件循环运行在进程主线程。
	runtime.LockOSThread()
}

func main() {
	if err := run(); err != nil && !errors.Is(err, context.Canceled) {
		fmt.Fprintln(os.Stderr, "osgb-tiles-app:", err)
		os.Exit(1)
	}
}

func run() error {
	flags := flag.NewFlagSet("osgb-tiles-app", flag.ContinueOnError)
	headless := flags.Bool("headless", false, "只启动 HTTP 服务，不打开桌面窗口")
	addr := flags.String("addr", "127.0.0.1:0", "HTTP 监听地址；非本机地址必须同时设置 --token")
	token := flags.String("token", os.Getenv("OSGB_TILES_TOKEN"), "访问令牌；对外监听时必填")
	dataDir := flags.String("data-dir", "", "数据目录，保存任务索引与日志")
	sidecar := flags.String("sidecar", "", "转换器路径；默认在本程序旁或 PATH 中查找 osgb-tiles")
	maxParallel := flags.Int("max-parallel", 1, "同时运行的转换数")
	showVersion := flags.Bool("version", false, "输出版本后退出")
	doctor := flags.Bool("doctor", false, "检查运行环境与转换器后退出")
	if err := flags.Parse(os.Args[1:]); err != nil {
		return err
	}
	if *showVersion {
		fmt.Printf("osgb-tiles-app %s (%s/%s)\n", version, runtime.GOOS, runtime.GOARCH)
		return nil
	}

	exec := &runner.Exec{Binary: *sidecar}
	features := exec.Features()
	if *doctor {
		fmt.Printf("平台：%s/%s\n", runtime.GOOS, runtime.GOARCH)
		fmt.Printf("转换器：%s\n", displayPath(features.Path))
		fmt.Printf("可用：%t\n", features.Available)
		fmt.Printf("结构化进度：%t\n", features.JSONEvents)
		if features.Detail != "" {
			fmt.Printf("说明：%s\n", features.Detail)
		}
		if !features.Available {
			return errors.New("转换器不可用")
		}
		return nil
	}
	if !features.Available {
		return fmt.Errorf("%s（osgb-tiles-app --doctor 可查看检查结果）", features.Detail)
	}

	if *dataDir == "" {
		def, err := api.DefaultDataDir()
		if err != nil {
			return err
		}
		*dataDir = def
	}
	manager, err := jobs.New(jobs.Config{DataDir: *dataDir, MaxParallel: *maxParallel, Runner: exec})
	if err != nil {
		return err
	}
	defer manager.Close()

	ctx, cancel := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer cancel()

	var picker *desktop.Picker
	if !*headless {
		picker = &desktop.Picker{}
	}
	options := api.Options{
		Manager:     manager,
		Runner:      exec,
		Web:         web.Files,
		Version:     version,
		Token:       *token,
		DataDir:     *dataDir,
		MaxParallel: *maxParallel,
		Desktop:     picker != nil,
	}
	if picker != nil {
		options.Picker = picker.Pick
		options.Reveal = func(path string) error { return mygo.Shell.OpenPath(path) }
	}
	server, err := api.New(options)
	if err != nil {
		return err
	}
	listener, err := api.Listen(*addr, *token)
	if err != nil {
		return err
	}
	httpServer := &http.Server{Handler: server.Handler(), ReadHeaderTimeout: 10 * time.Second}
	go func() {
		if err := httpServer.Serve(listener); err != nil && !errors.Is(err, http.ErrServerClosed) {
			log.Printf("HTTP 服务结束：%v", err)
			cancel()
		}
	}()
	defer func() {
		shutdown, stop := context.WithTimeout(context.Background(), 5*time.Second)
		defer stop()
		httpServer.Shutdown(shutdown)
	}()

	url := pageURL(listener.Addr(), *token)
	log.Printf("界面地址：%s", url)
	if *headless {
		<-ctx.Done()
		return nil
	}
	return desktop.RunWithPicker(ctx, desktop.Options{URL: url}, picker)
}

func pageURL(addr net.Addr, token string) string {
	host := "127.0.0.1"
	if tcp, ok := addr.(*net.TCPAddr); ok && tcp.IP != nil && !tcp.IP.IsUnspecified() {
		host = tcp.IP.String()
	}
	url := fmt.Sprintf("http://%s/", net.JoinHostPort(host, fmt.Sprint(addr.(*net.TCPAddr).Port)))
	if token != "" {
		url += "?token=" + token
	}
	return url
}

func displayPath(path string) string {
	if path == "" {
		return "（未找到）"
	}
	return path
}
