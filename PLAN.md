# Plan — open-birdhouse

XIAO ESP32S3 Sense (onboard camera, 8MB PSRAM) takes bird photos and POSTs
them to an ingest service on the LAN (dev: this laptop, prod: a Pi).
Hardwired to power; on-device bird/no-bird model gates capture.

## Architecture

Device: OV2640/OV3660 low-res probe frames -> int8 bird/no-bird classifier
(tflite-micro + esp-nn, ~1-2 fps) -> on bird-present edge (debounced):
capture XGA JPEG -> HTTP POST photo + probe frame + score. Timer-triggered
mode stays as fallback / bootstrap data collection.

Receiver (FastAPI): stores uploads under data/<kind>/<date>/HHMMSS_<device>.jpg.
Review UI: Vite+React+Tailwind+shadcn SPA (webui/) served from webui/dist at /.
Classification states: unclassified / no_bird / bird_identified / species_identified,
each row carrying provenance (manual|kimi, model, confidence, note) in ingest/labels.db.
Kimi (Moonshot vision API, ingest/kimi.py) makes the bird/no-bird + species call;
manual review always wins locally. Needs MOONSHOT_API_KEY in ingest/.env.

## Hardware facts (verified this session)

- Board: ESP32-S3 QFN56 rev v0.2, 8MB flash + 8MB octal PSRAM, MAC 7c:4f:ad:* (Seeed OUI) = XIAO ESP32S3 Sense.
- Camera pins (fixed by Sense board): XCLK=10, SCCB SDA=40 SCL=39,
  Y9..Y2 = 48,11,12,14,16,18,17,15, VSYNC=38, HREF=47, PCLK=13. Sensor model auto-detected.
- Console = native USB-Serial/JTAG (/dev/cu.usbmodem*). Reading the port must NOT
  assert DTR (pyserial open() stalls the app in download mode); scripts/grab_photo.py
  uses raw fds instead.
- No onboard WiFi antenna: U.FL rod antenna must be attached before Phase 3.
- IDF v5.2.1 at ~/esp/esp-idf-v5.2.1; export with IDF_PYTHON_ENV_PATH=~/.espressif/python_env/idf5.2_py3.14_env (system python upgrade pitfall).

## Phases

1. **Camera bring-up** — DONE 2026-10-31: XGA 1024x768 JPEG (28KB q12) captured on-device,
   base64 over the USB-CDC console, decoded by scripts/grab_photo.py, verified as a real
   scene (data/first_photo.jpg). Sensor model not printed in captured window; irrelevant.
   Firmware emits a photo every 8 s between --PHOTO-BEGIN--/--PHOTO-END-- markers.
   Note: sensor is mounted on the loose Sense board (image was sideways) — mount it before Phase 3 field tests.
2. **Ingest service** — DONE 2026-10-31: FastAPI /upload (photo+probe kinds) verified via
   curl (exact byte round-trip), gallery + static serving return 200. Runs on :8077 via
   ingest/run.sh. Pitfall fixed: StaticFiles mount needs DATA dir to exist at import.
3. **Capture+upload loop** — DONE 2026-10-31: firmware joins sleepycatwifi (U.FL fitted,
   +heatsink), POSTs XGA JPEG every 8 s to http://<laptop en0>:8077/upload?kind=photo with
   X-Device-Id xiao-<mac tail>; serial byte counts match stored file sizes exactly.
   Secrets in main/secrets.h (gitignored; .example provided). Laptop is wired on en0 —
   same LAN as the WiFi, reachable. Port note: re-enumerates on replug (currently usbmodem11301).
   3b **Deep-sleep cadence** — BUILT (not yet flashed): every wake = cold boot from deep
   sleep every 60 s: WiFi join (fast-join via BSSID/channel cached in RTC-slow mem,
   full-scan fallback), camera init, 2 warmup frames discarded, 1 XGA JPEG POSTed
   (+1 retry), OV3660 software standby (SCCB 0x3008 bit6 — same board/sensor measures
   ~1.5 mA asleep vs 22-38 mA if left running; OV2640 can't standby), then
   esp_deep_sleep_start(). BOOTLOADER_SKIP_VALIDATE_IN_DEEP_SLEEP trims boot.
   Any failure path (WiFi/camera/upload) still sleeps the full 60 s — fixed cadence.
4. **Training loop offline** — tooling DONE 2026-10-31: v1 clicker superseded by the
   branch-keen-kestrel pipeline: state model + provenance in labels.db (v1 binary labels
   auto-migrate at startup; sqlite WAL + startup-only migration after a lock race),
   kimi.py vision client (base64 image_url; URL-form images unsupported), /api/classify[_pending],
   review SPA with filter tabs, per-frame kimi/manual actions, species dialog, CSV export
   (path,state,species,source,confidence,note). Verified via curl + real browser; SPA build clean.
   Dev runs: ingest on :8077 (device unchanged), vite dev on :5173 proxying /api,/files.
   TODO: add MOONSHOT_API_KEY to ingest/.env (see .env.example) and batch-classify.
   Remaining: fine-tune MobileNetV3-Small + quantize once real-scene frames accumulate.
5. **On-device gating** — flash model, probe loop @1-2 fps, debounce + burst capture on bird edges.
6. **Pi deployment** — same ingest service on the Pi, point device at it. Hardening: retries, buffers.

Decision at end of Phase 3 (alt): stream probes to Pi for a real detector instead
of on-device model — pipeline is the same either way.

## Layout

- firmware/   ESP-IDF project (esp32-camera managed component)
- ingest/     FastAPI ingest + classification API + kimi client
- webui/      review SPA (Vite + React + TS + Tailwind v4 + shadcn)
- scripts/    host tooling (grab_photo.py, ...)
