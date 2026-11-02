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
TIMEOUT = 60  # image requests run in TypeSafe's long-context mode: allow slack
RETRY_HTTP = (429, 500, 502, 503)  # transient upstream: one retry, short backoff


def _bird_p() -> float:
    """JEV_BIRD_P with a bad value downgraded to the neutral 0.5 + a warning,
    so a typo cannot take the whole service (or every label) down at import."""
    raw = os.environ.get("JEV_BIRD_P", "0.5")
    try:
        p = float(raw)
        if not 0.0 < p < 1.0:
            raise ValueError("outside 0..1")
    except ValueError:
        print(f"[jev] JEV_BIRD_P={raw!r} unusable; using 0.5")
        return 0.5
    return p


BIRD_P = _bird_p()

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


def _supports_box(model: str) -> bool:
    """Only Jev 1.14+ answers Box questions (1.13 400s the whole request).

    Unknown models are treated as box-incapable: the noul verdict survives,
    and the box is an optional enrichment.
    """
    if model.startswith("~typesafe/jev-latest"):
        return True
    if model.startswith("typesafe/jev-"):
        parts = model.removeprefix("typesafe/jev-").split(".")
        try:
            return tuple(int(x) for x in parts[:2]) >= (1, 14)
        except ValueError:
            return False
    return False


def classify(jpeg: bytes) -> dict:
    """Return {bird, species, box, confidence, note, model} for one frame.

    Shape matches what app.py's apply_verdict stores, so the review pipeline is
    unchanged; species is always None (manual step). confidence is the model's
    certainty in the emitted verdict: P(bird) when bird, else 1 - P(bird); the
    raw probability goes in note. box is [x1,y1,x2,y2] in 0..1 or None.
    """
    if not configured():
        raise RuntimeError(f"{KEY_ENV} is not set")
    data_uri = "data:image/jpeg;base64," + base64.b64encode(jpeg).decode()
    questions = {
        "bird": {
            "type": "noul",
            "instructions": "Does the photo show a bird?",
            "criteria": BIRD_CRITERIA,
        },
    }
    ask_box = _supports_box(MODEL)
    if ask_box:
        questions["bird_box"] = {
            "type": "box",
            "instructions": "the most prominent real bird in the "
                            "birdhouse camera photo",
        }
    body = json.dumps({
        "model": MODEL,
        "state": [SCENE, {"type": "image_url", "image_url": {"url": data_uri}}],
        "questions": questions,
    }).encode()
    t0 = time.monotonic()
    resp = _post(body)
    if not isinstance(resp, dict):
        raise RuntimeError(f"jev returned non-object: {str(resp)[:200]}")
    answers = resp.get("answers") or {}

    a = answers.get("bird")
    if not isinstance(a, dict) or a.get("type") != "noul" or not isinstance(a.get("noul"), (int, float)):
        raise RuntimeError(f"jev gave no noul bird answer: {json.dumps(a)[:200]}")
    p = float(a["noul"])
    bird = p >= BIRD_P
    note = f"jev p_bird={p:.3f}"

    box = None
    b = answers.get("bird_box")
    if bird and ask_box:
        if isinstance(b, dict) and b.get("type") == "box":
            box = _frac_box(b.get("box"))
            if box and isinstance(b.get("confidence"), (int, float)):
                note += f" box_conf={float(b['confidence']):.3f}"
            elif not box:
                note += " box=rejected"  # bird=True with no usable box: observable

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


def _post(body: bytes) -> dict:
    req = urllib.request.Request(
        DECISIONS_URL,
        data=body,
        headers={
            "Content-Type": "application/json",
            "Authorization": f"Bearer {os.environ[KEY_ENV]}",
        },
    )
    for attempt in (1, 2):
        try:
            with urllib.request.urlopen(req, timeout=TIMEOUT, context=_SSL_CTX) as r:
                return json.load(r)
        except urllib.error.HTTPError as e:
            raw = e.read().decode("utf-8", "replace")
            try:  # OpenRouter wraps errors as {"error":{"message","code"}}
                raw = (json.loads(raw).get("error") or {}).get("message") or raw
            except ValueError:
                pass
            if e.code in RETRY_HTTP and attempt == 1:
                time.sleep(2)
                continue
            raise RuntimeError(f"jev HTTP {e.code}: {raw[:300]}") from None


def _frac_box(b) -> list[float] | None:
    """Coerce a model box to [x1,y1,x2,y2] in 0..1, or None if unusable.

    Accepts Jev's {top,bottom,left,right} fractions and the legacy 4-list
    convention (old chat models sometimes answered 0-1000 integers or swapped
    edges). Anything inverted is swapped, anything >1 rescaled, anything
    degenerate after that is dropped — and the caller traces the drop in note.
    """
    if isinstance(b, dict):
        try:
            f = [float(b["left"]), float(b["top"]), float(b["right"]), float(b["bottom"])]
        except (KeyError, TypeError, ValueError):
            return None
    elif isinstance(b, list) and len(b) == 4:
        try:
            f = [float(x) for x in b]
        except (TypeError, ValueError):
            return None
    else:
        return None
    if max(f) > 1.0:
        f = [x / 1000.0 for x in f]
    f = [min(max(x, 0.0), 1.0) for x in f]
    x1, y1, x2, y2 = f
    x1, x2 = min(x1, x2), max(x1, x2)
    y1, y2 = min(y1, y2), max(y1, y2)
    if x2 <= x1 or y2 <= y1:
        return None
    return [round(x, 4) for x in (x1, y1, x2, y2)]
