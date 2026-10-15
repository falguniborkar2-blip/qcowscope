# Your task: parse the qcow2 snapshot table

Implement `qcow2_read_snapshots()` in [`src/snapshot.c`](../src/snapshot.c).
When it works, `qcowscope info` lists internal snapshots (the ones made with
`qemu-img snapshot -c NAME disk.qcow2`) and 16 tests in
`tests/test_snapshots.py` stop being skipped and start passing.

Spec: <https://www.qemu.org/docs/master/interop/qcow2.html> (section "Snapshots").

## Where the data is

The header gives you two values, already parsed into `Qcow2Image`:

- `img->nb_snapshots`: how many entries there are
- `img->snapshots_offset`: file offset of the first entry

Entries are stored **one after another**. Each entry has a variable length,
and every entry starts at an offset that is a **multiple of 8**.

## Layout of one entry (big-endian)

| Bytes | Field | Put it in |
|---|---|---|
| 0-7 | L1 table offset | `l1_table_offset` |
| 8-11 | L1 table size | `l1_size` |
| 12-13 | length of the ID string | (local variable) |
| 14-15 | length of the name | (local variable) |
| 16-19 | date, seconds | `date_sec` |
| 20-23 | date, nanoseconds | `date_nsec` |
| 24-31 | guest run time in ns | `vm_clock_nsec` |
| 32-35 | VM state size (old 32-bit field) | `vm_state_size` |
| 36-39 | size of the extra data | (local variable) |
| 40 ... | extra data, `extra_data_size` bytes | see below |
| then | ID string (not NUL-terminated) | `id` |
| then | name (not NUL-terminated) | `name` |
| then | padding up to a multiple of 8 | |

Extra data (only the parts that are present):

| Extra bytes | Field |
|---|---|
| 0-7 | 64-bit VM state size: **use this instead of bytes 32-35** |
| 8-15 | disk size → `disk_size`, `has_disk_size = true` |
| 16-23 | icount → `icount`, `has_icount = true` |

## Steps

1. If `nb_snapshots == 0`, set `*snaps = NULL` and return 0.
2. `calloc` an array of `nb_snapshots` `Qcow2Snapshot` structs.
3. Keep a `uint64_t offset`, starting at `img->snapshots_offset`. For each entry:
   1. Read the fixed 40 bytes with `qcow2_read_at()` and decode them with
      `qcow2_be16/32/64()` (see how `parse_header()` in `src/qcow2.c` does it).
   2. Read the extra data and apply the table above.
   3. Read the ID and the name. Allocate `len + 1` bytes and add the `'\0'`.
   4. Move `offset` past everything and round it up to a multiple of 8:
      `offset = (offset + 7) & ~7ULL;`
4. On success: `*snaps = array; return 0;`
5. On any error: free what you allocated (`qcow2_free_snapshots()` already
   frees `id` and `name`), write a message into `err`, and `return -1`.

## Rules (this is what reviewers at Red Hat look for)

- **Never trust the file.** Always read through `qcow2_read_at()`; it rejects
  reads past the end of the file. Do not index buffers with values from the file
  without checking them.
- **Limit the extra data size** (for example, refuse more than 1024 bytes), so
  a broken image cannot make you allocate huge buffers.
- No memory leaks: `make asan` must pass.
- Match QEMU's style: 4-space indent, braces on the same line for `if`/`for`.

## Check your work

```bash
make && make test        # snapshot tests run now instead of being skipped
make asan                # same tests under AddressSanitizer
qemu-img create -f qcow2 /tmp/t.qcow2 16M
qemu-img snapshot -c first /tmp/t.qcow2
qemu-img snapshot -c second /tmp/t.qcow2
./qcowscope info /tmp/t.qcow2
qemu-img info /tmp/t.qcow2      # compare the two
```

`tests/test_qemu_compat.py::test_snapshots_match_qemu` does that comparison
automatically.

## Questions to be able to answer afterwards

- Why can't you calculate where entry 5 starts without reading entries 1-4?
- Why is there both a 32-bit and a 64-bit VM state size?
- What would happen if you used `id_len` from the file to `memcpy` into a
  fixed 64-byte buffer without checking it?
