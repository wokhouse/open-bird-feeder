// Zone motion detector: frame differencing restricted to a rectangle of the
// probe frame, with global-illumination rejection.
//
// Pure C on purpose: no ESP-IDF includes, so firmware/test/ can build it with
// the host gcc and run the same code the device runs.
//
// Usage per frame:
//   zone_motion_t z;  zone_motion_init(&z, w, h, &cfg);
//   zone_result_t r = zone_motion_update(&z, fb->buf, stride);
//   if (r.trigger) { ...capture photo... zone_motion_reset(&z); }
// The first update after init/reset only stores the reference frame.

#pragma once

#include <stdint.h>

typedef struct {
    int x0, y0, x1, y1;  /* zone rectangle in probe pixels, [x0,x1) x [y0,y1) */
    int cell;            /* cell side in pixels */
    int cell_thresh;     /* mean |diff| inside a cell to call the cell "hot" */
    int min_hot_cells;   /* hot cells needed for the frame to count as motion */
    int trigger_frames;  /* consecutive motion frames needed to trigger */
    int light_reject;    /* 1 = subtract the zone-wide mean diff from every cell */
} zone_cfg_t;

typedef struct {
    int hot_cells;   /* cells over cell_thresh this frame */
    int global_mean; /* zone-wide mean |diff|, 0..255 */
    int motion;      /* this frame counted as motion */
    int consec;      /* consecutive motion frames so far */
    int trigger;     /* 1 on the frame that fires a capture */
} zone_result_t;

typedef struct {
    zone_cfg_t cfg;
    int w, h;      /* probe frame size */
    int cols, rows;/* cell grid covering the zone */
    int zw, zh;    /* zone size in pixels */
    uint8_t *prev; /* zone pixels of the previous frame, zw*zh */
    uint32_t *cell_sum;
    int primed;
    int consec;
    uint32_t frames, triggers;
} zone_motion_t;

/* Returns 0 on success, -1 on a bad config or out of memory. */
int zone_motion_init(zone_motion_t *z, int w, int h, const zone_cfg_t *cfg);

/* Drop the reference frame and the trigger state (call after a capture). */
void zone_motion_reset(zone_motion_t *z);

/* Feed one frame. stride is the byte distance between rows of frame. */
zone_result_t zone_motion_update(zone_motion_t *z, const uint8_t *frame, int stride);

void zone_motion_free(zone_motion_t *z);
