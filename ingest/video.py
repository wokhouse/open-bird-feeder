"""Phase 7a/7c: video sessions streamed by the device (see PLAN.md 7).

The ESP32 has no video encoder and no storage: a "video" is a burst of the
SAME XGA JPEG frames the photo path already takes, streamed as one
/upload?kind=video POST per chunk (concatenated JPEGs; headers
X-Device-Id, X-Event-Id, X-Video-Seq, X-Session-Kind: motion|ordered).

The clip's end is decided by the SAME detector the firmware runs:
firmware/main/zone_motion.c compiled for the host with cc and driven
through ctypes, fed the received JPEG frames area-downscaled to the
160x120 probe geometry — so zone_config.h thresholds transfer 1:1 and
remain the single source of truth for device and Pi.

The decision model is in the loop (X-Event-Id names the capture event and
is sent on the trigger photo AND its video chunks — the join key):
 - a clip is only encoded when the event's decision says bird (Jev verdict
   or human label, same precedence as the review UI). Undecided: raw frames
   wait in data/pending/; on_verdict() (called by app.py after any label
   or verdict) decides them late. No-bird or stale: discarded. Without the
   model in the loop this would record every branch that moves.
 - streaming itself is gated by live windows (arm(): a model bird verdict
   opens VIDEO_MODEL_WINDOW_S of extra footage of the bird that is there
   right now; a browser Watch-Live arm opens a viewing window). A session
   with an open window streams past its quiet point; an ordered session
   exists only for its window. Browser windows cost the device nothing
   unless the device is already streaming.
 - every streamed frame is teed to an in-memory ring so /live.mjpg serves
   a browser within a frame or two. The browser never talks to the device.

Per chunk the device gets: HTTP 200 = keep streaming; HTTP 204 = session
over (quiet frames, the cap, the window closing, or silence — the device
bailed on a failed chunk upload). Encoded clips go to
data/video/<date>/HHMMSS_<device>.mp4 with ffmpeg (measured: 2.3 s wall
for an 8 s XGA clip on the Pi4) and a capture row kind='video' carrying
the event_id. app.py wires this module at startup (init), answers chunks
(handle_video), finalizes sessions (background task), and sweeps pending
raws from its /upload path so abandoned clips close without extra device
traffic.

Env: VIDEO_QUIET_FRAMES (stop after N static frames, default 10),
VIDEO_MAX_FRAMES (safety cap, 5 fps, default 100 = 20 s),
VIDEO_EXTENDED_MAX_FRAMES (cap while a live window is open, default 300),
VIDEO_MODEL_WINDOW_S (stream-on window after a model bird verdict,
default 60), VIDEO_LIVE_MAX_S (browser arm cap, default 300),
VIDEO_PENDING_MAX_S (hold raw frames awaiting a verdict, default 172800),
VIDEO_IDLE_S (silence that closes a session, default 10).
"""
import ctypes
import datetime
import json
import os
import re
import subprocess
import threading
import time
from collections import deque
from pathlib import Path

PROBE_W, PROBE_H = 160, 120  # must match firmware PROBE_FRAMESIZE (QQVGA)
VIDEO_QUIET_FRAMES = int(os.environ.get("VIDEO_QUIET_FRAMES", "10"))
VIDEO_MAX_FRAMES = int(os.environ.get("VIDEO_MAX_FRAMES", "100"))  # 20 s @ 5 fps
VIDEO_EXTENDED_MAX = int(os.environ.get("VIDEO_EXTENDED_MAX_FRAMES", "300"))
VIDEO_MODEL_WINDOW_S = float(os.environ.get("VIDEO_MODEL_WINDOW_S", "60"))
VIDEO_LIVE_MAX_S = float(os.environ.get("VIDEO_LIVE_MAX_S", "300"))
VIDEO_PENDING_MAX_S = float(os.environ.get("VIDEO_PENDING_MAX_S", "172800"))
MIN_VIDEO_FRAMES = 4
IDLE_SESSION_S = float(os.environ.get("VIDEO_IDLE_S", "10"))
MAX_SESSIONS = 8

