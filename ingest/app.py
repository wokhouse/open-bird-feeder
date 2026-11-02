"""open-birdhouse ingest service (runs on the laptop in dev, the Pi in prod).

Upload + AI classification + manual review for the bird training set.
Every uploaded photo is auto-classified in the background by the
OpenAI-compatible vision endpoint (llm.py): bird/no-bird, species, bounding
box. The review UI is for human (birder) review of those AI verdicts.

Image states: unclassified (no stored row) | no_bird | bird_identified |
species_identified. Every classification carries provenance: manual vs ai,
which model, confidence, note, and for AI verdicts a normalized bounding box
[x1,y1,x2,y2] in 0..1. Manual review always wins locally; re-running
AI re-runs replace the row either way in v0;
manual-vs-AI precedence is a hardening TODO in PLAN.md).

Endpoints:
  POST /upload?kind=photo|probe      raw JPEG body; headers: X-Device-Id,
                                     X-Motion-Hot, X-Motion-Global and
                                     X-Motion-Grid (probe: packed hot-cell
                                     map; stored as a .grid.json sidecar).
                                     kind=photo also queues AI review.
  GET  /api/images                   [{path,state,species,source,confidence,note,box}]
  GET  /api/stats                    counts per state
  POST /api/set_state {path,state,species?}   manual review ('unclassified' clears)
  POST /api/classify {path}          re-run AI on one frame, stores result
  POST /api/classify_pending {limit} AI over up to N unclassified frames (backfill)
  GET  /export.csv                   path,state,species,source,confidence,note,box
  /files/...                         static frames
  /                                  webui/dist if built, else API pointer
Run:  ./run.sh   (venv + uvicorn on 0.0.0.0:$PORT default 8078; reads ingest/.env)
"""
import csv
import datetime
import io
import json
import os
import sqlite3
from pathlib import Path

import llm
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

app = FastAPI(title="open-birdhouse ingest")


def db() -> sqlite3.Connection:
    conn = sqlite3.connect(DB_PATH, timeout=10)  # wait on locks, don't fail instantly
    conn.row_factory = sqlite3.Row  # rows() indexes columns by name
    conn.execute("PRAGMA journal_mode=WAL")
    conn.execute(
        "CREATE TABLE IF NOT EXISTS classifications("
        "path TEXT PRIMARY KEY, state TEXT NOT NULL, source TEXT NOT NULL, "
        "species TEXT, confidence REAL, note TEXT, model TEXT, box TEXT, ts TEXT NOT NULL)"
    )
    return conn


def migrate_box_column() -> None:
    """Add the AI bounding-box column to pre-existing DBs (runs at startup only)."""
    conn = db()
    cols = {r["name"] for r in conn.execute("PRAGMA table_info(classifications)")}
    if "box" not in cols:
        conn.execute("ALTER TABLE classifications ADD COLUMN box TEXT")
        conn.commit()


def migrate_v1_labels() -> None:
    """One-time migration from the v1 binary-label table (runs at startup only)."""
    conn = db()
    if not conn.execute("SELECT 1 FROM sqlite_master WHERE name='labels'").fetchone():
        return
    for path, lab, ts in conn.execute("SELECT path,label,ts FROM labels"):
        state = "bird_identified" if lab == "bird" else "no_bird"
        conn.execute(
            "INSERT OR IGNORE INTO classifications"
            "(path,state,source,species,confidence,note,model,ts) VALUES(?,?,?,?,?,?,?,?)",
            (path, state, "manual", None, None, None, "v1-labels-migrated", ts),
        )
    conn.execute("DROP TABLE labels")
    conn.commit()


migrate_box_column()
migrate_v1_labels()


