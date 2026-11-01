// Phase 3b: XIAO ESP32S3 Sense birdcam — duty-cycled deep-sleep capture.
//
// Every CAPTURE_INTERVAL_US the ESP32-S3 wakes from deep sleep (which is a
// cold boot), joins WiFi, inits the camera, grabs ONE XGA JPEG, HTTP-POSTs it
// to the network ingest service (see PLAN.md), parks the OV3660 in its
// software power-down mode (SCCB 0x3008 bit 6), and deep-sleeps again. Power
// model: race to sleep — the only awake time is one boot->join->capture->
// upload burst, then everything is off except the RTC timer. Abnormal resets
// (panic/brownout/watchdog) skip the burst entirely and back off on the RTC
// timer, so no failure mode can reboot-storm at full current. Fast-join keeps
// the last BSSID/channel in RTC-slow memory: IDF then scans that channel
// FIRST and pins association to that AP (it reorders the scan, it does not
// skip it). Measured on this exact board+sensor combo (Seeed forum /
// esp32-camera#672): ~1.5 mA in deep sleep once the OV3660 is parked, vs
// 22-38 mA left running — and parking happens on EVERY sleep path.
// Credentials/URL live in secrets.h.

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_attr.h"
#include "esp_camera.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_sleep.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "secrets.h"

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

#define CAPTURE_INTERVAL_US (60ULL * 1000000ULL)
#define FAULT_BACKOFF_US    (10ULL * 1000000ULL) /* after panic/brownout */
#define WIFI_FAST_JOIN_MS   10000 /* attempt on the cached AP/channel */
#define WIFI_FULL_JOIN_MS   15000 /* fallback: full band scan */
#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1 /* a connect attempt ended (DISCONNECTED) */

/* Survives deep sleep. Lets the next wake fast-join (scan the remembered
 * channel first, pin association) and gives a monotonic wake counter for log
 * correlation. */
#define RTC_STATE_MAGIC 0x4F424431u /* "OBD1" */
typedef struct {
    uint32_t magic;
    uint32_t wake_seq;
    uint8_t  bssid[6];
    uint8_t  channel;
} rtc_state_t;
static RTC_SLOW_ATTR rtc_state_t s_rtc;

static EventGroupHandle_t s_wifi_events;
static char s_device_id[24];

static esp_err_t camera_init(void)
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
        .pixel_format = PIXFORMAT_JPEG,
        .frame_size   = FRAMESIZE_XGA, /* 1024x768 */
        .jpeg_quality = 12,
        .fb_count     = 2,
        .fb_location  = CAMERA_FB_IN_PSRAM,
        .grab_mode    = CAMERA_GRAB_LATEST,
    };
    return esp_camera_init(&config);
}

/* Park the OV3660: SYSTEM_CTROL0 (0x3008) bit 6, the software power-down
 * mode (the datasheet's separate "standby" mode is not what works here; bit 6
 * is what esp32-camera#672 measured end-to-end). (esp32-camera set_reg
 * signature: reg, mask, value — read-modify-write, preserves other bits.)
 * Unparked the sensor draws ~60-90 mA while streaming and keeps the whole
 * board at 22-38 mA even during deep sleep; parked, the board measures ~1.5
 * mA asleep (see header). The OV2640 (older Sense boards) has no such mode —
 * silicon bug. The next wake does a full esp_camera_init() anyway, which
 * re-probes and re-programs the sensor, so no unpark path is needed. */
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

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        /* No auto-reconnect here: the join flow below owns retries.
         * esp_wifi_connect is one-shot, so a DISCONNECTED event ends the
         * current attempt — surface it instead of letting the caller burn
         * the rest of the join timeout. */
        ESP_LOGW(TAG, "wifi disconnected (reason %d)",
                 ((wifi_event_sta_disconnected_t *)data)->reason);
        xEventGroupSetBits(s_wifi_events, WIFI_FAIL_BIT);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got ip: " IPSTR, IP2STR(&event->ip_info.ip));
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            memcpy(s_rtc.bssid, ap.bssid, sizeof(s_rtc.bssid));
            s_rtc.channel = ap.primary;
        }
        xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
    }
}

/* Log-and-fail helper: a failed init must deep sleep, not panic-reboot into
 * a full burst again (a persistent fault would otherwise reboot-storm awake). */
