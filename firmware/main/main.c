// Phase 5: XIAO ESP32S3 Sense birdcam — zone motion gating.
//
// The camera runs in low-res grayscale "probe" mode. Each probe frame is
// compared with the previous one inside the seed zone only (zone_config.h), so
// leaves, cars and buildings moving in the background do not count. When the
// zone changes for ZONE_TRIGGER_FRAMES frames in a row the camera switches to
// JPEG XGA, grabs one photo, uploads it, and returns to probe mode.
// A heartbeat photo every HEARTBEAT_MS keeps the timer path alive for
// troubleshooting (set HEARTBEAT_MS to 0 to disable).
// Wi-Fi is dial-before-call (phase 5c): the radio stays off while probing.
// Each capture cycle dials once, waits WIFI_CONNECT_WAIT_MS for an IP,
// uploads photo + probe frame, then powers the radio down again. On dial
// failure the whole cycle is skipped and the motion gate backs off
// exponentially (refractory << consecutive skips, capped ~2 min).
// Daytime power: windowed idle. When the zone stays cold the sensor is
// parked between detection windows with its datasheet standby bit (OV3660
// 0x3008 bit6, measured 1.5 mA parked vs 22-38 mA streaming; OV2640 0x3009
// bit4, register-retentive per datasheet) instead of streaming all day.
// Any hot cell returns to continuous probing for PROBE_HOT_HOLD_MS, so
// wildlife is always handled by the 5a continuous loop unchanged; only the
// empty-feeder idle is duty-cycled. The periodic "probing:" log line
// reports active-vs-parked duty — it is the field power gauge.
//
// Credentials/URL live in secrets.h. Console is USB-CDC only.

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_camera.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_heap_caps.h" /* ps_malloc: put the video chunk buffer in PSRAM */
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "img_converters.h"
#include "nvs_flash.h"

#include "secrets.h"
#include "zone_config.h"
#include "zone_motion.h"

static const char *TAG = "birdcam";

/* Camera pin map fixed by the XIAO ESP32S3 Sense board (Seeed wiki). */
#define CAM_PIN_XCLK  GPIO_NUM_10
#define CAM_PIN_SIOD  GPIO_NUM_40
#define CAM_PIN_SIOC  GPIO_NUM_39
#define CAM_PIN_D7    GPIO_NUM_48 /* Y9 */
#define CAM_PIN_D6    GPIO_NUM_11 /* Y8 */
#define CAM_PIN_D5    GPIO_NUM_12 /* Y7 */
#define CAM_PIN_D4    GPIO_NUM_14 /* Y6 */
#define CAM_PIN_D3    GPIO_NUM_16 /* Y5 */
#define CAM_PIN_D2    GPIO_NUM_18 /* Y4 */
#define CAM_PIN_D1    GPIO_NUM_17 /* Y3 */
#define CAM_PIN_D0    GPIO_NUM_15 /* Y2 */
#define CAM_PIN_VSYNC GPIO_NUM_38
#define CAM_PIN_HREF  GPIO_NUM_47
#define CAM_PIN_PCLK  GPIO_NUM_13

/* Probe (motion) mode: small grayscale frames, several per second. Must match
 * the zone geometry only through percentages, so changing this size is safe. */
#define PROBE_FRAMESIZE   FRAMESIZE_QQVGA /* 160x120 */
#define PROBE_PIXFORMAT   PIXFORMAT_GRAYSCALE
#define PROBE_INTERVAL_MS 150             /* ~6.6 fps */
#define PROBE_SETTLE      2               /* frames discarded after each switch */
#define PROBE_JPEG_QUALITY 40             /* upload of the frame that triggered */

/* Windowed idle (daytime power): a cold zone parks the sensor between
 * detection windows instead of streaming continuously. One window =
 * unpark -> discard PROBE_SETTLE wake frames -> probe WINDOW_FRAMES frames.
 * Any hot cell (bird/paw/branch edge in the zone) extends a continuous
 * hot-hold, after which windows resume. These are the field tuning knobs;
 * the duty percent in the periodic log shows what they cost. */
#define PROBE_IDLE_MS     1500  /* sensor parked this long between windows */
#define WINDOW_FRAMES     2     /* detection frames grabbed per window */
#define PROBE_HOT_HOLD_MS 2500  /* stay continuous this long after a hot cell */

/* Photo mode: one XGA frame per capture. */
#define PHOTO_FRAMESIZE   FRAMESIZE_XGA /* 1024x768 */
#define PHOTO_JPEG_QUALITY 12
#define PHOTO_SETTLE      3 /* let auto-exposure converge after the mode switch */

/* Phase 7a/7c: motion captures also stream a clip to the Pi (no encoder or
 * storage on the device — raw JPEG frames; the Pi detects the quiet and
 * encodes, and only keeps the clip when the event's decision model says
 * bird). The device-side caps are safety limits; the Pi's zone detector
 * (same zone_config.h) plus the live-window rules decide when a session
 * really ends and say so with HTTP 204. A live window (bird verdict or
 * browser request) makes the Pi hold 204 back, so sessions run longer
 * than the quiet rules alone — VIDEO_MAX_FRAMES is the hard ceiling for
 * both; keep it >= ingest/video.py's VIDEO_EXTENDED_MAX. */
#define VIDEO_MAX_FRAMES      300           /* 60 s hard ceiling at VIDEO_FRAME_MS */
#define VIDEO_FRAME_MS        200           /* 5 fps; Pi encodes at this rate */
#define VIDEO_CHUNK_FRAMES    5             /* one POST per chunk (~330 KB) */
#define VIDEO_CHUNK_MAX_BYTES (700 * 1024)  /* flush early if frames are big */

/* Phase 7c: ask the gateway between captures whether anyone wants live
 * video (model verdict or browser request — the gateway decides, this
 * device obeys). Each poll dials the radio ~2 s: at 180000 ms that is
 * ~0.05% duty, negligible next to the probe baseline. 0 = only learn
 * about live windows at the next capture's response header. */
#define LIVE_POLL_MS 180000

#define REFRACTORY_MS 4000  /* ignore motion this long after a capture */
#define HEARTBEAT_MS  900000 /* 15 min: one photo when nothing moves (0 = off) */

