"""Header, feature and allocation parsing on synthetic images."""

import json
import subprocess

import pytest
from conftest import BIN

from qcow2gen import Qcow2Builder

MiB = 1024 * 1024


def make(tmp_path, builder: Qcow2Builder, name="disk.qcow2"):
    path = tmp_path / name
    path.write_bytes(builder.build())
    return path


def test_basic_v3_header(tmp_path, qcowscope):
    img = make(tmp_path, Qcow2Builder(size=64 * MiB))
    info = qcowscope("info", "--json", img).json()

    assert info["format"] == "qcow2"
    assert info["virtual-size"] == 64 * MiB
    assert info["cluster-size"] == 65536
    assert info["dirty-flag"] is False
    data = info["format-specific"]["data"]
    assert data["compat"] == "1.1"
    assert data["refcount-bits"] == 16
    assert data["lazy-refcounts"] is False
    assert data["extended-l2"] is False
    assert info["allocation"]["total-clusters"] == 1024
    assert info["allocation"]["allocated-clusters"] == 0


def test_version2_header(tmp_path, qcowscope):
    img = make(tmp_path, Qcow2Builder(size=8 * MiB, version=2))
    data = qcowscope("info", "--json", img).json()["format-specific"]["data"]
    assert data["compat"] == "0.10"
    assert "lazy-refcounts" not in data


@pytest.mark.parametrize("cluster_bits", [9, 12, 16, 21])
def test_cluster_sizes(tmp_path, qcowscope, cluster_bits):
    b = Qcow2Builder(size=4 * MiB, cluster_bits=cluster_bits).write(0).write(1)
    info = qcowscope("info", "--json", make(tmp_path, b)).json()
    assert info["cluster-size"] == 1 << cluster_bits
    assert info["allocation"]["allocated-clusters"] == 2


def test_allocation_kinds(tmp_path, qcowscope):
    b = (Qcow2Builder(size=16 * MiB)
         .write(0).write(5).write(200)          # normal
         .write(7, "compressed")
         .write(8, "zero_alloc")                # zero flag, still owns a cluster
         .write(9, "zero_plain"))               # zero flag, no host cluster
    alloc = qcowscope("info", "--json", make(tmp_path, b)).json()["allocation"]
    assert alloc == {"total-clusters": 256, "allocated-clusters": 5,
                     "compressed-clusters": 1, "zero-clusters": 2}


def test_allocation_across_multiple_l2_tables(tmp_path, qcowscope):
    # 512-byte clusters: one L2 table covers 64 clusters, so this needs many.
    b = Qcow2Builder(size=1 * MiB, cluster_bits=9)
    for guest in range(0, 2048, 7):
        b.write(guest)
    alloc = qcowscope("info", "--json", make(tmp_path, b)).json()["allocation"]
    assert alloc["total-clusters"] == 2048
    assert alloc["allocated-clusters"] == len(range(0, 2048, 7))


def test_extended_l2(tmp_path, qcowscope):
    b = Qcow2Builder(size=8 * MiB, extended_l2=True).write(3).write(100)
    info = qcowscope("info", "--json", make(tmp_path, b)).json()
    assert info["format-specific"]["data"]["extended-l2"] is True
    assert info["allocation"]["allocated-clusters"] == 2


def test_feature_flags(tmp_path, qcowscope):
    b = Qcow2Builder(size=MiB, lazy_refcounts=True, dirty=True, corrupt=True)
    info = qcowscope("info", "--json", make(tmp_path, b)).json()
    assert info["dirty-flag"] is True
    assert info["format-specific"]["data"]["lazy-refcounts"] is True
    assert info["format-specific"]["data"]["corrupt"] is True

    human = qcowscope("info", make(tmp_path, b, "h.qcow2")).out
    assert "lazy-refcounts" in human and "DIRTY" in human and "CORRUPT" in human


def test_backing_file_and_format(tmp_path, qcowscope):
    b = Qcow2Builder(size=MiB, backing_file="base.qcow2", backing_format="qcow2")
    info = qcowscope("info", "--json", make(tmp_path, b)).json()
    assert info["backing-filename"] == "base.qcow2"
    assert info["backing-filename-format"] == "qcow2"


def test_json_escapes_strange_backing_names(tmp_path, qcowscope):
    name = 'we"ird\\name\n.qcow2'
    b = Qcow2Builder(size=MiB, backing_file=name)
    info = qcowscope("info", "--json", make(tmp_path, b)).json()   # must stay valid JSON
    assert info["backing-filename"] == name


@pytest.mark.parametrize("raw, expected", [
    ("základ.qcow2".encode(), "základ.qcow2"),           # valid UTF-8 kept
    (b"bad\xffname", "bad\ufffdname"),                   # invalid byte replaced
    (b"\xed\xa0\x80x", "\ufffd\ufffd\ufffdx"),          # UTF-16 surrogate rejected
])
def test_json_is_valid_for_any_backing_name_bytes(tmp_path, qcowscope, raw, expected):
    data = bytearray(Qcow2Builder(size=MiB, backing_file="X" * len(raw)).build())
    offset = int.from_bytes(data[8:16], "big")
    data[offset:offset + len(raw)] = raw
    path = tmp_path / "names.qcow2"
    path.write_bytes(bytes(data))
    proc = subprocess.run([BIN, "info", "--json", str(path)], capture_output=True)
    info = json.loads(proc.stdout.decode("utf-8"))   # strict: must be valid UTF-8
    assert info["backing-filename"] == expected


def test_chain(tmp_path, qcowscope):
    make(tmp_path, Qcow2Builder(size=MiB).write(0).write(1), "base.qcow2")
    make(tmp_path, Qcow2Builder(size=MiB, backing_file="base.qcow2").write(2), "mid.qcow2")
    top = make(tmp_path, Qcow2Builder(size=MiB, backing_file="mid.qcow2"), "top.qcow2")

    res = qcowscope("chain", top)
    assert res.code == 0, res.err
    lines = res.out.splitlines()
    assert [line.split()[0] for line in lines] == [
        str(top), str(tmp_path / "mid.qcow2"), str(tmp_path / "base.qcow2")]
    assert "2/16 clusters" in lines[2]


def test_chain_missing_backing_file(tmp_path, qcowscope):
    top = make(tmp_path, Qcow2Builder(size=MiB, backing_file="gone.qcow2"))
    res = qcowscope("chain", top)
    assert res.code == 1 and "(missing)" in res.out


def test_chain_loop_is_detected(tmp_path, qcowscope):
    make(tmp_path, Qcow2Builder(size=MiB, backing_file="b.qcow2"), "a.qcow2")
    make(tmp_path, Qcow2Builder(size=MiB, backing_file="a.qcow2"), "b.qcow2")
    res = qcowscope("chain", tmp_path / "a.qcow2")
    assert res.code == 1 and "loop detected" in res.out


def test_raw_base_image_ends_chain(tmp_path, qcowscope):
    (tmp_path / "base.raw").write_bytes(b"\0" * 4096)
    top = make(tmp_path, Qcow2Builder(size=MiB, backing_file="base.raw",
                                      backing_format="raw"))
    res = qcowscope("chain", top)
    assert res.code == 0 and "bad magic" in res.out


def test_usage_errors(qcowscope):
    assert qcowscope().code == 2
    assert qcowscope("explode", "x").code == 2
    assert qcowscope("--version").out.startswith("qcowscope ")
