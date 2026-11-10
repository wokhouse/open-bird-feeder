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
      STATUS 2026-11-02: flashed and running from a 5000 mAh battery (device
      xiao-1e85fc uploading since 09:43). Capture activity measured from
      labels.db: 24 photo+probe pairs in 28.9 min = ~50 photos/hour (gate
      fires often; likely moving leaves). Runtime ESTIMATE, not measured:
      5000 mAh = 18.5 Wh, ~16 Wh usable through the 5V path; probe-mode draw
      with Wi-Fi off between captures ≈ 1.0-1.4 W (pre-5c 1.3-1.7 W minus the
      0.2-0.3 W Wi-Fi saving) → ~11-16 h, i.e. half a day. Capture rate is
      noise in that number (see key fact above); 5000 mAh is structurally too
      small for multi-day operation on this architecture — PIR + deep sleep
      (step 2) is the fix. Bank low-load auto-off will NOT trip here
      (~200 mA at 5V >> the ~50 mA cutoff). MEASURE: note bank % now, again
      after N hours, draw_W = bank_Wh x pct/100 / N.
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
   POST /upload?kind=video, headers X-Device-Id, X-Video-Session <id>,
   X-Video-Seq <n>, body = concatenated JPEG frames (SOI/EOI framing; a
   truncated tail frame is dropped). Chunk = VIDEO_CHUNK_FRAMES=5 frames
   (~330 KB, ~1 s wall). Ingest keeps per-session state in memory, runs the
   detector per frame, answers per chunk. On stop or idle-sweep (session
   silent > 10 s — covers a mid-session upload failure): concatenate clean
   frames -> ffmpeg mp4 -> data/video/<date>/HHMMSS_<device>.mp4 + capture
   row kind='video' (schema needs no change; kind is free text). Fewer than
   MIN_VIDEO_FRAMES=4 clean frames: drop the session. /api/images stays
   photo-only until the review UI handles video (milestone 7b); files are
   already served by the existing /files mount.
   Firmware flow per motion capture (heartbeat stays photo-only): existing
   photo path unchanged through the still upload; then, still in JPEG XGA
   mode, stream chunks until stop/error/cap; then camera_stop + probe_start
   (existing tail). Refractory starts after the clip ends (last_capture_ms
   set after everything). Chunk transport failure = end session now; the Pi
   keeps whatever arrived. PSRAM chunk buffer ~750 KB, freed after the
   session.
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
