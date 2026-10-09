#!/usr/bin/env python3
"""Native tests of HydroNodeBatteryGuard and the X-Device-Config format.

Builds extras/test/test_main.cpp with the host compiler, runs the built-in cases, then checks
the threshold rules against vectors/threshold-rules-vectors.json, the same table the backend,
web, firmware and station test against (copy of
hydronode-backend/src/test/resources/devicesettings/threshold-rules-vectors.json).
"""

from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
SRC = HERE.parents[1] / "src"
VECTORS = HERE / "vectors" / "threshold-rules-vectors.json"


def build(out: Path) -> None:
    compiler = os.environ.get("CXX", "c++")
    subprocess.run(
        [
            compiler,
            "-std=c++17",
            "-Wall",
            "-Wextra",
            "-Werror",
            f"-I{SRC}",
            str(HERE / "test_main.cpp"),
            str(SRC / "HydroNodeBatteryGuard.cpp"),
            str(SRC / "HydroNodeDeviceConfig.cpp"),
            "-o",
            str(out),
        ],
        check=True,
    )


def check_vectors(binary: Path) -> int:
    cases = json.loads(VECTORS.read_text())["cases"]
    lines = [
        f'{c["chemistry"] or "null"} {c["cells"]} {c["save"]} {c["recovery"]} {c["standby"]} {c["resume"]}'
        for c in cases
    ]
    result = subprocess.run(
        [str(binary), "vectors"], input="\n".join(lines) + "\n", capture_output=True, text=True, check=True
    )
    got = result.stdout.splitlines()
    failed = 0
    for case, line in zip(cases, got, strict=True):
        actual = [f for f in line.split(",") if f]
        if actual != sorted(set(case["expected"])):
            failed += 1
            print(f'vector "{case["name"]}": expected {case["expected"]}, got {actual}')
    print(f"{len(cases)} threshold vectors, {failed} failed")
    return failed


def main() -> int:
    with tempfile.TemporaryDirectory() as tmp:
        binary = Path(tmp) / "test_main"
        build(binary)
        cases = subprocess.run([str(binary)])
        failed = check_vectors(binary)
    return 0 if cases.returncode == 0 and failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