/* Wi-Fi dial (phase 5c): one IP budget, then exponential skip backoff. */
#define WIFI_CONNECT_WAIT_MS 8000  /* IP budget after dialing */
#define SKIP_BACKOFF_MAX 5         /* motion gate: refractory << skips, cap 128 s */
#define LOG_EVERY_FRAMES 400 /* one info line per ~60 s of probing */

/* Night hold — the FAST feed-forward half of the bank-keeper (the duty
 * floor below is the slow feedback half; it would eventually latch a dark
 * night too, but ~1 DUTY_TAU late, so this path exists to act at dusk).
 * The same luma signal closes the CAPTURE gate: after dark the scene has
 * no wildlife in it, and the only zone "motion" left is traffic — those
 * photos are black and worthless (field: 33 uploads at luma 10-40,
 * 2026-11-08).
 * Mobile power banks cut their output below a low-load threshold
 * (~50 mA avg at 5 V); windowed night idle sits under that and the bank
 * trips off mid-night. When the scene stays dark, stop parking and hold
 * the full-stream load (~0.3-0.4 W at the bank) that kept banks alive in
 * pre-5c firmware. Luma thresholds verified in the 5d night-gate tests:
 * camera AEC lands real night scenes at values of 20-30, never 0. */
#define NIGHT_LUMA_ENTER   40   /* sustained whole-frame mean luma below this
                                 * = dark: captures stop, and (when windowing
                                 * is allowed) parking stops too. Uses
                                 * frame_mean_luma, NOT the zone |diff| mean —
                                 * see its comment */
#define NIGHT_LUMA_EXIT    60   /* any frame at/above this ends the night */
#define NIGHT_DARK_FRAMES  30   /* detection frames before declaring night:
                                 * ≈35 s while windowed but only ≈4.5 s while
                                 * streaming — mode-dependent; make wall-clock
                                 * when the idle policy is unified (PLAN 5c) */

/* Field reality (2026-11-08): this pole's power bank still trips its
 * low-load cutoff even with the duty floor below — it wants continuous
 * load. Set to 1: the camera sensor is NEVER parked, all day. Every
 * other power optimization (dial-before-call, capture pipeline, and the
 * whole windowed-idle machinery for a friendlier bank) stays compiled
 * and armed; only sensor parking is suppressed. Set to 0 if a bank
 * without a low-load auto-off is fitted, and windowed idle returns. */
#define INCREASE_POWER_CONSUMPTION 1

/* Bank-keeper duty floor — the SLOW feedback half of the bank-keeper
 * (the night luma hold above is the fast feed-forward half). A cold
 * windowed DAY sinks average draw under the
 * same bank low-load cutoff as the night idle (field report 2026-11-07:
 * daytime trips the bank too). Streaming duty is tracked with an EMA
 * (fixed-point, DUTY_SCALE = 100%) on the bank's trip timescale; below the floor,
 * parking is skipped (continuous probe) until duty recovers with
 * hysteresis. Busy scenes clear the floor by themselves and still get
 * full windowed savings. Field-tuning knob: MIN_DUTY_PCT. */
#define MIN_DUTY_PCT   45      /* hold streaming duty at/above this while idle */
#define DUTY_HYST_PCT  10      /* windowing resumes at MIN+HYST */
#define DUTY_TAU_MS    120000  /* bank cutoff timers are minutes, not seconds */

/* Fixed-point duty arithmetic. Scale is 65536 = 100% (Q16): at Q8 the
 * realistic 150-1500 ms segment folds truncate toward zero ASYMMETRICALLY
 * and the controller drifts below its own math (field-caught once, PLAN
 * 5c-1d); at Q16 even a single 150 ms frame fold carries exact units.
 * Never hand-write these conversions at a call site. */
#define DUTY_SCALE       65536u
#define DUTY_SCALED(pct) ((uint32_t)(((uint64_t)(pct) * DUTY_SCALE) / 100))
#define DUTY_PCT(ema)    ((unsigned)(((uint64_t)(ema) * 100) / DUTY_SCALE))

/* Cap on the hex-encoded hot-cell bitmap we put in the X-Motion-Grid header
 * (150 bytes = 1200 cells; today's grid is 480). Over-cap = no header. */
#define GRID_HEX_MAX_BYTES 150

#define WIFI_CONNECTED_BIT BIT0

static EventGroupHandle_t s_wifi_events;
/* Written from the event-loop task, read from the app task: volatile so the
 * cross-task checks stay honest under -O2. The event-group bit is the sync
 * point; these flags only mirror driver state. */
static volatile bool s_wifi_wanted;   /* retry connects while true */
static volatile bool s_wifi_started;  /* esp_wifi driver is up */
static volatile bool s_wifi_up;       /* got an ip */
static char s_device_id[24];
/* One id per capture cycle, sent on the trigger photo AND its video
 * chunks: it is the join key the Pi uses to ask the decision model
 * whether this footage is a bird. */
static char s_event_id[64];
static volatile int s_live_secs;  /* X-Live from the gateway's response */
static zone_motion_t s_zone;
static bool s_zone_ready;

/* One monotonic millisecond clock. Every timestamp in the probe loop goes
 * through it; the loop's snapshots are named for WHEN they were taken, on
 * purpose: an upload consumes seconds, so tail pacing must see post-capture
 * time and must not be "unified" with the frame-time snapshot. */
static inline int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

/* Discard n frames after a sensor state change (format switch, unpark):
 * the first frames can be partial or stale. Returns false if a grab came
 * back NULL; callers decide what that means. The 100 ms spacing matches
 * the probe frame period. */
static bool discard_frames(int n)
{
    for (int i = 0; i < n; i++) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) return false;
        esp_camera_fb_return(fb);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return true;
}

