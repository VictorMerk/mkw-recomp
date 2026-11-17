"""Build the production capture-memory path with synthetic pages; no game or GPU required.

Each capture must start independently, including pages unchanged since an earlier capture and read-trapped pages.
Usage: uv run pytest tests/runtime/test_capture_snapshot.py
"""

import os
from pathlib import Path
import shutil
import subprocess

import pytest


def test_independent_capture_snapshots(tmp_path: Path) -> None:
    cc = shutil.which("clang")
    if not cc:
        pytest.skip("no Clang (required by the runtime CPU headers)")
    root = Path(__file__).resolve().parents[2]
    exe = tmp_path / ("capture-snapshot.exe" if os.name == "nt" else "capture-snapshot")
    includes = [
        f"-I{root / 'runtime' / part}"
        for part in ("", "cpu", "core/base", "core/boot", "gfx/cap", "gfx/nvn/host", "gfx/nvn/api", "gfx/nvn/procs", "platform")
    ]
    compilation = subprocess.run(
        [cc, "-std=gnu11", "-O1", "-Wall", "-Wextra", *includes, str(Path(__file__).with_name("t_capture_snapshot.c")), "-o", str(exe)],
        timeout=60, capture_output=True, text=True,
    )
    assert compilation.returncode == 0, compilation.stdout + compilation.stderr
    result = subprocess.run([str(exe)], timeout=10, capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
    assert "unchanged deltas stay empty" in result.stdout
