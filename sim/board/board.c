/*
 * Mosaico board simulator: the ESP-IDF, FreeRTOS and BSP calls the firmware
 * makes, implemented against a model of the board. See board.h.
 *
 * Threads: the application thread runs app_main() and is the only caller of
 * everything here except board_touch_set(), board_panel_snapshot() and
 * board_stats(), which the window thread also uses. s_lock guards what those
 * three share.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* mmap(MAP_ANON) is outside strict POSIX. */
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#else
#define _DEFAULT_SOURCE
#endif

#include "board.h"

#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "bsp/display.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#ifndef CONFIG_ESP_MAIN_TASK_STACK_SIZE
#error "CONFIG_ESP_MAIN_TASK_STACK_SIZE must match the firmware's sdkconfig"
#endif

/* trans_queue_depth of the BSP's CO5300 panel IO. */
#define BOARD_IO_QUEUE_DEPTH 10

/* CO5300 column/row address granularity. */
#define BOARD_X_ALIGN 4
#define BOARD_Y_ALIGN 2

static board_config_t s_config;
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static board_stats_t s_stats;           /* guarded by s_lock */
static uint32_t s_echoed_problems;      /* guarded by s_lock */

/* --- clock ---------------------------------------------------------------- */

static int64_t s_virtual_us;
static struct timespec s_boot;

int64_t esp_timer_get_time(void)
{
    if (s_config.clock == BOARD_CLOCK_VIRTUAL) {
        return s_virtual_us;
    }
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)(now.tv_sec - s_boot.tv_sec) * 1000000 +
           (int64_t)(now.tv_nsec - s_boot.tv_nsec) / 1000;
}

uint32_t esp_log_timestamp(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void sleep_ticks(TickType_t ticks)
{
    int64_t us = (int64_t)ticks * 1000000 / configTICK_RATE_HZ;
    if (s_config.clock == BOARD_CLOCK_VIRTUAL) {
        s_virtual_us += us;
        return;
    }
    if (us == 0) {
        sched_yield();
        return;
    }
    struct timespec ts = { .tv_sec = (time_t)(us / 1000000), .tv_nsec = (long)(us % 1000000) * 1000 };
    nanosleep(&ts, NULL);
}

/* --- log ------------------------------------------------------------------ */

void board_log(char level, const char *tag, const char *fmt, ...)
{
    bool echo;
    pthread_mutex_lock(&s_lock);
    if (level == 'E' || level == 'W') {
        if (level == 'E') {
            s_stats.errors++;
        } else {
            s_stats.warnings++;
        }
        s_echoed_problems++;
        echo = s_config.max_echoed_problems == 0 ||
               s_echoed_problems <= s_config.max_echoed_problems;
        if (s_config.max_echoed_problems != 0 &&
            s_echoed_problems == s_config.max_echoed_problems + 1) {
            fprintf(stderr, "board: further warnings and errors are counted, not shown\n");
        }
    } else {
        echo = s_config.echo_info;
    }
    pthread_mutex_unlock(&s_lock);
    if (!echo) {
        return;
    }

    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    fprintf(stderr, "%c (%u) %s: %s\n", level, (unsigned)esp_log_timestamp(), tag, line);
}

const char *esp_err_to_name(esp_err_t code)
{
    switch (code) {
    case ESP_OK: return "ESP_OK";
    case ESP_FAIL: return "ESP_FAIL";
    case ESP_ERR_NO_MEM: return "ESP_ERR_NO_MEM";
    case ESP_ERR_INVALID_ARG: return "ESP_ERR_INVALID_ARG";
    case ESP_ERR_INVALID_STATE: return "ESP_ERR_INVALID_STATE";
    case ESP_ERR_NOT_SUPPORTED: return "ESP_ERR_NOT_SUPPORTED";
    case ESP_ERR_TIMEOUT: return "ESP_ERR_TIMEOUT";
    default: return "ERROR";
    }
}

/* Counts a board-model complaint as an error so tests fail on it. */
#define BOARD_COMPLAIN(...) board_log('E', "board", __VA_ARGS__)

/* --- heap ----------------------------------------------------------------- */

void *heap_caps_malloc(size_t size, uint32_t caps)
{
    (void)caps;
    return malloc(size);
}

void *heap_caps_calloc(size_t n, size_t size, uint32_t caps)
{
    (void)caps;
    return calloc(n, size);
}

void heap_caps_free(void *ptr)
{
    free(ptr);
}

/* --- semaphores ----------------------------------------------------------- */

struct board_sem {
    int count;
};

SemaphoreHandle_t xSemaphoreCreateBinary(void)
{
    return calloc(1, sizeof(struct board_sem));
}

void vSemaphoreDelete(SemaphoreHandle_t sem)
{
    free(sem);
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t sem)
{
    if (sem->count) {
        return pdFALSE;
    }
    sem->count = 1;
    return pdTRUE;
}

BaseType_t xSemaphoreGiveFromISR(SemaphoreHandle_t sem, BaseType_t *woken)
{
    if (woken) {
        *woken = pdFALSE;
    }
    return xSemaphoreGive(sem);
}

static void drain_transfers(void);

BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t ticks)
{
    /* Blocking lets the hardware run: everything queued completes and its
     * callbacks fire before we look at the count. */
    drain_transfers();
    if (sem->count) {
        sem->count = 0;
        return pdTRUE;
    }
    sleep_ticks(ticks);
    return pdFALSE;
}