static esp_err_t camera_start(pixformat_t format, framesize_t size, int quality, int settle)
{
    camera_config_t config = {
        .pin_pwdn     = -1,
        .pin_reset    = -1,
        .pin_xclk     = CAM_PIN_XCLK,
        .pin_sccb_sda = CAM_PIN_SIOD,
        .pin_sccb_scl = CAM_PIN_SIOC,
        .pin_d7       = CAM_PIN_D7,
        .pin_d6       = CAM_PIN_D6,
        .pin_d5       = CAM_PIN_D5,
        .pin_d4       = CAM_PIN_D4,
        .pin_d3       = CAM_PIN_D3,
        .pin_d2       = CAM_PIN_D2,
        .pin_d1       = CAM_PIN_D1,
        .pin_d0       = CAM_PIN_D0,
        .pin_vsync    = CAM_PIN_VSYNC,
        .pin_href     = CAM_PIN_HREF,
        .pin_pclk     = CAM_PIN_PCLK,
        .xclk_freq_hz = 20000000,
        .ledc_timer   = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,
        .pixel_format = format,
        .frame_size   = size,
        .jpeg_quality = quality,
        .fb_count     = 2,
        .fb_location  = CAMERA_FB_IN_PSRAM,
        .grab_mode    = CAMERA_GRAB_LATEST,
    };

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "camera init failed: %s", esp_err_to_name(err));
        return err;
    }
    /* The first frames after a format switch can be partial or stale. */
    (void)discard_frames(settle);
    return ESP_OK;
}

static void camera_stop(void)
{
    esp_camera_deinit();
    vTaskDelay(pdMS_TO_TICKS(100));
}

/* Fold a closed streaming-duty segment into the bank-keeper EMA (scale:
 * DUTY_SCALE = 100%, time constant DUTY_TAU_MS). Segments longer than the
 * time constant saturate the estimate — exact math is pointless past that. */
static uint32_t duty_ema_advance(uint32_t ema, bool active, int64_t elapsed_ms)
{
    if (elapsed_ms <= 0) return ema;
    if (elapsed_ms >= DUTY_TAU_MS) return active ? DUTY_SCALE : 0;
    uint64_t fold = (uint64_t)(active ? DUTY_SCALE - ema : ema)
                    * (uint64_t)elapsed_ms / DUTY_TAU_MS;
    return active ? (uint32_t)(ema + fold) : (uint32_t)(ema - fold);
}

/* Absolute scene luma for the night bank-keeper: zone_result_t.global_mean is
 * the mean DIFFERENCE (0 = static scene, day or night), not light level.
 * Stride 7 over the grayscale frame = 2700 samples per QQVGA: negligible. */
static int frame_mean_luma(const uint8_t *px, size_t len)
{
    unsigned sum = 0; size_t n = 0;
    for (size_t i = 0; i < len; i += 7) { sum += px[i]; n++; }
    return n ? (int)(sum / n) : 0;
}

/* Park/unpark the sensor's own streaming via its datasheet standby bit,
 * leaving esp_camera, SCCB and the DVP receiver up — register contents are
 * retained on both supported sensors, so a window re-opens with just a
 * set_reg plus discarded wake frames:
 *   OV3660: SYSTEM_CTROL0 (0x3008) bit6 — verified on this device in the
 *     night-gate work: ~1.5 mA parked vs 22-38 mA while streaming.
 *   OV2640: COM2 (0x3009) bit4 — datasheet: standby keeps all register
 *     content, DVP stops. set_reg() takes (bank<<8)|addr for OV2640, so
 *     0x3009 is bank 0, reg 0x09; the driver does the bank switch.
 * Returns ESP_OK, ESP_ERR_NOT_SUPPORTED (no known standby bit), or a driver
 * error. */
