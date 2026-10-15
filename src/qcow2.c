/*
 * qcowscope - qcow2 header, header extension and L1/L2 parsing
 *
 * Every offset and length read from the image is treated as untrusted:
 * it is checked against the file size before it is used.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "qcow2.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fail(char *err, size_t errlen, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static int fail(char *err, size_t errlen, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
    return -1;
}

uint16_t qcow2_be16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

uint32_t qcow2_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

uint64_t qcow2_be64(const uint8_t *p)
{
    return ((uint64_t)qcow2_be32(p) << 32) | qcow2_be32(p + 4);
}

int qcow2_read_at(const Qcow2Image *img, uint64_t offset, void *buf,
                  size_t len, char *err, size_t errlen)
{
    uint8_t *dst = buf;

    /* Written this way so that offset + len can never overflow. */
    if (len > img->file_size || offset > img->file_size - len) {
        return fail(err, errlen,
                    "read of %zu bytes at offset %llu is beyond end of file "
                    "(%llu bytes)", len, (unsigned long long)offset,
                    (unsigned long long)img->file_size);
    }
    while (len > 0) {
        ssize_t n = pread(img->fd, dst, len, (off_t)offset);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return fail(err, errlen, "read error: %s", strerror(errno));
        }
        if (n == 0) {
            return fail(err, errlen, "unexpected end of file");
        }
        dst += n;
        offset += (uint64_t)n;
        len -= (size_t)n;
    }
    return 0;
}

static char *dup_bytes(const uint8_t *p, size_t len)
{
    char *s = malloc(len + 1);

    if (s) {
        memcpy(s, p, len);
        s[len] = '\0';
    }
    return s;
}

static int parse_header_extensions(Qcow2Image *img, const uint8_t *cluster0,
                                   size_t cluster0_len, char *err,
                                   size_t errlen)
{
    uint64_t offset = img->version == 2 ? QCOW2_V2_HEADER_LEN
                                        : img->header_length;
    uint64_t end = cluster0_len;

    /* Extensions end where the backing file name starts (if it is in cluster 0). */
    if (img->backing_file_offset && img->backing_file_offset < end) {
        end = img->backing_file_offset;
    }

    while (offset + 8 <= end) {
        uint32_t type = qcow2_be32(cluster0 + offset);
        uint32_t len = qcow2_be32(cluster0 + offset + 4);
        const uint8_t *data;

        offset += 8;
        if (type == QCOW2_EXT_END) {
            break;
        }
        if (len > end - offset) {
            return fail(err, errlen, "header extension 0x%08x is too large "
                        "(%u bytes)", type, len);
        }
        data = cluster0 + offset;

        switch (type) {
        case QCOW2_EXT_BACKING_FMT:
            if (len == 0 || len > 255 || img->backing_format) {
                return fail(err, errlen, "invalid backing format extension");
            }
            img->backing_format = dup_bytes(data, len);
            break;
        case QCOW2_EXT_DATA_FILE:
            if (len == 0 || len > 4096 || img->data_file) {
                return fail(err, errlen, "invalid data file extension");
            }
            img->data_file = dup_bytes(data, len);
            break;
        case QCOW2_EXT_FEATURE_NAME:
            img->has_feature_table = true;
            break;
        case QCOW2_EXT_BITMAPS:
            img->has_bitmaps = true;
            break;
        case QCOW2_EXT_CRYPTO:
            img->has_crypto_header = true;
            break;
        default:
            /* Unknown extensions must be ignored (spec). */
            break;
        }
        /* Extension data is padded to a multiple of 8 bytes. */
        offset += ((uint64_t)len + 7) & ~7ULL;
    }
    return 0;
}

