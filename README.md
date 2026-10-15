# qcowscope

A small, read-only inspector for **QEMU qcow2** disk images, written in C,
with a Python test suite that checks its output against QEMU's own tools.

```
$ qcowscope info mid.qcow2
image:            mid.qcow2
format:           qcow2 version 3 (compat 1.1)
virtual size:     64.0 MiB (67108864 bytes)
cluster size:     65536
refcount bits:    16
compression:      zlib
backing file:     base.qcow2 (format: qcow2)
allocated:        17 of 1024 clusters (1.7%), 1 compressed, 16 zero
encrypted:        no
features:         lazy-refcounts
snapshots:        0

$ qcowscope chain top.qcow2
top.qcow2  qcow2 v3, 64.0 MiB, 0/1024 clusters allocated
  mid.qcow2  qcow2 v3, 64.0 MiB, 17/1024 clusters allocated
    base.qcow2  qcow2 v3, 64.0 MiB, 64/1024 clusters allocated
```

I built it to learn how qcow2 stores data on disk: the header and its
extensions, backing files, the two-level L1/L2 cluster mapping, compressed and
zero clusters, extended L2 entries, and the snapshot table.

## What it reads

| Part of the format | Shown as |
|---|---|
| Header (v2 and v3) | version, virtual size, cluster size, refcount bits, compression type, encryption |
| Feature bits | dirty, corrupt, lazy refcounts, extended L2, external data file |
| Header extensions | backing file format, feature name table, bitmaps, encryption header, data file |
| Backing file name | backing file and full chain (`qcowscope chain`) with loop detection |
| L1 / L2 tables | allocated, compressed and zero clusters, counted the same way as `qemu-img check` |
| Snapshot table | ID, name, date, VM state size, guest clock, icount |

`qcowscope info --json` uses the same key names as `qemu-img info --output=json`.

## Build and test

```bash
make                          # needs only a C compiler
sudo apt install qemu-utils   # optional: enables the QEMU comparison tests
pip install pytest
make test                     # unit + robustness + QEMU comparison tests
make asan                     # the same tests under AddressSanitizer + UBSan
```

## How it is tested

- **Unit tests** build small qcow2 images in Python (`tests/qcow2gen.py`), so
  they run anywhere, even without QEMU.
- **QEMU comparison tests** create images with `qemu-img` / `qemu-io` in 7
  configurations (compat 0.10, 4 KiB and 2 MiB clusters, lazy refcounts,
  64-bit refcounts, extended L2, compressed, zstd, backing chains, snapshots)
  and check that qcowscope agrees with `qemu-img info` and `qemu-img check`.
- **Robustness tests** feed broken images (bad header fields, offsets past
  the end of the file, truncated files) and a small fuzzer that corrupts
  metadata bytes at random. The tool must print an error and exit 1, never
  crash, and every successful run must print valid JSON.
- **CI** (GitHub Actions) builds with gcc and clang using `-Wall -Wextra
  -Werror` and runs everything under AddressSanitizer and UndefinedBehaviorSanitizer.

## Design notes

- **Every value from the file is untrusted.** All reads go through
  `qcow2_read_at()`, which rejects reads beyond the end of the file without
  integer overflow. Sizes are capped using the same limits as QEMU (L1 table
  up to 32 MiB, up to 65536 snapshots, backing name up to 1023 bytes).
  Image parsers in hypervisors are a common source of security bugs, so this
  was the main design rule.
- **Read-only:** the image is opened with `O_RDONLY` and never modified.
- **No dependencies:** plain C11 and POSIX.

## Bugs found by the tests

- The fuzzer found that a backing file name with invalid UTF-8 bytes made
  `--json` print invalid JSON. Invalid bytes are now replaced with U+FFFD.
- gcc's `-Werror=restrict` caught a path-resolution helper writing into the
  buffer it was reading from.

## Limitations / next steps

- Refcount table checking (a mini `qemu-img check`)
- Bitmap extension details
- Support for LUKS-encrypted images is limited to detecting them

## License

GPL-2.0-or-later, the same as QEMU.
