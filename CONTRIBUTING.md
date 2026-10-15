# Contributing

Commits follow the QEMU / Linux kernel style:

```
snapshot: parse the snapshot table

Read each entry, including the 64-bit VM state size from the extra data.
Entries are padded to 8 bytes, so their offsets have to be computed one
by one.

Signed-off-by: Your Name <you@example.com>
```

- Subject: `area: short summary`, max ~70 characters, imperative mood.
- Body: what changed and **why**.
- `git commit -s` adds the `Signed-off-by` line.
- Before sending: `make && make test && make asan`.
