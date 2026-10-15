/*
 * qcowscope - command line interface
 *
 * Usage:
 *   qcowscope info [--json] IMAGE
 *   qcowscope chain IMAGE
 *
 * Exit status: 0 success, 1 invalid or corrupt image, 2 usage or I/O error.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "qcow2.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define QCOWSCOPE_VERSION "0.1.0"
#define MAX_CHAIN_DEPTH   64

static const char *compression_name(uint8_t type)
{
    return type == 1 ? "zstd" : "zlib";
}

/* Length of a valid UTF-8 sequence starting at p, or 0 if it is invalid. */
static int utf8_len(const unsigned char *p)
{
    int n;

    if (p[0] < 0x80) {
        return 1;
    } else if (p[0] >= 0xc2 && p[0] <= 0xdf) {
        n = 2;
    } else if (p[0] >= 0xe0 && p[0] <= 0xef) {
        n = 3;
    } else if (p[0] >= 0xf0 && p[0] <= 0xf4) {
        n = 4;
    } else {
        return 0;
    }
    for (int i = 1; i < n; i++) {
        if ((p[i] & 0xc0) != 0x80) {   /* also stops at the NUL terminator */
            return 0;
        }
    }
    /* Reject overlong forms, UTF-16 surrogates and code points > U+10FFFF. */
    if ((p[0] == 0xe0 && p[1] < 0xa0) || (p[0] == 0xed && p[1] >= 0xa0) ||
        (p[0] == 0xf0 && p[1] < 0x90) || (p[0] == 0xf4 && p[1] >= 0x90)) {
        return 0;
    }
    return n;
}

/*
 * Print s as a JSON string. Names inside an image are arbitrary bytes, so
 * invalid UTF-8 is replaced with U+FFFD to keep the output valid JSON.
 */
static void json_string(const char *s)
{
    const unsigned char *p = (const unsigned char *)s;

    putchar('"');
    while (*p) {
        int n = utf8_len(p);

        if (n == 0) {
            fputs("\\ufffd", stdout);
            p++;
        } else if (n > 1) {
            fwrite(p, 1, (size_t)n, stdout);
            p += n;
        } else {
            switch (*p) {
            case '"':  fputs("\\\"", stdout); break;
            case '\\': fputs("\\\\", stdout); break;
            case '\n': fputs("\\n", stdout); break;
            case '\t': fputs("\\t", stdout); break;
            default:
                if (*p < 0x20 || *p == 0x7f) {
                    printf("\\u%04x", *p);
                } else {
                    putchar(*p);
                }
            }
            p++;
        }
    }
    putchar('"');
}

static const char *bool_str(bool b)
{
    return b ? "true" : "false";
}

static void human_size(uint64_t bytes, char *buf, size_t len)
{
    const char *units[] = { "B", "KiB", "MiB", "GiB", "TiB", "PiB" };
    double v = (double)bytes;
    int u = 0;

    while (v >= 1024 && u < 5) {
        v /= 1024;
        u++;
    }
    snprintf(buf, len, u == 0 ? "%.0f %s" : "%.1f %s", v, units[u]);
}

static void print_snapshots_json(const Qcow2Snapshot *s, uint32_t n)
{
    printf(",\n  \"snapshots\": [");
    for (uint32_t i = 0; i < n; i++) {
        printf("%s\n    {\"id\": ", i ? "," : "");
        json_string(s[i].id);
        printf(", \"name\": ");
        json_string(s[i].name);
        printf(", \"vm-state-size\": %llu, \"date-sec\": %u, "
               "\"date-nsec\": %u, \"vm-clock-sec\": %llu, "
               "\"vm-clock-nsec\": %llu",
               (unsigned long long)s[i].vm_state_size, s[i].date_sec,
               s[i].date_nsec,
               (unsigned long long)(s[i].vm_clock_nsec / 1000000000ULL),
               (unsigned long long)(s[i].vm_clock_nsec % 1000000000ULL));
        if (s[i].has_icount) {
            printf(", \"icount\": %llu", (unsigned long long)s[i].icount);
        }
        printf("}");
    }
    printf("\n  ]");
}

