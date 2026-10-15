"""Snapshot table parsing - the feature implemented in src/snapshot.c.

These tests skip themselves until qcow2_read_snapshots() is implemented,
then they run automatically. See docs/snapshot-table.md.
"""

import struct

import pytest

from qcow2gen import Qcow2Builder, Snapshot

MiB = 1024 * 1024
GiB = 1024 * MiB


def snapshots_of(tmp_path, qcowscope, builder, name="snap.qcow2"):
    path = tmp_path / name
    path.write_bytes(builder.build())
    res = qcowscope("info", "--json", path)
    assert res.code == 0, res.err
    info = res.json()
    if "snapshots" not in info:
        pytest.skip("snapshot listing not implemented yet - see docs/snapshot-table.md")
    return info["snapshots"]


def test_two_snapshots(tmp_path, qcowscope):
    b = Qcow2Builder(size=64 * MiB, snapshots=[
        Snapshot("1", "before-upgrade", date_sec=1_760_000_000, date_nsec=123,
                 vm_clock_nsec=5_500_000_000, vm_state_size=0),
        Snapshot("2", "after-upgrade", date_sec=1_760_000_100, vm_state_size=8 * MiB,
                 icount=42),
    ])
    s1, s2 = snapshots_of(tmp_path, qcowscope, b)

    assert s1 == {"id": "1", "name": "before-upgrade", "vm-state-size": 0,
                  "date-sec": 1_760_000_000, "date-nsec": 123,
                  "vm-clock-sec": 5, "vm-clock-nsec": 500_000_000}
    assert s2["id"] == "2" and s2["name"] == "after-upgrade"
    assert s2["vm-state-size"] == 8 * MiB
    assert s2["icount"] == 42


@pytest.mark.parametrize("name_len", range(1, 10))
def test_entries_are_padded_to_8_bytes(tmp_path, qcowscope, name_len):
    """Entry sizes vary with the name length; each entry starts 8-byte aligned."""
    names = ["n" * name_len, "second", "x" * (name_len + 3)]
    b = Qcow2Builder(size=MiB, snapshots=[Snapshot(str(i + 1), n) for i, n in enumerate(names)])
    assert [s["name"] for s in snapshots_of(tmp_path, qcowscope, b)] == names


def test_large_vm_state_uses_64bit_field(tmp_path, qcowscope):
    """The old 32-bit field only holds the low bits; the extra data has the real size."""
    b = Qcow2Builder(size=MiB, snapshots=[Snapshot("1", "big", vm_state_size=6 * GiB)])
    assert snapshots_of(tmp_path, qcowscope, b)[0]["vm-state-size"] == 6 * GiB


def test_legacy_entry_without_extra_data(tmp_path, qcowscope):
    b = Qcow2Builder(size=MiB, version=2, snapshots=[
        Snapshot("1", "old", vm_state_size=4096, legacy=True)])
    snap = snapshots_of(tmp_path, qcowscope, b)[0]
    assert snap["vm-state-size"] == 4096
    assert "icount" not in snap


def test_human_output_lists_snapshots(tmp_path, qcowscope):
    b = Qcow2Builder(size=MiB, snapshots=[Snapshot("7", "my-snapshot")])
    snapshots_of(tmp_path, qcowscope, b)            # skips if not implemented
    path = tmp_path / "human.qcow2"
    path.write_bytes(b.build())
    out = qcowscope("info", path).out
    assert "[7] my-snapshot" in out


# --- malformed snapshot tables must be rejected, never crash ----------------

def _image_with_snapshot(tmp_path, qcowscope):
    b = Qcow2Builder(size=MiB, snapshots=[Snapshot("1", "s")])
    snapshots_of(tmp_path, qcowscope, b)            # skips if not implemented
    return bytearray(b.build())


def _expect_error(tmp_path, qcowscope, data):
    path = tmp_path / "bad-snap.qcow2"
    path.write_bytes(bytes(data))
    res = qcowscope("info", path)
    assert res.code == 1, res.out
    assert res.err.strip(), "an error message is expected"


def test_snapshot_table_beyond_end_of_file(tmp_path, qcowscope):
    data = _image_with_snapshot(tmp_path, qcowscope)
    struct.pack_into(">Q", data, 64, 1 << 40)          # snapshots_offset
    _expect_error(tmp_path, qcowscope, data)


def test_snapshot_name_runs_past_end_of_file(tmp_path, qcowscope):
    data = _image_with_snapshot(tmp_path, qcowscope)
    table = struct.unpack_from(">Q", data, 64)[0]
    struct.pack_into(">H", data, table + 14, 0xFFFF)   # name_size
    del data[table + 4096:]                            # truncate the file
    _expect_error(tmp_path, qcowscope, data)


def test_more_snapshots_than_entries(tmp_path, qcowscope):
    data = _image_with_snapshot(tmp_path, qcowscope)
    struct.pack_into(">I", data, 60, 5000)             # nb_snapshots
    table = struct.unpack_from(">Q", data, 64)[0]
    del data[table + 65536:]
    _expect_error(tmp_path, qcowscope, data)