DATA = REPO = None          # set by init()
_db = _ensure_capture = None


def init(repo_root: Path, data_dir: Path, db_fn, ensure_capture_fn) -> None:
    global REPO, DATA, _db, _ensure_capture
    REPO, DATA, _db, _ensure_capture = repo_root, data_dir, db_fn, ensure_capture_fn


# ---------- host build of the firmware's detector (zone_motion.c) ----------

class _Cfg(ctypes.Structure):
    _fields_ = [(n, ctypes.c_int) for n in
                ("x0", "y0", "x1", "y1", "cell", "cell_thresh",
                 "min_hot_cells", "trigger_frames", "light_reject")]


class _ZoneMotionT(ctypes.Structure):
    _fields_ = [("cfg", _Cfg),
                ("w", ctypes.c_int), ("h", ctypes.c_int),
                ("cols", ctypes.c_int), ("rows", ctypes.c_int),
                ("zw", ctypes.c_int), ("zh", ctypes.c_int),
                ("prev", ctypes.c_void_p), ("cell_sum", ctypes.c_void_p),
                ("hot_bits", ctypes.c_void_p), ("hot_bytes", ctypes.c_int),
                ("primed", ctypes.c_int), ("consec", ctypes.c_int),
                ("frames", ctypes.c_uint32), ("triggers", ctypes.c_uint32)]


class _Result(ctypes.Structure):
    _fields_ = [("hot_cells", ctypes.c_int), ("global_mean", ctypes.c_int),
                ("motion", ctypes.c_int), ("consec", ctypes.c_int),
                ("trigger", ctypes.c_int), ("hot_bits", ctypes.c_void_p)]


_det_lock = threading.Lock()
_detector = None  # None = not tried yet; False = unavailable; (lib, cfg) = ready


def _detector_build():
    """cc-build zone_motion.c (cached .so in DATA/.det/) + read the tuned
    zone knobs from firmware/main/zone_config.h (first value under each
    #ifndef = the effective one; the same file the device compiles)."""
    cfg_h = REPO / "firmware" / "main" / "zone_config.h"
    src_c = REPO / "firmware" / "main" / "zone_motion.c"
    so = DATA / ".det" / "zone_motion.so"
    so.parent.mkdir(parents=True, exist_ok=True)
    if not so.exists() or so.stat().st_mtime < src_c.stat().st_mtime:
        tmp = so.with_name("zone_motion.building.so")
        subprocess.run(["cc", "-std=c11", "-O2", "-shared", "-fPIC",
                        "-o", str(tmp), str(src_c)], check=True)
        tmp.rename(so)
    lib = ctypes.CDLL(str(so))
    lib.zone_motion_init.restype = ctypes.c_int
    lib.zone_motion_init.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_void_p]
    lib.zone_motion_update.restype = _Result
    lib.zone_motion_update.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int]
    lib.zone_motion_free.argtypes = [ctypes.c_void_p]

    defs = {}
    for k, v in re.findall(r"#define\s+(ZONE_\w+)\s+(\d+)", cfg_h.read_text()):
        defs.setdefault(k, int(v))
    cfg = _Cfg(
        x0=defs["ZONE_X0_PCT"] * PROBE_W // 100, x1=defs["ZONE_X1_PCT"] * PROBE_W // 100,
        y0=defs["ZONE_Y0_PCT"] * PROBE_H // 100, y1=defs["ZONE_Y1_PCT"] * PROBE_H // 100,
        cell=defs["ZONE_CELL"], cell_thresh=defs["ZONE_CELL_THRESH"],
        min_hot_cells=defs["ZONE_MIN_HOT_CELLS"],
        trigger_frames=defs["ZONE_TRIGGER_FRAMES"],
        light_reject=defs["ZONE_LIGHT_REJECT"])
    return lib, cfg


