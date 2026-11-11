# Plan — open-bird-feeder

XIAO ESP32S3 Sense (onboard camera, 8MB PSRAM) takes bird photos and POSTs
them to an ingest service on the LAN (dev: this laptop, prod: a Pi).
Power: USB-C battery bank at the feeder (battery kept over a cord, 2026-11-01;
enclosure installs must be power-efficient without missing wildlife — see 5c).
On-device bird/no-bird model gates capture.

## Architecture

Device: OV2640/OV3660 grayscale QQVGA probe frames at ~6.6 fps while the zone is
hot; a cold zone parks the sensor between ~1.5 s detection windows (5c step 1b)
-> zone motion
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
   1b. Sensor-park windowed idle: IMPLEMENTED 2026-11-07 (branch
      power-consumption-review; build 1.01 MB, 68% partition free, host
      detector tests pass; flash + field A/B pending). Whole-app power audit
      verdict: the sensor + DVP stream free-running all day is the dominant
      daytime draw (Seeed forum: ~90 mA board floor after camera init vs 2-3
      mA uninitialized; board ready-to-operate floor 5V@38.3 mA; LDO means
      3.3V-side savings x1.51 at the bank). A fleet of read-only subagent
      audits (camera driver / CPU-PM / wifi / detector / board+measurement)
      ranked the levers; detector CPU (<2 mW), grab rate, fb layout, probe
      size are all rounding errors — do not chase those. This step parks the
      sensor between windows with its standby register, no esp_camera_deinit
      (SCCB + DVP receiver stay up, registers retained): OV3660 SYSTEM_CTROL0
      0x3008 bit6 (measured on this device: 1.5 mA parked vs 22-38 mA
      streaming), OV2640 COM2 0x3009 bit4 (datasheet register-retentive
      standby) — sensor_set_parked() in main.c picks by s->id.PID. Window =
      unpark -> discard 2 wake frames -> 2 probe frames -> park;
      PROBE_IDLE_MS=1500 between windows (est. ~25-40% sensor duty, NOT
      measured). Any hot cell extends a 2.5 s continuous hot-hold, so
      wildlife always runs the untouched 5a trigger pipeline; only the
      empty-feeder phase duty-cycles. Unpark failure self-heals via full
      camera re-init; sensors without a known standby bit stay on the old
      continuous loop. No field ammeter: the "probing:" log line now reports
      windows + active-vs-parked duty %, and /api/stats capture counts A/B
      the quality. Desk-verified 2026-11-07 (14.5 min run): cold scene sat
      at duty=15% / 148 windows in the first 5 min; 8 motion captures ran
      the full dial -> upload -> hot-hold path, zero upload failures, zero
      faults; duty% climbs only while the zone is actually busy. Caveat for
      the pole: hot-hold extends on hot_cells>0, so constant single-cell
      ambient motion (leaves) would keep the sensor streaming — if the pole
      logs show high duty with no captures, raise the extension threshold. Tuning knobs: PROBE_IDLE_MS / WINDOW_FRAMES /
      PROBE_HOT_HOLD_MS in main.c. Also fixed: mDNS is in sdkconfig.defaults
      now — it had lived only in a generated sdkconfig, so any clean build
      would silently break sleepy.local uploads.
   1c. Night bank-keeper: IMPLEMENTED 2026-11-07, field test tonight.
       First field result of 1b: daytime captures great, but the battery
       bank cuts output at night — the windowed night idle sits under the
       bank low-load cutoff (~50 mA avg at 5V), exactly the PLAN's
       documented bank trap (this build has no 5d night gate; that lives
       unmerged on brightness-gate, so the pole ran windowed probing all
       night). Fix in the probe loop: absolute frame luma via
       frame_mean_luma() (zone_result_t.global_mean is a DIFFERENCE, not
       light level — first attempt using it was wrong), sustained <40
       (threshold verified in 5d's night-gate tests; AEC lifts real night
       scenes to the 20s-30s) for 30 detection frames (~50 s) = night ->
       windowing disabled, continuous probing restores the ~0.3-0.4 W
       full-stream load that kept banks alive pre-5c. Any frame >= 60
       (hysteresis) = dawn -> windowing resumes. Night cost ~4-5 Wh —
       intentional load; day savings untouched. If a bank has a
       low-current keep-on mode, that's the zero-waste alternative; and
       if the bank was actually DRY (not tripped), more night load makes
       things worse, not better — check which it was.
       1d. Bank-keeper duty floor (DAY): IMPLEMENTED + desk-verified
        2026-11-07/08. Field follow-up: the bank trips on cold windowed
        DAYS too (empty feeder = 15% duty is under its low-load cutoff).
        The probe loop now tracks streaming duty with an EMA (scaled
        256=100%, tau=120 s ~ bank trip timers); below MIN_DUTY_PCT=45%
        parking is skipped ("bank keeper: duty N% under floor, holding
        continuous") until projected duty recovers past 55%
        ("windowing resumes"). Busy scenes clear the floor by themselves —
        only the cold idle pays. Verified cycle on desk: boot hold ->
        release at 67%/t+84 s -> 85 windows of savings -> re-latch at
        44%/t+287 s -> re-release; cumulative duty pinned at 50%
        (≈88 mA at 5V, above any common bank cutoff); motion captures and
        hot-holds unaffected. First flash had a units bug (pct<<8 vs
        pct*2.56 — keeper latched forever); caught by the on-device test,
        fixed before this commit. Cost reality: a day at 50% duty is
        close to the pre-5c draw — keeping a low-load-tripping bank awake
        costs that; the zero-waste alternative stays a bank with a
        low-current keep-on mode (or no auto-off at all).
   1e. INCREASE_POWER_CONSUMPTION compile flag: IMPLEMENTED + verified
       2026-11-08. The field answer beat more tuning: this pole's bank
       STILL tripped at the 45% duty floor. The flag (default 1 for this
       bank) means the sensor is NEVER parked — no windowing, no keeper
       logic fires; plain continuous probe, i.e. the pre-5c load that
       always kept banks awake. All other power work (dial-before-call,
       capture pipeline) stays on, and the whole windowed/keeper
       machinery stays compiled and armed: set the flag to 0 when a
       bank without low-load auto-off is fitted and the savings return.
       User asked for maximum simplicity — one flag, one line disables
       the parking path (can_window init), keeper trigger gated on
       can_window so its logs go silent too. Verified on-device: zero
       park/keeper lines, zero wake noise, duty=100%, captures still
       uploading, zero faults. Daytime draw is now ~130 mA at 5 V by
       design; the bank, not the firmware, remains the constraint.
       (The night path was NOT gated as of this step — fixed in 1f.)
   1f. Pre-merge review cleanup (4-reviewer fan-out, 2026-11-08):
       applied, behavior-preserving: the night luma state-machine is now
       gated on can_window (it previously ran and logged "night:
       bank-keeper on"/"dawn detected" claims even with the flag=1
       build, contradicting 1e); the hold decision is one named source
       (HOLD_REASON: night|duty-floor|hot|no-park) consulted at both
       decision points, replacing the duplicated De-Morgan'd predicate
       and the half-running window churn under flag=1; the probing
       gauge now includes the open mode segment (duty= was stale during
       long holds — wrong exactly when keepers act) and logs hold=<reason>
       instead of keeper=0/1, so the field can see WHICH hold is active;
       shared helpers replace three copies of the probe-restart loop and
       two same-value settle macros (now_ms(), discard_frames(),
       PROBE_SETTLE); clock snapshots renamed for honesty
       (loop_start_ms/frame_ms/iter_end_ms — they are deliberately
       different: uploads consume seconds); misleading comments fixed
       (night luma is whole-frame mean NOT zone mean; ~50s claim was
       mode-dependent ~4.5-35 s; "20s-30s" was luma values).
       Deferred to a post-merge follow-up (verified behavior, don't
       disturb pre-merge): the day duty-floor has no steady state with
       current knobs — windowed duty ≈28% < 45% floor means HOLD/RESUME
       oscillation every ~70-90 s in a flag=0 build (reviewer simulated
       the fixed-point loop; matches the 84s→287s→320s desk pattern);
       duty_ema integer folds truncate; unify the three latches into one
       per-iteration mode decision; SCCB park-failure fallback; the
       windows= counter mixes two window kinds. Re-run 5c-1b desk
       verification before ever setting the flag to 0.
       -> ALL FIVE ADDRESSED IN 1g (branch flag0-power-followups).
   1g. Flag=0 follow-ups: IMPLEMENTED + desk-verified 2026-11-08.
       (1) Duty math moved to Q16 (DUTY_SCALE/DUTY_SCALED/DUTY_PCT
       macros, scale 65536=100%): Q8 folds of real 150-1500 ms segments
       truncated asymmetrically, so the old controller drifted below its
       own math. Proof of accuracy: on-device release fired at exactly
       the computed 55% (t=67.7 s; host sim of the exact integer loop
       predicted 66 s) and re-latch at 44%.
       (2) Steady-state precheck: window ceiling duty =
       (PROBE_SETTLE+WINDOW_FRAMES)*PROBE_INTERVAL_MS / (that +
       PROBE_IDLE_MS) is computed once at boot. If the ceiling is under
       MIN_DUTY_PCT (it is: 28% vs 45% with shipped knobs), releasing
       bank_hold only re-latches a minute later — so the keeper latches
       ONCE and holds continuous until reboot, with a log naming the
       ceiling and the retune needed. The ~70-90 s oscillation cannot
       happen anymore.
       (3) 'parked' is now the ONE sensor-state flag: mode_active
       deleted, MODE_CLOSE() folds the gauge/EMA before every parked
       mutation (no second flag to desync).
       (4) Degraded SCCB: 3 consecutive set_reg park failures =>
       windowing off with ONE error log (was: warn every ~2 s forever
       + re-init storm); continuous streaming is the fallback.
       (5) windows= counts every detection-window open (unpark AND
       hot-expiry resume) — detection duty gauge, not park cycles.
       On-device test build (NOT shipped: flag=0, PROBE_IDLE_MS=400,
       LOG_EVERY_FRAMES=100): boot hold -> release at 55% -> windowing
       ran (windows 1->135, wake FB-SIZE noise normal, 4 captures
       uploaded, zero park/unpark/grab faults) -> one margin-cycle
       re-latch at 44% -> released again at 55%. Lesson for any real
       flag=0 deploy: bench window cycles run ~1.0-1.5 s wall (SCCB
       wake + 100 ms wait granularity), so measured window duty lands
       near the floor with IDLE=400 — pick PROBE_IDLE_MS so the FIELD
       'probing:' duty sits clearly above MIN_DUTY_PCT, else keep
       flag=1. Board returned to the shipped flag=1 build and re-flashed.
   1h. Darkness gate on CAPTURES: IMPLEMENTED 2026-11-08 (field defect).
       Field report: the pole uploaded 33 photos whose stored JPEGs measure
       mean luma 10-40 (server-side measurement over the last 4 days of
       data/photo; 21 of them after 17:21 local on 11-08 while the scene
       went dark). Nothing in the shipped build ever gated captures on
       light: 5c-1c's night luma state machine only disables sensor
       PARKING, and 5c-1f gated that block on can_window — so in the
       flag=1 build it did not run at all. After dark the only zone
       "motion" left is traffic, and its photos are black.
       Fix: the luma state machine runs unconditionally (one 2700-sample
       strided pass per probe frame — negligible), and the shoot decision
       is gated on !night for BOTH the motion and the heartbeat path.
       Thresholds unchanged: enter <40 sustained (NIGHT_DARK_FRAMES=30 =
       ~4.5 s while streaming), exit >=60; measured night 9-30, day ~135,
       headlight frames sit near the night floor, so a passing car cannot
       open the gate. Night/dawn logs now name what they actually do
       ("captures off", windowing clause only when the build can window),
       and HOLD_REASON reports "no-park" first so the flag=1 gauge stops
       claiming "night" is acting.
       Deliberate scope: this is NOT the 5d deep-sleep night gate (still
       unmerged on brightness-gate). This bank wants continuous load, so
       the probe loop keeps streaming all night; only the photos stop —
       and with them the night probe frames, which ride the same shoot
       path. Field check with no console: uploads stop ~5 s after the
       scene goes dark and resume on the first trigger once luma >= 60.
       Not flashed here (no board on this box); built on bingbong.
   2. PIR wake + deep sleep (the trail-cam architecture): deep-sleep until the
      PIR fires, then run the existing probe+capture loop, idle-timeout back to
      sleep. Weeks per bank. Two hardware catches: the PIR must see the tray
      (window/lens in the enclosure), and average draw under ~50 mA trips the
      bank's low-load auto-off — a PIR build must move to the Sense BAT pads
      (onboard single-cell charger SGM40567, ≤110 mA charge) instead of a bank.
   3. Enclosure end-state for installers: LiPo/18650 on the BAT pads + small
      solar (charge caps at 110 mA ≈ 0.4 W average). Solves the auto-off catch.
   Keep the bank inside the enclosure (cold cuts Li-ion capacity).
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

6d. **Cheaper decision models for bird/no-bird (bake-off 2026-11-02)** — the
   Decisions category has grown to 33 models (23 image-capable); all speak the
   same `POST /api/alpha/decisions` contract, so `JEV_MODEL` in ingest/.env is a
   one-line switch (`_supports_box` already gates Box off non-Jev models).
   Measured on a 29-frame evalset (20 bird / 9 no-bird, Wikimedia feeder-scene
   photos: perched songbirds + squirrel/empty-feeder negatives, 900px, visual-QC
   relabelled), same noul payload as jev.py, threshold 0.5:
   | model | acc | min p(bird) | max p(nobird) | $/img | tok/img |
   | typesafe/jev-1.14 (current) | 29/29 | 0.970 | 0.300 | 0.000091 | 2170 |
   | perplexity/pplx-decider-flash-v1.2 | 29/29 | 0.976 | 0.026 | 0.000008 | 817 |
   | inception/mercury-decide | 29/29 | 0.998 | 0.004 | 0.000016 | 779 |
   | perplexity/pplx-decider-v1.2 | 29/29 | 0.990 | 0.088 | 0.000016 | 817 |
   | liquid/d1.1 | 29/29 | 0.996 | 0.069 | 0.000022 | 1112 |
   | cloudflare/clef-flash | 29/29 | 0.822 | 0.117 | 0.000033 | 863 |
   | baseten/joey-1 | 28/29 | 0.976 | 0.904 | 0.000009 | — (1 FP on a birdbath frame) |
   Live Jev baseline from labels.db: $0.000106/img avg over 293 verdicts.
   Box questions remain Jev-only, so any switch drops the bbox enrichment —
   and on this question even Jev only reaches max-p 0.30 on no-birds: the
   cheap models separate the classes harder. Caveats: evalset is DSLR-closeup
   positives, not yet small
   birds in the fixed 1024x768 frame; re-run the margin check on real captures
   before flipping the default. Best all-round switch: pplx-decider-flash-v1.2
   (12x cheaper, margin 0.976/0.026); best margin: mercury-decide. Keep-a-box
   option: cheap noul gate first, Jev-1.14 Box question only on bird=True
   frames — with a ~100% no-bird live stream that pays for Box anyway.
   Scripts: ~/.hermes/cache/scratch/{fetch_commons,bakeoff}.py,
   bakeoff_results.json next to them.
   SWITCH EXECUTED 2026-11-02 per decision (bbox explicitly deprioritized
   until real bird photos exist): ingest/.env (backed up .env.bak-20261102)
   now sets JEV_MODEL=perplexity/pplx-decider-flash-v1.2; service restarted,
   active; verified through jev.py on evalset frames — bird p=0.991 / no-bird
   p=0.006, box=None as expected, model provenance
   perplexity/pplx-decider-flash-v1.2-20261016, ~$0.00001/img. Re-add a
   box-capable model (Jev or other) once bird photos are in hand.
   Adversarial probe 2026-11-02 (photo/2026-11-02/102158, human holding a
   phone showing a finch photo over an empty tray): p_bird — pplx-flash 0.164,
   jev-1.14 0.390, mercury-decide 0.016, liquid/d1.1 0.579 (only one to call
   bird). Picture-of-a-bird ≠ real bird is the production-correct reading
   under the current criteria; liquid/d1.1 fails this case — deprioritized.

6e. **ntfy bird alerts** — DONE + verified 2026-11-02: ingest/notify.py pushes
   to ntfy when a verdict FLIPS a capture's effective state to
   bird_identified — dedupe via effective_is_bird() checked before the verdict
   insert (human_label wins, else newest verdict), so re-classifying an
   already-bird frame pushes nothing (measured: re-POST /api/classify, pushes
   4→4). Wiring is inside apply_verdict, so /upload auto-classify,
   /api/classify and classify_pending are all covered by one hook. Message:
   JSON-body publish (POST {server}/, topic in body — sidesteps header
   encoding entirely), title "🐦 Bird at the feeder", body = HH:MM + p_bird +
   model, priority 3, tags [bird], click = the frame's /files/ URL on
   http://sleepy.tail643d2.ts.net:8077 (INGEST_PUBLIC_URL). No inline image:
   ntfy.sh's nginx 414s query-string base64 above ~5 KB (measured: 4985-byte
   URI 200, 9699-byte 414; a 160px preview of a real frame is already at the
   edge), and the Attach header would make ntfy.sh itself fetch the
   tailnet-only URL. End-to-end on a test server (:18077, real DATA/DB):
   Commons sparrow JPEG → /upload → verdict p=0.994 → push seen via
   poll API; test frames deleted after (total back to device-only count).
   Config: NTFY_TOPIC in ingest/.env (unset = feature off), NTFY_SERVER
   default ntfy.sh. Pitfall hit: writing the topic by hand into .env mangled
   the UUID into "729e9f…5a53" (an actual … byte, len<36) → ntfy
   400 "topic invalid"; assemble long hex from fragments in shell, and check
   ${#VAR} for length before trusting a written identifier.

6f. **Review UI: URL-pinned category + mobile tab row** — DONE + verified
   2026-11-02 (webui): the active filter tab now syncs to the address bar as
   ?view=<state> (replaceState on change, popstate + init read; unknown values
   fall back to unclassified), so refresh and shared links land on the same
   category. Verified with headless Chromium at 390x844: click species ->
   URL ?view=species_identified, reload -> species active. The old
   TabsList h-auto flex-wrap broke into a ragged two-row pill stack on
   phones; replaced with one non-wrapping full-width strip
   (overflow-x-auto overscroll-x-contain, sm:w-fit restores the compact
   desktop pill). Edge fades (mask-image gradient driven by a
   scroll+ResizeObserver probe) mark hidden tabs; re-run the probe on
   document.fonts.ready — web-font swap widens the row after mount and the
   initial update() misses it (measured pixel walk: fade absent at load,
   present after the fonts.ready fix; docW=390=innerW, no page overflow).

6g. **Sort by bird probability** — DONE + verified 2026-11-02 (test server
   :18077, real DATA/DB; live restart pending, sudo is blocked in the agent
   sandbox): rows() adds an ADDITIVE "p_bird" key to /api/images — the raw
   P(bird) of the newest verdict (null for manual/legacy/unclassified;
   confidence stays the decision-confidence display parity). CSV export
   untouched. webui: header select newest/oldest/bird p high→low/low→high,
   persisted as ?sort= next to ?view= (same replaceState+popstate path);
   p-less frames always sort last; cards show "p(bird) NN%" instead of the
   old confidence %. Verified by headless Chromium: 1395/1424 rows carry p,
   deep link ?view=all&sort=p_desc renders 15,14,13…% descending, p_asc
   ascending, both params survive reload, all 29 p-less cards land last,
   zero JS errors.

6h. **Toolbar merge** — DONE + verified 2026-11-02: sort select + Bulk
   Classify moved into the category-tab row (tabs flex-1, controls right;
   flex-wrap puts the controls on their own line below the strip under sm).
   Header keeps title/stats + refresh only. Live :8077 headless check:
   desktop all controls in one band [84,120]; at 390px controls row below
   tabs, strip [16,374]=cards column, docW=innerW, strip still scrollable,
   sort intact, zero JS errors.

6i. **Scroll performance (windowed grid)** — DONE + verified 2026-11-02:
   FrameGrid.tsx replaces the one-big-grid render with @tanstack/react-virtual
   useWindowVirtualizer row windowing (cols mirror sm/lg/xl breakpoints,
   rows keyed by first frame path, dynamic row measurement, gap=GAP,
   overscan 3). FrameCard img gains decoding="async". No user-visible
   pagination; sort/URL/counts untouched. Measured on live :8077 (1545
   frames), same 11-step scripted scroll, before → after: mounted cards
   1545→40 max (20 at rest), DOM nodes 78.9k→2.8k, layout 0.96s→0.15s,
   style recalc 0.71s→0.18s, script 6.2s→2.6s, heap 84MB→9MB, avg step
   1195ms→513ms; image requests unchanged (108). Functional pass at
   1280+390: top sort intact, bottom renders after a jump, sort switch
   re-orders, species dialog opens/closes, no overflow, zero JS errors.

6j. **Bulk actions + shift+click ranges** — DONE + verified 2026-11-03:
   Selection mode: checkbox top-left of every card (outside the img wrapper,
   z-10, size-5); click checkbox OR the image to toggle (image click also
   opens the label dialog — deliberate; a click on the img never reaches the
   dialog buttons, they sit below it). Shift+click extends a range over the
   current visible order (App keeps lastIndex + shownOrder refs; filter/sort
   change resets the anchor; range is cumulative union). Floating
   SelectionBar (fixed bottom, wraps; icon-only buttons below sm) with
   count, no bird / bird / species (one shared species dialog) / clear /
   AI (selected) / delete (2-click arm-confirm, destructive) / select all
   filtered / clear selection. Legacy "Bulk Classify" (all pending) kept in
   the category row. Backend: POST /api/bulk {paths, op: set_state|delete,
   state} + POST /api/classify_bulk {paths} (skip done, cap 5000; both run
   the existing label_write/decision_write paths so audit lines are kept).
   /api/undo unchanged: it restores the last single operation (AI run or
   manual edit) for the most recently operated frame; bulk sets and bulk
   delete are not undoable. Live :8077 headless pass (Playwright):
   shift 0..4→5 rings, extend→9, bulk no bird + API-verified state=manual,
   bulk delete + API-verified gone (2 sacrificial upload frames), species
   bulk via dialog, mobile 390 all bar buttons inside pill no clip, bar in
   viewport, zero JS errors both widths. Gotcha: live camera inserts frames
   between scripted clicks and shifts visible indices — range tests must
   retry in a fresh page (3rd attempt clean), the app range logic itself is
   index-consistent at click time.

6k. **Zoom-reflow scroll anchoring** — DONE + verified 2026-11-11:
   Mobile browsers that reflow on zoom change the CSS viewport; crossing a
   readCols() breakpoint re-slices every virtual row and the pixel scroll
   offset then points at different frames — the zoomed-at card vanished.
   FrameCard carries data-frame-path; FrameGrid samples the first visible
   card every rAF (last painted frame = what the user actually saw), and on
   a cols change a bounded (45-frame) rAF loop restores that card to its
   saved viewport offset: index-space steps toward its row while unmounted
   (estimate error cannot compound), real-rect correction once mounted, and
   it keeps re-correcting for the window because measureElement and Chrome's
   own scroll anchoring shift content after the swap. Measured on live
   :8077 (6585 frames, ?view=all, scroll 12000, viewport 850↔400):
   anchor kept both directions, drift 0px (before: anchor card unmounted
   after re-slice). Selection/bulk pass re-run unchanged ("1 selected").
   Known limit: the browser's own reflow (before JS sees the resize) can
   still move a partially-scrolled card a row's height; the anchor keeps
   that card on screen regardless.

7. **Bird video: motion-gated clips, encoded on the Pi** — design DONE
   2026-11-09, code in progress. No SD card on the device: the ESP32 streams
   the clip to sleepy, which stores and encodes it. The ESP32 has no video
   encoder — a "video" here is a JPEG-frame burst (MJPEG) streamed over HTTP,
   assembled to H.264 MP4 by ffmpeg on the Pi (ffmpeg 7.1 already installed).
   Start = the existing zone-motion trigger (unchanged). Stop = the SAME
   detector, also run on the Pi side over the streamed frames: the Pi
   downscales each received frame to the 160x120 probe geometry and feeds it
   to zone_motion.c compiled for the host (ctypes) — same grid, same tuned
   thresholds, no retune. When the detector sees VIDEO_QUIET_FRAMES consecutive
   non-motion frames it tells the device to stop via HTTP 204 (200 =
   continue); hard cap VIDEO_MAX_SECONDS=20. That closes the user's ask:
   the same pipeline decides when a clip starts AND when it ends.
   Measured bench (real XGA q12 frames from data/photo, Pi4):
   - frame p50 66 KB, p90 70 KB (n=1773, recent days); ~700 gate fires/day.
   - 40-frame burst (8 s @ 5 fps, 2.45 MB MJPEG) -> libx264 crf28 veryfast:
     875 KB MP4, 2.3 s wall encode. 640x480 crf30 fallback: 275 KB.
   - So: MVP streams the SAME XGA JPEG mode the photo capture already runs
     (zero extra camera reconfiguration), 5 fps pacing by 200 ms delays.
     Worst case 700 clips/day: ~2.3 GB/day upload, ~0.6 GB/day MP4 storage.
   Wire protocol (extends /upload, existing kinds untouched):
   POST /upload?kind=video, headers X-Device-Id, X-Event-Id <id>,
   X-Video-Seq <n>, X-Session-Kind motion|ordered, body = concatenated JPEG
   frames (SOI/EOI framing; a truncated tail frame is dropped). The
   X-Event-Id is generated once per capture cycle and ALSO sent on the
   trigger photo's upload — it is the join key between footage and the
   decision model. Chunk = VIDEO_CHUNK_FRAMES=5 frames (~330 KB, ~1 s wall).
   Ingest keeps per-session state in memory, runs the detector per frame,
   answers per chunk.
   THE DECISION MODEL IS IN THE LOOP (decided 2026-11-09): a clip is
   recorded only if the event's decision says bird. On session end the raw
   frames go to data/pending/; if the trigger photo already has a verdict
   (Jev) or human label, the clip is kept (encode + capture row
   kind='video' with the event_id) or discarded (no_bird) immediately; if
   undecided it stays pending, and any later verdict or review label
   decides it (on_verdict hook in app.py). Video Pending longer than
   VIDEO_PENDING_MAX_S (48 h) is kept as-is (undecided = give the human
   the footage) — cheap: raws are ~3x MP4 size and rare.
   LIVE WINDOWS (7c) decide when the camera may stream at all, and the
   gateway owns that state: arm(device, seconds, source=model|browser).
   model: a Jev bird verdict on the event photo arms
   VIDEO_MODEL_WINDOW_S (default 60) — the alert says bird, and the app
   can SHOW the bird: an in-flight session's quiet-stop/cap is overridden
   while the window is open (the footage of the bird is exactly what we
   want), and an idle device learns of the window from the X-Live header
   on its next upload response or from an optional dial poll
   (LIVE_POLL_MS dial + GET /api/live_status, body "<seconds>" or "0";
   ~0.05% duty at 180000 ms). browser: Watch-Live in the review UI posts
   /api/live_arm {device, minutes}; same mechanics; ordered sessions
   (X-Session-Kind: ordered) exist only for the window and are teed to
   the browser but NEVER recorded. Every streamed frame is teed to an
   in-memory ring; GET /live.mjpg?device= relays it as multipart MJPEG —
   the browser never talks to the device (tailnet-safe either way).
   On stop or idle-sweep (session silent > 10 s — covers a mid-session
   upload failure): decided as above; kept clips go through ffmpeg to
   data/video/<date>/HHMMSS_<device>.mp4 + capture row kind='video'. Fewer
   than MIN_VIDEO_FRAMES clean frames: drop. /api/images stays photo-only
   until the review UI handles video (milestone 7b); files are already
   served by the existing /files mount.
   Firmware flow per motion capture (heartbeat stays photo-only): existing
   photo path unchanged through the still upload (now with X-Event-Id);
   then, still in JPEG XGA mode, stream chunks until 204/error/hard cap
   (VIDEO_MAX_FRAMES=300 = 60 s ceiling; the Pi's 204 is the real end,
   and it keeps 204 back while a live window is open); then camera_stop +
   probe_start (existing tail). A heartbeat response carrying X-Live >0,
   or a live_status poll answer >0, runs one ordered session in the same
   dial. Refractory starts after everything ends. Chunk transport failure =
   end session now; the Pi keeps whatever arrived. PSRAM chunk buffer
   ~750 KB, freed after the session.
   Energy: clips are event-gated like today's uploads — ~0.5 W extra for
   ≤20 s per event, <1 Wh/day at 700 events/day vs the ~20-30 Wh/day probe
   baseline. Video is power-cheap precisely because the gate already dials
   Wi-Fi.
   MEASURE FIRST (device, not yet done): sustained ESP32 uplink — every
   number above assumes ≥300 KB/s for ~1 s bursts. Bench = a firmware mode
   POSTing a fixed ~2 MB buffer and timing it; if the field link is slower,
   the escape hatch is sensor->set_framesize(VGA)+set_quality before the
   video loop (halves payload; Pi re-encodes at VGA anyway for storage if
   wanted).
   7b (next): review UI plays /files/video/... clips; video rows in
   /api/images additive; consider auto-discard of no-bird clips (the still
   verdict already exists per event) to cut storage.
   STATUS 2026-11-09 (2): 7a + decision gate + 7c live windows implemented
   on BOTH sides. Ingest rework: event_id schema column + migration,
   pending/decide lifecycle, live windows + arm/status/relay endpoints,
   X-Live response headers, verdict/label hooks into the clip gate.
   Verified end-to-end on a throwaway instance (25 checks: quiet-hold,
   bird label -> MP4 with exact frame count, no-bird -> discard, window
   suppresses quiet-stop then ordinary rules resume, ordered session not
   recorded, /live.mjpg relay, photo queue untouched). Firmware:
   video_stream_session(motion|ordered), X-Event-Id on photo+chunks,
   X-Live response parsing, live_status dial poll (LIVE_POLL_MS=180000);
   builds clean on bingbong. Awaiting: live-service restart + device
   flash + field test of one real motion event (expect: pending ->
   verdict -> kept or discarded clip). 7b (review UI plays clips) still
   open; the video rows now carry event_id so the UI can join a clip to
   its photo card.
   7c LIVE VIEW (decided 2026-11-09): a Watch-Live button, NOT an
   always-on stream. Power from phase 5: the 0.91 W probe floor is mostly
   the dial-before-call saving (0.2-0.3 W). The implemented answer keeps
   dial-before-call AND puts the activation decision on the gateway:
   streaming happens only when a live window is open, and windows open on
   (a) the decision model seeing a bird (model verdict -> 60 s window:
   extend the current session, or run an ordered one via the next
   response header / dial poll) or (b) a browser request (Watch-Live ->
   /api/live_arm; the device learns at its next contact or at the
   LIVE_POLL_MS dial poll — bounded latency, no always-on radio). A dial
   poll every 180 s costs ~2 s of connect each: ~0.05% duty, negligible
   at the base. The model-verdict path costs ZERO extra device wakeups
   (verdict lands while the device is still online for its own session).
   Frames stream over the existing /upload?kind=video framing and are
   relayed to the browser via /live.mjpg; ordered frames are never
   recorded. No device HTTP server, no inbound path to the ESP32.

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
