#!/usr/bin/env python3
"""Real OSGB, real CLI processes: a paused run must resume without rewriting assets."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile

for _stream in (sys.stdout, sys.stderr):
    if hasattr(_stream, "reconfigure"):
        _stream.reconfigure(encoding="utf-8", errors="replace")

native, fixture = map(str, map(Path, sys.argv[1:3]))
root = Path(tempfile.mkdtemp(prefix="checkpoint-cli-", dir=Path.cwd()))
subprocess.run([fixture, "--fixture", str(root)], check=True, timeout=30)
source = root / "source"

def call(output, *args):
    return subprocess.run([native, str(source), str(output), "--threads", "1", "--json-events", *map(str, args)],
                          text=True, encoding="utf-8", capture_output=True, timeout=60)

def events(process):
    lines = [json.loads(line) for line in process.stdout.splitlines() if line.strip()]
    assert all(event["type"] in {"progress", "log", "result"} for event in lines), lines
    return lines

def file_hashes(directory):
    return {str(file.relative_to(directory)): hashlib.sha256(file.read_bytes()).hexdigest()
            for file in directory.rglob("*") if file.is_file() and file.name != "conversion-report.json"}

cap = json.loads(subprocess.check_output([native, "--capabilities"], text=True, encoding="utf-8", timeout=10))
assert cap["protocolVersion"] == 1 and cap["pauseResume"] is True
assert cap["defaults"] == {"threads": 2, "geometry": "meshopt", "memoryBudgetMiB": 0, "texturePixels": False, "rebuildTop": False, "preview": False, "topSealDistance": 2.0, "textureFormat": "original", "dracoSpeed": 5}
baseline = call(root / "baseline")
assert baseline.returncode == 0, baseline.stderr
assert events(baseline)[-1]["report"]["status"] == "complete"
expected = file_hashes(root / "baseline")
checkpoint, output, pause = root / "checkpoint", root / "output", root / "pause"

def stop_in_progress(mode):
    token = pause if mode == "pause" else root / "cancel"
    command = [native, str(source), str(output), "--threads", "1", "--json-events",
               "--checkpoint-dir", str(checkpoint), f"--{mode}-file", str(token)]
    received = []
    with (root / f"{mode}.stderr").open("w", encoding="utf-8") as error:
        process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=error, text=True, encoding="utf-8", bufsize=1)
        try:
            for line in process.stdout:
                event = json.loads(line)
                received.append(event)
                if event.get("stage") == "convert" and event["done"] >= 1:
                    token.touch()
            code = process.wait(timeout=60)
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
    (root / f"{mode}.jsonl").write_text("\n".join(map(json.dumps, received)) + "\n", encoding="utf-8")
    assert token.exists(), "No actual converted-file progress observed"
    return code, received

code, paused = stop_in_progress("pause")
assert code == 75, (code, paused, (root / "pause.stderr").read_text(encoding="utf-8"))
assert paused[-1]["type"] == "result" and paused[-1]["report"]["status"] == "paused", paused
count = paused[-1]["report"]["filesCompleted"]
assert 1 <= count < 6, ("Fixture must pause before finishing all file instances", count)
assert Path(paused[-1]["report"]["checkpointDir"]) == checkpoint
assert not output.exists() and not (checkpoint / "work" / "tileset.json").exists()
held = {file.relative_to(checkpoint / "work"): (file.stat().st_mtime_ns, file.read_bytes())
        for file in (checkpoint / "work").rglob("*") if file.is_file()}
assert held
pause.unlink()
resumed = call(output, "--checkpoint-dir", checkpoint, "--pause-file", pause, "--resume")
assert resumed.returncode == 0, resumed.stderr
report = events(resumed)[-1]["report"]
assert report["status"] == "complete" and report["filesReused"] == count and report["filesCompleted"] == 6, report
assert not checkpoint.exists()
assert file_hashes(output) == expected, "Assets differ after process restart"
assert json.loads((output / "conversion-report.json").read_text(encoding="utf-8"))["records"] == json.loads((root / "baseline/conversion-report.json").read_text(encoding="utf-8"))["records"]
for path, (mtime, content) in held.items():
    assert (output / path).read_bytes() == content
    assert (output / path).stat().st_mtime_ns == mtime, f"Re-encoded persisted asset: {path}"

checkpoint, output = root / "cancel-checkpoint", root / "cancel-output"
code, canceled = stop_in_progress("cancel")
assert code != 0 and code != 75 and not checkpoint.exists() and not output.exists(), (code, canceled)
assert not any(event.get("report", {}).get("status") in {"complete", "paused"} for event in canceled)
for args in [("--resume",), ("--pause-file", pause)]:
    invalid = call(root / "invalid", *args)
    assert invalid.returncode == 1 and not (root / "invalid").exists()
print(f"CLI 通过：处理中暂停退出 75，跨进程复用 {count}/6 文件实例，{len(expected)} 个资产逐字节相同；取消清理，参数边界通过。证据: {root}")
