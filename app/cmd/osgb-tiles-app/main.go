// osgb-tiles-app 是 OSGB → 3D Tiles 转换器的桌面与 HTTP 外壳。
//
// 默认启动桌面窗口；加 --headless 只启动本机 HTTP 服务，供浏览器或自动化调用。
package main

import (
	"context"
	"crypto/tls"
	"errors"
	"flag"
	"fmt"
	"log"
	"net"
	"net/http"
	"os"
	"os/exec"
	"os/signal"
	"runtime"
	"strings"
	"sync"
	"sync/atomic"
	"syscall"
	"time"

	"github.com/egoist/mygo"
	"github.com/egoist/mygo/plugins/updater"
	"github.com/muq9738/uav-tools/app/internal/api"
	"github.com/muq9738/uav-tools/app/internal/desktop"
	"github.com/muq9738/uav-tools/app/internal/jobs"
	"github.com/muq9738/uav-tools/app/internal/runner"
	"github.com/muq9738/uav-tools/app/web"
)

const defaultCompatHTTPSAddr = "0.0.0.0:42225"

// version 由构建参数覆盖。
var version = "0.6.7"

func appVersion() string {
	if v := strings.TrimSpace(mygo.App.Version()); v != "" && v != "0.0.0" {
		return v
	}
	return version
}

func resolveCompatHTTPSAddr(httpsAddr, primaryAddr string, headless, primaryTLS bool) string {
	trimmed := strings.TrimSpace(httpsAddr)
	if strings.EqualFold(trimmed, "off") || strings.EqualFold(trimmed, "none") {
		return ""
	}
	if trimmed != "" {
		return trimmed
	}
	// 双击桌面窗口或浏览器模式下，自动在 0.0.0.0:42225 伴随监听内置 HTTPS 切片兼容服务；
	// --headless 模式保持仅监听 --addr（除非显式传递 --https-addr），避免影响现有自动化测试。
	if !headless && !primaryTLS && strings.TrimSpace(primaryAddr) == "127.0.0.1:0" {
		return defaultCompatHTTPSAddr
	}
	return ""
}

func startHTTPSCompatServer(handler http.Handler, listenAddr, token, certFile, keyFile, dataDir string, onErr func(error)) (*http.Server, net.Addr, string, error) {
	rawListener, err := api.Listen(listenAddr, token)
	if err != nil {
		return nil, nil, "", fmt.Errorf("无法在 %s 启动内置 HTTPS 切片服务（端口可能已被占用；可用 --https-addr=off 禁用或指定其他端口）：%w", listenAddr, err)
	}
	tlsConfig, certSource, err := api.LoadOrGenerateTLSConfig(certFile, keyFile, dataDir, listenAddr)
	if err != nil {
		rawListener.Close()
		return nil, nil, "", fmt.Errorf("初始化内置 HTTPS 证书失败：%w", err)
	}
	dualListener := api.NewDualProtocolListener(rawListener, tlsConfig)
	srv := &http.Server{Handler: handler, ReadHeaderTimeout: 10 * time.Second}
	go func() {
		if err := srv.Serve(dualListener); err != nil && !errors.Is(err, http.ErrServerClosed) && onErr != nil {
			onErr(err)
		}
	}()
	return srv, dualListener.Addr(), certSource, nil
}

func init() {
	// MyGo 要求窗口事件循环运行在进程主线程。
	runtime.LockOSThread()
}

func main() {
	if err := run(); err != nil && !errors.Is(err, context.Canceled) {
		fmt.Fprintln(os.Stderr, "osgb-tiles-app:", err)
		showStartupError(err)
		os.Exit(1)
	}
}

