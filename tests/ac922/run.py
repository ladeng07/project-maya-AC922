"""Run the AC922 CPU-only byte/copy/fallback regressions; no model or GPU required.

Linux: python3 tests/ac922/run.py
Uses temporary build/fixture files and the current repository's production headers.
"""
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    root = Path(__file__).resolve().parents[2]
    with tempfile.TemporaryDirectory(prefix="maya-ac922-tests-") as tmp:
        build = Path(tmp)
        for name in ("test_slice", "test_queue_previous", "test_reader"):
            cmd = [os.environ.get("CXX", "c++"), "-O2", "-std=c++20", "-pthread",
                   "-I" + str(root / "src/ds4"), str(root / "tests/ac922" / (name + ".cpp"))]
            if name == "test_reader":
                cmd.append("-Wl,--wrap=open,--wrap=pread,--wrap=close")
            cmd.extend(["-o", str(build / name)])
            subprocess.run(cmd, check=True)
            args = [str(build / name)]
            if name == "test_reader":
                args.append(str(build / "reader-fixture.bin"))
            subprocess.run(args, check=True, timeout=180)
            print(name + ": PASS", flush=True)


if __name__ == "__main__":
    main()
