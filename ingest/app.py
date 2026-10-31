"""open-birdhouse ingest service (runs on the laptop in dev, the Pi in prod).

Endpoints:
  POST /upload?kind=photo|probe   raw JPEG body; headers: X-Device-Id, X-Score (optional)
  GET  /                          minimal gallery of stored images

Run:  ./run.sh   (venv + uvicorn on 0.0.0.0:8077)
"""
import datetime
import os
from pathlib import Path

from fastapi import FastAPI, HTTPException, Request
from fastapi.responses import HTMLResponse

DATA = Path(os.environ.get("INGEST_DATA", Path(__file__).parent / "data")).resolve()
DATA.mkdir(parents=True, exist_ok=True)  # StaticFiles refuses a missing dir at mount time
SAFE = lambda s: "".join(c for c in s if c.isalnum() or c in "-_.")[:48] or "unknown"

app = FastAPI(title="open-birdhouse ingest")


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
    path.write_bytes(body)
    return {"ok": True, "path": str(path.relative_to(DATA)), "bytes": len(body)}


@app.get("/", response_class=HTMLResponse)
def gallery():
    imgs = sorted(DATA.rglob("*.jpg"), reverse=True)[:200]
    items = "".join(
        f'<figure><img src="/files/{p.relative_to(DATA)}" width="320">'
        f"<figcaption>{p.relative_to(DATA)}</figcaption></figure>"
        for p in imgs
    )
    return f"<h3>open-birdhouse ({len(imgs)} shown)</h3><div>{items}</div>"


from fastapi.staticfiles import StaticFiles  # noqa: E402
app.mount("/files", StaticFiles(directory=DATA), name="files")