static int parse_header(Qcow2Image *img, const uint8_t *h, char *err,
                        size_t errlen)
{
    uint64_t l2_entries, max_l1_coverage, min_l1_size;

    if (qcow2_be32(h) != QCOW2_MAGIC) {
        return fail(err, errlen, "not a qcow2 image (bad magic)");
    }
    img->version = qcow2_be32(h + 4);
    if (img->version != 2 && img->version != 3) {
        return fail(err, errlen, "unsupported qcow2 version %u", img->version);
    }
    img->backing_file_offset = qcow2_be64(h + 8);
    img->backing_file_size = qcow2_be32(h + 16);
    img->cluster_bits = qcow2_be32(h + 20);
    img->size = qcow2_be64(h + 24);
    img->crypt_method = qcow2_be32(h + 32);
    img->l1_size = qcow2_be32(h + 36);
    img->l1_table_offset = qcow2_be64(h + 40);
    img->refcount_table_offset = qcow2_be64(h + 48);
    img->refcount_table_clusters = qcow2_be32(h + 56);
    img->nb_snapshots = qcow2_be32(h + 60);
    img->snapshots_offset = qcow2_be64(h + 64);

    if (img->version == 2) {
        img->refcount_order = 4;
        img->header_length = QCOW2_V2_HEADER_LEN;
    } else {
        img->incompatible_features = qcow2_be64(h + 72);
        img->compatible_features = qcow2_be64(h + 80);
        img->autoclear_features = qcow2_be64(h + 88);
        img->refcount_order = qcow2_be32(h + 96);
        img->header_length = qcow2_be32(h + 100);
    }

    if (img->cluster_bits < 9 || img->cluster_bits > 21) {
        return fail(err, errlen, "invalid cluster_bits %u (must be 9-21)",
                    img->cluster_bits);
    }
    img->cluster_size = 1ULL << img->cluster_bits;

    if (img->version == 3) {
        if (img->header_length < QCOW2_V3_MIN_HEADER ||
            img->header_length > img->cluster_size) {
            return fail(err, errlen, "invalid header_length %u",
                        img->header_length);
        }
        if (img->header_length > 104) {
            img->compression_type = h[104];
        }
    }
    if (img->compression_type > 1) {
        return fail(err, errlen, "unknown compression type %u",
                    img->compression_type);
    }
    if (img->refcount_order > 6) {
        return fail(err, errlen, "invalid refcount_order %u",
                    img->refcount_order);
    }
    if (img->incompatible_features & ~QCOW2_INCOMPAT_KNOWN) {
        return fail(err, errlen, "unknown incompatible features 0x%llx",
                    (unsigned long long)(img->incompatible_features &
                                         ~QCOW2_INCOMPAT_KNOWN));
    }
    if (qcow2_has_extl2(img) && img->cluster_bits < 14) {
        return fail(err, errlen, "extended L2 entries need clusters of at "
                    "least 16 KiB");
    }

    /* L1 table: size limit, alignment, and it must cover the whole disk. */
    if ((uint64_t)img->l1_size * 8 > QCOW2_MAX_L1_BYTES) {
        return fail(err, errlen, "L1 table too large (%u entries)",
                    img->l1_size);
    }
    if (img->l1_size && (img->l1_table_offset & (img->cluster_size - 1))) {
        return fail(err, errlen, "L1 table offset is not cluster aligned");
    }
    l2_entries = img->cluster_size / (qcow2_has_extl2(img) ? 16 : 8);
    max_l1_coverage = l2_entries * img->cluster_size;
    min_l1_size = img->size / max_l1_coverage +
                  (img->size % max_l1_coverage != 0);
    if (img->l1_size < min_l1_size) {
        return fail(err, errlen, "L1 table too small for a %llu byte disk",
                    (unsigned long long)img->size);
    }

    if (img->nb_snapshots > QCOW2_MAX_SNAPSHOTS) {
        return fail(err, errlen, "too many snapshots (%u)", img->nb_snapshots);
    }
    if (img->nb_snapshots &&
        (img->snapshots_offset & (img->cluster_size - 1))) {
        return fail(err, errlen, "snapshot table offset is not cluster "
                    "aligned");
    }
    if (img->backing_file_offset) {
        if (img->backing_file_size == 0 ||
            img->backing_file_size > QCOW2_MAX_BACKING_NAME ||
            img->backing_file_offset > img->cluster_size) {
            return fail(err, errlen, "invalid backing file name location");
        }
    }
    return 0;
}

