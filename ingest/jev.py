"""Jev (TypeSafe) decision-model client: bird/no-bird verdict + bird bounding box.

Jev is a decision model, not a chat model — there is no prompt to phrase and
no text to parse. POST https://openrouter.ai/api/alpha/decisions (OpenRouter)
with a `state` (scene text + the frame as an image_url part) and typed
questions; it answers with probabilities:
  noul — P(yes) for one yes/no question  -> the bird verdict
  box  — top/bottom/left/right as image fractions for a described target
         (jev-1.14 only; Box has no "not found" answer, so the noul verdict
         gates whether the box is used)
Both questions ride in one request; they are answered in parallel.
Species is intentionally absent: Jev cannot name species, so species stays a
manual step in the review UI (set_state 'species_identified').

Auth/model/threshold via ingest/.env: OPENROUTER_API_KEY (required),
JEV_MODEL (default typesafe/jev-1.14), JEV_BIRD_P (P(bird) >= threshold →
bird; pick from labeled frames, 0.5 is the neutral default).
Docs: openrouter.ai/docs/guides/community/jev +
openrouter.ai/docs/api/api-reference/alphadecisions/submit-a-decisions-request
"""
import base64
import json
import os
import ssl
import time
import urllib.error
import urllib.request

try:  # fresh venvs (esp. on macOS) lack a system CA bundle; certifi provides one
    import certifi
    _SSL_CTX = ssl.create_default_context(cafile=certifi.where())
except ImportError:
    _SSL_CTX = ssl.create_default_context()

DECISIONS_URL = "https://openrouter.ai/api/alpha/decisions"
MODEL = os.environ.get("JEV_MODEL", "typesafe/jev-1.14")
KEY_ENV = "OPENROUTER_API_KEY"
BIRD_P = float(os.environ.get("JEV_BIRD_P", "0.5"))
TIMEOUT = 60  # image requests run in TypeSafe's long-context mode: allow slack

# Noul criteria say what counts as yes/no; Jev reads them as the decision rule.
BIRD_CRITERIA = {
    "true": ("One or more real birds are present anywhere in a photo — perched "
             "on the house, at the entrance hole, in flight, blurred, or "
             "partially occluded. Feathers, beak, or wings count."),
    "false": ("No bird in any photo — empty birdhouse, feeder, seeds, leaves, "
              "branches, sky, insects, or other non-bird animals (squirrels)."),
}

SCENE = ("Frame from a fixed camera aimed at a birdhouse in a backyard; "
         "the photo follows. ")


def configured() -> bool:
    return bool(os.environ.get(KEY_ENV))


def classify(jpeg: bytes) -> dict:
    """Return {bird, species, box, confidence, note, model} for one frame.

    Shape matches what app.py's apply_verdict stores, so the review pipeline is
    unchanged; species is always None (manual step). confidence is the model's
    certainty in the emitted verdict: P(bird) when bird, else 1 - P(bird); the
    raw probability goes in note. box is [x1,y1,x2,y2] in 0..1 or None.
    """
    data_uri = "data:image/jpeg;base64," + base64.b64encode(jpeg).decode()
    body = json.dumps({
        "model": MODEL,
        "state": [SCENE, {"type": "image_url", "image_url": {"url": data_uri}}],
        "questions": {
            "bird": {
                "type": "noul",
                "instructions": "Does the photo show a bird?",
                "criteria": BIRD_CRITERIA,
            },
            "bird_box": {
                "type": "box",
                "instructions": "the most prominent real bird in the "
                                "birdhouse camera photo",
            },
        },
    }).encode()
    req = urllib.request.Request(
        DECISIONS_URL,
        data=body,
        headers={
            "Content-Type": "application/json",
            "Authorization": f"Bearer {os.environ[KEY_ENV]}",
        },
    )
    t0 = time.monotonic()
    try:
        with urllib.request.urlopen(req, timeout=TIMEOUT, context=_SSL_CTX) as r:
            resp = json.load(r)
    except urllib.error.HTTPError as e:
        raise RuntimeError(f"jev HTTP {e.code}: {e.read().decode()[:200]}") from None
    answers = resp.get("answers") or {}

    a = answers.get("bird")
    if not isinstance(a, dict) or a.get("type") != "noul" or not isinstance(a.get("noul"), (int, float)):
        raise RuntimeError(f"jev gave no noul bird answer: {json.dumps(a)[:200]}")
    p = float(a["noul"])
    bird = p >= BIRD_P
    note = f"jev p_bird={p:.3f}"

    box = None
    b = answers.get("bird_box")
    if bird and isinstance(b, dict) and b.get("type") == "box":
        box = _frac_box(b.get("box"))
        if box and isinstance(b.get("confidence"), (int, float)):
            note += f" box_conf={float(b['confidence']):.3f}"

    return {
        "bird": bird,
        "species": None,  # Jev cannot name species: manual step in the UI
        "box": box,
        "confidence": round(p if bird else 1.0 - p, 4),
        "note": note,
        "model": resp.get("model") or MODEL,  # provenance: exact build that answered
        "latency_ms": int((time.monotonic() - t0) * 1000),
        "cost_usd": (resp.get("usage") or {}).get("cost"),
    }


def _frac_box(b) -> list[float] | None:
    """Coerce Jev's {top,bottom,left,right} fractions to [x1,y1,x2,y2] 0..1.

    Jev always returns four edges even when the target is absent (docs), so
    anything degenerate or out of range is dropped, not rescued.
    """
    if not isinstance(b, dict):
        return None
    try:
        top, bottom = float(b["top"]), float(b["bottom"])
        left, right = float(b["left"]), float(b["right"])
    except (KeyError, TypeError, ValueError):
        return None
    f = [min(max(x, 0.0), 1.0) for x in (left, top, right, bottom)]
    x1, y1, x2, y2 = f
    if x2 <= x1 or y2 <= y1:
        return None
    return [round(x, 4) for x in (x1, y1, x2, y2)]
