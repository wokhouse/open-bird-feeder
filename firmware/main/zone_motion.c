#include "zone_motion.h"

#include <stdlib.h>
#include <string.h>

static int clamp_int(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

int zone_motion_init(zone_motion_t *z, int w, int h, const zone_cfg_t *cfg)
{
    if (!z || !cfg) return -1;
    memset(z, 0, sizeof(*z));
    z->cfg = *cfg;

    /* Clamp the zone to the frame so a mis-set percentage cannot read past the buffer. */
    z->cfg.x0 = clamp_int(cfg->x0, 0, w);
    z->cfg.x1 = clamp_int(cfg->x1, 0, w);
    z->cfg.y0 = clamp_int(cfg->y0, 0, h);
    z->cfg.y1 = clamp_int(cfg->y1, 0, h);
    if (z->cfg.cell < 1) z->cfg.cell = 1;
    if (z->cfg.trigger_frames < 1) z->cfg.trigger_frames = 1;

    z->w = w;
    z->h = h;
    z->zw = z->cfg.x1 - z->cfg.x0;
    z->zh = z->cfg.y1 - z->cfg.y0;
    if (z->zw <= 0 || z->zh <= 0) return -1;

    z->cols = (z->zw + z->cfg.cell - 1) / z->cfg.cell;
    z->rows = (z->zh + z->cfg.cell - 1) / z->cfg.cell;

    z->hot_bytes = (z->cols * z->rows + 7) / 8;
    z->prev = malloc((size_t)z->zw * (size_t)z->zh);
    z->cell_sum = calloc((size_t)z->cols * (size_t)z->rows, sizeof(uint32_t));
    z->hot_bits = calloc((size_t)z->hot_bytes, 1);
    if (!z->prev || !z->cell_sum || !z->hot_bits) {
        zone_motion_free(z);
        return -1;
    }
    return 0;
}

void zone_motion_reset(zone_motion_t *z)
{
    if (!z) return;
    z->primed = 0;
    z->consec = 0;
}

void zone_motion_free(zone_motion_t *z)
{
    if (!z) return;
    free(z->prev);
    free(z->cell_sum);
    free(z->hot_bits);
    z->prev = NULL;
    z->cell_sum = NULL;
    z->hot_bits = NULL;
}

static void copy_zone_in(zone_motion_t *z, const uint8_t *frame, int stride)
{
    for (int y = 0; y < z->zh; y++) {
        memcpy(z->prev + (size_t)y * z->zw,
               frame + (size_t)(z->cfg.y0 + y) * stride + z->cfg.x0,
               (size_t)z->zw);
    }
}

zone_result_t zone_motion_update(zone_motion_t *z, const uint8_t *frame, int stride)
{
    zone_result_t r = { 0 };
    if (!z || !z->prev || !frame) return r;

    if (!z->primed) {
        copy_zone_in(z, frame, stride);
        z->primed = 1;
        return r;
    }

    const int cell = z->cfg.cell;
    const int cols = z->cols;
    memset(z->cell_sum, 0, (size_t)cols * (size_t)z->rows * sizeof(uint32_t));

    /* Pass 1: per-cell sum of |diff| over the zone. */
    uint64_t total = 0;
    for (int y = 0; y < z->zh; y++) {
        const uint8_t *cur = frame + (size_t)(z->cfg.y0 + y) * stride + z->cfg.x0;
        const uint8_t *ref = z->prev + (size_t)y * z->zw;
        uint32_t *row_sum = z->cell_sum + (size_t)(y / cell) * cols;
        for (int x = 0; x < z->zw; x++) {
            int d = (int)cur[x] - (int)ref[x];
            if (d < 0) d = -d;
            total += (uint32_t)d;
            row_sum[x / cell] += (uint32_t)d;
        }
    }

    const int zone_px = z->zw * z->zh;
    r.global_mean = (int)(total / (uint32_t)zone_px);
    const int bias = z->cfg.light_reject ? r.global_mean : 0;

    /* Pass 2: cells whose mean |diff| beats the threshold count as hot.
     * Record which ones: the caller uploads the grid as a sidecar signal for
     * later non-LLM recognizers (and boxes it onto the probe frame). */
    memset(z->hot_bits, 0, (size_t)z->hot_bytes);
    for (int cy = 0; cy < z->rows; cy++) {
        const int ph = (cy + 1 == z->rows) ? z->zh - cy * cell : cell;
        for (int cx = 0; cx < cols; cx++) {
            const int pw = (cx + 1 == cols) ? z->zw - cx * cell : cell;
            const int mean = (int)(z->cell_sum[(size_t)cy * cols + cx] / (uint32_t)(pw * ph));
            if (mean - bias >= z->cfg.cell_thresh) {
                const int i = cy * cols + cx;
                z->hot_bits[i >> 3] |= (uint8_t)(1u << (i & 7));
                r.hot_cells++;
            }
        }
    }
    r.hot_bits = z->hot_bits;

    r.motion = r.hot_cells >= z->cfg.min_hot_cells;
    z->consec = r.motion ? z->consec + 1 : 0;
    r.consec = z->consec;
    if (z->consec >= z->cfg.trigger_frames) {
        r.trigger = 1;
        z->consec = 0; /* one trigger per motion burst; caller resets the reference anyway */
        z->triggers++;
    }

    copy_zone_in(z, frame, stride);
    z->frames++;
    return r;
}
