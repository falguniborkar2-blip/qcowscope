/*
 * qcowscope - snapshot table parsing
 *
 * TODO(Falguni): implement qcow2_read_snapshots().
 * See docs/snapshot-table.md for the step-by-step task.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qcow2.h"

#include <stdio.h>
#include <stdlib.h>

int qcow2_read_snapshots(const Qcow2Image *img, Qcow2Snapshot **snaps,
                         char *err, size_t errlen)
{
    (void)img;
    *snaps = NULL;
    snprintf(err, errlen, "snapshot listing is not implemented yet");
    return -2;
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