def get_detector():
    global _detector
    with _det_lock:
        if _detector is None:
            try:
                _detector = _detector_build()
                print("[video] Pi-side zone detector ready (zone_motion.c host build)")
            except Exception as e:
                _detector = False
                print(f"[video] detector build FAILED ({e}); clips close at frame cap")
        return _detector or None


# ---------- live windows: who may stream, who may watch ----------

_sessions: dict[str, dict] = {}
_windows: dict[str, dict] = {}  # device -> {"until": monotonic, "source": model|browser}
_live: dict[str, deque] = {}    # device -> [(frame, monotonic)] ring, ~10 s of stream

# RLock: handle_video holds it while reading window_state/arm bookkeeping.
_lock = threading.RLock()


def arm(device: str, seconds: float, source: str) -> float:
    """Open/extend a live window. model = a bird verdict just landed: stream
    the bird while it is presumably still there (this is also what extends a
    motion session past its quiet point). browser = Watch-Live request.
    A browser arm never shortens an open model window. Returns the length."""
    cap = VIDEO_MODEL_WINDOW_S if source == "model" else VIDEO_LIVE_MAX_S
    seconds = max(0.0, min(seconds, cap))
    now = time.monotonic()
    with _lock:
        cur = _windows.get(device)
        if cur and cur["until"] > now and cur["source"] == "model" and source != "model":
            seconds = cur["until"] - now
        _windows[device] = {"until": now + seconds, "source": source}
    return seconds


def window_state(device: str) -> tuple[int, str | None]:
    """(seconds remaining, source). Expired windows are dropped here."""
    with _lock:
        w = _windows.get(device)
        if not w:
            return 0, None
        rem = int(w["until"] - time.monotonic())
        if rem <= 0:
            del _windows[device]
            return 0, None
        return rem, w["source"]


def live_snapshot(device: str) -> list[tuple[bytes, float]]:
    with _lock:
        return list(_live.get(device, ()))


# ---------- clip sessions ----------
# (_sessions/_lock live with the window bookkeeping above: same critical section)


def split_frames(body: bytes) -> list[bytes]:
    """Split a concatenated-JPEG chunk into frames; drop a truncated tail
    (a chunk cut mid-frame by the firmware's byte budget)."""
    starts = [m.start() for m in re.finditer(b"\xff\xd8\xff", body)]
    frames = [body[a:b] for a, b in zip(starts, starts[1:] + [len(body)])]
    return [f for f in frames if f.endswith(b"\xff\xd9")]


