//go:build windows

package jobs

import (
	"errors"
	"os"
	"path/filepath"

	"golang.org/x/sys/windows"
)

// acquireLock 用 Windows 文件锁独占数据目录；进程退出时锁自动释放。
func acquireLock(dir string) (func(), error) {
	path := filepath.Join(dir, ".lock")
	file, err := os.OpenFile(path, os.O_CREATE|os.O_RDWR, 0o600)
	if err != nil {
		return nil, err
	}
	var overlapped windows.Overlapped
	err = windows.LockFileEx(windows.Handle(file.Fd()), windows.LOCKFILE_EXCLUSIVE_LOCK|windows.LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &overlapped)
	if err != nil {
		file.Close()
		return nil, errors.New("数据目录已被另一个 osgb-tiles-app 实例占用；请先关闭该实例，或为本次运行指定其他 --data-dir")
	}
	return func() {
		windows.UnlockFileEx(windows.Handle(file.Fd()), 0, 1, 0, &overlapped)
		file.Close()
	}, nil
}
