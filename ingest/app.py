"""open-birdhouse ingest service (runs on the laptop in dev, the Pi in prod).

Upload + label + browse for the bird/no-bird training set.

Endpoints:
  POST /upload?kind=photo|probe   raw JPEG body; headers: X-Device-Id
  GET  /                          gallery (label state shown)
  GET  /label                     labeling UI (keys: b=no-bird n=bird arrows browse)
  GET  /api/images                [{path,label}]
  POST /label  {path,label}       label in {bird,nobird} (other = clear)
  GET  /export.csv                path,label
Run:  ./run.sh   (venv + uvicorn on 0.0.0.0:8077)
"""
import csv
import datetime
import io
import os
import sqlite3
from pathlib import Path

from fastapi import FastAPI, HTTPException, Request
from fastapi.responses import HTMLResponse, PlainTextResponse
from fastapi.staticfiles import StaticFiles

DATA = Path(os.environ.get("INGEST_DATA", Path(__file__).parent / "data")).resolve()
DATA.mkdir(parents=True, exist_ok=True)  # StaticFiles refuses a missing dir at mount time
DB_PATH = Path(__file__).parent / "labels.db"  # labels are metadata: keep in git
SAFE = lambda s: "".join(c for c in s if c.isalnum() or c in "-_.")[:48] or "unknown"
LABELS = ("bird", "nobird")

app = FastAPI(title="open-birdhouse ingest")


def db() -> sqlite3.Connection:
    conn = sqlite3.connect(DB_PATH)
    conn.execute(
        "CREATE TABLE IF NOT EXISTS labels("
        "path TEXT PRIMARY KEY, label TEXT NOT NULL, ts TEXT NOT NULL)"
    )
    return conn


def list_images() -> list[str]:
    return sorted(str(p.relative_to(DATA)) for p in DATA.rglob("*.jpg"))


def safe_relpath(rel: str) -> str:
    """Resolve a stored-image relative path, refusing escapes out of DATA."""
    p = (DATA / rel).resolve()
    if not p.is_relative_to(DATA) or not p.is_file():
        raise HTTPException(404, f"no such image: {rel}")
    return str(p.relative_to(DATA))


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
    while path.exists():  # same-second collision: never clobber a labeled frame
        path = folder / f"{ts.strftime('%H%M%S')}_{device}_{n}.jpg"
        n += 1
    path.write_bytes(body)
    return {"ok": True, "path": str(path.relative_to(DATA)), "bytes": len(body)}


@app.post("/label")
async def label(request: Request):
    body = await request.json()
    rel = safe_relpath(str(body.get("path", "")))
    lab = str(body.get("label", ""))
    with db() as conn:
        if lab in LABELS:
            conn.execute(
                "INSERT INTO labels(path,label,ts) VALUES(?,?,?) "
                "ON CONFLICT(path) DO UPDATE SET label=?, ts=?",
                (rel, lab, datetime.datetime.now().isoformat(timespec="seconds"), lab,
                 datetime.datetime.now().isoformat(timespec="seconds")),
            )
        elif lab == "":
            conn.execute("DELETE FROM labels WHERE path=?", (rel,))
        else:
            raise HTTPException(400, "label must be bird|nobird|''")
    return {"ok": True, "path": rel, "label": lab}


@app.get("/api/images")
def api_images():
    with db() as conn:
        labeled = dict(conn.execute("SELECT path,label FROM labels"))
    return [{"path": p, "label": labeled.get(p, "")} for p in list_images()]


@app.get("/export.csv", response_class=PlainTextResponse)
def export_csv():
    buf = io.StringIO()
    w = csv.writer(buf)
    w.writerow(["path", "label"])
    with db() as conn:
        for path, lab in conn.execute("SELECT path,label FROM labels ORDER BY path"):
            w.writerow([path, lab])
    return buf.getvalue()


