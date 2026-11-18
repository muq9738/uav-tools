// Package desktop 用 MyGo 打开桌面窗口，页面由同一个本机 HTTP 服务提供。
//
// 这样桌面端与浏览器使用一份界面、一份接口；桌面端额外提供系统目录选择器。
package desktop

import (
	"context"
	"errors"
	"strings"
	"sync"

	"github.com/egoist/mygo"
)

// Options 配置桌面窗口。
type Options struct {
	URL   string
	Title string
}

// Picker 通过系统对话框选择目录；窗口未就绪时返回可读错误。
type Picker struct {
	mu     sync.Mutex
	window *mygo.Window
}

// Pick 打开系统目录选择器，用户取消时返回空路径。
func (p *Picker) Pick(ctx context.Context) (string, error) {
	if err := ctx.Err(); err != nil {
		return "", err
	}
	p.mu.Lock()
	window := p.window
	p.mu.Unlock()
	if window == nil {
		return "", errors.New("窗口尚未就绪，请稍后重试")
	}
	paths, err := mygo.Dialog.Open(mygo.OpenDialogOptions{
		Parent:      window,
		Title:       "选择目录",
		ButtonLabel: "选择",
		Directory:   true,
	})
	if err != nil {
		return "", err
	}
	if len(paths) == 0 {
		return "", nil
	}
	return paths[0], nil
}

// Run 打开窗口并运行 MyGo 事件循环，直到窗口关闭或 ctx 结束。
func Run(ctx context.Context, options Options) error {
	if ctx == nil {
		return errors.New("desktop: 需要 context")
	}
	if options.URL == "" {
		return errors.New("desktop: 需要页面地址")
	}
	if options.Title == "" {
		options.Title = "OSGB 转 3D Tiles"
	}
	return RunWithPicker(ctx, options, &Picker{})
}

// RunWithPicker 打开窗口，并在就绪后把窗口交给 picker。
func RunWithPicker(ctx context.Context, options Options, picker *Picker) error {
	finished := make(chan struct{})
	var once sync.Once
	mygo.App.WhenReady(func() {
		window := mygo.NewWindow(mygo.WindowOptions{
			Title:     options.Title,
			URL:       options.URL,
			Width:     1360,
			Height:    880,
			MinWidth:  980,
			MinHeight: 640,
		})
		if picker != nil {
			picker.mu.Lock()
			picker.window = window
			picker.mu.Unlock()
		}
		window.OnClosed(func() { once.Do(func() { close(finished) }) })
	})
	err := mygo.App.Run()
	switch {
	case err == nil:
	case strings.Contains(err.Error(), "App.Run called twice"):
		// 事件循环已由其他组件持有：改为等待本窗口关闭。
	default:
		return err
	}
	select {
	case <-finished:
	case <-ctx.Done():
	}
	return ctx.Err()
}