/* --- panel ---------------------------------------------------------------- */

struct esp_lcd_panel_t {
    int unused;
};

struct esp_lcd_panel_io_t {
    esp_lcd_panel_io_color_trans_done_cb_t on_color_trans_done;
    void *user_ctx;
};

typedef struct {
    int x0, y0, x1, y1;
    const uint16_t *src;
} transfer_t;

static uint16_t s_panel_px[BOARD_PANEL_W * BOARD_PANEL_H];  /* guarded by s_lock */
static struct esp_lcd_panel_t s_panel;
static struct esp_lcd_panel_io_t s_io;
static bool s_display_up;
static transfer_t s_queue[BOARD_IO_QUEUE_DEPTH];
static int s_queued;

/* Completes every queued transfer in order, reading each source buffer now --
 * not when it was queued. */
static void drain_transfers(void)
{
    for (int i = 0; i < s_queued; i++) {
        const transfer_t *t = &s_queue[i];
        const int w = t->x1 - t->x0;
        pthread_mutex_lock(&s_lock);
        for (int y = t->y0; y < t->y1; y++) {
            if (y < 0 || y >= BOARD_PANEL_H) {
                continue;
            }
            for (int x = t->x0; x < t->x1; x++) {
                if (x >= 0 && x < BOARD_PANEL_W) {
                    s_panel_px[y * BOARD_PANEL_W + x] = t->src[(y - t->y0) * w + (x - t->x0)];
                }
            }
        }
        pthread_mutex_unlock(&s_lock);
        if (s_io.on_color_trans_done) {
            esp_lcd_panel_io_event_data_t edata = { 0 };
            s_io.on_color_trans_done(&s_io, &edata, s_io.user_ctx);
        }
    }
    s_queued = 0;
}

esp_err_t esp_lcd_panel_draw_bitmap(esp_lcd_panel_handle_t panel, int x_start, int y_start,
                                    int x_end, int y_end, const void *color_data)
{
    if (panel != &s_panel || !s_display_up) {
        ESP_LOGE("lcd_panel", "esp_lcd_panel_draw_bitmap: invalid panel handle");
        pthread_mutex_lock(&s_lock);
        s_stats.rejected_transfers++;
        pthread_mutex_unlock(&s_lock);
        return ESP_ERR_INVALID_ARG;
    }
    if (x_start >= x_end || y_start >= y_end) {
        ESP_LOGE("lcd_panel", "esp_lcd_panel_draw_bitmap: start position must be smaller than end position");
        pthread_mutex_lock(&s_lock);
        s_stats.rejected_transfers++;
        pthread_mutex_unlock(&s_lock);
        return ESP_ERR_INVALID_ARG;
    }

    /* The panel would take these, so they are not errors for the driver, but
     * they draw garbage or the wrong pixels on the device. */
    bool violation = false;
    if (x_start < 0 || y_start < 0 || x_end > BOARD_PANEL_W || y_end > BOARD_PANEL_H) {
        BOARD_COMPLAIN("window [%d,%d)-[%d,%d) is outside the %dx%d panel",
                       x_start, y_start, x_end, y_end, BOARD_PANEL_W, BOARD_PANEL_H);
        violation = true;
    }
    if (x_start % BOARD_X_ALIGN || (x_end - x_start) % BOARD_X_ALIGN ||
        y_start % BOARD_Y_ALIGN || (y_end - y_start) % BOARD_Y_ALIGN) {
        BOARD_COMPLAIN("window [%d,%d)-[%d,%d) breaks CO5300 alignment (x by %d, y by %d)",
                       x_start, y_start, x_end, y_end, BOARD_X_ALIGN, BOARD_Y_ALIGN);
        violation = true;
    }

    if (s_queued == BOARD_IO_QUEUE_DEPTH) {
        /* A full queue blocks the caller until the oldest transfers finish. */
        drain_transfers();
    }
    s_queue[s_queued++] = (transfer_t){ x_start, y_start, x_end, y_end, color_data };

    pthread_mutex_lock(&s_lock);
    s_stats.transfers++;
    s_stats.pixels += (uint64_t)(x_end - x_start) * (uint64_t)(y_end - y_start);
    if (violation) {
        s_stats.window_violations++;
    }
    pthread_mutex_unlock(&s_lock);
    return ESP_OK;
}

