"""open-bird-feeder ingest service (runs on the laptop in dev, the Pi in prod).

Upload + AI classification + manual review for the bird training set.
Every uploaded photo is auto-classified in the background by the Jev
decision model (jev.py, OpenRouter Decisions API): bird/no-bird probability
+ bird bounding box. Species stays manual (Jev cannot name species): the
birder sets 'species_identified' in the review UI.

Image states: unclassified (no stored verdict or human label) | no_bird |
bird_identified | species_identified. Storage is schema v2: three tables that
separate the concerns v1 fused into one row per path:

  capture      one row per stored file: path, kind, device_id, taken_at, bytes
  verdict      append-only AI history: model, p_bird, box, note, cost, latency
  human_label  the birder's decision: state + species; only set_state writes it

Read-side state derivation, in order: human_label if present, else the newest
verdict (bird iff p_bird >= JEV_BIRD_P; pre-Jev verdicts, which have no
structured probability, keep their old state verbatim), else unclassified.
Payoffs: an AI re-run cannot touch manual work (different table); JEV_BIRD_P
can be retuned against all past labels with zero API calls (raw probabilities
are stored); new model builds can be compared on the same captures. The
/api/* wire format is identical to v1 — state/confidence/note/box are derived
at read time — so the review UI and firmware need no changes.
migrate_v2() rewrites a v1 `classifications` table at startup and keeps the
original as `classifications_v1` for audit.

Endpoints:
  POST /upload?kind=photo|probe      raw JPEG body; headers: X-Device-Id,
                                     X-Motion-Hot, X-Motion-Global and
                                     X-Motion-Grid (probe: packed hot-cell
                                     map; stored as a .grid.json sidecar).
                                     kind=photo also queues AI review.
  GET  /api/images                   [{path,state,species,source,confidence,note,box,taken_at}]
  GET  /api/stats                    counts per state
  POST /api/set_state {path,state,species?}   manual review ('unclassified' clears)
  POST /api/delete_photo {path}               delete file + all its rows for good
  POST /api/classify {path}          re-run AI on one frame, stores result
  POST /api/classify_pending {limit} AI over up to N unclassified frames (backfill)
  GET  /export.csv                   path,state,species,source,confidence,note,box
  /files/...                         static frames
  /                                  webui/dist if built, else API pointer
Run:  ./run.sh   (venv + uvicorn on 0.0.0.0:$PORT default 8078; reads ingest/.env)
"""
import asyncio
import csv
import datetime
import io
import json
import os
import re
import sqlite3
from pathlib import Path

import jev
from fastapi import BackgroundTasks, FastAPI, HTTPException, Request
from fastapi.concurrency import run_in_threadpool
from fastapi.responses import PlainTextResponse
from fastapi.staticfiles import StaticFiles

DATA = Path(os.environ.get("INGEST_DATA", Path(__file__).parent / "data")).resolve()
DATA.mkdir(parents=True, exist_ok=True)  # StaticFiles refuses a missing dir at mount time
# classifications are metadata: keep in git (INGEST_DB override for tests/dev)
DB_PATH = Path(os.environ.get("INGEST_DB", Path(__file__).parent / "labels.db"))
SAFE = lambda s: "".join(c for c in s if c.isalnum() or c in "-_.")[:48] or "unknown"
STATES = ("no_bird", "bird_identified", "species_identified")
# concurrent jev.classify calls per /api/classify_pending batch (each call is
# 2 sequential upstream HTTP requests, so 4 here = at most 8 open connections)
AI_CONCURRENCY = max(1, int(os.environ.get("JEV_CONCURRENCY", "4")))

app = FastAPI(title="open-bird-feeder ingest")