static void print_info_json(const char *path, const Qcow2Image *img,
                            const Qcow2AllocStats *st,
                            const Qcow2Snapshot *snaps, int snap_ret)
{
    bool v3 = img->version == 3;

    printf("{\n  \"filename\": ");
    json_string(path);
    printf(",\n  \"format\": \"qcow2\",\n");
    printf("  \"virtual-size\": %llu,\n", (unsigned long long)img->size);
    printf("  \"cluster-size\": %llu,\n",
           (unsigned long long)img->cluster_size);
    printf("  \"dirty-flag\": %s,\n",
           bool_str(img->incompatible_features & QCOW2_INCOMPAT_DIRTY));
    printf("  \"encrypted\": %s", bool_str(img->crypt_method != 0));
    if (img->backing_file) {
        printf(",\n  \"backing-filename\": ");
        json_string(img->backing_file);
    }
    if (img->backing_format) {
        printf(",\n  \"backing-filename-format\": ");
        json_string(img->backing_format);
    }
    printf(",\n  \"format-specific\": {\n    \"type\": \"qcow2\",\n"
           "    \"data\": {\n");
    printf("      \"compat\": \"%s\",\n", v3 ? "1.1" : "0.10");
    printf("      \"compression-type\": \"%s\",\n",
           compression_name(img->compression_type));
    if (v3) {
        printf("      \"lazy-refcounts\": %s,\n",
               bool_str(img->compatible_features &
                        QCOW2_COMPAT_LAZY_REFCOUNTS));
    }
    printf("      \"refcount-bits\": %u", 1u << img->refcount_order);
    if (v3) {
        printf(",\n      \"corrupt\": %s,\n      \"extended-l2\": %s",
               bool_str(img->incompatible_features & QCOW2_INCOMPAT_CORRUPT),
               bool_str(qcow2_has_extl2(img)));
    }
    if (img->data_file) {
        printf(",\n      \"data-file\": ");
        json_string(img->data_file);
    }
    printf("\n    }\n  },\n");
    printf("  \"allocation\": {\n"
           "    \"total-clusters\": %llu,\n"
           "    \"allocated-clusters\": %llu,\n"
           "    \"compressed-clusters\": %llu,\n"
           "    \"zero-clusters\": %llu\n  }",
           (unsigned long long)st->total_clusters,
           (unsigned long long)st->allocated_clusters,
           (unsigned long long)st->compressed_clusters,
           (unsigned long long)st->zero_clusters);
    printf(",\n  \"snapshot-count\": %u", img->nb_snapshots);
    if (snap_ret == 0 && img->nb_snapshots > 0) {
        print_snapshots_json(snaps, img->nb_snapshots);
    }
    printf("\n}\n");
}

static void print_info_human(const char *path, const Qcow2Image *img,
                             const Qcow2AllocStats *st,
                             const Qcow2Snapshot *snaps, int snap_ret,
                             const char *snap_err)
{
    char sz[32];
    double pct = st->total_clusters
                     ? 100.0 * (double)st->allocated_clusters /
                           (double)st->total_clusters
                     : 0.0;

    human_size(img->size, sz, sizeof(sz));
    printf("image:            %s\n", path);
    printf("format:           qcow2 version %u (compat %s)\n", img->version,
           img->version == 3 ? "1.1" : "0.10");
    printf("virtual size:     %s (%llu bytes)\n", sz,
           (unsigned long long)img->size);
    printf("cluster size:     %llu\n", (unsigned long long)img->cluster_size);
    printf("refcount bits:    %u\n", 1u << img->refcount_order);
    printf("compression:      %s\n", compression_name(img->compression_type));
    if (img->backing_file) {
        printf("backing file:     %s (format: %s)\n", img->backing_file,
               img->backing_format ? img->backing_format : "not recorded");
    }
    if (img->data_file) {
        printf("data file:        %s\n", img->data_file);
    }
    printf("allocated:        %llu of %llu clusters (%.1f%%), %llu compressed,"
           " %llu zero\n",
           (unsigned long long)st->allocated_clusters,
           (unsigned long long)st->total_clusters, pct,
           (unsigned long long)st->compressed_clusters,
           (unsigned long long)st->zero_clusters);
    printf("encrypted:        %s\n", img->crypt_method == 2   ? "yes (LUKS)"
                                     : img->crypt_method == 1 ? "yes (AES)"
                                                              : "no");
    printf("features:        %s%s%s%s%s\n",
           img->compatible_features & QCOW2_COMPAT_LAZY_REFCOUNTS
               ? " lazy-refcounts" : "",
           qcow2_has_extl2(img) ? " extended-l2" : "",
           img->has_bitmaps ? " bitmaps" : "",
           img->incompatible_features & QCOW2_INCOMPAT_DIRTY ? " DIRTY" : "",
           img->incompatible_features & QCOW2_INCOMPAT_CORRUPT ? " CORRUPT"
                                                                : "");
    printf("snapshots:        %u\n", img->nb_snapshots);
    if (img->nb_snapshots > 0) {
        if (snap_ret == 0) {
            for (uint32_t i = 0; i < img->nb_snapshots; i++) {
                char vm[32];

                human_size(snaps[i].vm_state_size, vm, sizeof(vm));
                printf("  [%s] %-20s vm state %s, created %u\n", snaps[i].id,
                       snaps[i].name, vm, snaps[i].date_sec);
            }
        } else {
            printf("  (%s)\n", snap_err);
        }
    }
}