int qcow2_open(Qcow2Image *img, const char *path, char *err, size_t errlen)
{
    struct stat st;
    uint8_t *cluster0 = NULL;
    uint8_t header[QCOW2_V3_MIN_HEADER + 8];
    size_t cluster0_len;

    memset(img, 0, sizeof(*img));
    img->fd = open(path, O_RDONLY);
    if (img->fd < 0) {
        return fail(err, errlen, "cannot open %s: %s", path, strerror(errno));
    }
    if (fstat(img->fd, &st) < 0) {
        fail(err, errlen, "cannot stat %s: %s", path, strerror(errno));
        goto error;
    }
    img->file_size = (uint64_t)st.st_size;

    if (img->file_size < QCOW2_V2_HEADER_LEN) {
        fail(err, errlen, "file too small to be a qcow2 image");
        goto error;
    }
    memset(header, 0, sizeof(header));
    if (qcow2_read_at(img, 0,
                      header, img->file_size < sizeof(header)
                                  ? (size_t)img->file_size : sizeof(header),
                      err, errlen) < 0 ||
        parse_header(img, header, err, errlen) < 0) {
        goto error;
    }

    /* Header extensions and the backing file name live in cluster 0. */
    cluster0_len = img->file_size < img->cluster_size
                       ? (size_t)img->file_size : (size_t)img->cluster_size;
    cluster0 = malloc(cluster0_len);
    if (!cluster0) {
        fail(err, errlen, "out of memory");
        goto error;
    }
    if (qcow2_read_at(img, 0, cluster0, cluster0_len, err, errlen) < 0 ||
        parse_header_extensions(img, cluster0, cluster0_len, err,
                                errlen) < 0) {
        goto error;
    }
    if (img->backing_file_offset) {
        uint8_t name[QCOW2_MAX_BACKING_NAME];

        if (qcow2_read_at(img, img->backing_file_offset, name,
                          img->backing_file_size, err, errlen) < 0) {
            goto error;
        }
        img->backing_file = dup_bytes(name, img->backing_file_size);
    }
    free(cluster0);
    return 0;

error:
    free(cluster0);
    qcow2_close(img);
    return -1;
}

void qcow2_close(Qcow2Image *img)
{
    if (img->fd >= 0) {
        close(img->fd);
    }
    free(img->backing_file);
    free(img->backing_format);
    free(img->data_file);
    memset(img, 0, sizeof(*img));
    img->fd = -1;
}

/*
 * Count allocated clusters in the active L1/L2 tables, using the same rules
 * as `qemu-img check`: normal clusters, compressed clusters and zero clusters
 * that still own a host cluster count as allocated.
 */
int qcow2_alloc_stats(const Qcow2Image *img, Qcow2AllocStats *stats,
                      char *err, size_t errlen)
{
    size_t entry_size = qcow2_has_extl2(img) ? 16 : 8;
    uint64_t l2_entries = img->cluster_size / entry_size;
    uint8_t *l1 = NULL, *l2 = NULL;
    int ret = -1;

    memset(stats, 0, sizeof(*stats));
    stats->total_clusters = img->size / img->cluster_size +
                            (img->size % img->cluster_size != 0);
    if (img->l1_size == 0) {
        return 0;
    }

    l1 = malloc((size_t)img->l1_size * 8);
    l2 = malloc((size_t)img->cluster_size);
    if (!l1 || !l2) {
        fail(err, errlen, "out of memory");
        goto out;
    }
    if (qcow2_read_at(img, img->l1_table_offset, l1,
                      (size_t)img->l1_size * 8, err, errlen) < 0) {
        goto out;
    }

    for (uint64_t i = 0; i < img->l1_size; i++) {
        uint64_t l2_offset = qcow2_be64(l1 + i * 8) & QCOW2_OFFSET_MASK;

        if (l2_offset == 0) {
            continue;
        }
        if (l2_offset & (img->cluster_size - 1)) {
            fail(err, errlen, "L1 entry %llu: L2 table offset 0x%llx is not "
                 "cluster aligned", (unsigned long long)i,
                 (unsigned long long)l2_offset);
            goto out;
        }
        if (qcow2_read_at(img, l2_offset, l2, (size_t)img->cluster_size,
                          err, errlen) < 0) {
            goto out;
        }
        for (uint64_t j = 0; j < l2_entries; j++) {
            uint64_t guest_cluster = i * l2_entries + j;
            uint64_t entry = qcow2_be64(l2 + j * entry_size);
            uint64_t host = entry & QCOW2_OFFSET_MASK;

            if (guest_cluster >= stats->total_clusters) {
                break;
            }
            if (entry & QCOW2_L2_COMPRESSED) {
                stats->compressed_clusters++;
                stats->allocated_clusters++;
            } else if (!qcow2_has_extl2(img) && (entry & QCOW2_L2_ZERO)) {
                stats->zero_clusters++;
                if (host) {
                    stats->allocated_clusters++;
                }
            } else if (host) {
                stats->allocated_clusters++;
            }
        }
    }
    ret = 0;

out:
    free(l1);
    free(l2);
    return ret;
}