static esp_err_t sensor_set_parked(bool parked)
{
    sensor_t *s = esp_camera_sensor_get();
    if (!s || !s->set_reg) return ESP_ERR_NOT_SUPPORTED;
    int reg, mask, val;
    if (s->id.PID == OV3660_PID)      { reg = 0x3008; mask = 0x40; val = parked ? 0x40 : 0x00; }
    else if (s->id.PID == OV2640_PID) { reg = 0x3009; mask = 0x10; val = parked ? 0x10 : 0x00; }
    else return ESP_ERR_NOT_SUPPORTED;
    return s->set_reg(s, reg, mask, val) == 0 ? ESP_OK : ESP_FAIL;
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_wifi_wanted) {
            esp_err_t err = esp_wifi_connect();
            if (err != ESP_OK) ESP_LOGE(TAG, "connect from STA_START: %s", esp_err_to_name(err));
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (!s_wifi_started) return; /* deferred event from our own esp_wifi_stop */
        s_wifi_up = false;
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
        if (s_wifi_wanted) {
            ESP_LOGW(TAG, "wifi disconnected, retrying");
            esp_err_t err = esp_wifi_connect();
            if (err != ESP_OK) ESP_LOGE(TAG, "connect retry: %s", esp_err_to_name(err));
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        /* A late GOT_IP after wifi_power_down() must not resurrect "up":
         * the radio is off, and only wifi_connect_start() may dial it. */
        if (!s_wifi_wanted) return;
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got ip: " IPSTR, IP2STR(&event->ip_info.ip));
        s_wifi_up = true;
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

/* Bring up the wifi stack WITHOUT connecting: the radio stays off until a
 * capture cycle dials it (phase 5c, ~0.2-0.3 W saved while probing). */
static void wifi_init(void)
{
    s_wifi_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid, WIFI_SSID, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, WIFI_PASS, sizeof(wc.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
}

/* Power the radio on and start connecting. Completion is the GOT_IP bit;
 * wait for it with wifi_ready(). Returns non-ESP_OK when the dial itself
 * failed, so callers can skip the IP wait entirely. */
static esp_err_t wifi_connect_start(void)
{
    if (s_wifi_up) return ESP_OK;
    s_wifi_wanted = true;
    xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
    if (s_wifi_started) {
        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
            ESP_LOGE(TAG, "wifi connect: %s", esp_err_to_name(err));
            return err;
        }
        return ESP_OK; /* ESP_ERR_WIFI_CONN: a connect is already in flight */
    }
    esp_err_t err = esp_wifi_start(); /* STA_START event connects via handler */
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi start: %s", esp_err_to_name(err));
        s_wifi_wanted = false;
        return err;
    }
    s_wifi_started = true;
    return ESP_OK;
}

/* Block until an IP is held (returns at once when already connected). */
static bool wifi_ready(int timeout_ms)
{
    EventBits_t bits = xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT,
                                           pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    return (bits & WIFI_CONNECTED_BIT) != 0;
}

/* Power the radio back down; the resting state while probing. */
static void wifi_power_down(void)
{
    s_wifi_wanted = false;
    s_wifi_up = false;
    xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
    if (s_wifi_started) {
        esp_wifi_disconnect(); /* result is noise; the stop below is what counts */
        esp_err_t err = esp_wifi_stop();
        if (err != ESP_OK) {
            /* keep s_wifi_started set: the flags must mirror the driver */
            ESP_LOGE(TAG, "wifi stop: %s", esp_err_to_name(err));
        } else {
            s_wifi_started = false;
        }
    }
}

/* POST a buffer to the ingest service; returns the HTTP status code,
 * 0 = transport failure. hot/global < 0 = no motion headers.
 * grid non-NULL = also send it as X-Motion-Grid (probe frames only).
 * event non-NULL = X-Event-Id (the capture cycle this belongs to).
 * vsid non-NULL = video chunk: X-Event-Id vsid, X-Session-Kind skind,
 * X-Video-Seq vseq. Any X-Live response header is recorded in
 * s_live_secs (seconds of live streaming the gateway wants). */
static int upload_http(const uint8_t *buf, size_t len, const char *kind,
                       const char *content_type, int hot, int global,
                       const char *grid, const char *vsid, int vseq,
                       const char *event, const char *skind)
{
    char url[128];
    snprintf(url, sizeof(url), "%s/upload?kind=%s", INGEST_BASE, kind);

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 10000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return 0;

    char num[16];
    esp_http_client_set_header(client, "Content-Type", content_type);
    esp_http_client_set_header(client, "X-Device-Id", s_device_id);
    if (hot >= 0) {
        snprintf(num, sizeof(num), "%d", hot);
        esp_http_client_set_header(client, "X-Motion-Hot", num);
    }
    if (global >= 0) {
        snprintf(num, sizeof(num), "%d", global);
        esp_http_client_set_header(client, "X-Motion-Global", num);
    }
    if (grid) esp_http_client_set_header(client, "X-Motion-Grid", grid);
    if (event) esp_http_client_set_header(client, "X-Event-Id", event);
    if (vsid) {
        esp_http_client_set_header(client, "X-Event-Id", vsid);
        if (skind) esp_http_client_set_header(client, "X-Session-Kind", skind);
        snprintf(num, sizeof(num), "%d", vseq);
        esp_http_client_set_header(client, "X-Video-Seq", num);
    }
    esp_http_client_set_post_field(client, (const char *)buf, len);

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "upload %s failed: %s", kind, esp_err_to_name(err));
        status = 0;
    } else if (status != 200 && status != 204) {
        ESP_LOGE(TAG, "upload %s failed: status=%d", kind, status);
    }
    if (status == 200 || status == 204) {
        /* IDF 5.2: esp_http_client_get_header(client, key, &value) */
        char *lv = NULL;
        if (esp_http_client_get_header(client, "X-Live", &lv) == ESP_OK && lv)
            s_live_secs = (int)strtol(lv, NULL, 10);
    }
    esp_http_client_cleanup(client);
    return status;
}

static bool upload_bytes(const uint8_t *buf, size_t len, const char *kind,
                         const char *content_type, int hot, int global,
                         const char *grid, const char *event)
{
    return upload_http(buf, len, kind, content_type, hot, global, grid,
                       NULL, -1, event, NULL) == 200;
}

/* Draw 1px white outlines on the hot cells of a grayscale probe fb, so the
 * uploaded tuning frame shows what fired without needing the sidecar. */
static void draw_hot_cells(camera_fb_t *fb, const zone_result_t *r)
{
    if (!r->hot_bits) return;
    const int cell = s_zone.cfg.cell;
    const int cols = s_zone.cols, rows = s_zone.rows;
    for (int cy = 0; cy < rows; cy++) {
        for (int cx = 0; cx < cols; cx++) {
            const int i = cy * cols + cx;
            if (!(r->hot_bits[i >> 3] & (1u << (i & 7)))) continue;
            int x0 = s_zone.cfg.x0 + cx * cell, y0 = s_zone.cfg.y0 + cy * cell;
            int x1 = s_zone.cfg.x0 + (cx + 1) * cell - 1;
            int y1 = s_zone.cfg.y0 + (cy + 1) * cell - 1;
            if (x1 >= fb->width)  x1 = fb->width - 1;
            if (y1 >= fb->height) y1 = fb->height - 1;
            for (int x = x0; x <= x1; x++) {
                fb->buf[(size_t)y0 * fb->width + x] = 255;
                fb->buf[(size_t)y1 * fb->width + x] = 255;
            }
            for (int y = y0; y <= y1; y++) {
                fb->buf[(size_t)y * fb->width + x0] = 255;
                fb->buf[(size_t)y * fb->width + x1] = 255;
            }
        }
    }
}

/* X-Motion-Grid value: packed hot-cell bitmap (hex) plus the mapping the
 * server needs to turn probe pixels into boxes later (non-LLM recognizers)
 * without re-running the detector. "" when the grid is too big to carry. */
static void build_grid_json(char *out, size_t outsz)
{
    static const char hx[16] = "0123456789abcdef";
    const uint8_t *b = s_zone.hot_bits;
    const int nb = s_zone.hot_bytes;
    char hex[2 * GRID_HEX_MAX_BYTES + 1];
    out[0] = '\0';
    if (!b || nb > GRID_HEX_MAX_BYTES) return;
    for (int i = 0; i < nb; i++) {
        hex[2 * i] = hx[b[i] >> 4];
        hex[2 * i + 1] = hx[b[i] & 15];
    }
    hex[2 * nb] = '\0';
    snprintf(out, outsz,
             "{\"gw\":%d,\"gh\":%d,\"cell\":%d,\"zone\":[%d,%d,%d,%d],"
             "\"frame\":[%d,%d],\"bits\":\"%s\"}",
             s_zone.cols, s_zone.rows, s_zone.cfg.cell,
             s_zone.cfg.x0, s_zone.cfg.y0, s_zone.cfg.x1, s_zone.cfg.y1,
             s_zone.w, s_zone.h, hex);
}

/* Upload the grayscale probe frame that triggered a capture, as small JPEG.
 * Lets us see what the detector saw and retune the zone from real data. */
