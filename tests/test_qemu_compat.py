"""Compare qcowscope with real QEMU tools on images created by QEMU itself.

Skipped when qemu-img is not installed. Install it with:
    sudo apt install qemu-utils
"""

import json
import subprocess

import pytest

from conftest import requires_qemu

pytestmark = requires_qemu

MiB = 1024 * 1024


def sh(*cmd: str) -> str:
    return subprocess.run(cmd, check=True, capture_output=True, text=True).stdout


def qemu_info(path) -> dict:
    return json.loads(sh("qemu-img", "info", "--output=json", str(path)))


def qemu_check(path) -> dict:
    proc = subprocess.run(["qemu-img", "check", "--output=json", str(path)],
                          capture_output=True, text=True)
    return json.loads(proc.stdout)


def create(path, size="64M", options=""):
    cmd = ["qemu-img", "create", "-q", "-f", "qcow2"]
    if options:
        cmd += ["-o", options]
    sh(*cmd, str(path), size)


def write(path, *commands):
    args = ["qemu-io", "-f", "qcow2"]
    for c in commands:
        args += ["-c", c]
    sh(*args, str(path))


IMAGE_VARIANTS = {
    "default": "",
    "compat-0.10": "compat=0.10",
    "small-clusters": "cluster_size=4096",
    "big-clusters": "cluster_size=2M",
    "lazy-refcounts": "lazy_refcounts=on",
    "refcount-bits-64": "refcount_bits=64",
    "extended-l2": "extended_l2=on,cluster_size=128k",
}


@pytest.fixture(params=list(IMAGE_VARIANTS), ids=list(IMAGE_VARIANTS))
def qemu_image(request, tmp_path):
    path = tmp_path / f"{request.param}.qcow2"
    create(path, "64M", IMAGE_VARIANTS[request.param])
    write(path, "write -P 0xab 0 3M", "write -P 0xcd 10M 64k",
          "write -z 20M 1M", "write -P 0x11 63M 1M")
    return path


def test_info_matches_qemu_img_info(qemu_image, qcowscope):
    ours = qcowscope("info", "--json", qemu_image).json()
    theirs = qemu_info(qemu_image)

    for key in ("virtual-size", "cluster-size", "dirty-flag"):
        if key in theirs:
            assert ours[key] == theirs[key], key
    our_data = ours["format-specific"]["data"]
    their_data = theirs["format-specific"]["data"]
    for key in ("compat", "refcount-bits", "lazy-refcounts", "corrupt",
                "extended-l2", "compression-type"):
        if key in their_data:
            assert our_data[key] == their_data[key], key


def test_allocation_matches_qemu_img_check(qemu_image, qcowscope):
    ours = qcowscope("info", "--json", qemu_image).json()["allocation"]
    theirs = qemu_check(qemu_image)
    assert ours["total-clusters"] == theirs["total-clusters"]
    assert ours["allocated-clusters"] == theirs["allocated-clusters"]


def test_compressed_image_matches_qemu(tmp_path, qcowscope):
    src = tmp_path / "src.qcow2"
    create(src, "16M")
    write(src, "write -P 0x41 0 4M", "write -P 0x42 8M 2M")
    comp = tmp_path / "compressed.qcow2"
    sh("qemu-img", "convert", "-c", "-O", "qcow2", str(src), str(comp))

    ours = qcowscope("info", "--json", comp).json()["allocation"]
    theirs = qemu_check(comp)
    assert ours["allocated-clusters"] == theirs["allocated-clusters"]
    assert ours["compressed-clusters"] > 0
    if "compressed-clusters" in theirs:
        assert ours["compressed-clusters"] == theirs["compressed-clusters"]


def test_zstd_compression_type(tmp_path, qcowscope):
    path = tmp_path / "zstd.qcow2"
    try:
        create(path, "8M", "compression_type=zstd")
    except subprocess.CalledProcessError:
        pytest.skip("this QEMU build has no zstd support")
    ours = qcowscope("info", "--json", path).json()
    assert ours["format-specific"]["data"]["compression-type"] == "zstd"
    assert qemu_info(path)["format-specific"]["data"]["compression-type"] == "zstd"


def test_backing_chain_matches_qemu(tmp_path, qcowscope):
    base, mid, top = (tmp_path / n for n in ("base.qcow2", "mid.qcow2", "top.qcow2"))
    create(base, "32M")
    write(base, "write -P 1 0 4M")
    sh("qemu-img", "create", "-q", "-f", "qcow2", "-b", "base.qcow2", "-F", "qcow2",
       str(mid))
    write(mid, "write -P 2 4M 1M")
    sh("qemu-img", "create", "-q", "-f", "qcow2", "-b", "mid.qcow2", "-F", "qcow2",
       str(top))

    ours = qcowscope("info", "--json", top).json()
    theirs = qemu_info(top)
    assert ours["backing-filename"] == theirs["backing-filename"] == "mid.qcow2"
    assert ours["backing-filename-format"] == theirs["backing-filename-format"]

    chain = qcowscope("chain", top)
    assert chain.code == 0, chain.err
    assert [line.split()[0] for line in chain.out.splitlines()] == [
        str(top), str(mid), str(base)]


def test_snapshots_match_qemu(tmp_path, qcowscope):
    path = tmp_path / "snaps.qcow2"
    create(path, "16M")
    write(path, "write -P 1 0 1M")
    sh("qemu-img", "snapshot", "-c", "first", str(path))
    write(path, "write -P 2 0 1M")
    sh("qemu-img", "snapshot", "-c", "second", str(path))

    ours = qcowscope("info", "--json", path).json()
    if "snapshots" not in ours:
        pytest.skip("snapshot listing not implemented yet - see docs/snapshot-table.md")
    theirs = qemu_info(path)["snapshots"]
    assert len(ours["snapshots"]) == len(theirs) == 2
    for mine, qemu in zip(ours["snapshots"], theirs):
        for key in ("id", "name", "vm-state-size", "date-sec", "date-nsec",
                    "vm-clock-sec", "vm-clock-nsec", "icount"):
            if key in qemu:
                assert mine[key] == qemu[key], key
