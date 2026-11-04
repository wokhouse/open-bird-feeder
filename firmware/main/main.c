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
// At night the un-illuminated camera produces black frames (no night vision
// out of the box), so a brightness gate ends the day: after DARK_SUSTAIN_MS
// of sustained darkness the device deep-sleeps and wakes every
// NIGHT_WAKE_MS for a light check (phase 5d). Measured frame-means:
// night 9-15, day ~135. The OV3660 is parked in software power-down on
// every camera stop, so the night sleep actually draws ~1.5 mA (22-38 mA
// unparked; tall-mule measurements ported).
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
#include "esp_http_client.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_sleep.h"
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

/* Photo mode: one XGA frame per capture. */
#define PHOTO_FRAMESIZE   FRAMESIZE_XGA /* 1024x768 */
#define PHOTO_JPEG_QUALITY 12
#define PHOTO_SETTLE      3 /* let auto-exposure converge after the mode switch */

#define REFRACTORY_MS 4000  /* ignore motion this long after a capture */
#define HEARTBEAT_MS  900000 /* 15 min: one photo when nothing moves (0 = off) */

/* Wi-Fi dial (phase 5c): one IP budget, then exponential skip backoff. */
#define WIFI_CONNECT_WAIT_MS 8000  /* IP budget after dialing */
#define SKIP_BACKOFF_MAX 5         /* motion gate: refractory << skips, cap 128 s */
#define LOG_EVERY_FRAMES 400 /* one info line per ~60 s of probing */

/* Brightness gate (phase 5d). The light meter is the mean luma of a probe
 * frame: at night the scene is unlit and frames are truly black (measured on
 * 2026-11-01/02 captures: mean luma 9-15 at night, headlight frames
 * included, vs ~135 in day). DARK_SUSTAIN_MS of samples under
 * DARK_FRAME_MEAN ends the day: deep sleep, then a light check every
 * NIGHT_WAKE_MS. The check grabs NIGHT_CHECK_FRAMES probe frames
 * NIGHT_CHECK_INTERVAL_MS apart and needs NIGHT_BRIGHT_FRAMES of them bright
 * to resume; a passing car's lights at night sit far under DARK_FRAME_MEAN,
 * so they cannot end the night.
 * The check window is ALSO the bank-load mechanism (see PLAN 5d): USB banks
 * low-load auto-off below some mA floor, and a 5 s wake every 5 min is too
 * short for their trip detector to see (field: 11-02 night survived, 11-03
 * night did not, at ~2-4 mA avg either way). A longer check window raises
 * the AVERAGE load: mA at the rail ~= camera_on_mA * window / NIGHT_WAKE_MS,
 * camera_on ~110 mA at 5V. 8 frames at 3000 ms = ~24 s window ~= 9 mA
 * average, ~0.5 Wh/night. If the bank still trips, step the interval up
 * (8000 ms ~= 24 mA, ~1.3 Wh). The frames also spread in time, which makes
 * the dawn vote more robust than 8 samples in 4 s. */
#define DARK_FRAME_MEAN 40      /* frame mean below this = dark */
#define LIGHT_SAMPLE_EVERY 20   /* measure every 20th probe frame (~3 s) */
#define DARK_SUSTAIN_MS 120000  /* sustained darkness that ends the day */
#define NIGHT_WAKE_MS 300000    /* deep-sleep period between light checks */
#define NIGHT_CHECK_FRAMES 8    /* frames sampled per light check */
#ifndef NIGHT_CHECK_INTERVAL_MS
#define NIGHT_CHECK_INTERVAL_MS 3000 /* x frames ~= bank-load hold pulse */
#endif
#define NIGHT_BRIGHT_FRAMES 5   /* bright frames (of 8) that end the night */

/* Survives deep sleep (RTC slow memory, uninitialized on cold boot): the
 * magic + timer wake-up identify a "night check" boot vs a real start. */
