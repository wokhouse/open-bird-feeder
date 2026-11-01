"""open-birdhouse ingest service (runs on the laptop in dev, the Pi in prod).

Upload + Kimi classification + manual review for the bird training set.

Image states: unclassified (no stored row) | no_bird | bird_identified |
species_identified. Every classification carries provenance: manual vs kimi,
which model, confidence, note. Manual review always wins locally; re-running
Kimi replaces only kimi-sourced rows... (it replaces the row either way in v0;
manual-vs-kimi precedence is a hardening TODO in PLAN.md).

Endpoints:
  POST /upload?kind=photo|probe      raw JPEG body; headers: X-Device-Id
  GET  /api/images                   [{path,state,species,source,confidence,note}]
  GET  /api/stats                    counts per state
  POST /api/set_state {path,state,species?}   manual review ('unclassified' clears)
  POST /api/classify {path}          Kimi bird/no-bird + species call, stores result
  POST /api/classify_pending {limit} Kimi over up to N unclassified frames
  GET  /export.csv                   path,state,species,source,confidence,note
  /files/...                         static frames
  /                                  webui/dist if built, else API pointer
Run:  ./run.sh   (venv + uvicorn on 0.0.0.0:$PORT default 8078; reads ingest/.env)
"""
import csv
import datetime
import io
import os
import sqlite3
from pathlib import Path

import kimi
from fastapi import FastAPI, HTTPException, Request
from fastapi.responses import PlainTextResponse
from fastapi.staticfiles import StaticFiles

DATA = Path(os.environ.get("INGEST_DATA", Path(__file__).parent / "data")).resolve()
DATA.mkdir(parents=True, exist_ok=True)  # StaticFiles refuses a missing dir at mount time
DB_PATH = Path(__file__).parent / "labels.db"  # classifications are metadata: keep in git
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
        "species TEXT, confidence REAL, note TEXT, model TEXT, ts TEXT NOT NULL)"
    )
    return conn


def migrate_v1_labels() -> None:
    """One-time migration from the v1 binary-label table (runs at startup only)."""
    conn = db()
    if not conn.execute("SELECT 1 FROM sqlite_master WHERE name='labels'").fetchone():
        return
    for path, lab, ts in conn.execute("SELECT path,label,ts FROM labels"):
        state = "bird_identified" if lab == "bird" else "no_bird"
        conn.execute(
            "INSERT OR IGNORE INTO classifications VALUES(?,?,?,?,?,?,?,?)",
            (path, state, "manual", None, None, None, "v1-labels-migrated", ts),
        )
    conn.execute("DROP TABLE labels")
    conn.commit()


migrate_v1_labels()


def now() -> str:
    return datetime.datetime.now().isoformat(timespec="seconds")


def list_images() -> list[str]:
    return sorted(str(p.relative_to(DATA)) for p in DATA.rglob("*.jpg"))


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
            }
        )
    return out


@app.post("/upload")
async def upload(request: Request, kind: str = "photo"):
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
    return {"ok": True, "path": str(path.relative_to(DATA)), "bytes": len(body)}


def set_row(rel: str, state: str, source: str, species=None, confidence=None,
            note=None, model=None) -> None:
    ts = now()
    with db() as conn:
        conn.execute(
            "INSERT INTO classifications(path,state,source,species,confidence,note,model,ts) "
            "VALUES(?,?,?,?,?,?,?,?) "
            "ON CONFLICT(path) DO UPDATE SET state=?,source=?,species=?,confidence=?,"
            "note=?,model=?,ts=?",
            (rel, state, source, species, confidence, note, model, ts,
             state, source, species, confidence, note, model, ts),
        )


def apply_verdict(rel: str, v: dict) -> str:
    if not v["bird"]:
        state = "no_bird"
    elif v["species"]:
        state = "species_identified"
    else:
        state = "bird_identified"
    set_row(rel, state, "kimi", v["species"], v["confidence"], v["note"], v["model"])
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


async def run_kimi(rel: str) -> dict:
    if not kimi.configured():
        raise HTTPException(502, f"kimi not configured: set {kimi.KEY_ENV} in ingest/.env")
    jpeg = (DATA / rel).read_bytes()
    try:
        verdict = kimi.classify(jpeg)
    except Exception as e:  # upstream hiccup: surface it, never poison the store
        raise HTTPException(502, f"kimi call failed: {e}")
    state = apply_verdict(rel, verdict)
    return {"path": rel, "state": state, **verdict}


@app.post("/api/classify")
async def classify(request: Request):
    body = await request.json()
    return await run_kimi(safe_relpath(str(body.get("path", ""))))


@app.post("/api/classify_pending")
async def classify_pending(request: Request):
    try:
        body = await request.json()
    except Exception:
        body = {}
    limit = max(1, min(int(body.get("limit", 10)), 50))
    unclassified = [r["path"] for r in rows() if r["state"] == "unclassified"]
    results = [await run_kimi(p) for p in unclassified[:limit]]
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
    w.writerow(["path", "state", "species", "source", "confidence", "note"])
    with db() as conn:
        for r in conn.execute(
            "SELECT path,state,species,source,confidence,note FROM classifications "
            "ORDER BY path"
        ):
            w.writerow(r)
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
