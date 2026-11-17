"""Exercise Windows memory collection against an owned process, without the game or a GPU workload.

Run on Windows with Python directly or pytest; other hosts skip these native process tests.
"""

import csv
import io
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import time
import unittest


@unittest.skipUnless(sys.platform == "win32", "native Windows memory counters")
class MemoryWatchTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.out = Path(self.temp.name) / "spaces and ü" / "memory.tsv"
        self.script = Path(__file__).resolve().parents[1] / "tools/run/memwatch.ps1"
        self.shell = shutil.which("powershell")
        if not self.shell:
            self.skipTest("Windows PowerShell is unavailable")
        # Commit a new OS allocation after the observer's baseline. Python/CRT allocators may reuse memory already
        # committed at interpreter startup, so bytearray() does not guarantee an increase in process-private bytes.
        self.allocate_signal = Path(self.temp.name) / "allocate.request"
        self.exit_signal = Path(self.temp.name) / "exit.request"
        code = (
            "import ctypes,os,sys,time\n"
            "kernel=ctypes.WinDLL('kernel32',use_last_error=True)\n"
            "kernel.VirtualAlloc.argtypes=[ctypes.c_void_p,ctypes.c_size_t,ctypes.c_ulong,ctypes.c_ulong]\n"
            "kernel.VirtualAlloc.restype=ctypes.c_void_p\n"
            "print(os.getpid(),flush=True)\n"
            "while not os.path.exists(sys.argv[1]):time.sleep(0.01)\n"
            "data=kernel.VirtualAlloc(None,64*1024*1024,0x3000,0x04)\n"
            "if not data:raise ctypes.WinError(ctypes.get_last_error())\n"
            "ctypes.memset(data,1,64*1024*1024)\n"
            "print('ALLOCATED',flush=True)\n"
            "while not os.path.exists(sys.argv[2]):time.sleep(0.01)\n"
        )
        self.child = subprocess.Popen(
            [sys.executable, "-u", "-c", code, str(self.allocate_signal), str(self.exit_signal)],
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            text=True,
            creationflags=subprocess.CREATE_NO_WINDOW,
        )
        self.addCleanup(self.stop_child)
        # Windows venv python.exe can be a redirector with a different PID. Observe the interpreter that actually
        # owns the allocation, and let it exit cooperatively so cleanup also works through that redirector.
        self.target_pid = int(self.child.stdout.readline().strip())

    def stop_child(self):
        self.allocate_signal.touch()
        self.exit_signal.touch()
        try:
            self.child.communicate(timeout=10)
        except subprocess.TimeoutExpired:
            self.stop(self.child)

    @staticmethod
    def stop(process):
        if process.poll() is None:
            process.kill()
        process.communicate(timeout=10)

    def rows(self):
        if not self.out.exists():
            return []
        lines = [line for line in self.out.read_text(encoding="utf-8").splitlines() if not line.startswith("#")]
        return list(csv.DictReader(io.StringIO("\n".join(lines)), delimiter="\t"))

    def wait_rows(self, count, watcher):
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline:
            if watcher.poll() is not None:
                _, stderr = watcher.communicate(timeout=5)
                self.fail(f"observer exited before sampling: {stderr}")
            if len(self.rows()) >= count:
                return
            time.sleep(0.05)
        self.fail("memory samples did not arrive")

    def command(self, *extra):
        return [
            self.shell,
            "-NoProfile",
            "-ExecutionPolicy",
            "Bypass",
            "-File",
            str(self.script),
            "-TargetProcessId",
            str(self.target_pid),
            "-OutPath",
            str(self.out),
            "-IntervalSeconds",
            "0.25",
            "-NoGpuCounters",
            *extra,
        ]

    def watcher(self, *extra):
        process = subprocess.Popen(
            self.command(*extra),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            errors="replace",
            creationflags=subprocess.CREATE_NO_WINDOW,
        )
        self.addCleanup(self.stop, process)
        return process

    def test_measures_allocation_and_preserves_an_existing_log(self):
        watcher = self.watcher("-Samples", "12")
        self.wait_rows(2, watcher)
        self.allocate_signal.touch()
        self.assertEqual(self.child.stdout.readline().strip(), "ALLOCATED")
        _, stderr = watcher.communicate(timeout=20)
        self.assertEqual(watcher.returncode, 0, stderr)
        rows = self.rows()
        self.assertEqual(len(rows), 12)
        private = [int(row["private_bytes"]) for row in rows]
        self.assertTrue(all(value > 0 for value in private), private)
        self.assertGreater(max(private) - min(private[:2]), 48 * 1024 * 1024, private)
        self.assertTrue(all(row["gpu_dedicated_bytes"] == "" for row in rows))
        ticks = [int(row["qpc_ticks"]) for row in rows]
        self.assertEqual(ticks, sorted(ticks))
        self.assertIsNone(self.child.poll(), "the observer must leave the target running")
        before = self.out.read_bytes()
        retry = subprocess.run(
            self.command("-Samples", "1"),
            capture_output=True,
            text=True,
            timeout=15,
            errors="replace",
            creationflags=subprocess.CREATE_NO_WINDOW,
        )
        self.assertNotEqual(retry.returncode, 0)
        self.assertEqual(self.out.read_bytes(), before)

    def test_exits_when_the_observed_process_exits(self):
        watcher = self.watcher()
        self.wait_rows(2, watcher)
        self.stop_child()
        _, stderr = watcher.communicate(timeout=10)
        self.assertEqual(watcher.returncode, 0, stderr)
        self.assertGreaterEqual(len(self.rows()), 2)


if __name__ == "__main__":
    unittest.main()