def db() -> sqlite3.Connection:
    conn = sqlite3.connect(DB_PATH, timeout=10)  # wait on locks, don't fail instantly
    conn.row_factory = sqlite3.Row  # rows() indexes columns by name
    conn.execute("PRAGMA journal_mode=WAL")
    conn.executescript(
        """
        CREATE TABLE IF NOT EXISTS capture(
          id INTEGER PRIMARY KEY, path TEXT UNIQUE NOT NULL, kind TEXT NOT NULL,
          device_id TEXT, taken_at TEXT, bytes INTEGER, ingested_at TEXT NOT NULL);
        CREATE TABLE IF NOT EXISTS verdict(
          id INTEGER PRIMARY KEY, capture_id INTEGER NOT NULL REFERENCES capture(id),
          model TEXT NOT NULL, p_bird REAL, state_hint TEXT, box TEXT, note TEXT,
          confidence REAL, cost_usd REAL, latency_ms INTEGER, ts TEXT NOT NULL);
        CREATE TABLE IF NOT EXISTS human_label(
          capture_id INTEGER PRIMARY KEY REFERENCES capture(id),
          state TEXT NOT NULL, species TEXT, note TEXT, ts TEXT NOT NULL);
        CREATE INDEX IF NOT EXISTS verdict_latest
          ON verdict(capture_id, id DESC);
        """
    )
    return conn


def now() -> str:
    return datetime.datetime.now().isoformat(timespec="seconds")


# photo/2026-11-01/194416_xiao-1e85fc(_2).jpg -> date, HHMMSS, device
CAP_PARSE = re.compile(
    r"(\w+)/(\d{4}-\d{2}-\d{2})/(\d{6})_([\w.-]+?)(?:_\d+)?\.jpg$")


def ensure_capture(conn, rel: str, kind: str = "photo", device: str | None = None,
                   taken_at: str | None = None, size: int | None = None) -> int:
    """One capture row per stored file. Device and capture time are parsed from
    the storage path when not supplied at upload — the filename has been
    carrying these facts all along; now they are columns."""
    row = conn.execute("SELECT id FROM capture WHERE path=?", (rel,)).fetchone()
    if row:
        return row["id"]
    m = CAP_PARSE.match(rel)
    if m:
        taken_at = taken_at or f"{m.group(2)}T{m.group(3)[:2]}:{m.group(3)[2:4]}:{m.group(3)[4:6]}"
        device = device or m.group(4)
    if size is None:
        p = DATA / rel
        size = p.stat().st_size if p.is_file() else None
    conn.execute(
        "INSERT OR IGNORE INTO capture(path,kind,device_id,taken_at,bytes,ingested_at) "
        "VALUES(?,?,?,?,?,?)", (rel, kind, device, taken_at, size, now()))
    return conn.execute("SELECT id FROM capture WHERE path=?", (rel,)).fetchone()["id"]


def migrate_v2() -> None:
    """Rewrite a v1 `classifications` table (one fused row per path) into
    capture/verdict/human_label. Runs at startup while the old table exists;
    leaves it as `classifications_v1` for audit.

    Manual rows become human_label. AI rows become verdicts, with an honest
    split: Jev notes carry a structured p_bird we can parse back; pre-Jev
    chat-LLM rows get NO invented probability (their "confidence" was not
    1-P(bird) — two live rows disprove that), so they keep their old state
    verbatim in state_hint and their old confidence for display parity.
    """
    conn = db()
    if not conn.execute("SELECT 1 FROM sqlite_master WHERE name='classifications'").fetchone():
        return
    n_human = n_verdict = 0
    for r in conn.execute("SELECT * FROM classifications"):
        cid = ensure_capture(conn, r["path"])
        if r["source"] == "manual":
            conn.execute(
                "INSERT OR REPLACE INTO human_label(capture_id,state,species,note,ts) "
                "VALUES(?,?,?,?,?)", (cid, r["state"], r["species"], r["note"], r["ts"]))
            n_human += 1
        else:
            m = re.search(r"p_bird=([0-9.]+)", r["note"] or "")
            conn.execute(
                "INSERT INTO verdict(capture_id,model,p_bird,state_hint,box,note,"
                "confidence,cost_usd,latency_ms,ts) VALUES(?,?,?,?,?,?,?,?,?,?)",
                (cid, r["model"] or "legacy", float(m.group(1)) if m else None,
                 r["state"], r["box"], r["note"], r["confidence"], None, None, r["ts"]))
            n_verdict += 1
    conn.execute("ALTER TABLE classifications RENAME TO classifications_v1")
    conn.commit()
    print(f"[migrate_v2] rewrote {n_human} human labels, {n_verdict} verdicts; "
          f"old table kept as classifications_v1")


migrate_v2()


def list_images() -> list[str]:
    """Review/training frames only. data/probe/ holds detector diagnostics
    (the grayscale frame that triggered a capture), not bird photos."""
    return sorted(str(p.relative_to(DATA)) for p in (DATA / "photo").rglob("*.jpg"))


