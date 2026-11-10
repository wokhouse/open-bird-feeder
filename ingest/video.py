"""Phase 7a: motion-gated video clips streamed by the device (see PLAN.md 7).

The ESP32 has no video encoder and no storage: a "video" is a burst of the
SAME XGA JPEG frames the photo path already takes, streamed as one
/upload?kind=video POST per chunk (concatenated JPEGs; headers
X-Device-Id, X-Video-Session, X-Video-Seq).

The clip's end is decided by the SAME detector the firmware runs:
firmware/main/zone_motion.c compiled for the host with cc and driven
through ctypes (same host build the firmware/test harness uses), fed the
received JPEG frames area-downscaled to the 160x120 probe geometry — so
zone_config.h thresholds transfer 1:1 and remain the single source of
truth for device and Pi.

Per chunk the device gets: HTTP 200 = keep streaming; HTTP 204 = clip
over (VIDEO_QUIET_FRAMES consecutive non-motion frames, or the frame cap
VIDEO_MAX_FRAMES). When a session ends (stop signal, or silence — the
device bailed on a failed chunk upload) the clean frames are encoded to
data/video/<date>/HHMMSS_<device>.mp4 with ffmpeg (measured: 2.3 s wall
for an 8 s XGA clip on the Pi4) and recorded as a capture row kind='video'.
Sessions with < MIN_VIDEO_FRAMES frames are dropped. app.py wires this
module at startup (init), answers chunks (handle_video), and schedules
assemble() as a background task for every finished session — including
the expired_sessions() sweep its /upload runs, so abandoned clips close
without extra device traffic.
"""
import ctypes
import datetime
import os
import re
import subprocess
import threading
import time
from pathlib import Path

PROBE_W, PROBE_H = 160, 120  # must match firmware PROBE_FRAMESIZE (QQVGA)
VIDEO_QUIET_FRAMES = int(os.environ.get("VIDEO_QUIET_FRAMES", "10"))
VIDEO_MAX_FRAMES = int(os.environ.get("VIDEO_MAX_FRAMES", "100"))  # 20 s @ 5 fps
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


# ---------- clip sessions ----------

_sessions: dict[str, dict] = {}
_lock = threading.Lock()


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
    sessions to finalize now). 200 = keep streaming, 204 = clip over.
    A finalized session is removed from the table before being handed out,
    so a late chunk starts a new session rather than racing the encoder."""
    sid = re.sub(r"[^\w.-]", "", headers.get("x-video-session", ""))[:64]
    device = headers.get("x-device-id", "unknown")
    finished: list[dict] = []
    stop = False

    with _lock:
        for sid_old in [s for s, v in _sessions.items()
                        if time.monotonic() - v["seen"] > IDLE_SESSION_S]:
            sess = _sessions.pop(sid_old)
            sess["stop_reason"] = sess["stop_reason"] or "idle"
            finished.append(sess)
        if not sid:
            return 200, finished  # header-less chunk: nothing to hold frames for
        sess = _sessions.get(sid)
        if sess is None:
            while len(_sessions) >= MAX_SESSIONS:  # oldest first
                drop = min(_sessions.values(), key=lambda s: s["seen"])
                drop["stop_reason"] = drop["stop_reason"] or "evicted"
                del _sessions[drop["sid"]]
                finished.append(drop)
            sess = {"sid": sid, "device": device,
                    "started": datetime.datetime.now(),
                    "frames": [], "quiet": 0, "seen": time.monotonic(),
                    "stop_reason": None, "det": None}
            _sessions[sid] = sess
        sess["seen"] = time.monotonic()
        new = split_frames(body)
        sess["frames"].extend(new)
        if len(sess["frames"]) >= VIDEO_MAX_FRAMES and not sess["stop_reason"]:
            sess["stop_reason"] = f"cap {len(sess['frames'])} frames"
        _score_chunk(sess, new)
        if sess["stop_reason"]:
            stop = True
            del _sessions[sid]
            finished.append(sess)

    return (204 if stop else 200), finished


def expired_sessions() -> list[dict]:
    """Sessions past their idle deadline (the device vanished mid-clip).
    Called from every /upload; the caller runs assemble() for each as a
    background task, so an abandoned clip gets encoded without needing
    more device video traffic."""
    now = time.monotonic()
    with _lock:
        due = [s for s, v in _sessions.items() if now - v["seen"] > IDLE_SESSION_S]
        sess_list = [_sessions.pop(s) for s in due]
    for sess in sess_list:
        sess["stop_reason"] = sess["stop_reason"] or "idle"
    return sess_list


def assemble(sess: dict) -> None:
    """Encode a finished session to MP4 + insert the capture row. Runs in a
    worker thread (BackgroundTasks): ffmpeg is 2.3 s wall per 8 s clip on
    the Pi4 — 4x faster than real time, and off the request path anyway."""
    frames = sess["frames"]
    if sess.get("det"):
        try:
            sess["det"][0].zone_motion_free(ctypes.byref(sess["det"][1]))
        except Exception:
            pass
    if len(frames) < MIN_VIDEO_FRAMES:
        print(f"[video] {sess['sid']}: {len(frames)} frames < {MIN_VIDEO_FRAMES}, "
              f"dropped ({sess['stop_reason']})")
        return
    ts = sess["started"]
    folder = DATA / "video" / ts.strftime("%Y-%m-%d")
    folder.mkdir(parents=True, exist_ok=True)
    stem = f"{ts.strftime('%H%M%S')}_{sess['device']}"
    out = folder / f"{stem}.mp4"
    n = 2
    while out.exists():  # same-second collision: never clobber
        out = folder / f"{stem}_{n}.mp4"
        n += 1
    raw = out.with_suffix(".mjpeg")
    raw.write_bytes(b"".join(frames))
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
        rel = str(out.relative_to(DATA))
        with _db() as conn:
            _ensure_capture(conn, rel, "video", sess["device"],
                            ts.isoformat(timespec="seconds"), out.stat().st_size)
        print(f"[video] {sess['sid']}: {len(frames)} frames ({sess['stop_reason']}) "
              f"-> {rel} {out.stat().st_size} B")
    except Exception as e:
        print(f"[video] {sess['sid']}: assemble failed: {e}")
