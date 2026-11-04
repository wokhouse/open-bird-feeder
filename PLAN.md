# Plan — open-bird-feeder

XIAO ESP32S3 Sense (onboard camera, 8MB PSRAM) takes bird photos and POSTs
them to an ingest service on the LAN (dev: this laptop, prod: a Pi).
Power: USB-C battery bank at the feeder (battery kept over a cord, 2026-11-01;
enclosure installs must be power-efficient without missing wildlife — see 5c).
On-device bird/no-bird model gates capture.

## Architecture

Device: OV2640/OV3660 grayscale QQVGA probe frames at ~6.6 fps -> zone motion
detector (firmware/main/zone_motion.c) -> on zone change (debounced, refractory
4 s): capture XGA JPEG -> HTTP POST photo + the probe frame that triggered it +
cell scores. Heartbeat photo every 15 min as fallback / liveness.
Later phase 5b replaces the motion gate with an int8 bird/no-bird classifier
(tflite-micro + esp-nn); the capture/upload path stays the same.

Receiver (FastAPI): stores uploads under data/<kind>/<date>/HHMMSS_<device>.jpg.
Review UI: Vite+React+Tailwind+shadcn SPA (webui/) served from webui/dist at /.
Classification states: unclassified / no_bird / bird_identified / species_identified.
ingest/labels.db is schema v2 (see 6c): capture (file identity + device/time/bytes) /
verdict (append-only AI history: model, p_bird, bbox, cost) / human_label (state +
species, only set_state writes it). State is derived at read time: human label wins,
else newest verdict vs JEV_BIRD_P.
Every /upload photo is auto-reviewed in the background by the Jev decision
model (ingest/jev.py, OpenRouter Decisions API, model typesafe/jev-1.14): a
noul question answers P(bird) and a box question returns the bird bbox as
image fractions — typed probabilities, no prompt-and-parse. Species is not
auto: Jev cannot name species, so the birder sets species_identified in the
UI (the fun part). The UI shows the box over each card. Manual review always
wins locally. Needs OPENROUTER_API_KEY in ingest/.env (optional JEV_MODEL /
JEV_BIRD_P, default threshold P(bird) >= 0.5).

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
5c. **Battery operation** — step 1 implemented 2026-11-01, steps 2-3 are design
   notes. The feeder device runs from a USB-C power bank. Pre-d4bd059 firmware
   (sensor streaming + always-on Wi-Fi) drew an estimated 1.3-1.7 W, NOT measured
   (Seeed board specs: Wi-Fi-on ~100 mA, light-sleep 2 mA, deep-sleep 14 uA;
   QQVGA streaming draw unknown). A 10 000 mAh bank = 37 Wh ≈ one day.
   MEASURE FIRST: charge the bank to 100%, run N hours, read the remaining %;
   draw_W = bank_Wh × pct/100 / N.
   Key fact: dropping probe fps saves almost nothing — the OV2640 + DVP DMA
   stream at their own rate no matter how slowly we grab frames. Power comes
   from stopping the stream, not from slower polling.
   Ladder (capture quality first):
   1. Wi-Fi dial-before-call: connect only to upload. ~0.2-0.3 W saved, zero
      capture cost. IMPLEMENTED 2026-11-01 (d4bd059, review fixes c2dd79d):
      radio off while probing; each capture dials once with one 8 s IP gate
      before any encode or upload; a failed dial or missing IP skips the whole
      cycle (no probe encode, no camera mode switch for an un-uploadable
      photo) and the motion gate backs off exponentially (refractory <<
      streak, cap 128 s); stale reference frame reset (skips= counter in
      logs). Review-clean build on bingbong (bin 0.96 MB, 68% app partition
      free). FLASH + bank-gauge measurement pending — flashing in the am.
   2. PIR wake + deep sleep (the trail-cam architecture): deep-sleep until the
      PIR fires, then run the existing probe+capture loop, idle-timeout back to
      sleep. Weeks per bank. Two hardware catches: the PIR must see the tray
      (window/lens in the enclosure), and average draw under ~50 mA trips the
      bank's low-load auto-off — a PIR build must move to the Sense BAT pads
      (onboard single-cell charger SGM40567, ≤110 mA charge) instead of a bank.
   3. Enclosure end-state for installers: LiPo/18650 on the BAT pads + small
      solar (charge caps at 110 mA ≈ 0.4 W average). Solves the auto-off catch.
   Keep the bank inside the enclosure (cold cuts Li-ion capacity).