def now() -> str:
    return datetime.datetime.now().isoformat(timespec="seconds")


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
    cls = {r["path"]: dict(r) for r in db().execute("SELECT * FROM classifications")}
    out = []
    for p in list_images():
        c = cls.get(p) or {}
        out.append(
            {
                "path": p,
                "state": c.get("state", "unclassified"),
                "species": c.get("species"),
                "source": c.get("source"),
                "confidence": c.get("confidence"),
                "note": c.get("note"),
                "box": json.loads(c["box"]) if c.get("box") else None,
            }
        )
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
    if not llm.configured():
        print(f"[auto] {rel}: skipped, no {llm.KEY_ENV} set")
        return
    try:
        verdict = llm.classify((DATA / rel).read_bytes())
        state = apply_verdict(rel, verdict)
        box = f" box={verdict['box']}" if verdict.get("box") else ""
        print(f"[auto] {rel}: {state}{box}")
    except Exception as e:
        print(f"[auto] {rel}: AI FAILED: {e}")


def set_row(rel: str, state: str, source: str, species=None, confidence=None,
            note=None, model=None, box=None) -> None:
    ts = now()
    box_json = json.dumps(box) if box else None
    with db() as conn:
        conn.execute(
            "INSERT INTO classifications(path,state,source,species,confidence,note,model,box,ts) "
            "VALUES(?,?,?,?,?,?,?,?,?) "
            "ON CONFLICT(path) DO UPDATE SET state=?,source=?,species=?,confidence=?, "
            "note=?,model=?,box=?,ts=?",
            (rel, state, source, species, confidence, note, model, box_json, ts,
             state, source, species, confidence, note, model, box_json, ts),
        )


def apply_verdict(rel: str, v: dict) -> str:
    if not v["bird"]:
        state = "no_bird"
    elif v["species"]:
        state = "species_identified"
    else:
        state = "bird_identified"
    set_row(rel, state, "ai", v["species"], v["confidence"], v["note"],
            v.get("model"), box=v.get("box"))
    return state


@app.post("/api/set_state")
async def set_state(request: Request):
    body = await request.json()
    rel = safe_relpath(str(body.get("path", "")))
    state = str(body.get("state", ""))
    species = str(body.get("species", "") or "").strip() or None
    if state == "unclassified":
        with db() as conn:
            conn.execute("DELETE FROM classifications WHERE path=?", (rel,))
        return {"ok": True, "path": rel, "state": "unclassified"}
    if state not in STATES:
        raise HTTPException(400, f"state must be one of {STATES + ('unclassified',)}")
    if state == "species_identified" and not species:
        raise HTTPException(400, "species_identified requires a species name")
    set_row(rel, state, "manual", species)
    return {"ok": True, "path": rel, "state": state, "species": species}


async def run_llm(rel: str) -> dict:
    if not llm.configured():
        raise HTTPException(502, f"AI not configured: set {llm.KEY_ENV} in ingest/.env")
    jpeg = (DATA / rel).read_bytes()
    try:
        # llm.classify blocks on urllib: keep it off the event loop
        verdict = await run_in_threadpool(llm.classify, jpeg)
    except Exception as e:  # upstream hiccup: surface it, never poison the store
        raise HTTPException(502, f"AI call failed: {e}")
    state = apply_verdict(rel, verdict)
    return {"path": rel, "state": state, **verdict}


@app.post("/api/classify")
async def classify(request: Request):
    body = await request.json()
    return await run_llm(safe_relpath(str(body.get("path", ""))))


@app.post("/api/classify_pending")
async def classify_pending(request: Request):
    try:
        body = await request.json()
    except Exception:
        body = {}
    limit = max(1, min(int(body.get("limit", 10)), 50))
    unclassified = [r["path"] for r in rows() if r["state"] == "unclassified"]
    results = [await run_llm(p) for p in unclassified[:limit]]
    return {"classified": len(results), "results": results}


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
            "service": "open-birdhouse ingest",
            "ui": "webui/dist not built — run the vite dev server (webui/)",
            "endpoints": ["/upload", "/api/images", "/api/stats", "/api/set_state",
                          "/api/classify", "/api/classify_pending", "/export.csv",
                          "/files/..."],
        }
