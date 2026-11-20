#!/usr/bin/env python3
"""在 Windows 上从完整目录及个人安装目录启动随包程序，检查 native 与分发资源。"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import shutil
import ssl
import subprocess
import sys
import tempfile
import time
import urllib.request

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
        browser_result = subprocess.check_output([str(folder / 'osgb-tiles-browser.exe'), '--doctor'], env=env, timeout=30)
        assert '可用：true' in browser_result.decode('utf-8'), browser_result
        exe_bytes = (folder / 'bin/osgb-tiles.exe').read_bytes()
        assert b'activeCodePage' in exe_bytes and b'UTF-8' in exe_bytes, '缺少 UTF-8 activeCodePage manifest'
        capabilities = json.loads(subprocess.check_output(
            [str(folder / 'bin/osgb-tiles.exe'), '--capabilities'], env=env, timeout=30))
        assert capabilities['protocolVersion'] == 1, capabilities

    check(package)
    installers = list(package.glob('*-setup-*.exe'))
    assert len(installers) == 1, ('需要一个可选个人安装包', installers)
    with tempfile.TemporaryDirectory(prefix='osgb-user-install-') as directory:
        installed = Path(directory) / 'OSGB 转 3D Tiles 不乱码'
        # NSIS 的 /D= 必须位于最后，包含空格时也不能给路径加引号。
        command = subprocess.list2cmdline([str(installers[0]), '/S']) + ' /D=' + str(installed)
        subprocess.run(command, check=True, timeout=120)
        check(installed)
        sample = next((module_path.parents[1] / 'build').glob('*/test-data-*/texture+alpha.osgb'), None)
        if sample is not None:
            cn_in_dir = Path(directory) / '测试输入目录'
            cn_in_dir.mkdir(parents=True, exist_ok=True)
            cn_in = cn_in_dir / '测试模型.osgb'
            shutil.copy2(sample, cn_in)
            cn_out = Path(directory) / '测试输出目录'
            subprocess.run(
                [str(installed / 'bin/osgb-tiles.exe'), str(cn_in), str(cn_out), '--srs', 'ENU:31.2,121.4,10'],
                env=env, timeout=60, check=True,
            )
            assert (cn_out / 'tileset.json').is_file(), ('中文路径转换未生成 tileset.json', cn_out)

            # 验证已安装的 GUI exe 在未传 --tls 时也可自动监听内置 HTTPS 兼容端口（/task/create）。
            https_state = Path(directory) / 'https-state'
            https_out = Path(directory) / '测试HTTPS输出'
            https_log = Path(directory) / 'https-service.log'
            with https_log.open('w+', encoding='utf-8') as log_stream:
                process = subprocess.Popen(
                    [str(installed / app_name), '--headless', '--addr', '127.0.0.1:0',
                     '--https-addr', '127.0.0.1:42225', '--data-dir', str(https_state)],
                    stdout=log_stream, stderr=subprocess.STDOUT, env=env,
                )
                try:
                    ctx = ssl._create_unverified_context()
                    deadline = time.monotonic() + 20
                    while time.monotonic() < deadline:
                        assert process.poll() is None, ('HTTPS 兼容服务提前退出', https_log.read_text(encoding='utf-8', errors='replace'))
                        try:
                            with urllib.request.urlopen('https://127.0.0.1:42225/api/health', context=ctx, timeout=2) as resp:
                                if resp.status == 200:
                                    break
                        except Exception:
                            time.sleep(0.2)
                    else:
                        raise AssertionError('等待 HTTPS /api/health 超时')

                    body = json.dumps({
                        'options': {
                            'name': 'windows-https-smoke',
                            'inputPath': str(cn_in),
                            'outputPath': str(https_out),
                            'settings': {'srs': 'ENU:31.2,121.4,10'},
                        }
                    }).encode('utf-8')
                    req = urllib.request.Request(
                        'https://127.0.0.1:42225/task/create',
                        data=body,
                        headers={'Content-Type': 'application/json'},
                        method='POST',
                    )
                    with urllib.request.urlopen(req, context=ctx, timeout=10) as resp:
                        assert resp.status == 201, resp.status
                    deadline = time.monotonic() + 60
                    while time.monotonic() < deadline:
                        if (https_out / 'tileset.json').is_file():
                            break
                        time.sleep(0.2)
                    assert (https_out / 'tileset.json').is_file(), ('HTTPS /task/create 未生成 tileset.json', https_out)
                finally:
                    process.terminate()
                    try:
                        process.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=5)
        # NSIS 的 _?= 必须位于最后且不加引号，确保卸载器同步执行完毕再退出，避免与临时目录清理竞态。
        uninstall_cmd = subprocess.list2cmdline([str(installed / 'Uninstall.exe'), '/S']) + ' _?=' + str(installed)
        subprocess.run(uninstall_cmd, check=True, timeout=60)
    if os.environ.get('MYGO_UPDATER_PRIVATE_KEY'):
        assert (package / 'update-windows-amd64.json').is_file(), '缺少 Windows 自动更新清单 update-windows-amd64.json'
        assert len(list(package.glob('*-windows-amd64.tar.gz'))) == 1, '缺少签名更新压缩包'
    print('Windows 完整便携目录、可选个人安装及卸载检查通过；未验证交互式桌面画面或企业权限策略')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--package', type=Path, required=True)
    verify(parser.parse_args().package)