static void upload_probe(const camera_fb_t *fb, const zone_result_t *r)
{
    uint8_t *jpg = NULL;
    size_t jpg_len = 0;
    draw_hot_cells((camera_fb_t *)fb, r); /* box the trigger onto the frame */
    char grid[512];
    build_grid_json(grid, sizeof(grid));
    if (!frame2jpg((camera_fb_t *)fb, PROBE_JPEG_QUALITY, &jpg, &jpg_len)) {
        ESP_LOGW(TAG, "probe encode failed");
        return;
    }
    bool ok = upload_bytes(jpg, jpg_len, "probe", "image/jpeg", r->hot_cells,
                           r->global_mean, grid[0] ? grid : NULL, NULL);
    ESP_LOGI(TAG, "probe %ux%u %u B -> %s (hot=%d global=%d)", fb->width, fb->height,
             (unsigned)jpg_len, ok ? "OK" : "FAIL", r->hot_cells, r->global_mean);
    free(jpg);
}

static esp_err_t probe_start(void)
{
    esp_err_t err = camera_start(PROBE_PIXFORMAT, PROBE_FRAMESIZE, 0, PROBE_SETTLE);
    if (err == ESP_OK) zone_motion_reset(&s_zone);
    return err;
}

/* Phase 7a/7c: stream a session to the Pi; camera already in photo (XGA
 * JPEG) mode. skind "motion" = right after the trigger still; "ordered"
 * = the gateway asked for live video (model verdict or browser request).
 * The Pi runs the SAME zone detector on these frames and answers each
 * chunk 200 (keep streaming) or 204 (session over); any other answer ends
 * it too — the still is already stored, and hammering a struggling link
 * is what the probe-loop backoff exists to avoid. */
static void video_stream_session(const char *sid, const char *skind, int max_frames)
{
    char *buf = heap_caps_malloc(VIDEO_CHUNK_MAX_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        ESP_LOGW(TAG, "video: no %d KB chunk buffer; session skipped",
                 VIDEO_CHUNK_MAX_BYTES / 1024);
        return;
    }

    unsigned total = 0, chunk = 0, frames = 0;
    size_t len = 0;
    const char *end = "cap";
    const int64_t t0 = esp_timer_get_time() / 1000;
    while (total < (unsigned)max_frames) {
        /* one chunk, paced at VIDEO_FRAME_MS: 5 fps stays honest even
         * when the sensor could deliver more */
        while (frames < VIDEO_CHUNK_FRAMES && total < (unsigned)max_frames) {
            const int64_t due_ms = t0 + (int64_t)total * VIDEO_FRAME_MS;
            camera_fb_t *fb = esp_camera_fb_get();
            if (!fb) {
                ESP_LOGW(TAG, "video: grab failed at frame %u", total);
                end = "grab fail";
                break;
            }
            if (len + fb->len <= VIDEO_CHUNK_MAX_BYTES) {
                memcpy(buf + len, fb->buf, fb->len);
                len += fb->len;
                frames++;
                total++;
            }
            esp_camera_fb_return(fb);
            const int64_t now = esp_timer_get_time() / 1000;
            if (now < due_ms) vTaskDelay(pdMS_TO_TICKS((int)(due_ms - now)));
        }
        if (!frames) break;
        int status = upload_http((unsigned char *)buf, len, "video", "image/jpeg",
                                 -1, -1, NULL, sid, (int)chunk++, sid, skind);
        if (status == 204) { end = "Pi stop"; break; }
        if (status != 200) { end = "link/err"; break; }
        len = 0;
        frames = 0;
    }
    const int64_t dur = esp_timer_get_time() / 1000 - t0;
    ESP_LOGI(TAG, "video %s: %u frames, %u chunks, %lld ms (end=%s)",
             skind, total, chunk, (long long)dur, end);
    heap_caps_free(buf);
}

/* One failure policy for getting back into probe mode (after a photo-mode
 * switch, a failed unpark, or at boot): try, and on failure tear the driver
 * down and retry every 2 s until it works. zone_motion_reset on a zeroed
 * zone is safe, so the boot path can use this too. */
static void probe_restart_or_halt(void)
{
    while (probe_start() != ESP_OK) {
        ESP_LOGE(TAG, "probe mode failed, retrying in 2 s");
        vTaskDelay(pdMS_TO_TICKS(2000));
        camera_stop();
    }
}

/* Switch to photo mode, grab and upload one XGA JPEG, switch back to probe. */
static bool capture_photo(const char *why, const zone_result_t *r)
{
    s_event_id[0] = 0;
    snprintf(s_event_id, sizeof(s_event_id), "%s-%lld", s_device_id,
             (long long)(esp_timer_get_time() / 1000000));
    s_live_secs = 0;

    camera_stop();
    if (camera_start(PIXFORMAT_JPEG, PHOTO_FRAMESIZE, PHOTO_JPEG_QUALITY, PHOTO_SETTLE) != ESP_OK) {
        ESP_LOGE(TAG, "photo mode failed, back to probing");
        probe_start();
        return false;
    }

    camera_fb_t *fb = esp_camera_fb_get();
    bool ok = false;
    if (fb) {
        ok = upload_bytes(fb->buf, fb->len, "photo", "image/jpeg", -1, -1, NULL,
                         s_event_id);
        ESP_LOGI(TAG, "%s photo: %u bytes -> %s (hot=%d global=%d)", why, (unsigned)fb->len,
                 ok ? "OK" : "FAIL", r->hot_cells, r->global_mean);
        esp_camera_fb_return(fb);
    } else {
        ESP_LOGE(TAG, "%s photo: frame grab failed", why);
    }

    /* Motion events also stream the event as a clip (phase 7a): the Pi
     * keeps the session open past its quiet rules while a live window is
     * open (a bird verdict just landed — the footage of the bird is what
     * we want), and only records the clip if the decision model agrees.
     * Heartbeats stay photo-only — but if the gateway has a live window
     * open (browser request), the heartbeat answers X-Live and we stream
     * ordered footage right away, camera already in photo mode. */
    if (ok && strcmp(why, "heartbeat") != 0) {
        video_stream_session(s_event_id, "motion", VIDEO_MAX_FRAMES);
    } else if (ok && s_live_secs > 0) {
        char osid[72];
        snprintf(osid, sizeof(osid), "%s-live", s_event_id);
        int max_frames = s_live_secs * 1000 / VIDEO_FRAME_MS;
        video_stream_session(osid, "ordered",
                             max_frames > VIDEO_MAX_FRAMES ? VIDEO_MAX_FRAMES : max_frames);
    }

    camera_stop();
    probe_restart_or_halt();
    return ok;
}

