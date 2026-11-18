#!/usr/bin/env python3
"""真实 App/native：处理中暂停、重启、续转、资产一致性及暂停后取消清理。"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import time
import urllib.request


def hashes(directory):
    return {p.relative_to(directory).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in directory.rglob('*') if p.is_file() and p.name != 'conversion-report.json'}


def verify(app, native, source, baseline, work):
    work.mkdir(parents=True, exist_ok=False)
    log = (work / 'app.log').open('w+', encoding='utf-8')
    process = None
    base = None
    transitions = []
    def start():
        nonlocal process, base
        log.seek(0, 2)
        beginning = log.tell()
        process = subprocess.Popen([str(app), '--headless', '--addr', '127.0.0.1:0', '--data-dir', str(work / 'state'), '--sidecar', str(native)],
                                   stdout=log, stderr=subprocess.STDOUT, start_new_session=os.name != 'nt')
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            assert process.poll() is None, 'App 提前退出'
            log.seek(beginning)
            match = re.search(r'http://127\.0\.0\.1:\d+/', log.read())
            if match:
                base = match.group(0).rstrip('/')
                return
            time.sleep(0.05)
        raise AssertionError('未取得 App 服务地址')
    def stop():
        nonlocal process
        if process is None:
            return
        process.terminate()
        try:
            process.wait(timeout=15)
        except subprocess.TimeoutExpired:
            if os.name != 'nt':
                os.killpg(process.pid, signal.SIGKILL)
            else:
                process.kill()
            process.wait(timeout=5)
        process = None
    def request(route, body=None):
        payload = None if body is None else json.dumps(body).encode()
        req = urllib.request.Request(base + route, data=payload, headers={'Content-Type': 'application/json'})
        with urllib.request.urlopen(req, timeout=15) as response:
            return json.load(response)
    def wait(job_id, predicate, label, timeout=300):
        deadline = time.monotonic() + timeout
        last = None
        while time.monotonic() < deadline:
            job = request('/api/jobs/' + job_id)
            current = (job['state'], job['progress']['stage'], job['progress']['done'])
            if current != last:
                transitions.append({'label': label, 'state': job['state'], 'progress': job['progress']})
                (work / 'progress.json').write_text(json.dumps(transitions, ensure_ascii=False, indent=2) + '\n')
                last = current
            if predicate(job):
                return job
            assert job['state'] not in {'failed', 'canceled', 'succeeded'}, (label, job)
            time.sleep(0.1)
        raise AssertionError('等待超时: ' + label)
    def pause_while_running(output):
        job = request('/api/jobs', {'input': str(source), 'output': str(output), 'threads': 2, 'geometry': 'meshopt', 'memoryBudgetMiB': 256})
        job = wait(job['id'], lambda j: j['state'] == 'running' and j['progress']['done'] >= 2, 'actual-conversion')
        request('/api/jobs/' + job['id'] + '/pause', {})
        job = wait(job['id'], lambda j: j['state'] == 'paused', 'paused')
        assert job['resumeRequired'] and job['report']['status'] == 'paused'
        assert job['report']['filesCompleted'] >= 2 and not output.exists()
        checkpoint = Path(job['checkpointDir'])
        assert checkpoint.is_dir() and not (checkpoint / 'work/tileset.json').exists()
        return job
    try:
        start()
        assert request('/api/config')['sidecar']['pauseResume'] is True
        output = work / 'resumed-output'
        paused = pause_while_running(output)
        checkpoint = Path(paused['checkpointDir'])
        held = {p.relative_to(checkpoint / 'work').as_posix(): (p.stat().st_mtime_ns, hashlib.sha256(p.read_bytes()).hexdigest())
                for p in (checkpoint / 'work').rglob('*') if p.is_file()}
        assert held, '暂停前没有保存真实资产'
        stop()
        assert checkpoint.is_dir(), '退出 App 不应删除已确认的暂停检查点'
        start()
        restored = request('/api/jobs/' + paused['id'])
        assert restored['state'] == 'paused' and restored['resumeRequired']
        request('/api/jobs/' + paused['id'] + '/resume', {})
        resumed = wait(paused['id'], lambda j: j['state'] == 'succeeded', 'resumed-complete')
        assert resumed['report']['filesReused'] >= 2
        assert resumed['report']['files'] == 1006 and resumed['report']['glbs'] == 1005
        assert not checkpoint.exists(), '完成后残留检查点'
        actual, expected = hashes(output), hashes(baseline)
        assert actual == expected, '续转后资产与完整转换不同'
        for relative, (mtime, digest) in held.items():
            p = output / relative
            assert hashlib.sha256(p.read_bytes()).hexdigest() == digest
            assert p.stat().st_mtime_ns == mtime, '持久化资产被重新生成: ' + relative
        canceled = pause_while_running(work / 'cancel-output')
        canceled_checkpoint = Path(canceled['checkpointDir'])
        request('/api/jobs/' + canceled['id'] + '/cancel', {})
        final = request('/api/jobs/' + canceled['id'])
        assert final['state'] == 'canceled'
        assert not canceled_checkpoint.exists(), '取消已暂停任务后残留检查点'
        result = {'status': 'passed', 'files': 1006, 'glbs': 1005, 'assetFilesCompared': len(actual), 'assetHashesExact': True,
                  'filesReused': resumed['report']['filesReused'], 'persistedAssetsNotReencoded': len(held),
                  'restartPreservedPause': True, 'pausedCancelCleaned': True, 'scope': '真实App和native，完整模型；不代表画面或资源性能已达标'}
        (work / 'result.json').write_text(json.dumps(result, ensure_ascii=False, indent=2) + '\n')
        print(json.dumps(result, ensure_ascii=False, indent=2), flush=True)
    finally:
        stop()
        log.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for option in ['app', 'native', 'input', 'baseline', 'output']:
        parser.add_argument('--' + option, type=Path, required=True)
    args = parser.parse_args()
    try:
        verify(args.app.resolve(), args.native.resolve(), args.input.resolve(), args.baseline.resolve(), args.output.resolve())
    except Exception as error:
        args.output.mkdir(parents=True, exist_ok=True)
        (args.output / 'failure.json').write_text(json.dumps({'status': 'failed', 'error': str(error)}, ensure_ascii=False, indent=2) + '\n')
        raise