def gray_planes(frames: list[bytes]) -> bytes:
    """Decode frames to PROBE_W*PROBE_H luma planes (area scaling: same
    block-averaging the sensor's own QQVGA path does, so cell means — the
    detector's whole metric — stay comparable)."""
    p = subprocess.run(
        ["ffmpeg", "-hide_banner", "-loglevel", "error",
         "-f", "image2pipe", "-vcodec", "mjpeg", "-i", "pipe:0",
         "-vf", f"scale={PROBE_W}:{PROBE_H}:flags=area",
         "-f", "rawvideo", "-pix_fmt", "gray", "pipe:1"],
        input=b"".join(frames), capture_output=True, timeout=30)
    if p.returncode != 0:
        raise RuntimeError(p.stderr.decode(errors="replace")[:200])
    plane = PROBE_W * PROBE_H
    return p.stdout[: (len(p.stdout) // plane) * plane]


def _score_chunk(sess: dict, frames: list[bytes]) -> None:
    """Update the session's quiet counter from the detector. A session whose
    detector build failed just runs on the frame cap."""
    if not frames:
        return
    det = get_detector()
    if det is None:
        return
    lib, cfg = det
    if sess.get("det") is None:
        ctx = _ZoneMotionT()
        if lib.zone_motion_init(ctypes.byref(ctx), PROBE_W, PROBE_H, ctypes.byref(cfg)) != 0:
            return
        sess["det"] = (lib, ctx)
    lib, ctx = sess["det"]
    try:
        planes = gray_planes(frames)
    except Exception as e:
        print(f"[video] {sess['sid']}: decode failed: {e}")
        return
    plane = PROBE_W * PROBE_H
    for i in range(len(planes) // plane):
        r = lib.zone_motion_update(ctypes.byref(ctx), planes[i * plane:(i + 1) * plane], PROBE_W)
        if r.motion:
            sess["quiet"] = 0
        else:
            sess["quiet"] += 1
            if sess["quiet"] >= VIDEO_QUIET_FRAMES and not sess["stop_reason"]:
                sess["stop_reason"] = f"quiet {sess['quiet']} frames"


def handle_video(body: bytes, headers) -> tuple[int, list[dict]]:
    """One chunk of one session. Returns (http_status for the device,
    sessions to finalize now). 200 = keep streaming, 204 = session over.
    A finalized session is removed from the table before being handed out,
    so a late chunk starts a new session rather than racing the encoder.

    The live window is the real-time authority: while it is open, quiet and
    cap stops are overridden (the model or the birder asked for live
    video); when it closes, the ordinary rules apply again — and an
    ordered session ends immediately, since its whole reason existed only
    for the window."""
    sid = re.sub(r"[^\w.-]", "", headers.get("x-event-id", ""))[:64]
    device = headers.get("x-device-id", "unknown")
    kind = headers.get("x-session-kind", "motion")
    finished: list[dict] = []

    with _lock:
        for sid_old in [s for s, v in _sessions.items()
                        if time.monotonic() - v["seen"] > IDLE_SESSION_S]:
            sess = _sessions.pop(sid_old)
            sess["stop_reason"] = sess["stop_reason"] or "idle"
            finished.append(sess)
        if not sid:
            return 200, finished  # header-less chunk: nothing to hold frames for
        rem, source = window_state(device)
        sess = _sessions.get(sid)
        if sess is None:
            while len(_sessions) >= MAX_SESSIONS:  # oldest first
                drop = min(_sessions.values(), key=lambda s: s["seen"])
                drop["stop_reason"] = drop["stop_reason"] or "evicted"
                del _sessions[drop["sid"]]
                finished.append(drop)
            sess = {"sid": sid, "device": device, "kind": kind,
                    "model_window": rem > 0 and source == "model",
                    "started": datetime.datetime.now(),
                    "frames": [], "quiet": 0, "seen": time.monotonic(),
                    "stop_reason": None, "det": None}
            _sessions[sid] = sess
        sess["seen"] = time.monotonic()
        new = split_frames(body)
        sess["frames"].extend(new)
        if new:  # the tee: whoever watches /live.mjpg sees these now
            _live.setdefault(device, deque(maxlen=50)).extend(
                (f, time.monotonic()) for f in new)
        cap = VIDEO_EXTENDED_MAX if rem else VIDEO_MAX_FRAMES
        if len(sess["frames"]) >= cap and not sess["stop_reason"]:
            sess["stop_reason"] = f"cap {len(sess['frames'])} frames"
        _score_chunk(sess, new)
        if sess["stop_reason"] and rem:
            sess["stop_reason"] = None  # an open live window outranks quiet/cap
        if not sess["stop_reason"] and sess["kind"] == "ordered" and not rem:
            sess["stop_reason"] = "window expired"
        if sess["stop_reason"]:
            finished.append(sess)
            del _sessions[sid]

    stop = any(s["sid"] == sid for s in finished)
    return (204 if stop else 200), finished


def expired_sessions() -> list[dict]:
    """Sessions past their idle deadline (the device vanished mid-clip).
    Called from every /upload; the caller finalizes each as a background
    task, so an abandoned clip closes without needing more device video
    traffic."""
    now = time.monotonic()
    with _lock:
        due = [s for s, v in _sessions.items() if now - v["seen"] > IDLE_SESSION_S]
        sess_list = [_sessions.pop(s) for s in due]
    for sess in sess_list:
        sess["stop_reason"] = sess["stop_reason"] or "idle"
    return sess_list


def sweep_pending() -> list[dict]:
    """Raw frames whose verdict window expired: decide them on whatever is
    known (still unclassified -> keep the clip; older than the hard cap ->
    drop). Called from every /upload; cheap — one directory scan of a
    handful of files."""
    out = []
    pending = DATA / "pending"
    if not pending.is_dir():
        return out
    now = time.time()
    with _lock:
        for meta_path in pending.glob("*.json"):
            try:
                if now - meta_path.stat().st_mtime > VIDEO_PENDING_MAX_S:
                    sess = _load_meta(meta_path)
                    if sess:
                        sess["stop_reason"] = "pending window expired"
                        out.append(sess)
            except OSError:
                continue
    return out


def _load_meta(meta_path: Path) -> dict | None:
    raw = meta_path.with_suffix(".mjpeg")
    if not raw.is_file():
        meta_path.unlink(missing_ok=True)
        return None
    try:
        meta = json.loads(meta_path.read_text())
        return {"sid": meta["sid"], "device": meta["device"],
                "kind": meta.get("kind", "motion"),
                "model_window": meta.get("model_window", False),
                "started": datetime.datetime.fromisoformat(meta["started"]),
                "frames": None, "raw_on_disk": True,
                "quiet": 0, "seen": 0,
                "stop_reason": meta.get("stop_reason"), "det": None}
    except Exception:
        raw.unlink(missing_ok=True)
        meta_path.unlink(missing_ok=True)
        return None


def on_verdict(event_id: str) -> list[dict]:
    """Hook from app.py after any verdict or human label. Returns sessions
    the caller must finalize as background tasks: a held clip for this
    event is decided now (bird -> encode, no-bird -> discard)."""
    sid = re.sub(r"[^\w.-]", "", event_id)[:64]
    with _lock:
        if sid in _sessions:  # still streaming; it will be decided at its end
            return []
        meta_path = DATA / "pending" / f"{sid}.json"
        sess = _load_meta(meta_path) if meta_path.is_file() else None
        if sess:
            return [sess]
    return []


# ---------- the decision gate and the encoder ----------

def _photo_state(sess: dict) -> str | None:
    """The decision that gates this clip: the event's trigger photo, found
    by event id (or, for old firmware without it, the device's photo within
    5 s of the clip start). Precedence mirrors the review UI: human label
    wins, else the newest verdict against JEV_BIRD_P. None = undecided."""
    import app  # late import: app owns the DB layer and jev
    with _db() as conn:
        r = conn.execute(
            "SELECT id FROM capture WHERE event_id=? AND kind='photo' "
            "ORDER BY id LIMIT 1", (sess["sid"],)).fetchone()
        if r is None and sess["kind"] != "ordered":
            started = sess["started"].isoformat(timespec="seconds")
            until = (sess["started"] + datetime.timedelta(seconds=5)).isoformat(
                timespec="seconds")
            r = conn.execute(
                "SELECT id FROM capture WHERE kind='photo' AND device_id=? "
                "AND taken_at>=? AND taken_at<=? ORDER BY id LIMIT 1",
                (sess["device"], started, until)).fetchone()
        if r is None:
            return None
        # effective_is_bird answers False for "no decision yet"; the gate
        # must tell undecided apart from no-bird (undecided = hold).
        cid = r["id"]
        decided = conn.execute(
            "SELECT 1 FROM human_label WHERE capture_id=? "
            "UNION ALL SELECT 1 FROM verdict WHERE capture_id=? LIMIT 1",
            (cid, cid)).fetchone()
        if decided is None:
            return None
        return "bird" if app.effective_is_bird(conn, cid) else "no_bird"


def finalize(sess: dict) -> None:
    """Decide a finished session: keep it or throw it away.

    - ordered browser-only stream: live viewing, never recorded — that is
      what makes a Watch-Live arm free for the archive.
    - everything else: the event's decision gates the file. Bird ->
      encode. No-bird -> discard the raw frames. Undecided -> park raw
      frames in data/pending/ until on_verdict() or the sweep decides.
    """
    if sess.get("det"):
        try:
            sess["det"][0].zone_motion_free(ctypes.byref(sess["det"][1]))
        except Exception:
            pass
    raw = DATA / "pending" / f"{sess['sid']}.mjpeg"
    frames = sess.get("frames")
    if frames:  # first write: put the frames where any later decision can read them
        raw.parent.mkdir(parents=True, exist_ok=True)
        raw.write_bytes(b"".join(frames))
        sess["frames"] = None
        _write_meta(sess, raw)
    if not raw.is_file():
        return
    n_frames = raw.stat().st_size // 3000  # XGA q20 runs 25-50 KB; under-reads, never over
    if raw.stat().st_size < MIN_VIDEO_FRAMES * 2000:
        print(f"[video] {sess['sid']}: too few frames, dropped "
              f"({sess.get('stop_reason')})")
        _drop_raw(sess["sid"])
        return
    if sess["kind"] == "ordered" and not sess.get("model_window"):
        print(f"[video] {sess['sid']}: browser-viewed stream not recorded")
        _drop_raw(sess["sid"])
        return
    state = _photo_state(sess)
    if state is None:
        print(f"[video] {sess['sid']}: held pending (no decision yet)")
        return
    if state != "bird":
        print(f"[video] {sess['sid']}: discarded ({sess.get('stop_reason')}, {state})")
        _drop_raw(sess["sid"])
        return
    _encode(sess, raw)


def _write_meta(sess: dict, raw: Path) -> None:
    raw.with_suffix(".json").write_text(json.dumps({
        "sid": sess["sid"], "device": sess["device"], "kind": sess["kind"],
        "model_window": sess.get("model_window", False),
        "started": sess["started"].isoformat(timespec="seconds"),
        "stop_reason": sess.get("stop_reason")}))


def _drop_raw(sid: str) -> None:
    pending = DATA / "pending"
    (pending / f"{sid}.mjpeg").unlink(missing_ok=True)
    (pending / f"{sid}.json").unlink(missing_ok=True)


def _encode(sess: dict, raw: Path) -> None:
    """ffmpeg image2pipe -> H.264. Runs in a worker thread: ~4x faster than
    real time on the Pi4, and off the request path anyway. A clip late on
    its decision reads its raw frames from data/pending/ — encoding always
    goes through the file, so restarts and late decisions are the same
    code path."""
    ts = sess["started"]
    folder = DATA / "video" / ts.strftime("%Y-%m-%d")
    folder.mkdir(parents=True, exist_ok=True)
    stem = f"{ts.strftime('%H%M%S')}_{sess['device']}"
    out = folder / f"{stem}.mp4"
    n = 2
    while out.exists():  # same-second collision: never clobber
        out = folder / f"{stem}_{n}.mp4"
        n += 1
    try:
        p = subprocess.run(
            ["ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
             "-f", "image2pipe", "-vcodec", "mjpeg", "-framerate", "5",
             "-i", str(raw),
             "-c:v", "libx264", "-preset", "veryfast", "-crf", "28",
             "-pix_fmt", "yuv420p", "-movflags", "+faststart", str(out)],
            capture_output=True, timeout=120)
        if p.returncode != 0:
            print(f"[video] {sess['sid']}: encode failed: "
                  f"{p.stderr.decode(errors='replace')[:300]} (raw kept at {raw})")
            return
        raw.unlink()
        raw.with_suffix(".json").unlink(missing_ok=True)
        rel = str(out.relative_to(DATA))
        with _db() as conn:
            _ensure_capture(conn, rel, "video", sess["device"],
                            ts.isoformat(timespec="seconds"), out.stat().st_size,
                            sess["sid"])
        print(f"[video] {sess['sid']}: {sess['kind']} "
              f"({sess.get('stop_reason')}) -> {rel} {out.stat().st_size} B")
    except Exception as e:
        print(f"[video] {sess['sid']}: encode failed: {e}")