#define NIGHT_MAGIC 0x4e475431u /* 'NGT1' */
typedef struct {
    uint32_t magic;
    uint32_t checks; /* light checks run since the night started */
} night_state_t;
static RTC_NOINIT_ATTR night_state_t s_night;

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
static zone_motion_t s_zone;
static bool s_zone_ready;

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
    for (int i = 0; i < settle; i++) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb) esp_camera_fb_return(fb);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return ESP_OK;
}

/* Park the OV3660: SYSTEM_CTROL0 (0x3008) bit 6, the software power-down
 * mode (the datasheet's separate "standby" mode is not what works here; bit 6
 * is what esp32-camera#672 measured end-to-end). (esp32-camera set_reg
 * signature: reg, mask, value — read-modify-write, preserves other bits.)
 * Unparked the sensor keeps the whole board at 22-38 mA even during deep
 * sleep; parked, the board measures ~1.5 mA asleep (measured on this
 * board+sensor combo, tall-mule branch). The OV2640 (older Sense boards) has
 * no such mode — silicon bug. Every later camera_start() runs a full
 * esp_camera_init(), which re-probes and re-programs the sensor, so there is
 * no unpark path; parking on every stop is what makes "park on every sleep
 * path" true by construction, failure paths included. Must run before
 * esp_camera_deinit(): after deinit esp_camera_sensor_get() returns NULL. */
#define OV3660_REG_SYSTEM_CTROL0 0x3008
#define OV3660_BIT_SW_POWERDOWN  0x40

static void camera_sleep_powerdown(void)
{
    sensor_t *s = esp_camera_sensor_get();
    if (s && s->id.PID == OV3660_PID) {
        s->set_reg(s, OV3660_REG_SYSTEM_CTROL0, OV3660_BIT_SW_POWERDOWN, OV3660_BIT_SW_POWERDOWN);
        ESP_LOGI(TAG, "ov3660 powered down (0x3008 bit6)");
    }
}

static void camera_stop(void)
{
    camera_sleep_powerdown(); /* park the sensor BEFORE its handle goes away */
    esp_camera_deinit();
    vTaskDelay(pdMS_TO_TICKS(100));
}

/* Mean luma of a grayscale probe frame, sampled every 4th byte (4800
 * samples at QQVGA, negligible next to the detector pass). -1 on a short
 * frame. See the DARK_FRAME_MEAN block for the measured night/day gap. */
static int frame_mean_luma(const camera_fb_t *fb)
{
    uint32_t sum = 0, n = 0;
    for (size_t i = 0; i < fb->len; i += 4) { sum += fb->buf[i]; n++; }
    return n ? (int)(sum / n) : -1;
}

/* End the day: stop the camera and deep-sleep until the next light check.
 * The RTC state carries the night flag across the sleep; on wake the boot
 * path in app_main() runs light_check() instead of the probe loop.
 * Never returns. */
static void night_sleep(void)
{
    camera_stop();
    ESP_LOGI(TAG, "night: deep sleep %d ms (light check #%u)",
             NIGHT_WAKE_MS, (unsigned)(s_night.checks + 1));
    s_night.magic = NIGHT_MAGIC;
    s_night.checks++;
    vTaskDelay(pdMS_TO_TICKS(50)); /* let the console drain before the sleep */
    esp_sleep_enable_timer_wakeup((uint64_t)NIGHT_WAKE_MS * 1000ULL);
    esp_deep_sleep_start();
}

/* Boot right after a night deep sleep: start the camera, sample
 * NIGHT_CHECK_FRAMES frames, count the bright ones. Returns the bright
 * count, or -1 if the camera did not come up (stay asleep; the next timer
 * wake tries again - sleeping through a dead camera is better than spinning
 * without one). */
