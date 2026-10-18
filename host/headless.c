/*
 * headless.c - boot a Mac Plus under umac with no display, on emulated time.
 *
 * Feasibility probe for Mosaintosh: does this ROM + disk reach the Finder at
 * the panel's resolution, and how fast is the emulation compared with a real
 * 7.83 MHz Mac Plus? Time is the emulator's own (umac_loop() advances it by a
 * fixed quantum), so a run is deterministic and the screenshots land at the
 * same emulated moment every time.
 *
 * Screenshots are PBM (P4): MSB-first 1 bpp with 1 = black, which is the Mac
 * framebuffer byte for byte.
 *
 * usage: headless -r rom -d disk [-t seconds] [-s every_seconds] [-o prefix]
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "machw.h"
#include "rom.h"
#include "umac.h"

/* Emulated microseconds per umac_loop() call (UMAC_EXECLOOP_QUANTUM). */
#define QUANTUM_US 5000
#define VSYNC_US 16667

static void *map_file(const char *path, size_t *size, int writable_copy)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        perror(path);
        exit(1);
    }
    struct stat sb;
    fstat(fd, &sb);
    *size = (size_t)sb.st_size;
    /* Private copy: the Mac may write, the file never changes. */
    void *p = mmap(NULL, *size, PROT_READ | (writable_copy ? PROT_WRITE : 0), MAP_PRIVATE, fd, 0);
    close(fd);
    if (p == MAP_FAILED) {
        perror("mmap");
        exit(1);
    }
    return p;
}

static void write_pbm(const char *path, const uint8_t *fb)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        perror(path);
        return;
    }
    fprintf(f, "P4\n%d %d\n", DISP_WIDTH, DISP_HEIGHT);
    fwrite(fb, 1, DISP_WIDTH * DISP_HEIGHT / 8, f);
    fclose(f);
}

static double wall_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
    const char *rom_path = NULL, *disc_path = NULL, *prefix = "shot";
    int seconds = 60, every = 10;
    int ch;
    while ((ch = getopt(argc, argv, "r:d:t:s:o:")) != -1) {
        switch (ch) {
        case 'r': rom_path = optarg; break;
        case 'd': disc_path = optarg; break;
        case 't': seconds = atoi(optarg); break;
        case 's': every = atoi(optarg); break;
        case 'o': prefix = optarg; break;
        default:
            fprintf(stderr, "usage: %s -r rom -d disk [-t s] [-s every_s] [-o prefix]\n", argv[0]);
            return 1;
        }
    }
    if (!rom_path || every <= 0) {
        fprintf(stderr, "a ROM is required\n");
        return 1;
    }

    size_t rom_size, disc_size = 0;
    uint8_t *rom = map_file(rom_path, &rom_size, 1);
    if (rom_size != ROM_SIZE) {
        fprintf(stderr, "ROM is %zu bytes; a Mac Plus ROM is %d\n", rom_size, ROM_SIZE);
        return 1;
    }
    if (rom_patch(rom)) {
        fprintf(stderr, "ROM patch failed (umac needs a Mac Plus v3 ROM)\n");
        return 1;
    }

    uint8_t *ram = calloc(1, RAM_SIZE);
    disc_descr_t discs[DISC_NUM_DRIVES] = { 0 };
    if (disc_path) {
        discs[0].base = map_file(disc_path, &disc_size, 1);
        discs[0].size = (unsigned int)disc_size;
    }
    printf("Mac Plus: %d KB RAM, %dx%d, disc %zu KB\n", RAM_SIZE / 1024, DISP_WIDTH, DISP_HEIGHT,
           disc_size / 1024);

    umac_init(ram, rom, discs);

    const double t0 = wall_seconds();
    uint64_t now_us = 0, next_vsync = VSYNC_US, next_1hz = 1000000, next_shot = (uint64_t)every * 1000000;
    const uint64_t end_us = (uint64_t)seconds * 1000000;
    while (now_us < end_us) {
        if (umac_loop()) {
            printf("emulator stopped at %.2f s\n", (double)now_us / 1e6);
            break;
        }
        now_us += QUANTUM_US;
        if (now_us >= next_vsync) {
            umac_vsync_event();
            next_vsync += VSYNC_US;
        }
        if (now_us >= next_1hz) {
            umac_1hz_event();
            next_1hz += 1000000;
        }
        if (now_us >= next_shot) {
            char path[256];
            snprintf(path, sizeof path, "%s_%03ds.pbm", prefix, (int)(now_us / 1000000));
            write_pbm(path, ram + umac_get_fb_offset());
            next_shot += (uint64_t)every * 1000000;
        }
    }
    const double wall = wall_seconds() - t0;
    printf("%.1f s emulated in %.2f s wall: %.1fx a 7.83 MHz Mac Plus\n", (double)now_us / 1e6, wall,
           (double)now_us / 1e6 / wall);
    return 0;
}