def safe_relpath(rel: str) -> str:
    """Resolve a stored-image relative path, refusing escapes out of DATA."""
    p = (DATA / rel).resolve()
    if not p.is_relative_to(DATA) or not p.is_file():
        raise HTTPException(404, f"no such image: {rel}")
    return str(p.relative_to(DATA))


def rows() -> list[dict]:
    """Review-queue entries; the wire format is byte-for-byte what v1 stored.
    State derives at read time: human_label wins, else the newest verdict
    (bird iff p_bird >= JEV_BIRD_P; legacy state_hint verbatim), else
    unclassified. Because raw probabilities are stored, retuning JEV_BIRD_P
    re-labels all history with zero API calls."""
    conn = db()
    caps = {r["path"]: r for r in conn.execute(
        "SELECT id, path, taken_at FROM capture")}

    def ts_of(cap, rel):  # capture row first; the filename is the fallback
        if cap and cap["taken_at"]:
            return cap["taken_at"]
        m = CAP_PARSE.match(rel)
        if m:
            return f"{m.group(2)}T{m.group(3)[:2]}:{m.group(3)[2:4]}:{m.group(3)[4:6]}"
        return None

    humans = {r["capture_id"]: dict(r) for r in conn.execute("SELECT * FROM human_label")}
    latest = {r["capture_id"]: dict(r) for r in conn.execute(
        "SELECT * FROM verdict WHERE id IN "
        "(SELECT MAX(id) FROM verdict GROUP BY capture_id)")}
    out = []
    for p in list_images():
        cap = caps.get(p)
        cid = cap["id"] if cap else None
        taken = ts_of(cap, p)
        h = humans.get(cid) if cid is not None else None
        v = latest.get(cid) if cid is not None else None
        if h:
            out.append({"path": p, "state": h["state"], "species": h["species"],
                        "source": "manual", "confidence": None,
                        "note": h["note"], "box": None, "taken_at": taken})
        elif v:
            if v["p_bird"] is not None:
                bird = v["p_bird"] >= jev.BIRD_P
                conf = round(v["p_bird"] if bird else 1.0 - v["p_bird"], 4)
            else:  # pre-Jev verdict: state and confidence kept verbatim
                bird = v["state_hint"] == "bird_identified"
                conf = v["confidence"]
            out.append({"path": p, "state": "bird_identified" if bird else "no_bird",
                        "species": None, "source": "ai", "confidence": conf,
                        "note": v["note"],
                        "box": json.loads(v["box"]) if v["box"] else None,
                        "taken_at": taken})
        else:
            out.append({"path": p, "state": "unclassified", "species": None,
                        "source": None, "confidence": None, "note": None, "box": None,
                        "taken_at": taken})
    return out


