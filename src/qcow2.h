/*
 * qcowscope - read-only inspector for QEMU qcow2 disk images
 *
 * Format reference: https://www.qemu.org/docs/master/interop/qcow2.html
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef QCOWSCOPE_QCOW2_H
#define QCOWSCOPE_QCOW2_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define QCOW2_MAGIC            0x514649fbU   /* "QFI\xfb" */
#define QCOW2_V2_HEADER_LEN    72
#define QCOW2_V3_MIN_HEADER    104
#define QCOW2_MAX_L1_BYTES     (32u * 1024 * 1024)
#define QCOW2_MAX_SNAPSHOTS    65536
#define QCOW2_MAX_BACKING_NAME 1023

/* Incompatible feature bits */
#define QCOW2_INCOMPAT_DIRTY        (1ULL << 0)
#define QCOW2_INCOMPAT_CORRUPT      (1ULL << 1)
#define QCOW2_INCOMPAT_DATA_FILE    (1ULL << 2)
#define QCOW2_INCOMPAT_COMPRESSION  (1ULL << 3)
#define QCOW2_INCOMPAT_EXTL2        (1ULL << 4)
#define QCOW2_INCOMPAT_KNOWN        0x1fULL

/* Compatible feature bits */
#define QCOW2_COMPAT_LAZY_REFCOUNTS (1ULL << 0)

/* Header extension types */
#define QCOW2_EXT_END          0x00000000U
#define QCOW2_EXT_BACKING_FMT  0xe2792acaU
#define QCOW2_EXT_FEATURE_NAME 0x6803f857U
#define QCOW2_EXT_BITMAPS      0x23852875U
#define QCOW2_EXT_CRYPTO       0x0537be77U
#define QCOW2_EXT_DATA_FILE    0x44415441U

/* L1/L2 entry fields */
#define QCOW2_OFFSET_MASK      0x00fffffffffffe00ULL
#define QCOW2_L2_COMPRESSED    (1ULL << 62)
#define QCOW2_L2_ZERO          (1ULL << 0)

typedef struct Qcow2Image {
    int fd;
    uint64_t file_size;

    /* Header (all values converted from big-endian) */
    uint32_t version;
    uint64_t backing_file_offset;
    uint32_t backing_file_size;
    uint32_t cluster_bits;
    uint64_t cluster_size;
    uint64_t size;                 /* virtual disk size in bytes */
    uint32_t crypt_method;
    uint32_t l1_size;
    uint64_t l1_table_offset;
    uint64_t refcount_table_offset;
    uint32_t refcount_table_clusters;
    uint32_t nb_snapshots;
    uint64_t snapshots_offset;
    uint64_t incompatible_features;
    uint64_t compatible_features;
    uint64_t autoclear_features;
    uint32_t refcount_order;
    uint32_t header_length;
    uint8_t compression_type;      /* 0 = zlib, 1 = zstd */

    /* From header extensions / backing file name */
    char *backing_file;            /* NULL if none */
    char *backing_format;          /* NULL if not recorded */
    char *data_file;               /* NULL if none */
    bool has_feature_table;
    bool has_bitmaps;
    bool has_crypto_header;
} Qcow2Image;

typedef struct Qcow2AllocStats {
    uint64_t total_clusters;       /* guest clusters covered by the disk */
    uint64_t allocated_clusters;   /* same meaning as `qemu-img check` */
    uint64_t compressed_clusters;
    uint64_t zero_clusters;        /* clusters with the zero flag set */
} Qcow2AllocStats;

typedef struct Qcow2Snapshot {
    char *id;
    char *name;
    uint64_t l1_table_offset;
    uint32_t l1_size;
    uint32_t date_sec;
    uint32_t date_nsec;
    uint64_t vm_clock_nsec;
    uint64_t vm_state_size;
    bool has_disk_size;
    uint64_t disk_size;
    bool has_icount;
    uint64_t icount;
} Qcow2Snapshot;

/*
 * All functions return 0 on success. On failure they return -1 and write a
 * human-readable message into err (errlen bytes).
 */
int qcow2_open(Qcow2Image *img, const char *path, char *err, size_t errlen);
void qcow2_close(Qcow2Image *img);

int qcow2_alloc_stats(const Qcow2Image *img, Qcow2AllocStats *stats,
                      char *err, size_t errlen);

/*
 * Read the snapshot table. On success *snaps is a malloc'ed array of
 * img->nb_snapshots entries (NULL if there are none); free it with
 * qcow2_free_snapshots(). Returns -2 if not implemented yet.
 */
int qcow2_read_snapshots(const Qcow2Image *img, Qcow2Snapshot **snaps,
                         char *err, size_t errlen);
void qcow2_free_snapshots(Qcow2Snapshot *snaps, uint32_t count);

/* Small helpers shared with the CLI and the snapshot code */
int qcow2_read_at(const Qcow2Image *img, uint64_t offset, void *buf,
                  size_t len, char *err, size_t errlen);
uint16_t qcow2_be16(const uint8_t *p);
uint32_t qcow2_be32(const uint8_t *p);
uint64_t qcow2_be64(const uint8_t *p);

static inline bool qcow2_has_extl2(const Qcow2Image *img)
{
    return img->incompatible_features & QCOW2_INCOMPAT_EXTL2;
}

#endif /* QCOWSCOPE_QCOW2_H */
