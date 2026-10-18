/*
 * mac_disk.c - see mac_disk.h.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "mac_disk.h"

#include <stdint.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"

static const char *TAG = "mac_disk";

#define SECTOR 512u
#define NO_SECTOR (-1)

typedef struct {
    const esp_partition_t *part;
    const uint8_t *flash;   /* the image through the flash cache, or NULL: read it */
    uint32_t size;
    int32_t *overlay_of;    /* per disk sector: overlay slot, or NO_SECTOR */
    uint8_t *overlay;       /* slots of SECTOR bytes */
    uint32_t overlay_slots;
    uint32_t overlay_used;
    bool full_reported;
} mac_disk_t;

static mac_disk_t s_disk;

/* From the image as flashed, bypassing the overlay. */
static bool read_flash(const mac_disk_t *d, uint32_t offset, uint8_t *dst, uint32_t len)
{
    if (d->flash != NULL) {
        memcpy(dst, d->flash + offset, len);
        return true;
    }
    return esp_partition_read(d->part, offset, dst, len) == ESP_OK;
}

static bool in_disk(const mac_disk_t *d, unsigned int offset, unsigned int len)
{
    return offset <= d->size && len <= d->size - offset;
}

static int disk_read(void *ctx, uint8_t *data, unsigned int offset, unsigned int len)
{
    mac_disk_t *d = ctx;
    if (!in_disk(d, offset, len)) return -1;
    while (len) {
        const uint32_t sector = offset / SECTOR, in = offset % SECTOR;
        const uint32_t n = len < SECTOR - in ? len : SECTOR - in;
        const int32_t slot = d->overlay_of[sector];
        if (slot != NO_SECTOR) {
            memcpy(data, d->overlay + (uint32_t)slot * SECTOR + in, n);
        } else if (!read_flash(d, offset, data, n)) {
            return -1;
        }
        data += n;
        offset += n;
        len -= n;
    }
    return 0;
}

static int disk_write(void *ctx, uint8_t *data, unsigned int offset, unsigned int len)
{
    mac_disk_t *d = ctx;
    if (!in_disk(d, offset, len)) return -1;
    while (len) {
        const uint32_t sector = offset / SECTOR, in = offset % SECTOR;
        const uint32_t n = len < SECTOR - in ? len : SECTOR - in;
        int32_t slot = d->overlay_of[sector];
        if (slot == NO_SECTOR) {
            if (d->overlay_used == d->overlay_slots) {
                /* The Mac gets an I/O error; say so once, not per sector. */
                if (!d->full_reported) {
                    ESP_LOGW(TAG, "write overlay full (%u KB); disk writes now fail",
                             (unsigned)(d->overlay_slots * SECTOR / 1024));
                    d->full_reported = true;
                }
                return -1;
            }
            uint8_t *copy = d->overlay + d->overlay_used * SECTOR;
            if (!read_flash(d, sector * SECTOR, copy, SECTOR)) return -1;
            slot = (int32_t)d->overlay_used++;
            d->overlay_of[sector] = slot;
        }
        memcpy(d->overlay + (uint32_t)slot * SECTOR + in, data, n);
        data += n;
        offset += n;
        len -= n;
    }
    return 0;
}

bool mac_disk_open(disc_descr_t *out, size_t overlay_bytes)
{
    const esp_partition_t *p =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, MAC_DISK_PARTITION);
    if (p == NULL) {
        ESP_LOGE(TAG, "no '%s' partition", MAC_DISK_PARTITION);
        return false;
    }

    /* Mapped, reads are plain loads through the cache. If the MMU has no room
     * for the whole partition, every read goes through the flash driver. */
    const void *flash = NULL;
    esp_partition_mmap_handle_t handle;
    if (esp_partition_mmap(p, 0, p->size, ESP_PARTITION_MMAP_DATA, &flash, &handle) != ESP_OK) {
        ESP_LOGW(TAG, "cannot map the %u KB '%s' partition; reading it unmapped",
                 (unsigned)(p->size / 1024), p->label);
        flash = NULL;
    }

    const uint32_t sectors = p->size / SECTOR;
    s_disk = (mac_disk_t){
        .part = p,
        .flash = flash,
        .size = sectors * SECTOR,
        .overlay_of = heap_caps_malloc(sectors * sizeof(int32_t), MALLOC_CAP_SPIRAM),
        .overlay = heap_caps_malloc(overlay_bytes, MALLOC_CAP_SPIRAM),
        .overlay_slots = (uint32_t)(overlay_bytes / SECTOR),
    };
    if (s_disk.overlay_of == NULL || s_disk.overlay == NULL) {
        ESP_LOGE(TAG, "no PSRAM for the write overlay");
        heap_caps_free(s_disk.overlay_of);
        heap_caps_free(s_disk.overlay);
        return false;
    }
    for (uint32_t i = 0; i < sectors; i++) s_disk.overlay_of[i] = NO_SECTOR;

    /* "LK" boot blocks are what the ROM looks for on a startup disk. */
    uint8_t boot[2];
    if (!read_flash(&s_disk, 0, boot, sizeof boot) || boot[0] != 'L' || boot[1] != 'K') {
        ESP_LOGE(TAG, "'%s' holds no startup disk (no boot blocks)", p->label);
        heap_caps_free(s_disk.overlay_of);
        heap_caps_free(s_disk.overlay);
        return false;
    }

    *out = (disc_descr_t){
        .base = NULL,  /* not writable memory: go through the callbacks */
        .size = s_disk.size,
        .read_only = 0,
        .op_ctx = &s_disk,
        .op_read = disk_read,
        .op_write = disk_write,
    };
    ESP_LOGI(TAG, "disk: %u KB from '%s', %u KB write overlay", (unsigned)(s_disk.size / 1024),
             p->label, (unsigned)(overlay_bytes / 1024));
    return true;
}
