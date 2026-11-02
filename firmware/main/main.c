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
// Each capture cycle connects, uploads photo + probe frame, then powers the
// radio down again. On wifi failure the capture is skipped; retries are
// rate-limited by the refractory window.
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

/* Photo mode: one XGA frame per capture. */
#define PHOTO_FRAMESIZE   FRAMESIZE_XGA /* 1024x768 */
#define PHOTO_JPEG_QUALITY 12
#define PHOTO_SETTLE      3 /* let auto-exposure converge after the mode switch */

#define REFRACTORY_MS 4000  /* ignore motion this long after a capture */
#define HEARTBEAT_MS  900000 /* 15 min: one photo when nothing moves (0 = off) */
#define LOG_EVERY_FRAMES 400 /* one info line per ~60 s of probing */

/* Cap on the hex-encoded hot-cell bitmap we put in the X-Motion-Grid header
 * (150 bytes = 1200 cells; today's grid is 480). Over-cap = no header. */
#define GRID_HEX_MAX_BYTES 150

#define WIFI_CONNECTED_BIT BIT0

static EventGroupHandle_t s_wifi_events;
static bool s_wifi_wanted;   /* retry connects while true */
static bool s_wifi_started;  /* radio is powered on */
static bool s_wifi_up;       /* got an ip */
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

static void camera_stop(void)
{
    esp_camera_deinit();
    vTaskDelay(pdMS_TO_TICKS(100));
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_wifi_wanted) esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_wifi_up = false;
        xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
        if (s_wifi_wanted) {
            ESP_LOGW(TAG, "wifi disconnected, retrying");
            esp_wifi_connect();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
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
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));

    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid, WIFI_SSID, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, WIFI_PASS, sizeof(wc.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
}

/* Power the radio on and start connecting. Completion is the GOT_IP bit;
 * wait for it with wifi_ready(). */
static void wifi_connect_start(void)
{
    if (s_wifi_up) return;
    s_wifi_wanted = true;
    xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
    if (!s_wifi_started) {
        esp_err_t err = esp_wifi_start(); /* the STA_START event connects via the handler */
        if (err != ESP_OK) ESP_LOGE(TAG, "wifi start failed: %s", esp_err_to_name(err));
        else s_wifi_started = true;
    } else {
        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK) ESP_LOGW(TAG, "wifi connect request: %s", esp_err_to_name(err));
    }
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
        esp_wifi_disconnect();
        esp_wifi_stop();
        s_wifi_started = false;
    }
}

/* POST a buffer to the ingest service. hot/global < 0 = no motion headers.
 * grid non-NULL = also send it as X-Motion-Grid (probe frames only).
 * Returns true on HTTP 200. */
static bool upload_bytes(const uint8_t *buf, size_t len, const char *kind,
                         const char *content_type, int hot, int global,
                         const char *grid)
{
    if (!wifi_ready(15000)) {
        ESP_LOGW(TAG, "upload %s skipped: no wifi (connect failed?)", kind);
        return false;
    }

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
    wifi_init(); /* radio off until a capture dials it */

    while (camera_start(PROBE_PIXFORMAT, PROBE_FRAMESIZE, 0, PROBE_SETTLE) != ESP_OK) {
        ESP_LOGE(TAG, "camera init failed, retrying in 2 s");
        vTaskDelay(pdMS_TO_TICKS(2000));
        camera_stop();
    }
    ESP_LOGI(TAG, "probing at %.1f fps, heartbeat %d ms, wifi=dial-before-call", 1000.0f / PROBE_INTERVAL_MS, HEARTBEAT_MS);

    int64_t last_capture_ms = esp_timer_get_time() / 1000;
    unsigned captures = 0, skips = 0;

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

        bool shoot = false;
        const char *why = NULL;
        if (r.trigger && since_ms >= REFRACTORY_MS) {
            shoot = true;
            why = "motion";
        } else if (HEARTBEAT_MS > 0 && since_ms >= HEARTBEAT_MS) {
            shoot = true;
            why = "heartbeat";
        }

        if (shoot) {
            wifi_connect_start();
            upload_probe(fb, &r);
            esp_camera_fb_return(fb);
            fb = NULL;
            if (wifi_ready(1500)) {
                capture_photo(why, &r);
                last_capture_ms = esp_timer_get_time() / 1000;
                captures++;
            } else {
                /* No IP: skip the capture instead of switching camera modes
                 * for a photo we cannot upload. The timestamp still moves so
                 * retries stay rate-limited, and the reference frame is reset
                 * because it went stale during the connect wait. */
                ESP_LOGW(TAG, "%s capture skipped: wifi not up", why);
                last_capture_ms = esp_timer_get_time() / 1000;
                skips++;
                zone_motion_reset(&s_zone);
            }
            wifi_power_down();
        } else {
            if (s_zone.frames % LOG_EVERY_FRAMES == 0) {
                ESP_LOGI(TAG, "probing: frames=%u captures=%u skips=%u last hot=%d global=%d",
                         (unsigned)s_zone.frames, captures, skips, r.hot_cells, r.global_mean);
            }
            esp_camera_fb_return(fb);
        }
        vTaskDelay(pdMS_TO_TICKS(PROBE_INTERVAL_MS));
    }
}