5d. **Brightness gate: night deep-sleep** — code DONE, built clean on
   bingbong 2026-11-02 (bin 0xf9030) + on the laptop after the sensor-park
   port (birdcam.bin 0xf9120, 68% app partition free, ~+9.6 KB over 5c;
   host harness all-pass); FLASHED, night side VERIFIED ON BOARD 2026-11-02
   23:34 PST (branch brightness-gate).
   Why: the Sense camera has no night
   vision out of the box (IR-cut filter, no IR illuminator), and at night
   passing cars still trip the motion gate. Measured from live captures
   (ingest/data/photo, 2026-11-01/02): mean frame luma 9-15 at night
   (20:00 burst = 7 black photos in 5 min of traffic), headlight frames
   included; ~135 in day; the 17h hour falls through both in one step.
   The mean luma of a probe frame is therefore a light meter with a ~10x
   margin, and needs no extra hardware: frame_mean_luma() sums every 4th
   byte of the Y8 probe frame (4800 samples, negligible next to the
   detector pass). Gate: sample every 20th probe frame (~3 s); DARK_FRAME_MEAN
   = 40 separates night (max 15) from day (min ~130) with the measured
   margins; after DARK_SUSTAIN_MS = 120 s of sustained dark samples the
   device stops the camera and esp_deep_sleep_start()s with a 5-minute
   timer wake (Seeed deep-sleep spec 14 uA vs ~100 mA probing — the night
   hours are ~40% of the cycle, so this dominates the battery math).
   Night flag lives in RTC_NOINIT_ATTR (magic 'NGT1' + check counter); a
   boot with magic + ESP_SLEEP_WAKEUP_TIMER runs light_check() instead of
   the probe loop: 8 frames at 500 ms, >= 5 bright (>= DARK_FRAME_MEAN)
   resumes the day, else sleep again. Headlights at night sit under the
   threshold, so traffic cannot end the night; a false resume (porch
   light) re-sleeps after DARK_SUSTAIN_MS. Cold/brownout/button reset
   clears the flag (a power-on at night re-enters after 2 min of dark
   confirmation). Camera init failure during a check sleeps on (retry next
   period) instead of spinning. Logs now carry luma= on the periodic probe
   line; the startup line prints the gate config. NIGHT SIDE MEASURED ON
   BOARD 2026-11-02 (laptop build 0xf9120): dark detected at boot+1.6 s
   (luma=6), night entry at dark+120 s — USB-CDC drops with the sleep —
   timer wake at exactly +5m00s, port present 5.6 s (one full 8-frame
   check) then re-sleep; ingest total frozen through the cycle (no more
   black uploads). Ported from tall-mule (its duty-cycle architecture is
   superseded by 5c, the power pieces are not): OV3660 software power-down
   on EVERY camera_stop (must run before esp_camera_deinit, which NULLs the
   sensor handle; ~1.5 mA asleep vs 22-38 mA) and
   BOOTLOADER_SKIP_VALIDATE_IN_DEEP_SLEEP=y (~100 ms off every light-check
   wake). Verify wake cycles by PORT LIFECYCLE, never by opening the port:
   opening asserts DTR/RTS -> USB_UART_CHIP_RESET, aborting a running
   light check and making the next boot read as cold (fresh day, night
   flag cleared). DAWN SIDE MEASURED FROM INGEST 2026-11-03 (no console
   needed: journal + capture DB): installed outdoors with this build,
   motion captures until 00:17:08, night entry ~00:19 (dark+120 s), then
   6h14m with ZERO uploads (00:17:08 -> 06:31:04) - the old firmware
   posted a heartbeat at least every 15 min, so the silence is the sleep
   proof. First motion capture 06:31:04, around sunrise, inside the
   5-min wake budget. Rest of the day: photo/probe back 1:1, zero night
   frames. Pre-flash waste quantified: old firmware produced 200 black
   probe+photo cycles on car traffic 18:00-00:17 (photos later deleted
   from the review UI; the ingest journal stands). Bank math: gauge read
   3/4 of 5000 mAh after ~13 h of the old firmware = ~95 mA average;
   deleting the dark hours cuts ~0.8-1.2 Ah/day down to ~9 mAh (1.5 mA
   parked x 6 h) - expect ~2 -> ~4-5 days per charge, UNMEASURED.
   BANK LOW-LOAD AUTO-OFF (the night 5c step-2 catch, confirmed on the
   second night): night 11-02->03 did NOT trip (dawn wake from ingest,
   above). Night 11-03->04 DID: last capture 22:48, silence until
   08:58 - sunrise 06:38 PST, and the 5-min light-check wake should
   have resumed by ~07:00. Sleep draw (parked ~1.5 mA + 5.6 s wake
   every 5 min, ~2-4 mA average) is far under the ~50 mA cutoff;
   whether a bank trips is timing/bank luck, not a stable property.
   A sleeping device cannot self-recover: with the rail down nothing
   re-presents a load, so the 08:58 return = manual bank wake + cold
   boot into a bright day. Fix path = 5c step 2/3 (LiPo on the Sense
   BAT pads: no bank, no auto-off). Interim: a bank with a
   low-current/always-on mode, or wall power overnight.
   TODO: gate behaviour on overcast/rain days, the real sleep
   draw via the 5c bank-gauge method, and an endurance run to confirm
   the bank-life numbers.
