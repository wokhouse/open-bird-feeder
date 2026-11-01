// Host test for the zone motion detector. Runs the exact firmware source
// (../main/zone_motion.c) against synthetic 160x120 grayscale frames, and can
// replay real probe frames dumped as raw grayscale for threshold tuning.
//
//   make -C firmware/test run
//   firmware/test/zone_motion_test --raw a.raw b.raw ...   (160x120 raw)
//
// Exit code 0 = every assertion passed.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zone_config.h"
#include "zone_motion.h"

#define FW 160
#define FH 120
#define PX (FW * FH)

static uint32_t s_rng = 12345;

static int rnd(int span) /* deterministic +-span */
{
    s_rng = s_rng * 1103515245u + 12345u;
    return (int)((s_rng >> 16) % (uint32_t)(2 * span + 1)) - span;
}

static zone_cfg_t cfg_from_config(void)
{
    zone_cfg_t c = {
        .x0 = ZONE_X0_PCT * FW / 100,
        .x1 = ZONE_X1_PCT * FW / 100,
        .y0 = ZONE_Y0_PCT * FH / 100,
        .y1 = ZONE_Y1_PCT * FH / 100,
        .cell = ZONE_CELL,
        .cell_thresh = ZONE_CELL_THRESH,
        .min_hot_cells = ZONE_MIN_HOT_CELLS,
        .trigger_frames = ZONE_TRIGGER_FRAMES,
        .light_reject = ZONE_LIGHT_REJECT,
    };
    return c;
}

/* Seed-like base scene: brightness ramp plus fine texture. */
static void make_base(uint8_t *f)
{
    s_rng = 12345;
    for (int y = 0; y < FH; y++) {
        for (int x = 0; x < FW; x++) {
            int v = 90 + (y * 40) / FH + (x * 15) / FW + rnd(18);
            f[y * FW + x] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
        }
    }
}

static void add_blob(uint8_t *f, int cx, int cy, int hw, int hh, int delta)
{
    for (int y = cy - hh; y <= cy + hh; y++) {
        if (y < 0 || y >= FH) continue;
        for (int x = cx - hw; x <= cx + hw; x++) {
            if (x < 0 || x >= FW) continue;
            /* ellipse, so the blob has real edges */
            int dx = x - cx, dy = y - cy;
            if (dx * dx * hh * hh + dy * dy * hw * hw > hw * hw * hh * hh) continue;
            int v = (int)f[y * FW + x] + delta;
            f[y * FW + x] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
        }
    }
}

static void shift_all(uint8_t *f, int delta)
{
    for (int i = 0; i < PX; i++) {
        int v = (int)f[i] + delta;
        f[i] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
    }
}

static void add_noise(uint8_t *f, int span)
{
    s_rng = 999;
    for (int i = 0; i < PX; i++) {
        int v = (int)f[i] + rnd(span);
        f[i] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
    }
}

static int s_fail;

static void check(const char *name, zone_result_t r, int want_motion, int want_trigger)
{
    const int ok = (!!r.motion == !!want_motion) && (!!r.trigger == !!want_trigger);
    if (!ok) s_fail++;
    printf("%-28s hot=%3d global=%3d motion=%d trigger=%d  %s\n",
           name, r.hot_cells, r.global_mean, r.motion, r.trigger, ok ? "ok" : "FAIL");
}

static zone_motion_t s_zone;

static zone_result_t step(const uint8_t *f)
{
    return zone_motion_update(&s_zone, f, FW);
}

static void begin(void)
{
    zone_cfg_t c = cfg_from_config();
    zone_motion_free(&s_zone);
    if (zone_motion_init(&s_zone, FW, FH, &c) != 0) {
        fprintf(stderr, "zone_motion_init failed\n");
        exit(2);
    }
}

