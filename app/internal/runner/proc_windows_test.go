//go:build windows

package runner

import (
	"os/exec"
	"testing"

	"golang.org/x/sys/windows"
)

func TestConfigureCommandHidesConsoleWindow(t *testing.T) {
	configureCommand(nil)

	cmd := exec.Command("osgb-tiles.exe", "--capabilities")
	configureCommand(cmd)
	if cmd.SysProcAttr == nil {
		t.Fatal("SysProcAttr should not be nil")
	}
	if !cmd.SysProcAttr.HideWindow {
		t.Fatal("HideWindow should be true")
	}
	if cmd.SysProcAttr.CreationFlags&windows.CREATE_NO_WINDOW == 0 {
		t.Fatalf("CreationFlags = %#x, want CREATE_NO_WINDOW (%#x)", cmd.SysProcAttr.CreationFlags, windows.CREATE_NO_WINDOW)
	}
}
