/*
 * qcowscope - snapshot table parsing
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qcow2.h"

#include <stdio.h>
#include <stdlib.h>

#define SNAPSHOT_FIXED_SIZE   40
#define SNAPSHOT_MAX_EXTRA    1024

/* Read len bytes at offset into a new NUL-terminated string. */
static char *read_string(const Qcow2Image *img, uint64_t offset, size_t len,
                         char *err, size_t errlen)
{
    char *s = malloc(len + 1);

    if (!s) {
        snprintf(err, errlen, "out of memory");
        return NULL;
    }
    if (qcow2_read_at(img, offset, s, len, err, errlen) < 0) {
        free(s);
        return NULL;
    }
    s[len] = '\0';
    return s;
}

int qcow2_read_snapshots(const Qcow2Image *img, Qcow2Snapshot **snaps,
                         char *err, size_t errlen)
{
    Qcow2Snapshot *list;
    uint64_t offset = img->snapshots_offset;

    *snaps = NULL;
    if (img->nb_snapshots == 0) {
        return 0;
    }

    list = calloc(img->nb_snapshots, sizeof(*list));
    if (!list) {
        snprintf(err, errlen, "out of memory");
        return -1;
    }

    for (uint32_t i = 0; i < img->nb_snapshots; i++) {
        Qcow2Snapshot *sn = &list[i];
        uint8_t fixed[SNAPSHOT_FIXED_SIZE];
        uint8_t extra[SNAPSHOT_MAX_EXTRA];
        uint16_t id_len, name_len;
        uint32_t extra_len;

        /* 1. Fixed part of the entry */
        if (qcow2_read_at(img, offset, fixed, sizeof(fixed), err, errlen) < 0) {
            goto fail;
        }
        sn->l1_table_offset = qcow2_be64(fixed);
        sn->l1_size = qcow2_be32(fixed + 8);
        id_len = qcow2_be16(fixed + 12);
        name_len = qcow2_be16(fixed + 14);
        sn->date_sec = qcow2_be32(fixed + 16);
        sn->date_nsec = qcow2_be32(fixed + 20);
        sn->vm_clock_nsec = qcow2_be64(fixed + 24);
        sn->vm_state_size = qcow2_be32(fixed + 32);
        extra_len = qcow2_be32(fixed + 36);
        offset += sizeof(fixed);

        /* 2. Extra data: only use the fields that are present */
        if (extra_len > SNAPSHOT_MAX_EXTRA) {
            snprintf(err, errlen, "snapshot %u: extra data too large (%u bytes)",
                     i, extra_len);
            goto fail;
        }
        if (qcow2_read_at(img, offset, extra, extra_len, err, errlen) < 0) {
            goto fail;
        }
        if (extra_len >= 8) {
            sn->vm_state_size = qcow2_be64(extra);
        }
        if (extra_len >= 16) {
            sn->has_disk_size = true;
            sn->disk_size = qcow2_be64(extra + 8);
        }
        if (extra_len >= 24) {
            sn->has_icount = true;
            sn->icount = qcow2_be64(extra + 16);
        }
        offset += extra_len;

        /* 3. ID and name strings (not NUL-terminated in the file) */
        sn->id = read_string(img, offset, id_len, err, errlen);
        if (!sn->id) {
            goto fail;
        }
        offset += id_len;
        sn->name = read_string(img, offset, name_len, err, errlen);
        if (!sn->name) {
            goto fail;
        }
        offset += name_len;

        /* 4. The next entry starts at the next multiple of 8 */
        offset = (offset + 7) & ~7ULL;
    }

    *snaps = list;
    return 0;

fail:
    qcow2_free_snapshots(list, img->nb_snapshots);
    return -1;
}

void qcow2_free_snapshots(Qcow2Snapshot *snaps, uint32_t count)
{
    if (!snaps) {
        return;
    }
    for (uint32_t i = 0; i < count; i++) {
        free(snaps[i].id);
        free(snaps[i].name);
    }
    free(snaps);
}
