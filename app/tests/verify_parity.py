#!/usr/bin/env python3
"""真实验证 App 与随包 CLI 生成相同 Tiles，并验证进度和取消清理。"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import time
import urllib.request


def verify(package, source, evidence, srs=None, origin=None, geometry='meshopt', texture_format='original', draco_speed=5, rebuild_top=False):
    evidence.mkdir(parents=True, exist_ok=True)
    suffix = '.exe' if os.name == 'nt' else ''
    cli = package / 'bin' / ('osgb-tiles' + suffix)
    app = package / ('osgb-tiles-app' + suffix)
    if not app.exists():
        import plistlib
        info = plistlib.loads((package / 'Contents/Info.plist').read_bytes())
        app = package / 'Contents/MacOS' / info['CFBundleExecutable']
        cli = package / 'Contents/Resources/bin' / ('osgb-tiles' + suffix)
    assert cli.is_file() and app.is_file(), (cli, app)
    env = {k: v for k, v in os.environ.items() if not k.startswith(('OSG_', 'PROJ_', 'DYLD_', 'LD_LIBRARY_PATH', 'OSGB_TILES_BIN'))}
    if os.name != 'nt':
        env['PATH'] = '/usr/bin:/bin'
    capabilities = json.loads(subprocess.check_output([str(cli), '--capabilities'], env=env))
    assert capabilities['defaults']['geometry'] == 'meshopt'
    with tempfile.TemporaryDirectory(prefix='parity-', dir=evidence) as directory:
        work = Path(directory)
        cli_out, app_out = work / 'cli', work / 'app'
        command = [str(cli), str(source), str(cli_out), '--threads', '2', '--memory-budget', '256', '--geometry', geometry,
                   '--texture-format', texture_format, '--draco-speed', str(draco_speed)]
        if rebuild_top:
            command += ['--rebuild-top']
        if capabilities.get('pauseResume'):
            command += ['--checkpoint-dir', str(work / 'cli-checkpoint'), '--pause-file', str(work / 'cli-pause')]
        if srs:
            command += ['--srs', srs]
        if origin:
            command += ['--origin', origin]
        direct = json.loads(subprocess.check_output(command, env=env, timeout=180))
        log_path = evidence / 'app.log'
        with log_path.open('w+', encoding='utf-8') as log:
            process = subprocess.Popen([str(app), '--headless', '--addr', '127.0.0.1:0', '--data-dir', str(work / 'state')], stdout=log, stderr=subprocess.STDOUT, env=env)
            try:
                deadline = time.monotonic() + 15
                base = None
                while time.monotonic() < deadline:
                    assert process.poll() is None, 'App 提前退出'
                    log.seek(0)
                    match = re.search(r'http://127\.0\.0\.1:\d+/', log.read())
                    if match:
                        base = match.group(0).rstrip('/')
                        break
                    time.sleep(0.1)
                assert base, '没有取得本机地址'
                def request(path, body=None):
                    req = urllib.request.Request(base + path, data=None if body is None else json.dumps(body).encode(), headers={'Content-Type': 'application/json'})
                    with urllib.request.urlopen(req, timeout=10) as response:
                        return json.load(response)
                config = request('/api/config')
                assert config['sidecar']['available'], config
                assert Path(config['sidecar']['path']).resolve() == cli.resolve(), config
                spec = {'input': str(source), 'output': str(app_out), 'memoryBudgetMiB': 256,
                        'geometry': geometry, 'textureFormat': texture_format, 'dracoSpeed': draco_speed, 'rebuildTop': rebuild_top}
                if srs:
                    spec['srs'] = srs
                if origin:
                    spec['origin'] = origin
                created = request('/api/jobs', spec)
                samples = []
                deadline = time.monotonic() + 180
                while time.monotonic() < deadline:
                    job = request('/api/jobs/' + created['id'])
                    samples.append({'state': job['state'], 'progress': job['progress']})
                    if job['state'] in {'succeeded', 'failed', 'canceled'}:
                        break
                    time.sleep(0.1)
                assert job['state'] == 'succeeded', job
                report = job['report']
                expected_encoding = {'meshopt': 'EXT_meshopt_compression', 'draco': 'KHR_draco_mesh_compression', 'raw': 'raw'}[geometry]
                assert report['geometryEncoding'] == direct['geometryEncoding'] == expected_encoding
                assert report['textureFormat'] == direct['textureFormat'] == texture_format
                assert report['rebuildTop'] == direct['rebuildTop'] == rebuild_top
                assert report['dracoSpeed'] == direct['dracoSpeed'] == draco_speed
                assert report['files'] == direct['files'] and report['glbs'] == direct['glbs']
                def hashes(folder):
                    return {str(p.relative_to(folder)): hashlib.sha256(p.read_bytes()).hexdigest() for p in folder.rglob('*') if p.is_file() and p.name != 'conversion-report.json'}
                a, b = hashes(cli_out), hashes(app_out)
                assert a == b, 'App / CLI 的主入口、GLB、纹理或诊断清单不同'
                left = json.loads((cli_out / 'conversion-report.json').read_text())
                right = json.loads((app_out / 'conversion-report.json').read_text())
                for key in ['elapsedSeconds', 'peakResidentBytes', 'memoryThrottled', 'checkpointDir']:
                    left.pop(key, None)
                    right.pop(key, None)
                assert left == right, '除时间与RSS采样外，报告不一致'
                progressed = any(s['state'] == 'running' and (s['progress']['done'] > 0 or s['progress'].get('stage') == 'rebuild-top') and s['progress']['source'] == 'converter' for s in samples)
                if direct['elapsedSeconds'] > 1:
                    assert progressed, '转换中没有真实进度'
                # 大模型验证取消处于转换中时清理事务；超小 fixture 可能已完成，不强造时序。
                cancel_verified = False
                if direct['elapsedSeconds'] > 1:
                    canceled_output = work / 'canceled'
                    cancel_job = request('/api/jobs', dict(spec, output=str(canceled_output)))
                    deadline = time.monotonic() + 180
                    while time.monotonic() < deadline:
                        current = request('/api/jobs/' + cancel_job['id'])
                        if current['progress']['done'] > 0 or current['progress'].get('stage') == 'rebuild-top':
                            break
                        time.sleep(0.05)
                    assert current['state'] == 'running' and (current['progress']['done'] > 0 or current['progress'].get('stage') == 'rebuild-top'), current
                    request('/api/jobs/' + cancel_job['id'] + '/cancel', {})
                    deadline = time.monotonic() + 8
                    while time.monotonic() < deadline:
                        canceled = request('/api/jobs/' + cancel_job['id'])
                        if canceled['state'] in {'canceled', 'failed', 'succeeded'}:
                            break
                        time.sleep(0.1)
                    assert canceled['state'] == 'canceled', ('native 未合作取消', canceled)
                    assert not canceled_output.exists() and not list(work.glob('.canceled.building-*')), '取消残留暂存'
                    if canceled.get('checkpointDir'):
                        assert not Path(canceled['checkpointDir']).exists(), '取消残留检查点'
                    cancel_verified = True
                result = {'status': 'passed', 'platform': capabilities, 'input': str(source), 'files': direct['files'], 'glbs': direct['glbs'], 'assetFilesCompared': len(a), 'assetHashesExact': True, 'reportsExactExceptTimingAndMemorySampling': True, 'liveNativeProgress': progressed, 'cooperativeCancelCleaned': cancel_verified, 'hostEnvironmentRemoved': True, 'samples': samples}
                (evidence / 'parity.json').write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
                print(json.dumps({k:v for k,v in result.items() if k not in {'samples', 'platform'}}, ensure_ascii=False, indent=2))
            finally:
                process.terminate()
                process.wait(timeout=10)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--package', type=Path, required=True)
    parser.add_argument('--input', type=Path, required=True)
    parser.add_argument('--evidence', type=Path, required=True)
    parser.add_argument('--srs')
    parser.add_argument('--origin')
    parser.add_argument('--geometry', choices=['raw', 'meshopt', 'draco'], default='meshopt')
    parser.add_argument('--texture-format', choices=['original', 'uastc', 'etc1s'], default='original')
    parser.add_argument('--draco-speed', type=int, choices=[0, 5, 10], default=5)
    parser.add_argument('--rebuild-top', action='store_true')
    args = parser.parse_args()
    verify(args.package.resolve(), args.input.resolve(), args.evidence.resolve(), args.srs, args.origin,
           args.geometry, args.texture_format, args.draco_speed, args.rebuild_top)
