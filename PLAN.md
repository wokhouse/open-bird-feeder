# Plan — open-birdhouse

XIAO ESP32S3 Sense (onboard camera, 8MB PSRAM) takes bird photos and POSTs
them to an ingest service on the LAN (dev: this laptop, prod: a Pi).
Hardwired to power; on-device bird/no-bird model gates capture.

## Architecture

Device: OV2640/OV3660 grayscale QQVGA probe frames at ~6.6 fps -> zone motion
detector (firmware/main/zone_motion.c) -> on zone change (debounced, refractory
4 s): capture XGA JPEG -> HTTP POST photo + the probe frame that triggered it +
cell scores. Heartbeat photo every 15 min as fallback / liveness.
Later phase 5b replaces the motion gate with an int8 bird/no-bird classifier
(tflite-micro + esp-nn); the capture/upload path stays the same.

Receiver (FastAPI): stores uploads under data/<kind>/<date>/HHMMSS_<device>.jpg.
Review UI: Vite+React+Tailwind+shadcn SPA (webui/) served from webui/dist at /.
Classification states: unclassified / no_bird / bird_identified / species_identified,
each row carrying provenance (manual|ai, model, confidence, note, bbox) in ingest/labels.db.
Every /upload photo is auto-reviewed in the background by an OpenAI-compatible vision
endpoint (ingest/llm.py, LLM_* env): bird/no-bird + species + normalized bbox; the UI
shows the box over each card and is for manual birder review. Manual review always
wins locally. Needs LLM_API_KEY in ingest/.env.

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
4. **Training loop offline** — tooling DONE 2026-10-31: v1 clicker superseded by the
   branch-keen-kestrel pipeline: state model + provenance in labels.db (v1 binary labels
   auto-migrate at startup; sqlite WAL + startup-only migration after a lock race),
   kimi.py vision client (base64 image_url; URL-form images unsupported), /api/classify[_pending],
   review SPA with filter tabs, per-frame kimi/manual actions, species dialog, CSV export
   (path,state,species,source,confidence,note). Verified via curl + real browser; SPA build clean.
   Dev runs: ingest on :8077 (device unchanged), vite dev on :5173 proxying /api,/files.
   TODO: add MOONSHOT_API_KEY to ingest/.env (see .env.example) and batch-classify.
   Remaining: fine-tune MobileNetV3-Small + quantize once real-scene frames accumulate.
5a. **Zone motion gating** — DONE 2026-11-01: camera runs PIXFORMAT_GRAYSCALE/QQVGA
   (160x120) at 150 ms (~6.6 fps); each frame is compared with the previous one
   inside the seed zone only, so leaves/cars/buildings in the background never
   count. Zone = firmware/main/zone_config.h, as percent of the frame: x 0-100%,
   y 62-100% (tray rim + seed pile; measured from the mounted view, sharpness
   peaks at 65-75% height). Cell 4 px, cell mean |diff| >= 14 counts as hot,
   6 hot cells = motion, 2 consecutive motion frames = capture. The zone-wide
   mean |diff| is subtracted from every cell, so exposure/cloud jumps do not
   trigger. On capture: switch to JPEG XGA (3 settle frames for AEC), upload
   photo, switch back to probe, reset the reference frame. The triggering probe
   frame is uploaded as kind=probe (diag only; ingest keeps probe/ out of the
   gallery and training set). Verified: firmware builds (app 0.98 MB of a 3 MB
   partition, 38% DRAM), `make -C firmware/test run` passes 12 assertions on the
   firmware source, and the same binary run on real frames (her feeder photo ->
   QQVGA gray) scores 12 hot cells for a bird in the zone, 0 for a bird-sized
   blob in the background, 0 for global +20 brightness, 0 for a 3 px leaf shift.
   Tuning tool: `uv run --with numpy --with pillow scripts/zone_preview.py
   photo.jpg --pair after.jpg` draws the live zone and scores the pair.
   Pitfall: the default single-app partition table gives the app 1 MB and the
   build was at 4% free; firmware/partitions.csv (3 MB app) is now the default,
   so flashing the new table is required before the phase-5b model lands.
5b. **On-device gating (classifier)** — train bird/no-bird on the real mounting
   scene (negatives = the actual background), flash int8 model, probe loop
   replaced by the classifier, debounce + burst capture on bird edges.
6. **Pi deployment** — DONE 2026-11-01 (service part): ingest runs on sleepy (Pi, Debian 13)
   as systemd unit birdhouse-ingest.service — repo at /home/fungj/repos/open-birdhouse
   (pull from GitHub main), venv, uvicorn on 0.0.0.0:8077, Restart=on-failure. ingest/.env
   sets LLM_BASE_URL=http://100.73.247.126:8080/v1 (bingbong over its Tailscale IP —
   bingbong.local does not resolve from the Pi) + the API key. Data + labels.db migrated
   from the laptop; laptop service stopped, :8077 there is free. Device INGEST_BASE =
   http://192.168.1.217:8077 (secrets.h, reflashed; device now at 192.168.1.123 via DHCP).
   Web UI: http://sleepy.tail643d2.ts.net:8077 (tailnet/MagicDNS) or sleepy.local:8077 (LAN).
   Pitfall fixed during test: uploads must be the raw JPEG request body — `curl -F` sends
   multipart and the whole envelope gets stored verbatim (corrupt file, classifier 400).
   Hardening: retries, buffers — open.

Decision at end of Phase 3 (alt): stream probes to Pi for a real detector instead
of on-device model — pipeline is the same either way.

## Layout

- firmware/   ESP-IDF project (esp32-camera managed component)
  - main/zone_motion.c   zone frame-differencing detector (no IDF deps)
  - main/zone_config.h   zone rect + thresholds, the only knobs for tuning
  - test/                host gcc harness for the same detector source
  - partitions.csv       3 MB app partition (default 1 MB is too small)
- ingest/     FastAPI ingest + classification API + kimi client
- webui/      review SPA (Vite + React + TS + Tailwind v4 + shadcn)
- scripts/    host tooling (grab_photo.py, zone_preview.py)
