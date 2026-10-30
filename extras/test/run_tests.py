#!/usr/bin/env python3
"""Native tests of the parts of the library that need no board.

test_main.cpp       HydroNodeBatteryGuard and the X-Device-Config format, plus the threshold
                    rules against vectors/threshold-rules-vectors.json, the same table the backend,
                    web, firmware and station test against (copy of
                    hydronode-backend/src/test/resources/devicesettings/threshold-rules-vectors.json).
test_buffer.cpp     HydroNodeBuffer: storing, reading, full memory, persistence, dating rounds.
test_protocol.cpp   The wire contract: value formatting, canonical payloads, HMAC-SHA256 (portable
                    implementation). Random input is checked against Python's hmac/hashlib and
                    against the backend's formatting rule (Java "%.2f": half up on the exact float).
"""

from __future__ import annotations

import base64
import hashlib
import hmac
import json
import os
import random
import struct
import subprocess
from decimal import ROUND_HALF_UP, Decimal
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
SRC = HERE.parents[1] / "src"
VECTORS = HERE / "vectors" / "threshold-rules-vectors.json"


def build(out: Path, sources: list[str]) -> None:
    compiler = os.environ.get("CXX", "c++")
    subprocess.run(
        [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-DHYDRONODE_PORTABLE_CRYPTO", f"-I{SRC}"]
        + sources
        + ["-o", str(out)],
        check=True,
    )


def java_format(value: float) -> str:
    """What the backend writes: String.format(Locale.ROOT, "%.2f", (double) floatValue)."""
    text = str(Decimal(value).quantize(Decimal("0.01"), rounding=ROUND_HALF_UP))
    if text.startswith("-") or (value == 0 and str(value).startswith("-")):
        return "-" + text.lstrip("-")
    return text


def float32(bits: int) -> float:
    return struct.unpack("<f", struct.pack("<I", bits))[0]


def run_lines(binary: Path, mode: str, lines: list[str]) -> list[str]:
    result = subprocess.run(
        [str(binary), mode], input="\n".join(lines) + "\n", capture_output=True, text=True, check=True
    )
    return result.stdout.splitlines()


def check_protocol(binary: Path) -> int:
    rng = random.Random(20261030)
    failed = 0

    # Values: typical sensor ranges, exact binary ties (x.125, x.375 ...) and random bit patterns.
    floats = [21.5, 21.125, -21.125, 0.0, -0.0, 0.005, 1013.25, 99999.99, -40.0, 3039.0]
    floats += [rng.uniform(-50, 150) for _ in range(5000)]
    floats += [rng.randint(-100000, 100000) / 8 for _ in range(2000)]
    floats += [rng.uniform(-1e7, 1e7) for _ in range(2000)]
    bits = [struct.unpack("<I", struct.pack("<f", f))[0] for f in floats]
    bits += [b for b in (rng.getrandbits(32) for _ in range(3000)) if abs(float32(b)) < 1e12 and float32(b) == float32(b)]
    got = run_lines(binary, "format", [f"{b:08x}" for b in bits])
    for b, text in zip(bits, got):
        expected = java_format(float32(b))
        if text != expected:
            failed += 1
            if failed < 10:
                print(f"format {float32(b)!r}: expected {expected}, got {text}")
    print(f"{len(bits)} formatted values, {failed} failed")

    # Signatures: random keys (also longer than a SHA-256 block) and messages.
    cases = []
    for _ in range(500):
        key = bytes(rng.getrandbits(8) for _ in range(rng.choice([1, 16, 32, 44, 64, 65, 131])))
        message = bytes(rng.getrandbits(8) for _ in range(rng.randint(0, 1500)))
        cases.append((key, message))
    got = run_lines(binary, "sign", [f"{k.hex()} {m.hex() or '00'}" for k, m in cases])
    sign_failed = 0
    for (key, message), text in zip(cases, got):
        expected = base64.b64encode(hmac.new(key, message or b"\0", hashlib.sha256).digest()).decode()
        if text != expected:
            sign_failed += 1
    print(f"{len(cases)} signatures, {sign_failed} failed")

    # One-value payload text, as SensorHmacVerifier.canonicalValuePayload builds it.
    rows = []
    for _ in range(300):
        value = struct.unpack("<f", struct.pack("<f", rng.uniform(-100, 2000)))[0]
        rows.append(("550e8400-e29b-41d4-a716-446655440000", rng.choice(["TEMPERATURE", "CO2", "PM25"]), value,
                     rng.randint(1_600_000_000, 2_000_000_000)))
    got = run_lines(binary, "payload", [f"{s} {t} {struct.unpack('<I', struct.pack('<f', v))[0]:08x} {ts}" for s, t, v, ts in rows])
    payload_failed = 0
    for (sid, typ, value, ts), text in zip(rows, got):
        expected = f'{{"sensorId":"{sid}","type":"{typ}","value":{java_format(value)},"timestamp":{ts}}}'
        if text != expected:
            payload_failed += 1
    print(f"{len(rows)} payloads, {payload_failed} failed")
    return failed + sign_failed + payload_failed


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
    for case, line in zip(cases, got):
        actual = [f for f in line.split(",") if f]
        if actual != sorted(set(case["expected"])):
            failed += 1
            print(f'vector "{case["name"]}": expected {case["expected"]}, got {actual}')
    print(f"{len(cases)} threshold vectors, {failed} failed")
    return failed


def main() -> int:
    with tempfile.TemporaryDirectory() as tmp:
        guard = Path(tmp) / "test_main"
        build(guard, [str(HERE / "test_main.cpp"), str(SRC / "HydroNodeBatteryGuard.cpp"),
                      str(SRC / "HydroNodeDeviceConfig.cpp")])
        protocol = Path(tmp) / "test_protocol"
        build(protocol, [str(HERE / "test_protocol.cpp"), str(SRC / "HydroNodeProtocol.cpp"),
                         str(SRC / "HydroNodeCrypto.cpp")])
        buffer = Path(tmp) / "test_buffer"
        build(buffer, [str(HERE / "test_buffer.cpp"), str(SRC / "HydroNodeBuffer.cpp"),
                       str(SRC / "HydroNodeProtocol.cpp")])
        results = [subprocess.run([str(binary)]).returncode for binary in (guard, protocol, buffer)]
        failed = check_vectors(guard) + check_protocol(protocol)
    return 0 if not any(results) and failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