static int cmd_info(const char *path, bool json)
{
    Qcow2Image img;
    Qcow2AllocStats st;
    Qcow2Snapshot *snaps = NULL;
    char err[256], snap_err[256] = "";
    int snap_ret = 0;

    if (qcow2_open(&img, path, err, sizeof(err)) < 0) {
        fprintf(stderr, "qcowscope: %s: %s\n", path, err);
        return 1;
    }
    if (qcow2_alloc_stats(&img, &st, err, sizeof(err)) < 0) {
        fprintf(stderr, "qcowscope: %s: %s\n", path, err);
        qcow2_close(&img);
        return 1;
    }
    if (img.nb_snapshots > 0) {
        snap_ret = qcow2_read_snapshots(&img, &snaps, snap_err,
                                        sizeof(snap_err));
        if (snap_ret == -1) {
            fprintf(stderr, "qcowscope: %s: %s\n", path, snap_err);
            qcow2_close(&img);
            return 1;
        }
    }

    if (json) {
        print_info_json(path, &img, &st, snaps, snap_ret);
    } else {
        print_info_human(path, &img, &st, snaps, snap_ret, snap_err);
    }
    qcow2_free_snapshots(snaps, img.nb_snapshots);
    qcow2_close(&img);
    return 0;
}

/* Resolve a backing file name relative to the image that references it. */
static void resolve_backing(const char *image, const char *backing,
                            char *out, size_t len)
{
    const char *slash = strrchr(image, '/');

    if (backing[0] == '/' || !slash) {
        snprintf(out, len, "%s", backing);
    } else {
        snprintf(out, len, "%.*s/%s", (int)(slash - image), image, backing);
    }
}

static int cmd_chain(const char *path)
{
    char current[PATH_MAX], seen[MAX_CHAIN_DEPTH][PATH_MAX];
    int depth = 0;

    snprintf(current, sizeof(current), "%s", path);
    for (;;) {
        Qcow2Image img;
        Qcow2AllocStats st;
        char err[256], real[PATH_MAX], sz[32];

        if (!realpath(current, real)) {
            printf("%*s%s  (missing)\n", depth * 2, "", current);
            return 1;
        }
        for (int i = 0; i < depth; i++) {
            if (strcmp(seen[i], real) == 0) {
                printf("%*s%s  (loop detected)\n", depth * 2, "", current);
                return 1;
            }
        }
        if (depth == MAX_CHAIN_DEPTH) {
            fprintf(stderr, "qcowscope: backing chain deeper than %d\n",
                    MAX_CHAIN_DEPTH);
            return 1;
        }
        snprintf(seen[depth], PATH_MAX, "%s", real);

        if (qcow2_open(&img, current, err, sizeof(err)) < 0) {
            /* A raw base image is normal at the end of a chain. */
            printf("%*s%s  (%s)\n", depth * 2, "", current, err);
            return strstr(err, "bad magic") ? 0 : 1;
        }
        if (qcow2_alloc_stats(&img, &st, err, sizeof(err)) < 0) {
            printf("%*s%s  (error: %s)\n", depth * 2, "", current, err);
            qcow2_close(&img);
            return 1;
        }
        human_size(img.size, sz, sizeof(sz));
        printf("%*s%s  qcow2 v%u, %s, %llu/%llu clusters allocated\n",
               depth * 2, "", current, img.version, sz,
               (unsigned long long)st.allocated_clusters,
               (unsigned long long)st.total_clusters);
        if (!img.backing_file) {
            qcow2_close(&img);
            return 0;
        }
        {
            char next[PATH_MAX];

            resolve_backing(current, img.backing_file, next, sizeof(next));
            snprintf(current, sizeof(current), "%s", next);
        }
        qcow2_close(&img);
        depth++;
    }
}

static void usage(FILE *out)
{
    fprintf(out,
            "Usage: qcowscope info [--json] IMAGE\n"
            "       qcowscope chain IMAGE\n"
            "       qcowscope --version\n\n"
            "Read-only inspector for QEMU qcow2 disk images.\n");
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        printf("qcowscope %s\n", QCOWSCOPE_VERSION);
        return 0;
    }
    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        usage(stdout);
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "info") == 0) {
        return cmd_info(argv[2], false);
    }
    if (argc == 4 && strcmp(argv[1], "info") == 0 &&
        strcmp(argv[2], "--json") == 0) {
        return cmd_info(argv[3], true);
    }
    if (argc == 3 && strcmp(argv[1], "chain") == 0) {
        return cmd_chain(argv[2]);
    }
    usage(stderr);
    return 2;
}
