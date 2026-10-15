"""Malformed images must be rejected cleanly: an error message, never a crash.

Disk image parsers are a classic source of security bugs (QEMU has had
several CVEs in image format drivers), so every offset read from the file
is treated as untrusted.
"""

import json
import random
import struct
import subprocess

import pytest

from conftest import BIN
from qcow2gen import Qcow2Builder

MiB = 1024 * 1024


def patch(data: bytes, offset: int, fmt: str, value) -> bytes:
    b = bytearray(data)
    struct.pack_into(fmt, b, offset, value)
    return bytes(b)


def check_rejected(tmp_path, qcowscope, data: bytes, message: str):
    path = tmp_path / "bad.qcow2"
    path.write_bytes(data)
    res = qcowscope("info", path)
    assert res.code == 1, res.out
    assert message in res.err


GOOD = Qcow2Builder(size=4 * MiB).write(0).build()


@pytest.mark.parametrize("offset, fmt, value, message", [
    (0, ">I", 0x12345678, "bad magic"),
    (4, ">I", 4, "unsupported qcow2 version"),
    (20, ">I", 8, "invalid cluster_bits"),
    (20, ">I", 40, "invalid cluster_bits"),
    (36, ">I", 0xFFFFFFFF, "L1 table too large"),
    (36, ">I", 0, "L1 table too small"),
    (40, ">Q", 12345, "not cluster aligned"),
    (60, ">I", 100000, "too many snapshots"),
    (72, ">Q", 1 << 40, "unknown incompatible features"),
    (96, ">I", 7, "invalid refcount_order"),
    (100, ">I", 50, "invalid header_length"),
    (104, ">B", 9, "unknown compression type"),
])
def test_bad_header_fields(tmp_path, qcowscope, offset, fmt, value, message):
    check_rejected(tmp_path, qcowscope, patch(GOOD, offset, fmt, value), message)


def test_l1_table_beyond_end_of_file(tmp_path, qcowscope):
    check_rejected(tmp_path, qcowscope, patch(GOOD, 40, ">Q", 1 << 40),
                   "beyond end of file")


def test_l2_offset_beyond_end_of_file(tmp_path, qcowscope):
    l1_offset = struct.unpack_from(">Q", GOOD, 40)[0]
    check_rejected(tmp_path, qcowscope, patch(GOOD, l1_offset, ">Q", 1 << 40),
                   "beyond end of file")


def test_unaligned_l2_offset(tmp_path, qcowscope):
    l1_offset = struct.unpack_from(">Q", GOOD, 40)[0]
    check_rejected(tmp_path, qcowscope, patch(GOOD, l1_offset, ">Q", 3 * 512),
                   "not cluster aligned")


def test_header_extension_too_large(tmp_path, qcowscope):
    # First extension starts right after the 112-byte v3 header.
    check_rejected(tmp_path, qcowscope, patch(GOOD, 116, ">I", 0x7FFFFFFF),
                   "too large")


@pytest.mark.parametrize("size", [0, 10, 71, 100])
def test_truncated_files(tmp_path, qcowscope, size):
    path = tmp_path / "short.qcow2"
    path.write_bytes(GOOD[:size])
    assert qcowscope("info", path).code == 1


def test_missing_file(tmp_path, qcowscope):
    res = qcowscope("info", tmp_path / "nope.qcow2")
    assert res.code == 1 and "cannot open" in res.err


def test_random_corruption_never_crashes(tmp_path, qcowscope):
    """Small fuzz test: flip random bytes in the metadata and run the tool.

    A negative return code means the process was killed by a signal
    (segfault, abort from a sanitizer, ...), which is always a bug.
    """
    rng = random.Random(1234)
    builder = Qcow2Builder(size=4 * MiB, backing_file="b.qcow2", backing_format="qcow2")
    base = builder.write(0).write(9).write(40, "compressed").build()
    cs = builder.cluster_size
    l1 = struct.unpack_from(">Q", base, 40)[0]
    l2 = struct.unpack_from(">Q", base, l1)[0] & 0x00FFFFFFFFFFFE00
    # Bytes that matter: header + extensions + backing name, L1 entries, L2 entries.
    targets = [(0, 256), (l1, l1 + 16), (l2, l2 + 400)]
    path = tmp_path / "fuzz.qcow2"
    outcomes = {0: 0, 1: 0}
    for i in range(500):
        data = bytearray(base)
        for _ in range(rng.randint(1, 4)):
            start, end = rng.choice(targets)
            data[rng.randrange(start, end)] = rng.choice([0, 0xFF, 0x80, rng.randrange(256)])
        path.write_bytes(bytes(data))
        proc = subprocess.run([BIN, "info", "--json", str(path)], capture_output=True,
                              timeout=30)
        assert proc.returncode in (0, 1), f"iteration {i}: exit {proc.returncode}\n{proc.stderr}"
        if proc.returncode == 0:
            json.loads(proc.stdout.decode("utf-8"))   # output must be valid JSON
        outcomes[proc.returncode] += 1
    # Make sure the fuzzer exercises both the accept and the reject paths.
    assert outcomes[0] > 50 and outcomes[1] > 50, outcomes
    assert cs == 65536
