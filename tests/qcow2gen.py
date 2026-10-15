"""Build small synthetic qcow2 images for unit tests.

The images follow the qcow2 on-disk layout closely enough for a read-only
parser (header, header extensions, backing file name, L1/L2 tables, data
clusters and the snapshot table). Refcounts are not maintained, so QEMU
itself would report errors on these files. The tests in test_qemu_compat.py
use real QEMU-created images instead.
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field

MAGIC = 0x514649FB
EXT_END = 0x00000000
EXT_BACKING_FMT = 0xE2792ACA
EXT_FEATURE_NAME = 0x6803F857

OFLAG_COPIED = 1 << 63
OFLAG_COMPRESSED = 1 << 62
OFLAG_ZERO = 1


@dataclass
class Snapshot:
    id: str
    name: str
    date_sec: int = 1_700_000_000
    date_nsec: int = 0
    vm_clock_nsec: int = 0
    vm_state_size: int = 0
    disk_size: int | None = None   # written as extra data when set
    icount: int | None = None      # written as extra data when set
    legacy: bool = False           # True: no extra data (old version 2 style)


@dataclass
class Qcow2Builder:
    size: int
    cluster_bits: int = 16
    version: int = 3
    backing_file: str | None = None
    backing_format: str | None = None
    lazy_refcounts: bool = False
    dirty: bool = False
    corrupt: bool = False
    extended_l2: bool = False
    refcount_order: int = 4
    header_length: int = 112
    # guest cluster index -> "normal" | "compressed" | "zero_alloc" | "zero_plain"
    clusters: dict[int, str] = field(default_factory=dict)
    snapshots: list[Snapshot] = field(default_factory=list)

    @property
    def cluster_size(self) -> int:
        return 1 << self.cluster_bits

    @property
    def l2_entry_size(self) -> int:
        return 16 if self.extended_l2 else 8

    def write(self, guest_cluster: int, kind: str = "normal") -> Qcow2Builder:
        self.clusters[guest_cluster] = kind
        return self

    def build(self) -> bytes:
        cs = self.cluster_size
        l2_entries = cs // self.l2_entry_size
        total = -(-self.size // cs)
        l1_size = max(1, -(-total // l2_entries)) if self.size else 0
        l1_clusters = max(1, -(-(l1_size * 8) // cs))

        # Layout: [0] header, [1] refcount table, [2..] L1, then L2 + data.
        next_cluster = 2 + l1_clusters
        l1_offset = 2 * cs
        l2_tables: dict[int, int] = {}
        image = bytearray(next_cluster * cs)

        def alloc() -> int:
            nonlocal next_cluster
            off = next_cluster * cs
            next_cluster += 1
            image.extend(b"\0" * cs)
            return off

        l1 = [0] * l1_size
        for guest, kind in sorted(self.clusters.items()):
            l1_idx, l2_idx = divmod(guest, l2_entries)
            if l1_idx not in l2_tables:
                l2_tables[l1_idx] = alloc()
                l1[l1_idx] = l2_tables[l1_idx] | OFLAG_COPIED
            if kind == "zero_plain":
                entry = OFLAG_ZERO
            else:
                data = alloc()
                image[data:data + 4] = b"DATA"
                if kind == "normal":
                    entry = data | OFLAG_COPIED
                elif kind == "zero_alloc":
                    entry = data | OFLAG_COPIED | OFLAG_ZERO
                elif kind == "compressed":
                    entry = OFLAG_COMPRESSED | data   # sector count bits = 0
                else:
                    raise ValueError(kind)
            pos = l2_tables[l1_idx] + l2_idx * self.l2_entry_size
            image[pos:pos + 8] = struct.pack(">Q", entry)

        for i, e in enumerate(l1):
            image[l1_offset + i * 8:l1_offset + i * 8 + 8] = struct.pack(">Q", e)

        snapshots_offset = 0
        if self.snapshots:
            table = b"".join(self._snapshot_entry(s, l1_offset, l1_size)
                             for s in self.snapshots)
            snapshots_offset = len(image)
            image.extend(table + b"\0" * (-len(table) % cs))

        header = self._header(l1_size, l1_offset, snapshots_offset)
        image[0:len(header)] = header
        return bytes(image)

    def _snapshot_entry(self, s: Snapshot, l1_offset: int, l1_size: int) -> bytes:
        extra = b"" if s.legacy else struct.pack(">Q", s.vm_state_size)
        if s.legacy:
            pass
        elif s.disk_size is not None or s.icount is not None:
            extra += struct.pack(">Q", s.disk_size if s.disk_size is not None else self.size)
        if s.icount is not None and not s.legacy:
            extra += struct.pack(">Q", s.icount)
        id_b, name_b = s.id.encode(), s.name.encode()
        entry = struct.pack(">QIHHIIQII", l1_offset, l1_size, len(id_b), len(name_b),
                            s.date_sec, s.date_nsec, s.vm_clock_nsec,
                            s.vm_state_size & 0xFFFFFFFF, len(extra))
        entry += extra + id_b + name_b
        return entry + b"\0" * (-len(entry) % 8)

    def _header(self, l1_size: int, l1_offset: int, snapshots_offset: int) -> bytes:
        incompat = (self.dirty << 0) | (self.corrupt << 1) | (self.extended_l2 << 4)
        h = struct.pack(">IIQIIQIIQQIIQ", MAGIC, self.version, 0, 0, self.cluster_bits,
                        self.size, 0, l1_size, l1_offset, self.cluster_size, 1,
                        len(self.snapshots), snapshots_offset)
        if self.version == 3:
            h += struct.pack(">QQQII", incompat, int(self.lazy_refcounts), 0,
                             self.refcount_order, self.header_length)
            h += b"\0" * (self.header_length - len(h))

        exts = b""
        if self.backing_format:
            exts += _ext(EXT_BACKING_FMT, self.backing_format.encode())
        if self.version == 3:
            exts += _ext(EXT_FEATURE_NAME, struct.pack(">BB46s", 0, 0, b"dirty bit"))
        exts += struct.pack(">II", EXT_END, 0)
        h += exts

        if self.backing_file:
            name = self.backing_file.encode()
            offset = len(h)
            h = h[:8] + struct.pack(">QI", offset, len(name)) + h[20:] + name
        return h


def _ext(ext_type: int, data: bytes) -> bytes:
    return struct.pack(">II", ext_type, len(data)) + data + b"\0" * (-len(data) % 8)