static void synthetic_tests(void)
{
    static uint8_t base[PX], cur[PX];
    zone_result_t r;

    printf("zone=%d,%d..%d,%d cell=%d thresh=%d min_hot=%d frames=%d light_reject=%d\n",
           s_zone.cfg.x0, s_zone.cfg.y0, s_zone.cfg.x1, s_zone.cfg.y1, ZONE_CELL,
           ZONE_CELL_THRESH, ZONE_MIN_HOT_CELLS, ZONE_TRIGGER_FRAMES, ZONE_LIGHT_REJECT);

    /* 1. still scene */
    begin();
    make_base(base);
    step(base);
    memcpy(cur, base, PX);
    check("still scene", step(cur), 0, 0);

    /* 2. bird lands on the seed: 18x12 blob inside the zone */
    begin();
    step(base);
    memcpy(cur, base, PX);
    add_blob(cur, 80, 96, 9, 6, -55);
    r = step(cur);
    check("bird in zone (frame 1)", r, 1, 0);
    add_blob(cur, 84, 98, 9, 6, -55); /* keeps moving */
    check("bird in zone (frame 2)", step(cur), 1, 1);

    /* 3. bird-sized blob in the background only */
    begin();
    step(base);
    memcpy(cur, base, PX);
    add_blob(cur, 80, 40, 9, 6, -55);
    check("blob above zone", step(cur), 0, 0);

    /* 4. a leaf-sized blob above the zone */
    begin();
    step(base);
    memcpy(cur, base, PX);
    add_blob(cur, 30, 25, 14, 10, 40);
    check("leaf above zone", step(cur), 0, 0);

    /* 5. exposure / cloud jump over the whole frame */
    begin();
    step(base);
    memcpy(cur, base, PX);
    shift_all(cur, 12);
    check("global +12", step(cur), 0, 0);
    shift_all(cur, 30);
    check("global +42", step(cur), 0, 0);

    /* 6. sensor noise only */
    begin();
    step(base);
    memcpy(cur, base, PX);
    add_noise(cur, 8);
    check("noise +-8", step(cur), 0, 0);
    add_noise(cur, 14);
    check("noise +-14", step(cur), 0, 0);

    /* 7. a small seed being picked up (10x7 blob) */
    begin();
    step(base);
    memcpy(cur, base, PX);
    add_blob(cur, 40, 100, 5, 3, -45);
    r = step(cur);
    printf("%-28s hot=%3d global=%3d motion=%d trigger=%d  (info)\n",
           "small 10x7 object in zone", r.hot_cells, r.global_mean, r.motion, r.trigger);

    /* 8. slow drift: 3 frames of small movement must still trigger */
    begin();
    step(base);
    memcpy(cur, base, PX);
    add_blob(cur, 70, 96, 9, 6, -55);
    r = step(cur);
    int triggered = r.trigger;
    for (int i = 1; i < 3 && !triggered; i++) {
        memcpy(cur, base, PX);
        add_blob(cur, 70 + 2 * i, 96, 9, 6, -55);
        triggered = step(cur).trigger;
    }
    printf("%-28s triggered=%d  %s\n", "slow drift over 3 frames", triggered,
           triggered ? "ok" : "FAIL");
    if (!triggered) s_fail++;
}

/* Print the zone cell grid for one synthetic bird frame. '#' = hot cell,
 * digit = cell score / 10. Tuning aid, not an assertion. */
static void cell_dump(void)
{
    static uint8_t base[PX], cur[PX];
    begin();
    make_base(base);
    step(base);
    memcpy(cur, base, PX);
    add_blob(cur, 80, 96, 9, 6, -55);
    zone_result_t r = step(cur);
    printf("bird in zone: hot=%d global=%d\n", r.hot_cells, r.global_mean);

    const zone_cfg_t c = s_zone.cfg;
    const int cols = s_zone.cols, rows = s_zone.rows;
    for (int cy = 0; cy < rows; cy++) {
        const int ph = (cy + 1 == rows) ? s_zone.zh - cy * c.cell : c.cell;
        for (int cx = 0; cx < cols; cx++) {
            const int pw = (cx + 1 == cols) ? s_zone.zw - cx * c.cell : c.cell;
            const int mean = (int)(s_zone.cell_sum[(size_t)cy * cols + cx] / (uint32_t)(pw * ph));
            const int score = mean - (c.light_reject ? r.global_mean : 0);
            if (score >= c.cell_thresh) putchar('#');
            else if (score > 0) putchar('0' + (score / 10 > 9 ? 9 : score / 10));
            else putchar('.');
        }
        putchar('\n');
    }
    zone_motion_free(&s_zone);
}

/* Replay raw grayscale probe frames dumped from the device. */
static void raw_mode(int nfiles, char **files)
{
    begin();
    zone_cfg_t c = s_zone.cfg;
    printf("zone=%d,%d..%d,%d cell=%d thresh=%d min_hot=%d\n", c.x0, c.y0, c.x1, c.y1,
           c.cell, c.cell_thresh, c.min_hot_cells);

    static uint8_t buf[PX];
    for (int i = 0; i < nfiles; i++) {
        FILE *fp = fopen(files[i], "rb");
        if (!fp) {
            fprintf(stderr, "%s: cannot open\n", files[i]);
            exit(2);
        }
        size_t got = fread(buf, 1, PX, fp);
        fclose(fp);
        if (got != PX) {
            fprintf(stderr, "%s: %zu bytes, expected %d\n", files[i], got, PX);
            exit(2);
        }
        zone_result_t r = step(buf);
        printf("%-28s hot=%3d global=%3d motion=%d trigger=%d\n", files[i], r.hot_cells,
               r.global_mean, r.motion, r.trigger);
    }
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--cells") == 0) {
        cell_dump();
        return 0;
    }
    if (argc > 2 && strcmp(argv[1], "--raw") == 0) {
        begin();
        zone_motion_free(&s_zone);
        raw_mode(argc - 2, argv + 2);
    } else {
        begin();
        synthetic_tests();
        zone_motion_free(&s_zone);
    }
    printf("%s\n", s_fail ? "FAILED" : "all assertions passed");
    return s_fail ? 1 : 0;
}