static int light_check(void)
{
    if (camera_start(PROBE_PIXFORMAT, PROBE_FRAMESIZE, 0, PROBE_SETTLE) != ESP_OK)
        return -1;
    int bright = 0;
    for (int i = 0; i < NIGHT_CHECK_FRAMES; i++) {
        int luma = -1;
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb) {
            if (fb->len >= (size_t)fb->width * fb->height) luma = frame_mean_luma(fb);
            esp_camera_fb_return(fb);
        }
        if (luma >= DARK_FRAME_MEAN) bright++;
        ESP_LOGI(TAG, "night check %d/%d: luma=%d", i + 1, NIGHT_CHECK_FRAMES, luma);
        vTaskDelay(pdMS_TO_TICKS(NIGHT_CHECK_INTERVAL_MS));
    }
    camera_stop();
    return bright;
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

/* POST a buffer to the ingest service. hot/global < 0 = no motion headers.
 * grid non-NULL = also send it as X-Motion-Grid (probe frames only).
 * Returns true on HTTP 200. */
static bool upload_bytes(const uint8_t *buf, size_t len, const char *kind,
                         const char *content_type, int hot, int global,
                         const char *grid)
{
    char url[128];
    snprintf(url, sizeof(url), "%s/upload?kind=%s", INGEST_BASE, kind);

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 10000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return false;

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
    esp_http_client_set_post_field(client, (const char *)buf, len);

    bool ok = false;
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    if (err == ESP_OK && status == 200) {
        ok = true;
    } else {
        ESP_LOGE(TAG, "upload %s failed: err=%s status=%d", kind, esp_err_to_name(err), status);
    }
    esp_http_client_cleanup(client);
    return ok;
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
                           r->global_mean, grid[0] ? grid : NULL);
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