esp_err_t esp_lcd_panel_io_register_event_callbacks(esp_lcd_panel_io_handle_t io,
                                                    const esp_lcd_panel_io_callbacks_t *cbs,
                                                    void *user_ctx)
{
    if (io != &s_io || !cbs) {
        return ESP_ERR_INVALID_ARG;
    }
    s_io.on_color_trans_done = cbs->on_color_trans_done;
    s_io.user_ctx = user_ctx;
    return ESP_OK;
}

esp_err_t bsp_display_new(const bsp_display_config_t *config, esp_lcd_panel_handle_t *ret_panel)
{
    if (!config || !ret_panel) {
        return ESP_ERR_INVALID_ARG;
    }
    if (config->rotation != BSP_DISPLAY_ROTATE_0) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    /* Adopts the panel as the bootloader left it: no reset, splash still on. */
    s_display_up = true;
    *ret_panel = &s_panel;
    return ESP_OK;
}

esp_lcd_panel_handle_t bsp_display_get_panel(void)
{
    return s_display_up ? &s_panel : NULL;
}

esp_lcd_panel_io_handle_t bsp_display_get_panel_io(void)
{
    return s_display_up ? &s_io : NULL;
}

void board_panel_snapshot(uint16_t *dst)
{
    pthread_mutex_lock(&s_lock);
    memcpy(dst, s_panel_px, sizeof s_panel_px);
    pthread_mutex_unlock(&s_lock);
}

/* --- touch ---------------------------------------------------------------- */

struct esp_lcd_touch_s {
    uint8_t points;
    esp_lcd_touch_point_data_t point;
};

static struct esp_lcd_touch_s s_touch;
static bool s_touch_up;

/* What the controller senses right now. Guarded by s_lock. */
static bool s_finger_down;
static int s_finger_x, s_finger_y;

void board_touch_set(bool pressed, int x, int y)
{
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= BOARD_PANEL_W) x = BOARD_PANEL_W - 1;
    if (y >= BOARD_PANEL_H) y = BOARD_PANEL_H - 1;
    pthread_mutex_lock(&s_lock);
    s_finger_down = pressed;
    s_finger_x = x;
    s_finger_y = y;
    pthread_mutex_unlock(&s_lock);
}

esp_err_t bsp_touch_new(bsp_display_rotation_t rotation, esp_lcd_touch_handle_t *ret_touch)
{
    if (!ret_touch) {
        return ESP_ERR_INVALID_ARG;
    }
    if (rotation != BSP_DISPLAY_ROTATE_0) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    s_touch_up = true;
    *ret_touch = &s_touch;
    return ESP_OK;
}

esp_err_t esp_lcd_touch_read_data(esp_lcd_touch_handle_t tp)
{
    if (tp != &s_touch || !s_touch_up) {
        return ESP_ERR_INVALID_ARG;
    }
    pthread_mutex_lock(&s_lock);
    s_stats.touch_reads++;
    tp->points = s_finger_down ? 1 : 0;
    tp->point = (esp_lcd_touch_point_data_t){
        .x = (uint16_t)s_finger_x, .y = (uint16_t)s_finger_y, .strength = s_finger_down ? 64 : 0,
    };
    pthread_mutex_unlock(&s_lock);
    return ESP_OK;
}