func run() error {
	flags := flag.NewFlagSet("osgb-tiles-app", flag.ContinueOnError)
	headless := flags.Bool("headless", false, "只启动 HTTP 服务，不打开桌面窗口")
	browser := flags.Bool("browser", false, "在默认浏览器打开界面；关闭终端或按 Ctrl+C 退出")
	addr := flags.String("addr", "127.0.0.1:0", "HTTP/HTTPS 监听地址（例如 0.0.0.0:42225）")
	httpsAddr := flags.String("https-addr", "", "伴随启动的内置 HTTPS 兼容服务地址（桌面/浏览器模式默认 0.0.0.0:42225，设为 off 禁用）")
	token := flags.String("token", os.Getenv("OSGB_TILES_TOKEN"), "可选访问令牌；留空则不启用鉴权")
	useTLS := flags.Bool("tls", false, "启用内置 HTTPS（未指定证书时自动生成并持久化自签证书）")
	tlsCert := flags.String("tls-cert", "", "HTTPS 证书 PEM 路径（与 --tls-key 配合使用）")
	tlsKey := flags.String("tls-key", "", "HTTPS 私钥 PEM 路径（与 --tls-cert 配合使用）")
	dataDir := flags.String("data-dir", "", "数据目录，保存任务索引与日志")
	sidecar := flags.String("sidecar", "", "转换器路径；默认在本程序旁或 PATH 中查找 osgb-tiles")
	maxParallel := flags.Int("max-parallel", 1, "同时运行的转换数")
	showVersion := flags.Bool("version", false, "输出版本后退出")
	doctor := flags.Bool("doctor", false, "检查运行环境与转换器后退出")
	if err := flags.Parse(os.Args[1:]); err != nil {
		return err
	}
	if *headless && *browser {
		return errors.New("--headless 与 --browser 不能同时使用")
	}
	currentVersion := appVersion()
	if *showVersion {
		fmt.Printf("osgb-tiles-app %s (%s/%s)\n", currentVersion, runtime.GOOS, runtime.GOARCH)
		return nil
	}

	converter := &runner.Exec{Binary: *sidecar}
	features := converter.Features()
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
		return fmt.Errorf("%s（请解压完整分发包并保留 bin 目录；--doctor 可查看检查结果）", features.Detail)
	}
	if runtime.GOOS == "windows" && !*headless && !*browser && !mygo.WebViewAvailable() {
		return errors.New("未检测到 Microsoft WebView2 Runtime。\n\n可运行本目录的 Start Browser.cmd，无需安装 WebView2。\n如需桌面窗口，可从 https://go.microsoft.com/fwlink/p/?LinkId=2124703 下载微软安装器，以普通用户身份运行；无需选择“以管理员身份运行”。企业策略可能限制安装")
	}

	if *dataDir == "" {
		def, err := api.DefaultDataDir()
		if err != nil {
			return err
		}
		*dataDir = def
	}
	manager, err := jobs.New(jobs.Config{DataDir: *dataDir, MaxParallel: *maxParallel, Runner: converter})
	if err != nil {
		return err
	}
	defer manager.Close()

	ctx, cancel := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer cancel()

	var picker *desktop.Picker
	var checkUpdates func()
	var updaterStatus func() api.UpdaterInfo
	if !*headless && !*browser {
		picker = &desktop.Picker{}
		updateSource := newGitHubTokenSource(updateGitHubRepo, strings.TrimSpace(updateGitHubToken))
		mygo.Updater.SetSource(updateSource)
		var updaterReady atomic.Bool
		mygo.App.WhenReady(func() { updaterReady.Store(true) })
		mygo.Use(updater.Plugin)
		if runtime.GOOS == "darwin" {
			mygo.App.SetMenu(mygo.NewMenu([]*mygo.MenuItem{
				{Label: "OSGB 转 3D Tiles", Submenu: []*mygo.MenuItem{
					{Role: mygo.RoleAbout},
					updater.MenuItem(),
					mygo.Separator(),
					{Role: mygo.RoleQuit},
				}},
				{Role: mygo.RoleEditMenu},
				{Role: mygo.RoleWindowMenu},
			}))
		}
		checkUpdates = func() {
			if !updaterReady.Load() {
				return
			}
			updater.CheckForUpdates()
		}
		updaterStatus = func() api.UpdaterInfo {
			available := ""
			if updaterReady.Load() {
				available = updater.AvailableVersion()
			}
			return api.UpdaterInfo{
				Enabled:          mygo.Updater.Enabled(),
				AvailableVersion: available,
				TokenConfigured:  strings.TrimSpace(updateGitHubToken) != "",
				ActiveProxy:      updateSource.ActiveProxy(),
				ChinaMirror:      updateSource.ChinaMirrorActive(),
			}
		}
	}
	enableTLS := *useTLS || *tlsCert != "" || *tlsKey != ""
	compatHTTPSAddr := resolveCompatHTTPSAddr(*httpsAddr, *addr, *headless, enableTLS)
	activeHTTPSAddr := compatHTTPSAddr
	if activeHTTPSAddr == "" && enableTLS {
		activeHTTPSAddr = *addr
	}
	options := api.Options{
		Manager:       manager,
		Runner:        converter,
		Web:           web.Files,
		Version:       currentVersion,
		Token:         *token,
		DataDir:       *dataDir,
		MaxParallel:   *maxParallel,
		Desktop:       picker != nil,
		HTTPSAddr:     activeHTTPSAddr,
		CheckUpdates:  checkUpdates,
		UpdaterStatus: updaterStatus,
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
	if enableTLS {
		tlsConfig, certSource, err := api.LoadOrGenerateTLSConfig(*tlsCert, *tlsKey, *dataDir, *addr)
		if err != nil {
			listener.Close()
			return err
		}
		log.Printf("已启用内置 HTTPS（证书：%s）", certSource)
		listener = tls.NewListener(listener, tlsConfig)
	}
	httpServer := &http.Server{Handler: server.Handler(), ReadHeaderTimeout: 10 * time.Second}
	go func() {
		if err := httpServer.Serve(listener); err != nil && !errors.Is(err, http.ErrServerClosed) {
			log.Printf("HTTP 服务结束：%v", err)
			cancel()
		}
	}()

	var compatServer *http.Server
	var shutdownOnce sync.Once
	shutdownResources := func() {
		shutdownOnce.Do(func() {
			cancel()
			if compatServer != nil {
				shutdown, stop := context.WithTimeout(context.Background(), time.Second)
				if err := compatServer.Shutdown(shutdown); err != nil {
					_ = compatServer.Close()
				}
				stop()
			}
			shutdown, stop := context.WithTimeout(context.Background(), time.Second)
			if err := httpServer.Shutdown(shutdown); err != nil {
				_ = httpServer.Close()
			}
			stop()
			manager.Close()
		})
	}
	defer shutdownResources()

	if compatHTTPSAddr != "" {
		srv, compatBound, certSource, err := startHTTPSCompatServer(server.Handler(), compatHTTPSAddr, *token, *tlsCert, *tlsKey, *dataDir, func(err error) {
			log.Printf("内置 HTTPS 切片服务结束：%v", err)
			cancel()
		})
		if err != nil {
			return err
		}
		compatServer = srv
		log.Printf("内置 HTTP/HTTPS 切片兼容服务已监听：http(s)://%s（证书：%s）", compatBound.String(), certSource)
	}
	if picker != nil {
		// 热更新 Relaunch 在 OnQuit 之后重启新进程，必须提前释放数据目录锁与 42225 端口。
		mygo.App.OnQuit(shutdownResources)
	}

	url := pageURL(listener.Addr(), *token, enableTLS)
	log.Printf("界面地址：%s", url)
	if *browser {
		var command *exec.Cmd
		switch runtime.GOOS {
		case "windows":
			command = exec.Command("rundll32.exe", "url.dll,FileProtocolHandler", url)
		case "darwin":
			command = exec.Command("open", url)
		default:
			command = exec.Command("xdg-open", url)
		}
		if err := command.Run(); err != nil {
			return fmt.Errorf("打开默认浏览器失败：%w", err)
		}
		log.Print("浏览器模式已启动；按 Ctrl+C 停止服务。请手动填写本机输入、输出路径")
	}
	if *headless || *browser {
		<-ctx.Done()
		return nil
	}
	return desktop.RunWithPicker(ctx, desktop.Options{URL: url}, picker)
}

func pageURL(addr net.Addr, token string, https bool) string {
	host := "127.0.0.1"
	if tcp, ok := addr.(*net.TCPAddr); ok && tcp.IP != nil && !tcp.IP.IsUnspecified() {
		host = tcp.IP.String()
	}
	scheme := "http"
	if https {
		scheme = "https"
	}
	url := fmt.Sprintf("%s://%s/", scheme, net.JoinHostPort(host, fmt.Sprint(addr.(*net.TCPAddr).Port)))
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