/* Switch to photo mode, grab and upload one XGA JPEG, switch back to probe. */
static bool capture_photo(const char *why, const zone_result_t *r)
{
    camera_stop();
    if (camera_start(PIXFORMAT_JPEG, PHOTO_FRAMESIZE, PHOTO_JPEG_QUALITY, PHOTO_SETTLE) != ESP_OK) {
        ESP_LOGE(TAG, "photo mode failed, back to probing");
        probe_start();
        return false;
    }

    camera_fb_t *fb = esp_camera_fb_get();
    bool ok = false;
    if (fb) {
        ok = upload_bytes(fb->buf, fb->len, "photo", "image/jpeg", -1, -1, NULL);
        ESP_LOGI(TAG, "%s photo: %u bytes -> %s (hot=%d global=%d)", why, (unsigned)fb->len,
                 ok ? "OK" : "FAIL", r->hot_cells, r->global_mean);
        esp_camera_fb_return(fb);
    } else {
        ESP_LOGE(TAG, "%s photo: frame grab failed", why);
    }

    camera_stop();
    while (probe_start() != ESP_OK) {
        ESP_LOGE(TAG, "probe mode failed, retrying in 2 s");
        vTaskDelay(pdMS_TO_TICKS(2000));
        camera_stop();
    }
    return ok;
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

    /* Wake from our own night deep sleep: run exactly one light check, then
     * sleep again or fall through into the normal day start. */
    if (s_night.magic == NIGHT_MAGIC &&
        esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER) {
        ESP_LOGI(TAG, "night check after %u deep sleeps", (unsigned)s_night.checks);
        int bright = light_check();
        if (bright >= NIGHT_BRIGHT_FRAMES) {
            ESP_LOGI(TAG, "dawn: %d/%d bright frames, resuming",
                     bright, NIGHT_CHECK_FRAMES);
            s_night.magic = 0;
            s_night.checks = 0;
        } else {
            night_sleep(); /* does not return */
        }
    } else {
        s_night.magic = 0; /* cold boot, brownout reset, button reset: fresh day */
        s_night.checks = 0;
    }

    wifi_init(); /* radio off until a capture dials it */

    while (camera_start(PROBE_PIXFORMAT, PROBE_FRAMESIZE, 0, PROBE_SETTLE) != ESP_OK) {
        ESP_LOGE(TAG, "camera init failed, retrying in 2 s");
        vTaskDelay(pdMS_TO_TICKS(2000));
        camera_stop();
    }
    ESP_LOGI(TAG, "probing at %.1f fps, heartbeat %d ms, wifi=dial-before-call, night gate: luma<%d for %d ms -> deep sleep %d ms",
             1000.0f / PROBE_INTERVAL_MS, HEARTBEAT_MS, DARK_FRAME_MEAN, DARK_SUSTAIN_MS, NIGHT_WAKE_MS);

    int64_t last_capture_ms = esp_timer_get_time() / 1000;
    unsigned captures = 0, skips = 0, skip_streak = 0;
    int64_t dark_since_ms = 0; /* 0 = not in the dark timer; start time otherwise */
    int last_luma = -1;

    while (true) {
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
        const int64_t now_ms = esp_timer_get_time() / 1000;
        const int64_t since_ms = now_ms - last_capture_ms;

        /* Brightness gate: with the day's light gone the only "motion" left
         * is cars, and their photos are black (see DARK_FRAME_MEAN block).
         * Count samples below the threshold; DARK_SUSTAIN_MS of sustained
         * darkness deep-sleeps until a periodic light check. Headlight
         * frames sit near the night floor, not above the threshold, so a
         * passing car never clears the timer; the sun (or a porch light)
         * does, and the timer just restarts if it goes dark again. */
        if (s_zone.frames % LIGHT_SAMPLE_EVERY == 0) {
            const int luma = frame_mean_luma(fb);
            last_luma = luma;
            if (luma >= 0) {
                if (luma < DARK_FRAME_MEAN) {
                    if (!dark_since_ms) {
                        dark_since_ms = now_ms;
                        ESP_LOGI(TAG, "dark: luma=%d < %d, watching %d ms",
                                 luma, DARK_FRAME_MEAN, DARK_SUSTAIN_MS);
                    } else if (now_ms - dark_since_ms >= DARK_SUSTAIN_MS) {
                        esp_camera_fb_return(fb);
                        fb = NULL;
                        night_sleep(); /* does not return */
                    }
                } else if (dark_since_ms) {
                    ESP_LOGI(TAG, "light returned: luma=%d", luma);
                    dark_since_ms = 0;
                }
            }
        }

        bool shoot = false;
        const char *why = NULL;
        /* Consecutive dial failures back the motion gate off exponentially:
         * sustained zone motion during an outage must not hammer the radio
         * or leave long blind spots. The heartbeat path is gated by
         * HEARTBEAT_MS, not by this. */
        const int backoff_exp = (int)skip_streak > SKIP_BACKOFF_MAX ? SKIP_BACKOFF_MAX : (int)skip_streak;
        const int64_t motion_gap_ms = (int64_t)REFRACTORY_MS << backoff_exp;
        if (r.trigger && since_ms >= motion_gap_ms) {
            shoot = true;
            why = "motion";
        } else if (HEARTBEAT_MS > 0 && since_ms >= HEARTBEAT_MS) {
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
                last_capture_ms = esp_timer_get_time() / 1000;
                captures++;
                skip_streak = 0;
            } else {
                /* No IP: advance the timestamp anyway so the retry respects
                   the backoff, and reset the reference frame: it went stale
                   while the radio was dialing. */
                ESP_LOGW(TAG, "%s capture skipped: wifi not up", why);
                esp_camera_fb_return(fb);
                fb = NULL;
                last_capture_ms = esp_timer_get_time() / 1000;
                skips++;
                if (skip_streak < SKIP_BACKOFF_MAX) skip_streak++;
                zone_motion_reset(&s_zone);
            }
            wifi_power_down();
        } else {
            if (s_zone.frames % LOG_EVERY_FRAMES == 0) {
                ESP_LOGI(TAG, "probing: frames=%u captures=%u skips=%u luma=%d last hot=%d global=%d",
                         (unsigned)s_zone.frames, captures, skips, last_luma, r.hot_cells, r.global_mean);
            }
            esp_camera_fb_return(fb);
        }
        vTaskDelay(pdMS_TO_TICKS(PROBE_INTERVAL_MS));
    }
}