esp_err_t esp_lcd_touch_get_data(esp_lcd_touch_handle_t tp, esp_lcd_touch_point_data_t *data,
                                 uint8_t *point_cnt, uint8_t max_point_cnt)
{
    if (tp != &s_touch || !s_touch_up || !data || !point_cnt || max_point_cnt == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (tp->points == 0) {
        return ESP_OK;
    }
    *point_cnt = 1;
    data[0] = tp->point;
    tp->points = 0;
    return ESP_OK;
}

/* --- flash partitions ----------------------------------------------------- */

typedef struct {
    esp_partition_t part;
    uint8_t *data;  /* whole partition, read-only once loaded */
} flash_part_t;

static flash_part_t s_parts[BOARD_MAX_PARTITIONS];
static int s_part_count;

/* Erased flash with the file programmed at the start, then write-protected. */
static void load_partitions(void)
{
    uint32_t address = 0x400000;  /* plausible-looking, never dereferenced */
    for (int i = 0; i < s_config.partition_count; i++) {
        const board_partition_t *bp = &s_config.partitions[i];
        FILE *f = fopen(bp->path, "rb");
        if (!f) {
            fprintf(stderr, "board: partition %s: cannot open %s\n", bp->label, bp->path);
            exit(2);
        }
        fseek(f, 0, SEEK_END);
        const long file_size = ftell(f);
        fseek(f, 0, SEEK_SET);
        const uint32_t size = bp->size ? bp->size : (uint32_t)file_size;
        if ((long)size < file_size) {
            fprintf(stderr, "board: partition %s: %s (%ld B) is larger than %u B\n", bp->label,
                    bp->path, file_size, size);
            exit(2);
        }
        uint8_t *mem = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (mem == MAP_FAILED) {
            fprintf(stderr, "board: partition %s: out of memory\n", bp->label);
            exit(2);
        }
        memset(mem, 0xff, size);
        if (fread(mem, 1, (size_t)file_size, f) != (size_t)file_size) {
            fprintf(stderr, "board: partition %s: short read from %s\n", bp->label, bp->path);
            exit(2);
        }
        fclose(f);
        mprotect(mem, size, PROT_READ);

        flash_part_t *fp = &s_parts[s_part_count++];
        fp->data = mem;
        fp->part = (esp_partition_t){
            .type = ESP_PARTITION_TYPE_DATA,
            .subtype = (esp_partition_subtype_t)bp->subtype,
            .address = address,
            .size = size,
            .erase_size = 4096,
        };
        snprintf(fp->part.label, sizeof fp->part.label, "%s", bp->label);
        address += (size + 0xffff) & ~0xffffu;
    }
}

static flash_part_t *part_of(const esp_partition_t *p)
{
    for (int i = 0; i < s_part_count; i++) {
        if (&s_parts[i].part == p) return &s_parts[i];
    }
    return NULL;
}

const esp_partition_t *esp_partition_find_first(esp_partition_type_t type,
                                                esp_partition_subtype_t subtype, const char *label)
{
    for (int i = 0; i < s_part_count; i++) {
        const esp_partition_t *p = &s_parts[i].part;
        if ((type == ESP_PARTITION_TYPE_ANY || p->type == type) &&
            (subtype == ESP_PARTITION_SUBTYPE_ANY || p->subtype == subtype) &&
            (label == NULL || strcmp(p->label, label) == 0)) {
            return p;
        }
    }
    return NULL;
}

esp_err_t esp_partition_read(const esp_partition_t *partition, size_t src_offset, void *dst,
                             size_t size)
{
    flash_part_t *fp = part_of(partition);
    if (!fp || !dst) return ESP_ERR_INVALID_ARG;
    if (src_offset > fp->part.size || size > fp->part.size - src_offset) return ESP_ERR_INVALID_ARG;
    memcpy(dst, fp->data + src_offset, size);
    return ESP_OK;
}

esp_err_t esp_partition_mmap(const esp_partition_t *partition, size_t offset, size_t size,
                             esp_partition_mmap_flag_t flags, const void **out_ptr,
                             esp_partition_mmap_handle_t *out_handle)
{
    (void)flags;
    flash_part_t *fp = part_of(partition);
    if (!fp || !out_ptr || !out_handle) return ESP_ERR_INVALID_ARG;
    if (offset > fp->part.size || size > fp->part.size - offset) return ESP_ERR_INVALID_ARG;
    *out_ptr = fp->data + offset;
    *out_handle = (esp_partition_mmap_handle_t)(fp - s_parts) + 1;
    return ESP_OK;
}

void esp_partition_munmap(esp_partition_mmap_handle_t handle)
{
    (void)handle;
}

/* --- tasks ---------------------------------------------------------------- */

/*
 * The application stack is ours: painted with a pattern, several times the
 * firmware's budget so an overrun is measured instead of crashing, with a
 * guard page below. How much of the pattern is gone is the high-water mark.
 *
 * Host frames are not RISC-V frames, so this is not byte-exact; it is there
 * for the overflow that matters, a few kilobytes of local arrays too many.
 */
#define BOARD_STACK_PATTERN 0xa5u
#define BOARD_STACK_HEADROOM 4

static uint8_t *s_stack_lo;
static size_t s_stack_size;
static bool s_stack_reported;

static size_t stack_high_water(void)
{
    size_t untouched = 0;
    while (untouched < s_stack_size && s_stack_lo[untouched] == BOARD_STACK_PATTERN) {
        untouched++;
    }
    return s_stack_size - untouched;
}

static void check_stack(void)
{
    const size_t used = stack_high_water();
    pthread_mutex_lock(&s_lock);
    if (used > s_stats.stack_peak) {
        s_stats.stack_peak = (uint32_t)used;
    }
    pthread_mutex_unlock(&s_lock);
    if (used > CONFIG_ESP_MAIN_TASK_STACK_SIZE && !s_stack_reported) {
        s_stack_reported = true;
        BOARD_COMPLAIN("main task used %zu B of stack; the device has %d B",
                       used, CONFIG_ESP_MAIN_TASK_STACK_SIZE);
    }
}

static uint32_t s_frame;

void vTaskDelay(TickType_t ticks)
{
    drain_transfers();
    check_stack();
    pthread_mutex_lock(&s_lock);
    s_stats.frames++;
    pthread_mutex_unlock(&s_lock);
    if (s_config.frame_hook) {
        s_config.frame_hook(s_frame, s_config.hook_ctx);
    }
    s_frame++;
    sleep_ticks(ticks);
}

board_stats_t board_stats(void)
{
    /* Reading the pattern races only with the stack growing, which can only
     * make the answer a little low. */
    const size_t used = s_stack_lo ? stack_high_water() : 0;
    pthread_mutex_lock(&s_lock);
    if (used > s_stats.stack_peak) {
        s_stats.stack_peak = (uint32_t)used;
    }
    board_stats_t st = s_stats;
    pthread_mutex_unlock(&s_lock);
    return st;
}

static bool s_app_returned;  /* guarded by s_lock */

bool board_app_returned(void)
{
    pthread_mutex_lock(&s_lock);
    const bool r = s_app_returned;
    pthread_mutex_unlock(&s_lock);
    return r;
}

static void *app_thread(void *arg)
{
    (void)arg;
    app_main();
    /* ESP-IDF deletes the main task when app_main returns; nothing else runs
     * here, so the simulated board is done. */
    BOARD_COMPLAIN("app_main returned");
    pthread_mutex_lock(&s_lock);
    s_app_returned = true;
    pthread_mutex_unlock(&s_lock);
    return NULL;
}

void board_start(const board_config_t *config)
{
    s_config = *config;
    clock_gettime(CLOCK_MONOTONIC, &s_boot);
    load_partitions();
    for (size_t i = 0; i < BOARD_PANEL_W * BOARD_PANEL_H; i++) {
        s_panel_px[i] = board_rgb565_to_wire(BOARD_SPLASH_COLOR);
    }

    const size_t page = (size_t)sysconf(_SC_PAGESIZE);
    s_stack_size = ((size_t)CONFIG_ESP_MAIN_TASK_STACK_SIZE * BOARD_STACK_HEADROOM + page - 1) /
                   page * page;
    uint8_t *mem = mmap(NULL, s_stack_size + page, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANON, -1, 0);
    if (mem == MAP_FAILED || mprotect(mem, page, PROT_NONE) != 0) {
        fprintf(stderr, "board: cannot map the task stack\n");
        exit(2);
    }
    s_stack_lo = mem + page;
    memset(s_stack_lo, BOARD_STACK_PATTERN, s_stack_size);

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    if (pthread_attr_setstack(&attr, s_stack_lo, s_stack_size) != 0) {
        fprintf(stderr, "board: cannot use a %zu byte task stack\n", s_stack_size);
        exit(2);
    }
    pthread_t thread;
    if (pthread_create(&thread, &attr, app_thread, NULL) != 0) {
        fprintf(stderr, "board: cannot start the application task\n");
        exit(2);
    }
    pthread_attr_destroy(&attr);
    pthread_detach(thread);
}