static bool esp_call_ok(esp_err_t err, const char *what)
{
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s failed: %s", what, esp_err_to_name(err));
        return false;
    }
    return true;
}

/* Wait for join success or for the current connect attempt to end
 * (DISCONNECTED). Clears both bits on success so a later wait can never see
 * a stale GOT_IP from a cancelled attempt. */
static bool wait_join_result(int timeout_ms)
{
    EventBits_t bits = xEventGroupWaitBits(s_wifi_events,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdTRUE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    return (bits & WIFI_CONNECTED_BIT) != 0;
}

/* Join WiFi, fast (cached AP/channel) first, then full scan. Returns true
 * once we have an IP; false means "sleep and try next cycle". */
static bool wifi_join_blocking(void)
{
    s_wifi_events = xEventGroupCreate();
    if (!s_wifi_events) {
        ESP_LOGE(TAG, "no memory for wifi event group");
        return false;
    }
    if (!esp_call_ok(esp_netif_init(), "esp_netif_init")) return false;
    if (!esp_call_ok(esp_event_loop_create_default(), "event loop")) return false;
    if (!esp_netif_create_default_wifi_sta()) {
        ESP_LOGE(TAG, "no memory for default sta netif");
        return false;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (!esp_call_ok(esp_wifi_init(&cfg), "esp_wifi_init")) return false;
    /* Never persist WiFi config to NVS — we reconfigure every wake, and this
     * saves a flash write per cycle. */
    if (!esp_call_ok(esp_wifi_set_storage(WIFI_STORAGE_RAM), "wifi storage")) return false;
    if (!esp_call_ok(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL), "wifi handler")) return false;
    if (!esp_call_ok(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL), "ip handler")) return false;

    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid, WIFI_SSID, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, WIFI_PASS, sizeof(wc.sta.password));

    bool fast = (s_rtc.magic == RTC_STATE_MAGIC && s_rtc.channel >= 1 && s_rtc.channel <= 14);
    if (fast) {
        memcpy(wc.sta.bssid, s_rtc.bssid, sizeof(wc.sta.bssid));
        wc.sta.bssid_set = true;
        wc.sta.channel = s_rtc.channel;
        ESP_LOGI(TAG, "fast-joining %02x:%02x:%02x:%02x:%02x:%02x ch %u",
                 wc.sta.bssid[0], wc.sta.bssid[1], wc.sta.bssid[2],
                 wc.sta.bssid[3], wc.sta.bssid[4], wc.sta.bssid[5], wc.sta.channel);
    }

    if (!esp_call_ok(esp_wifi_set_mode(WIFI_MODE_STA), "wifi mode")) return false;
    if (!esp_call_ok(esp_wifi_set_config(WIFI_IF_STA, &wc), "wifi config")) return false;
    if (!esp_call_ok(esp_wifi_start(), "esp_wifi_start")) return false;

    if (wait_join_result(fast ? WIFI_FAST_JOIN_MS : WIFI_FULL_JOIN_MS)) return true;

    if (!fast) {
        ESP_LOGE(TAG, "wifi join failed after %d ms (antenna? AP down?)", WIFI_FULL_JOIN_MS);
        return false;
    }

    /* Fallback: full scan. The failed attempt has already set (and the failed
     * wait consumed) WIFI_FAIL_BIT; still ask for a settled DISCONNECTED so
     * the driver isn't mid-connect when we reissue — connect rejects while
     * one is pending. Clear bits first so a late GOT_IP from the cancelled
     * attempt can't satisfy the next wait. */
    ESP_LOGW(TAG, "fast join failed; full scan");
    wc.sta.bssid_set = false; /* bssid_set gates it; contents ignored */
    wc.sta.channel = 0;
    if (!esp_call_ok(esp_wifi_set_config(WIFI_IF_STA, &wc), "wifi config (full scan)")) return false;
    esp_wifi_disconnect();
    xEventGroupWaitBits(s_wifi_events, WIFI_FAIL_BIT, pdTRUE, pdFALSE, pdMS_TO_TICKS(2000));
    xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    if (!esp_call_ok(esp_wifi_connect(), "wifi connect (full scan)")) return false;
    if (wait_join_result(WIFI_FULL_JOIN_MS)) return true;

    ESP_LOGE(TAG, "wifi join failed (antenna? AP down?)");
    return false;
}