@app.get("/", response_class=HTMLResponse)
def gallery():
    with db() as conn:
        labeled = dict(conn.execute("SELECT path,label FROM labels"))
    imgs = list(reversed(list_images()))[:200]
    items = "".join(
        f'<figure class="{labeled.get(p, "un")}"><img src="/files/{p}" width="320">'
        f"<figcaption>{p} [{labeled.get(p, 'unlabeled')}]</figcaption></figure>"
        for p in imgs
    )
    n_lab = sum(1 for p in imgs if p in labeled)
    return f"""<!doctype html><meta charset="utf-8"><title>open-birdhouse</title>
<style>
body{{font-family:system-ui;margin:2em;background:#111;color:#ccc}}
h3 a{{color:#ddd}} figure{{display:inline-block;margin:.5em}}
figure.un img{{outline:2px solid #444}} figure.bird img{{outline:3px solid #2a2}}
figure.nobird img{{outline:3px solid #a22}} img{{display:block}}
figcaption{{font-size:11px}}
</style>
<h3><a href="/">open-birdhouse</a> — {len(imgs)} shown, {n_lab} labeled ·
<a href="/label">labeling view →</a></h3><div>{items}</div>"""


@app.get("/label", response_class=HTMLResponse)
def label_ui():
    return """<!doctype html><meta charset="utf-8"><title>label — open-birdhouse</title>
<style>
body{font-family:system-ui;background:#111;color:#ccc;margin:2em;text-align:center}
img{max-width:92vw;max-height:70vh;outline:3px solid #444}
img.bird{outline:3px solid #2a2} img.nobird{outline:3px solid #a22}
.row{margin:1em} button{font-size:1.2em;padding:.5em 1.5em;margin:0 .3em;cursor:pointer}
#status{margin:.5em;font-size:14px} #path{font-size:12px;color:#777}
</style>
<h3><a href="/" style="color:#ddd">open-birdhouse</a> labeling</h3>
<div id="status"></div><img id="img"><div id="path"></div>
<div class="row">
  <button onclick="mark('nobird')">✗ no bird (n)</button>
  <button onclick="mark('bird')">🐦 bird (b)</button>
  <button onclick="go(-1)">← prev</button>
  <button onclick="go(1)">next →</button>
  <button onclick="mark('')">clear</button>
</div>
<script>
const statusEl=document.getElementById('status'), pathEl=document.getElementById('path'),
      imgEl=document.getElementById('img');
let imgs=[], i=0;
async function refresh(){
  imgs = await (await fetch('/api/images')).json();
  const unl = imgs.findIndex(m=>!m.label);
  if (unl>=0) i=unl; show();
}
function show(){
  const m=imgs[i]; if(!m){statusEl.textContent='no images';return;}
  imgEl.src='/files/'+m.path; imgEl.className=m.label;
  pathEl.textContent=m.path;
  const done=imgs.filter(m=>m.label).length;
  statusEl.textContent=`${i+1}/${imgs.length} · labeled ${done}/${imgs.length}`
    +(m.label?` · current: ${m.label}`:'');
}
function go(d){ i=Math.max(0,Math.min(imgs.length-1,i+d)); show(); }
async function mark(lab){
  await fetch('/label',{method:'POST',headers:{'content-type':'application/json'},
    body:JSON.stringify({path:imgs[i].path,label:lab})});
  imgs[i].label=lab;
  for(let k=1;k<=imgs.length;k++){           // advance to next unlabeled
    const j=(i+k)%imgs.length;
    if(!imgs[j].label){ i=j; break; }
  }
  show();
}
document.addEventListener('keydown',e=>{
  if(e.key==='b')mark('bird'); else if(e.key==='n')mark('nobird');
  else if(e.key==='ArrowLeft')go(-1); else if(e.key==='ArrowRight')go(1);
});
refresh();
</script>"""


app.mount("/files", StaticFiles(directory=DATA), name="files")