/* Phase 7c: ask the gateway whether anyone wants live video right now.
 * Body "N": stream N seconds starting now; "0": nothing pending. The
 * decision of WHEN to stream lives on the Pi (model verdict or browser
 * request); this device only ever obeys. */
static int live_status_get(void)
{
    char url[192];
    snprintf(url, sizeof(url), "%s/api/live_status?device=%s", INGEST_BASE, s_device_id);
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .timeout_ms = 8000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return 0;
    int secs = 0;
    if (esp_http_client_perform(client) == ESP_OK &&
        esp_http_client_get_status_code(client) == 200) {
        char body[16];
        int n = esp_http_client_read(client, body, sizeof(body) - 1);
        if (n > 0) {
            body[n] = 0;
            secs = (int)strtol(body, NULL, 10);
        }
    }
    esp_http_client_cleanup(client);
    return secs;
}

/* Dial, ask, and if live video is wanted: pay for it in this same dial
 * (photo-mode camera, ordered session, back to probe). ~2 s of connect
 * when the answer is no; the footage when the answer is yes. */
static void maybe_live_poll(int64_t *last_poll_ms)
{
    *last_poll_ms = esp_timer_get_time() / 1000;
    if (wifi_connect_start() != ESP_OK || !wifi_ready(WIFI_CONNECT_WAIT_MS)) {
        wifi_power_down();
        return;
    }
    int secs = live_status_get();
    if (secs > 0) {
        char sid[72];
        snprintf(sid, sizeof(sid), "%s-%lld-o", s_device_id,
                 (long long)(esp_timer_get_time() / 1000000));
        camera_stop();
        if (camera_start(PIXFORMAT_JPEG, PHOTO_FRAMESIZE, PHOTO_JPEG_QUALITY,
                         PHOTO_SETTLE) == ESP_OK) {
            int max_frames = secs * 1000 / VIDEO_FRAME_MS;
            video_stream_session(sid, "ordered",
                                 max_frames > VIDEO_MAX_FRAMES ? VIDEO_MAX_FRAMES : max_frames);
        }
        camera_stop();
        while (probe_start() != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            camera_stop();
        }
        zone_motion_reset(&s_zone);  /* the reference frame went stale */
    }
    wifi_power_down();
}

static zone_cfg_t zone_config_from_header(int w, int h)
{
    zone_cfg_t c = {
        .x0 = ZONE_X0_PCT * w / 100,
        .x1 = ZONE_X1_PCT * w / 100,
        .y0 = ZONE_Y0_PCT * h / 100,
        .y1 = ZONE_Y1_PCT * h / 100,
        .cell = ZONE_CELL,
        .cell_thresh = ZONE_CELL_THRESH,
        .min_hot_cells = ZONE_MIN_HOT_CELLS,
        .trigger_frames = ZONE_TRIGGER_FRAMES,
        .light_reject = ZONE_LIGHT_REJECT,
    };
    return c;
}

void app_main(void)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_device_id, sizeof(s_device_id), "xiao-%02x%02x%02x", mac[3], mac[4], mac[5]);
    ESP_LOGI(TAG, "open-bird-feeder cam %s starting", s_device_id);

    ESP_ERROR_CHECK(nvs_flash_init());
    wifi_init(); /* radio off until a capture dials it */

    probe_restart_or_halt();
    ESP_LOGI(TAG, "probe loop: continuous while hot, windowed idle=%d ms x%d frames hot-hold=%d ms, heartbeat %d ms, wifi=dial-before-call, sensor-parking=%s",
             PROBE_IDLE_MS, WINDOW_FRAMES, PROBE_HOT_HOLD_MS, HEARTBEAT_MS,
             INCREASE_POWER_CONSUMPTION ? "DISABLED (INCREASE_POWER_CONSUMPTION)" : "enabled");

    int64_t last_capture_ms = now_ms();
    int64_t last_live_poll_ms = last_capture_ms;
    unsigned captures = 0, skips = 0, skip_streak = 0;

    /* Windowed idle state (see PROBE_IDLE_MS). Duty accounting is the field
     * power gauge: active = sensor streaming, parked = sensor standby.
     * 'parked' is the ONE sensor-state flag — the gauge derives streaming
     * from it; never cache a second copy of that fact. */
    bool can_window = !INCREASE_POWER_CONSUMPTION, parked = false, night = false;
    bool bank_hold = false;
    int window_left = 0, dark_run = 0, park_fails = 0;
    int64_t hot_until_ms = 0, park_until_ms = 0;
    int64_t mode_since_ms = now_ms();
    uint32_t windows = 0;
    uint32_t duty_ema = 0; /* scale DUTY_SCALE=100%; starts at 0 = keeper engaged at boot */
    uint64_t active_ms = 0, parked_ms = 0;

    /* Steady-state precheck (PLAN 5c-1f defect): a cold scene windowed with
     * the current knobs can only reach win_duty = active/(active+PROBE_IDLE_MS)
     * streaming. If that ceiling sits under the engage floor, releasing
     * bank_hold just re-latches a minute later — an endless HOLD/RESUME
     * oscillation that parks/unparks and wakes the sensor every cycle while
     * logging both edges. So: if windowing cannot hold this bank, the keeper
     * latches ONCE and holds until reboot; retune PROBE_IDLE_MS (and/or
     * WINDOW_FRAMES) so window duty clears MIN_DUTY_PCT before expecting
     * savings from a flag=0 build. */
    const int64_t win_active_ms = (int64_t)(PROBE_SETTLE + WINDOW_FRAMES) * PROBE_INTERVAL_MS;
    const uint32_t win_duty = (uint32_t)((uint64_t)win_active_ms * DUTY_SCALE /
                                         (win_active_ms + PROBE_IDLE_MS));
    const bool window_clears_floor = win_duty >= DUTY_SCALED(MIN_DUTY_PCT);

    /* Fold the just-closed streaming/parked segment into the gauge and the
     * keeper EMA. Called exactly once BEFORE every parked mutation; the
     * current parked value identifies which segment closed, so there is no
     * second mode flag to keep in sync. */
