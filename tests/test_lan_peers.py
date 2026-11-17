"""Compile real LAN overrides and exercise owned loopback sockets, without a game or external network.

Why: VPN discovery must preserve opt-in behavior, reject public destinations and fan out only broadcasts.
Usage: uv run pytest tests/test_lan_peers.py; Windows supports Clang with either the Microsoft CRT or MinGW.
"""

from pathlib import Path
import os
import shutil
import subprocess

import pytest


def test_lan_peer_sockets(tmp_path: Path) -> None:
    cc = shutil.which("clang")
    if not cc:
        pytest.skip("no Clang (required by the runtime CPU headers)")
    root = Path(__file__).resolve().parents[1]
    exe = tmp_path / ("lan-peers.exe" if os.name == "nt" else "lan-peers")
    includes = [
        f"-I{root / 'runtime' / part}"
        for part in (
            "",
            "cpu",
            "core/base",
            "sdk/ov",
            "gfx/nvn/host",
            "gfx/nvn/api",
            "gfx/nvn/procs",
        )
    ]
    libraries = ["-lws2_32"] if os.name == "nt" else []
    compilation = subprocess.run(
        [cc, "-O1", *includes, str(root / "tests/c/t_lan_peers.c"), *libraries, "-o", str(exe)],
        timeout=60,
        capture_output=True,
        text=True,
    )
    assert compilation.returncode == 0, compilation.stdout + compilation.stderr
    result = subprocess.run([str(exe)], check=True, timeout=10, capture_output=True, text=True)
    assert "public-address rejection passed" in result.stdout
