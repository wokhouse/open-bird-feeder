// Phase 1: camera bring-up for the open-birdhouse XIAO ESP32S3 Sense.
// Initializes the DVP camera (OV2640/OV3660/OV5640 auto-detected by esp32-camera),
// captures one XGA JPEG every 8 s, and dumps it as base64 between
// --PHOTO-BEGIN-- / --PHOTO-END-- markers on the console (USB-Serial/JTAG CDC).
// Host side: scripts/grab_photo.py decodes and saves the JPEG.

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_camera.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "mbedtls/base64.h"

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

#define SHOT_INTERVAL_MS 8000

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
        .frame_size   = FRAMESIZE_XGA,   /* 1024x768 */
        .jpeg_quality = 12,
        .fb_count     = 2,
        .fb_location  = CAMERA_FB_IN_PSRAM,
        .grab_mode    = CAMERA_GRAB_LATEST,
    };
    return esp_camera_init(&config);
}

/* Base64-encode the framebuffer to stdout in bounded chunks. */
static void emit_photo(const camera_fb_t *fb, unsigned seq)
{
    static char b64[4100]; /* 4/3 * 3000 + padding slack */
    printf("--PHOTO-BEGIN-- seq=%u bytes=%u\n", seq, (unsigned)fb->len);
    size_t off = 0;
    while (off < fb->len) {
        size_t chunk = (fb->len - off < 3000) ? (fb->len - off) : 3000;
        size_t olen = 0;
        int rc = mbedtls_base64_encode((unsigned char *)b64, sizeof(b64),
                                       &olen, fb->buf + off, chunk);
        if (rc != 0) {
            ESP_LOGE(TAG, "base64 encode failed rc=-0x%04x at off=%u", -rc, (unsigned)off);
            break;
        }
        fwrite(b64, 1, olen, stdout);
        off += chunk;
    }
    printf("\n--PHOTO-END--\n");
    fflush(stdout);
}

void app_main(void)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    ESP_LOGI(TAG, "open-birdhouse cam, mac=%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    while (camera_init() != ESP_OK) {
        ESP_LOGE(TAG, "camera init failed, retrying in 2 s (check camera ribbon/seat)");
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    ESP_LOGI(TAG, "camera ready");

    unsigned seq = 0;
    while (true) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) {
            ESP_LOGE(TAG, "frame grab failed");
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        emit_photo(fb, seq++);
        esp_camera_fb_return(fb);
        vTaskDelay(pdMS_TO_TICKS(SHOT_INTERVAL_MS));
    }
}