#define MODE_CLOSE() do {                                              \
        int64_t t_ = now_ms();                                         \
        if (!parked) active_ms += t_ - mode_since_ms;                  \
        else parked_ms += t_ - mode_since_ms;                          \
        duty_ema = duty_ema_advance(duty_ema, !parked, t_ - mode_since_ms); \
        mode_since_ms = t_;                                            \
    } while (0)

    /* THE idle policy in one place: why are we forcing continuous streaming
     * right now (NULL = windowing/parking allowed). A new hold reason gets
     * added HERE and every decision site picks it up. Call sites pass
     * deliberately different clock snapshots on purpose: frame-time answers
     * "was the zone hot during this frame"; iter-end answers "is a hold
     * still live after an upload that consumed seconds". */
#define HOLD_REASON(now) (!can_window ? "no-park" : night ? "night" : \
                          bank_hold ? "duty-floor" :                    \
                          (now) < hot_until_ms ? "hot" : NULL)

    while (true) {
        /* While parked, wait out PROBE_IDLE_MS; then open a window: unpark,
         * discard wake frames, and probe normally for WINDOW_FRAMES frames. */
        int64_t loop_start_ms = now_ms();
        if (parked) {
            if (loop_start_ms < park_until_ms) {
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
            bool wake_ok = sensor_set_parked(false) == ESP_OK &&
                           discard_frames(PROBE_SETTLE);
            if (!wake_ok) {
                ESP_LOGE(TAG, "sensor unpark failed, re-initializing camera");
                camera_stop();
                probe_restart_or_halt();
                MODE_CLOSE(); /* was parked; re-init streams again */
                parked = false;
                window_left = 0;
                continue;
            }
            MODE_CLOSE(); /* closing the parked segment */
            parked = false;
            windows++;
            window_left = WINDOW_FRAMES;
        } else if (!HOLD_REASON(loop_start_ms) && window_left == 0) {
            /* Hot-hold expired with the sensor still streaming (e.g. after a
             * capture): resume windows without a park cycle first. */
            windows++; /* every detection-window open counts: windows= is a
                        * detection duty gauge, not a park-cycle counter */
            window_left = WINDOW_FRAMES;
        }

        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) {
            ESP_LOGE(TAG, "probe grab failed");
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        const int w = fb->width, h = fb->height;
        if (fb->len < (size_t)w * (size_t)h) {
            ESP_LOGW(TAG, "probe frame %ux%u len=%u unusable", w, h, (unsigned)fb->len);
            esp_camera_fb_return(fb);
            vTaskDelay(pdMS_TO_TICKS(PROBE_INTERVAL_MS));
            continue;
        }

        /* Zone size follows the real probe frame, so PROBE_FRAMESIZE can change
         * without touching the percentages in zone_config.h. */
        if (!s_zone_ready || s_zone.w != w || s_zone.h != h) {
            zone_motion_free(&s_zone);
            zone_cfg_t zc = zone_config_from_header(w, h);
            if (zone_motion_init(&s_zone, w, h, &zc) != 0) {
                ESP_LOGE(TAG, "zone motion init failed (out of memory?)");
                esp_camera_fb_return(fb);
                esp_restart();
            }
            s_zone_ready = true;
            ESP_LOGI(TAG, "zone %d,%d..%d,%d of %dx%d, cell=%d thresh=%d min_hot=%d",
                     s_zone.cfg.x0, s_zone.cfg.y0, s_zone.cfg.x1, s_zone.cfg.y1, w, h,
                     s_zone.cfg.cell, s_zone.cfg.cell_thresh, s_zone.cfg.min_hot_cells);
        }

        zone_result_t r = zone_motion_update(&s_zone, fb->buf, w);
        const int64_t frame_ms = now_ms();
        const int64_t since_ms = frame_ms - last_capture_ms;

        if (r.hot_cells > 0) {
            /* Anything moving inside the zone: probe continuously while it
             * does. The 5a trigger pipeline (150 ms frames, 2 consecutive)
             * runs untouched for the whole hot-hold. */
            hot_until_ms = frame_ms + PROBE_HOT_HOLD_MS;
        }

        /* Night state machine runs UNCONDITIONALLY. It gates two things:
         * sensor parking (meaningful only when can_window) and the capture
         * decision (always). Gating it on can_window — as the pre-merge
         * cleanup did — made it dead code in the shipped flag=1 build, and
         * the pole then uploaded black frames all night (field report
         * 2026-11-08: 33 photos with mean luma 10-40; the only "motion"
         * after dark is traffic). Thresholds are the 5d night-gate
         * measurements: real night scenes land 9-30, day ~135, headlight
         * frames sit near the night floor, so a passing car never opens
         * the gate. */
        const int luma = frame_mean_luma(fb->buf, fb->len);
        if (!night) {
            dark_run = luma < NIGHT_LUMA_ENTER ? dark_run + 1 : 0;
            if (dark_run >= NIGHT_DARK_FRAMES) {
                night = true;
                ESP_LOGI(TAG, "night: luma=%d under %d for %d frames: captures off%s",
                         luma, NIGHT_LUMA_ENTER, NIGHT_DARK_FRAMES,
                         can_window ? ", windowing off (bank-keeper)" : "");
            }
        } else if (luma >= NIGHT_LUMA_EXIT) {
            night = false;
            dark_run = 0;
            ESP_LOGI(TAG, "night: dawn luma=%d: captures resume%s", luma,
                     can_window ? ", windowing resumes" : "");
        }

        bool shoot = false;
        const char *why = NULL;
        /* Consecutive dial failures back the motion gate off exponentially:
         * sustained zone motion during an outage must not hammer the radio
         * or leave long blind spots. The heartbeat path is gated by
         * HEARTBEAT_MS, not by this. */
        const int backoff_exp = (int)skip_streak > SKIP_BACKOFF_MAX ? SKIP_BACKOFF_MAX : (int)skip_streak;
        const int64_t motion_gap_ms = (int64_t)REFRACTORY_MS << backoff_exp;
        /* Darkness gate: while the scene reads dark, neither motion nor the
         * heartbeat takes a photo. Detection and the luma watch keep
         * streaming (the bank-keeper wants that load anyway), so dawn
         * reopens this gate by itself — last_capture_ms only fell behind,
         * which makes the first dawn trigger fire at once. */
        if (!night && r.trigger && since_ms >= motion_gap_ms) {
            shoot = true;
            why = "motion";
        } else if (!night && HEARTBEAT_MS > 0 && since_ms >= HEARTBEAT_MS) {
            shoot = true;
            why = "heartbeat";
        }

        if (shoot) {
            /* One IP gate up front: with no wifi the whole cycle is skipped
               before any encode or camera mode switch is paid for. */
            esp_err_t dial = wifi_connect_start();
            if (dial == ESP_OK && wifi_ready(WIFI_CONNECT_WAIT_MS)) {
                upload_probe(fb, &r);
                esp_camera_fb_return(fb);
                fb = NULL;
                capture_photo(why, &r);
                last_capture_ms = now_ms();
                captures++;
                skip_streak = 0;
                /* A capture means wildlife: keep the fresh streaming sensor
                 * continuous for a hot-hold (capture_photo left it running). */
                hot_until_ms = last_capture_ms + PROBE_HOT_HOLD_MS;
            } else {
                /* No IP: advance the timestamp anyway so the retry respects
                   the backoff, and reset the reference frame: it went stale
                   while the radio was dialing. */
                ESP_LOGW(TAG, "%s capture skipped: wifi not up", why);
                esp_camera_fb_return(fb);
                fb = NULL;
                last_capture_ms = now_ms();
                skips++;
                if (skip_streak < SKIP_BACKOFF_MAX) skip_streak++;
                zone_motion_reset(&s_zone);
            }
            wifi_power_down();
        } else {
            if (s_zone.frames % LOG_EVERY_FRAMES == 0) {
                /* The field power gauge must include the currently open mode
                 * segment: closed-segments-only duty goes stale exactly during
                 * long holds, which is when the hold reasons matter. */
                const int64_t t = now_ms();
                uint64_t da = active_ms, dp = parked_ms;
                if (!parked) da += t - mode_since_ms; else dp += t - mode_since_ms;
                uint64_t total_ms = da + dp;
                const char *hold = HOLD_REASON(t);
                ESP_LOGI(TAG, "probing: frames=%u captures=%u skips=%u windows=%u duty=%u%% hold=%s last hot=%d global=%d",
                         (unsigned)s_zone.frames, captures, skips, (unsigned)windows,
                         total_ms ? (unsigned)(100 * da / total_ms) : 100,
                         hold ? hold : "none", r.hot_cells, r.global_mean);
            }
            esp_camera_fb_return(fb);
            fb = NULL;
            if (LIVE_POLL_MS > 0 && now_ms - last_live_poll_ms >= LIVE_POLL_MS) {
                maybe_live_poll(&last_live_poll_ms);
            }
        }

        /* Window pacing: continuous while any hold reason is live (single
         * source: HOLD_REASON); otherwise count the window frames down and
         * park the sensor once the zone is cold. */
        const int64_t iter_end_ms = now_ms();
        if (can_window) {
            /* Bank-keeper (slow feedback half; the night luma block above is
             * the fast feed-forward half): project the open duty segment and
             * latch the hold with hysteresis, so a cold idle never sits under
             * the bank cutoff. Scale/thresholds: DUTY_SCALED / DUTY_PCT. */
            uint32_t dema = duty_ema_advance(duty_ema, !parked, iter_end_ms - mode_since_ms);
            if (!bank_hold && dema < DUTY_SCALED(MIN_DUTY_PCT)) {
                bank_hold = true;
                if (window_clears_floor) {
                    ESP_LOGI(TAG, "bank keeper: duty %u%% under floor %d%%, holding continuous",
                             DUTY_PCT(dema), MIN_DUTY_PCT);
                } else {
                    ESP_LOGI(TAG, "bank keeper: windowed duty caps at %u%% under floor %d%%: holding continuous until reboot (set INCREASE_POWER_CONSUMPTION=1 or retune PROBE_IDLE_MS=%d -> window duty >= %d%%)",
                             DUTY_PCT(win_duty), MIN_DUTY_PCT, PROBE_IDLE_MS, MIN_DUTY_PCT);
                }
            } else if (bank_hold && window_clears_floor &&
                       dema >= DUTY_SCALED(MIN_DUTY_PCT + DUTY_HYST_PCT)) {
                bank_hold = false;
                ESP_LOGI(TAG, "bank keeper: duty %u%% recovered, windowing resumes",
                         DUTY_PCT(dema));
            }
        }
        if (HOLD_REASON(iter_end_ms)) {
            window_left = 0;
        } else if (window_left > 0) {
            window_left--;
            if (window_left == 0 && can_window) {
                esp_err_t pk = sensor_set_parked(true);
                if (pk == ESP_OK) {
                    park_fails = 0;
                    MODE_CLOSE(); /* closing the streaming segment */
                    parked = true;
                    park_until_ms = iter_end_ms + PROBE_IDLE_MS;
                } else if (pk == ESP_ERR_NOT_SUPPORTED) {
                    can_window = false;
                    ESP_LOGW(TAG, "sensor has no standby bit: staying on continuous probe");
                } else if (++park_fails >= 3) {
                    /* Degraded SCCB (connector corrosion, ribbon fatigue) is a
                     * fault, not a feature: after 3 straight failures stop
                     * paying a failed bus transaction per window and stop the
                     * warn/re-init storm — continuous streaming is the
                     * field-proven known-good state. One log, then quiet. */
                    can_window = false;
                    ESP_LOGE(TAG, "sensor park failed %d times (%s): falling back to continuous probe",
                             park_fails, esp_err_to_name(pk));
                } else {
                    ESP_LOGW(TAG, "sensor park failed: %s", esp_err_to_name(pk));
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(PROBE_INTERVAL_MS));
    }
}