/* POST one frame to the ingest service. Returns true on HTTP 200. */
static bool upload_frame(const camera_fb_t *fb, const char *kind)
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

    esp_http_client_set_header(client, "Content-Type", "image/jpeg");
    esp_http_client_set_header(client, "X-Device-Id", s_device_id);
    esp_http_client_set_post_field(client, (const char *)fb->buf, fb->len);

    bool ok = false;
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    if (err == ESP_OK && status == 200) {
        ok = true;
    } else {
        ESP_LOGE(TAG, "upload failed: err=%s status=%d", esp_err_to_name(err), status);
    }
    esp_http_client_cleanup(client);
    return ok;
}

static void goto_sleep(void)
{
    /* Best-effort park on EVERY sleep path (frame-grab/camera-init failures
     * otherwise sleep 60 s with an unparked sensor at ~22-38 mA). No-op when
     * the sensor was never initialized. No WiFi/camera/netif teardown either:
     * deep sleep is a cold reboot, so teardown would buy nothing. */
    camera_sleep_powerdown();
    esp_sleep_enable_timer_wakeup(CAPTURE_INTERVAL_US);
    ESP_LOGI(TAG, "deep sleep for %llu s", CAPTURE_INTERVAL_US / 1000000ULL);
    esp_deep_sleep_start();
    __builtin_unreachable();
}

void app_main(void)
{
    /* RTC-slow memory is uninitialized on a cold power-up; the magic marks
     * anything carried across a deep sleep. */
    if (s_rtc.magic != RTC_STATE_MAGIC) memset(&s_rtc, 0, sizeof(s_rtc));
    s_rtc.magic = RTC_STATE_MAGIC;
    s_rtc.wake_seq++;

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_device_id, sizeof(s_device_id), "xiao-%02x%02x%02x", mac[3], mac[4], mac[5]);
    ESP_LOGI(TAG, "open-birdhouse cam %s, wake #%lu (cause=%d, reset=%d)",
             s_device_id, (unsigned long)s_rtc.wake_seq,
             (int)esp_sleep_get_wakeup_cause(), (int)esp_reset_reason());

    /* A panic/brownout/watchdog reset must NOT re-run the full burst right
     * away: that re-loads RF+camera onto a sagging rail (brownout boot-loop
     * at full current). Back off first, then resume the 60 s cadence. */
    esp_reset_reason_t rr = esp_reset_reason();
    if (rr == ESP_RST_BROWNOUT || rr == ESP_RST_PANIC || rr == ESP_RST_INT_WDT ||
        rr == ESP_RST_TASK_WDT || rr == ESP_RST_WDT) {
        ESP_LOGW(TAG, "abnormal reset, backing off without working");
        esp_sleep_enable_timer_wakeup(FAULT_BACKOFF_US);
        esp_deep_sleep_start();
        __builtin_unreachable();
    }

    /* Persistent NVS corruption must not panic-reboot-storm either: repair
     * once the standard way, else sleep to the next cycle. */
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_err = nvs_flash_init();
    }
    if (!esp_call_ok(nvs_err, "nvs_flash_init")) goto_sleep();

    if (!wifi_join_blocking()) goto_sleep(); /* retry next cycle */

    if (camera_init() != ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(200));
        if (camera_init() != ESP_OK) {
            ESP_LOGE(TAG, "camera init failed, skipping this cycle");
            goto_sleep();
        }
    }

    /* Discard a couple of frames so the fresh-start AE/AWB settles before the
     * shot we keep. */
    for (int i = 0; i < 2; i++) {
        camera_fb_t *warm = esp_camera_fb_get();
        if (warm) esp_camera_fb_return(warm);
    }

    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        ESP_LOGE(TAG, "frame grab failed, skipping this cycle");
        goto_sleep();
    }

    /* Frame is now ours (PSRAM buffer owned until fb_return); park the sensor
     * before the upload so it doesn't idle-draw through the network I/O. */
    camera_sleep_powerdown();

    bool ok = upload_frame(fb, "photo") || upload_frame(fb, "photo");
    ESP_LOGI(TAG, "shot: %u bytes -> %s", (unsigned)fb->len, ok ? "OK" : "FAIL");
    esp_camera_fb_return(fb);

    goto_sleep();
}