6. **Pi deployment** — DONE 2026-11-01 (service part): ingest runs on sleepy (Pi, Debian 13)
   as systemd unit open-bird-feeder-ingest.service — repo at /home/fungj/repos/open-bird-feeder
   (pull from GitHub main), venv, uvicorn on 0.0.0.0:8077, Restart=on-failure. ingest/.env
   sets LLM_BASE_URL=http://100.73.247.126:8080/v1 (bingbong over its Tailscale IP —
   bingbong.local does not resolve from the Pi) + the API key. NOTE superseded by phase 6b:
   the service now reads OPENROUTER_API_KEY + JEV_MODEL; the LLM_* variables are retired.
   Data + labels.db migrated
   from the laptop; laptop service stopped, :8077 there is free. Device INGEST_BASE =
   http://192.168.1.217:8077 (secrets.h, reflashed; device now at 192.168.1.123 via DHCP).
   Web UI: http://sleepy.tail643d2.ts.net:8077 (tailnet/MagicDNS) or sleepy.local:8077 (LAN).
   Pitfall fixed during test: uploads must be the raw JPEG request body — `curl -F` sends
   multipart and the whole envelope gets stored verbatim (corrupt file, classifier 400).
   Hardening: retries, buffers — open.
6b. **AI switch: chat LLM → Jev decision model** — code DONE + verified 2026-11-01 (branch jev):
   llm.py (OpenAI-compat chat + JSON parsing) replaced by jev.py: one POST
   https://openrouter.ai/api/alpha/decisions with the frame in `state` asks two
   typed questions — noul (P(bird)) and box (bird bbox as image fractions).
   Same verdict shape as before, so app.py/review UI/CSV are unchanged; species
   is intentionally never auto-set (Jev cannot name it; manual step stays).
   Verified on real calls (typesafe/jev-1.14-20261014): 3 downloaded real bird
   photos -> p_bird 0.95-0.98 with usable boxes (visual check: 2 tight, 1 clips
   a tail tip); 2 of our human-labeled empty-feeder frames -> p_bird 0.05-0.07,
   no box. ~0.3-0.5 s per frame, ~$0.00011 per call. End-to-end through
   /upload -> background auto_classify -> /api/images on a throwaway :18077
   server: no_bird + bird_identified rows correct, set_state species works.
   Pitfalls learned: Jev 400s "Invalid image dimensions" on a 1280x1707 image
   (1280x960 passes; our camera frames are 1024x768, fine); Box needs 1.14+;
   Box has no not-found answer so the noul verdict gates it; Box/asset formats
   are marked early features by TypeSafe. Reviewed by two read-only subagents
   (API contract + integration); fixes applied: AI re-runs never destroy an
   existing species label (set_state is the only species editor); the box
   question is gated on model support so a JEV_MODEL < 1.14 still yields the
   noul verdict; bad/out-of-range JEV_BIRD_P falls back to 0.5 with a warning
   instead of crashing the service or poisoning every label; one retry on
   429/5xx; error bodies decoded defensively with error.message surfaced;
   rejected boxes traced as box=rejected; stale LLM_API_KEY hints removed
   (webui banner, run.sh). DEPLOY: prod ingest/.env now carries
   OPENROUTER_API_KEY (~/.hermes/.env key updated too; the old one 401s),
   merge jev -> main, restart open-bird-feeder-ingest. Tune JEV_BIRD_P once real bird
   captures exist (all 327 live labels are no_bird, so no positive calibration
   yet).
6c. **Storage schema v2: captures / verdicts / human labels split** — code DONE +
   verified 2026-11-01 (branch schema-v2, from main@15dc71f): the fused
   `classifications` table (one row per path, AI re-runs overwrote provenance,
   p_bird smeared into note text) is replaced by capture (path/kind/device_id/
   taken_at/bytes — the facts the filename was already carrying), verdict
   (append-only AI history with structured p_bird REAL, model build, cost,
   latency) and human_label (state+species; set_state is the only writer, so AI
   re-runs physically cannot touch manual work). State derives at read time:
   human wins, else newest verdict vs JEV_BIRD_P (legacy pre-Jev verdicts have no
   structured probability — their confidence was not 1-P(bird) — so they keep
   their old state verbatim), else unclassified. Payoff proven on the live data
   copy: setting JEV_BIRD_P=0.05 flipped exactly the 2 Jev verdicts (p 0.070/
   0.080) to bird without any API calls — threshold calibration now works on
   history. /api/* wire format byte-identical to v1 (verified: /api/images,
   /api/stats, /export.csv diff clean against the live server), so review UI,
   CSV export and firmware are untouched. migrate_v2() rewrites at startup
   (328 rows -> 328 capture + 274 verdict + 54 human_label) and keeps the old
   table as classifications_v1. set_state unclassified now clears both label
   and verdict history. Deploy: restart open-bird-feeder-ingest (migration runs on
   boot); back up labels.db first.

Decision at end of Phase 3 (alt): stream probes to Pi for a real detector instead
of on-device model — pipeline is the same either way.

## Layout

- firmware/   ESP-IDF project (esp32-camera managed component)
  - main/zone_motion.c   zone frame-differencing detector (no IDF deps)
  - main/zone_config.h   zone rect + thresholds, the only knobs for tuning
  - test/                host gcc harness for the same detector source
  - partitions.csv       3 MB app partition (default 1 MB is too small)
- ingest/     FastAPI ingest + classification API + Jev decision client (jev.py)
- webui/      review SPA (Vite + React + TS + Tailwind v4 + shadcn)
- scripts/    host tooling (grab_photo.py, zone_preview.py)
