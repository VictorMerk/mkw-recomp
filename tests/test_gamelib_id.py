"""Check line-ending-only compatibility and the production library guard without game files or a GPU.

Run: uv run pytest tests/test_gamelib_id.py
"""

import hashlib
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools/build"))
import build  # noqa: E402


def test_library_ids_normalize_checkout_but_track_header_and_flag_changes(tmp_path, monkeypatch):
    runtime = tmp_path / "runtime"
    (runtime / "cpu").mkdir(parents=True)
    (runtime / "core/boot").mkdir(parents=True)
    headers = [runtime / "core/boot/gamelib.h", runtime / "cpu/cpu.h"]
    monkeypatch.setattr(build, "RUNTIME", runtime)
    for header in headers:
        header.write_bytes(b"int value;\n")
    canonical, legacy = build.game_id(), build.game_id(legacy_crlf=True)
    expected = hashlib.sha256(" ".join(build.GEN_FLAGS).encode())
    for _ in headers:
        expected.update(b"int value;\r\n")
    assert legacy == expected.hexdigest()[:16] and legacy != canonical
    for header in headers:
        header.write_bytes(b"int value;\r\n")
    assert build.game_id() == canonical
    assert build.game_id(legacy_crlf=True) == legacy
    headers[0].write_bytes(b"long value;\r\n")
    assert build.game_id() != canonical
    assert build.game_id(legacy_crlf=True) != legacy
    headers[0].write_bytes(b"int value;\r\n")
    monkeypatch.setattr(build, "GEN_FLAGS", [*build.GEN_FLAGS, "-DCHANGED_ABI=1"])
    assert build.game_id() != canonical
    assert build.game_id(legacy_crlf=True) != legacy


def test_actual_library_guard(tmp_path):
    cc = shutil.which("clang") or shutil.which("cc")
    if not cc:
        pytest.skip("no C compiler")
    exe = tmp_path / "gamelib-id"
    inc = [f"-I{ROOT / p}" for p in ("runtime", "runtime/cpu", "runtime/core/base", "runtime/core/boot")]
    subprocess.run([cc, *inc, '-DRT_GAME_ID="canonical"', '-DRT_GAME_ID_LEGACY_CRLF="legacy"',
                    str(ROOT / "tests/c/t_gamelib_id.c"), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
