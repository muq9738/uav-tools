#!/usr/bin/env python3
"""在 Windows 上从完整目录及个人安装目录启动随包程序，检查 native 与分发资源。"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

for _stream in (sys.stdout, sys.stderr):
    if hasattr(_stream, 'reconfigure'):
        _stream.reconfigure(encoding='utf-8', errors='replace')


def verify(package):
    if os.name != 'nt':
        raise RuntimeError('此检查必须在 Windows 执行')
    module_path = Path(__file__).resolve().parents[1] / 'scripts' / 'build.py'
    spec = importlib.util.spec_from_file_location('osgb_build', module_path)
    build = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(build)
    package = package.resolve()
    config = json.loads((module_path.parents[1] / 'mygo.json').read_text(encoding='utf-8'))
    app_name = config['name'] + '.exe'
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(('OSG_', 'PROJ_', 'OSGB_TILES_BIN'))}
    # 编译机的 vcpkg / MSVC PATH 不能掩盖缺少随包 DLL 的问题。
    env['PATH'] = str(Path(os.environ['SystemRoot']) / 'System32')

    def check(folder):
        build.verify_resources(folder, expected_platform='windows-amd64')
        for name in [app_name, 'osgb-tiles-browser.exe', 'Start Browser.cmd', 'README-Windows.txt']:
            assert (folder / name).is_file(), ('缺少启动资源', folder / name)
        result = subprocess.check_output([str(folder / app_name), '--doctor'], env=env, timeout=30)
        assert '可用：true' in result.decode('utf-8'), result
        capabilities = json.loads(subprocess.check_output(
            [str(folder / 'bin/osgb-tiles.exe'), '--capabilities'], env=env, timeout=30))
        assert capabilities['protocolVersion'] == 1, capabilities

    check(package)
    installers = list(package.glob('*-setup-*.exe'))
    assert len(installers) == 1, ('需要一个可选个人安装包', installers)
    with tempfile.TemporaryDirectory(prefix='osgb-user-install-') as directory:
        installed = Path(directory) / 'OSGB User App'
        # NSIS 的 /D= 必须位于最后，包含空格时也不能给路径加引号。
        command = subprocess.list2cmdline([str(installers[0]), '/S']) + ' /D=' + str(installed)
        subprocess.run(command, check=True, timeout=120)
        check(installed)
        # NSIS 的 _?= 必须位于最后且不加引号，确保卸载器同步执行完毕再退出，避免与临时目录清理竞态。
        uninstall_cmd = subprocess.list2cmdline([str(installed / 'Uninstall.exe'), '/S']) + ' _?=' + str(installed)
        subprocess.run(uninstall_cmd, check=True, timeout=60)
    print('Windows 完整便携目录、可选个人安装及卸载检查通过；未验证交互式桌面画面或企业权限策略')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--package', type=Path, required=True)
    verify(parser.parse_args().package)
