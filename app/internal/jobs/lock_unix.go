//go:build unix

package jobs

import (
	"errors"
	"os"
	"path/filepath"
	"syscall"
)

// acquireLock 独占数据目录，防止两个进程各自持有不一致的任务状态。
// 文件锁随进程退出自动释放，不会留下阻止下次启动的残留锁。
func acquireLock(dir string) (func(), error) {
	path := filepath.Join(dir, ".lock")
	file, err := os.OpenFile(path, os.O_CREATE|os.O_RDWR, 0o600)
	if err != nil {
		return nil, err
	}
	if err := syscall.Flock(int(file.Fd()), syscall.LOCK_EX|syscall.LOCK_NB); err != nil {
		file.Close()
		return nil, errors.New("数据目录已被另一个 osgb-tiles-app 实例占用；请先关闭该实例，或为本次运行指定其他 --data-dir")
	}
	return func() {
		syscall.Flock(int(file.Fd()), syscall.LOCK_UN)
		file.Close()
	}, nil
}