def save_motion_grid(path: Path, headers) -> None:
    """Decode the X-Motion-Grid header (packed hot-cell bitmap + zone mapping)
    into a .grid.json sidecar beside the probe frame. Later non-LLM recognizers
    can derive bounding boxes without re-running the detector. A missing or
    malformed header (older firmware) is skipped silently."""
    try:
        g = json.loads(headers.get("x-motion-grid") or "")
        gw, gh, cell = int(g["gw"]), int(g["gh"]), int(g["cell"])
        zone, frame = [int(v) for v in g["zone"]], [int(v) for v in g["frame"]]
        bits = bytes.fromhex(g["bits"])
        hot = int(headers.get("x-motion-hot", "-1"))
        glob = int(headers.get("x-motion-global", "-1"))
    except (ValueError, KeyError, TypeError):
        return
    if cell < 1 or gw < 1 or gh < 1 or len(bits) < (gw * gh + 7) // 8:
        return
    cells = [[i % gw, i // gw] for i in range(gw * gh) if bits[i >> 3] >> (i & 7) & 1]
    sidecar = path.with_name(path.stem + ".grid.json")
    sidecar.write_text(json.dumps({
        "frame_wh": frame, "zone": zone, "cell": cell, "grid_wh": [gw, gh],
        "hot": hot, "global": glob, "cells": cells,
    }))


@app.post("/upload")
async def upload(request: Request, background_tasks: BackgroundTasks, kind: str = "photo"):
    if kind not in ("photo", "probe"):
        raise HTTPException(400, "kind must be photo|probe")
    body = await request.body()
    if not body or len(body) > 2_000_000:
        raise HTTPException(400, f"body must be 1B..2MB, got {len(body)}")
    device = SAFE(request.headers.get("x-device-id", "unknown"))
    ts = datetime.datetime.now()
    folder = DATA / kind / ts.strftime("%Y-%m-%d")
    folder.mkdir(parents=True, exist_ok=True)
    path = folder / f"{ts.strftime('%H%M%S')}_{device}.jpg"
    n = 2
    while path.exists():  # same-second collision: never clobber a classified frame
        path = folder / f"{ts.strftime('%H%M%S')}_{device}_{n}.jpg"
        n += 1
    path.write_bytes(body)
    rel = str(path.relative_to(DATA))
    with db() as conn:  # the device/time/size facts the filename was faking
        ensure_capture(conn, rel, kind, device, ts.isoformat(timespec="seconds"), len(body))
    if kind == "probe":
        save_motion_grid(path, request.headers)
    else:
        # photos get AI review as they land; probes stay manual (they're for the
        # future on-device model comparison, not the birder queue)
        background_tasks.add_task(auto_classify, rel)
    return {"ok": True, "path": rel, "bytes": len(body)}


def auto_classify(rel: str) -> None:
    """Background task kicked off by /upload. Sync on purpose: Starlette
    runs it in a worker thread, so a slow AI call never stalls the device POST.
    A failed verdict leaves the frame unclassified — 'AI bulk process' backfills."""
    if not jev.configured():
        print(f"[auto] {rel}: skipped, no {jev.KEY_ENV} set")
        return
    try:
        verdict = jev.classify((DATA / rel).read_bytes())
        state = apply_verdict(rel, verdict)
        extra = ""
        if verdict.get("box"):
            extra += f" box={verdict['box']}"
        if verdict.get("cost_usd") is not None:
            extra += f" cost={verdict['cost_usd']:.6f} latency={verdict.get('latency_ms')}ms"
        print(f"[auto] {rel}: {state}{extra}")
    except Exception as e:
        print(f"[auto] {rel}: AI FAILED: {e}")


def apply_verdict(rel: str, v: dict) -> str:
    """Append an AI verdict. Verdicts are append-only history and human labels
    live in their own table, so a re-run can never destroy manual work and
    every model's probability stays on file for calibration and comparison."""
    with db() as conn:
        cid = ensure_capture(conn, rel)
        conn.execute(
            "INSERT INTO verdict(capture_id,model,p_bird,box,note,cost_usd,latency_ms,ts) "
            "VALUES(?,?,?,?,?,?,?,?)",
            (cid, v.get("model") or jev.MODEL, v.get("p_bird"),
             json.dumps(v["box"]) if v.get("box") else None, v.get("note"),
             v.get("cost_usd"), v.get("latency_ms"), now()))
        h = conn.execute("SELECT state FROM human_label WHERE capture_id=?", (cid,)).fetchone()
    return h["state"] if h else ("bird_identified" if v["bird"] else "no_bird")


@app.post("/api/delete_photo")
async def delete_photo(request: Request):
    """Delete a stored image for good: the file, its .grid.json sidecar, and
    every metadata row (human_label, all verdicts, the capture). Distinct from
    set_state unclassified, which keeps the photo and clears only its labels."""
    body = await request.json()
    rel = safe_relpath(str(body.get("path", "")))
    path = DATA / rel
    sidecar = path.with_name(path.stem + ".grid.json")
    if sidecar.is_file():
        sidecar.unlink()
    path.unlink()
    with db() as conn:
        cid = conn.execute("SELECT id FROM capture WHERE path=?", (rel,)).fetchone()
        if cid:
            conn.execute("DELETE FROM human_label WHERE capture_id=?", (cid["id"],))
            conn.execute("DELETE FROM verdict WHERE capture_id=?", (cid["id"],))
            conn.execute("DELETE FROM capture WHERE id=?", (cid["id"],))
    return {"ok": True, "path": rel}


@app.post("/api/set_state")
async def set_state(request: Request):
    body = await request.json()
    rel = safe_relpath(str(body.get("path", "")))
    state = str(body.get("state", ""))
    species = str(body.get("species", "") or "").strip() or None
    if state == "unclassified":  # full reset: human label AND AI history go
        with db() as conn:
            cid = conn.execute("SELECT id FROM capture WHERE path=?", (rel,)).fetchone()
            if cid:
                conn.execute("DELETE FROM human_label WHERE capture_id=?", (cid["id"],))
                conn.execute("DELETE FROM verdict WHERE capture_id=?", (cid["id"],))
        return {"ok": True, "path": rel, "state": "unclassified"}
    if state not in STATES:
        raise HTTPException(400, f"state must be one of {STATES + ('unclassified',)}")
    if state == "species_identified" and not species:
        raise HTTPException(400, "species_identified requires a species name")
    with db() as conn:
        cid = ensure_capture(conn, rel)
        conn.execute(
            "INSERT INTO human_label(capture_id,state,species,ts) VALUES(?,?,?,?) "
            "ON CONFLICT(capture_id) DO UPDATE SET state=?,species=?,ts=?",
            (cid, state, species, now(), state, species, now()))
    return {"ok": True, "path": rel, "state": state, "species": species}


async def run_ai(rel: str) -> dict:
    if not jev.configured():
        raise HTTPException(502, f"AI not configured: set {jev.KEY_ENV} in ingest/.env")
    jpeg = (DATA / rel).read_bytes()
    try:
        # jev.classify blocks on urllib: keep it off the event loop
        verdict = await run_in_threadpool(jev.classify, jpeg)
    except Exception as e:  # upstream hiccup: surface it, never poison the store
        raise HTTPException(502, f"AI call failed: {e}")
    state = apply_verdict(rel, verdict)
    return {"path": rel, "state": state, **verdict}


@app.post("/api/classify")
async def classify(request: Request):
    body = await request.json()
    return await run_ai(safe_relpath(str(body.get("path", ""))))


@app.post("/api/classify_pending")
async def classify_pending(request: Request):
    """AI over up to 100 unclassified frames. Runs JEV_CONCURRENCY (default 4)
    calls at a time: one jev.classify is two sequential HTTP round-trips of up
    to 60 s each, so a serial batch looks dead from the browser. A failed
    image is counted and the rest of the batch proceeds - one upstream 502
    must not throw away the verdicts already stored."""
    try:
        body = await request.json()
    except Exception:
        body = {}
    limit = max(1, min(int(body.get("limit", 10)), 100))
    unclassified = [r["path"] for r in rows() if r["state"] == "unclassified"]
    batch = unclassified[:limit]
    sem = asyncio.Semaphore(AI_CONCURRENCY)

    async def one(p: str):
        async with sem:
            return await run_ai(p)

    done = await asyncio.gather(*(one(p) for p in batch), return_exceptions=True)
    results = [d for d in done if not isinstance(d, BaseException)]
    errors = [str(d) for d in done if isinstance(d, BaseException)]
    if not results and errors:
        raise HTTPException(502, errors[0])
    return {"classified": len(results), "failed": len(errors),
            "error": errors[0] if errors else None, "results": results}


@app.get("/api/images")
def api_images():
    return rows()


@app.get("/api/stats")
def api_stats():
    counts = {"unclassified": 0, **{s: 0 for s in STATES}}
    for r in rows():
        counts[r["state"]] += 1
    return {"total": sum(counts.values()), **counts}


@app.get("/export.csv", response_class=PlainTextResponse)
def export_csv():
    buf = io.StringIO()
    w = csv.writer(buf)
    fields = ["path", "state", "species", "source", "confidence", "note", "box"]
    w.writerow(fields)
    for r in rows():  # rows() skips labels whose file is gone (e.g. trashed frames)
        row = [r[c] for c in fields]
        row[-1] = json.dumps(r["box"]) if r["box"] else ""  # one cell, parseable later
        w.writerow(row)
    return buf.getvalue()


app.mount("/files", StaticFiles(directory=DATA), name="files")

_DIST = Path(__file__).parent.parent / "webui" / "dist"
if _DIST.is_dir():  # built SPA takes the root route in dev and prod
    app.mount("/", StaticFiles(directory=_DIST, html=True), name="web")
else:
    @app.get("/")
    def api_index():
        return {
            "service": "open-bird-feeder ingest",
            "ui": "webui/dist not built — run the vite dev server (webui/)",
            "endpoints": ["/upload", "/api/images", "/api/stats", "/api/set_state",
                          "/api/classify", "/api/classify_pending", "/export.csv",
                          "/files/..."],
        }
