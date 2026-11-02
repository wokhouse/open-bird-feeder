"""Generic OpenAI-compatible vision client for bird/no-bird + species calls.

Provider-agnostic: any endpoint implementing POST {BASE}/chat/completions with
Bearer auth and OpenAI message format works. Defaults point at synthetic.new
(https://api.synthetic.new/openai/v1); its syn:*:vision aliases auto-route to
the current recommended vision model. Override everything via ingest/.env.
"""
import base64
import json
import os
import ssl
import urllib.request

try:  # fresh venvs (esp. on macOS) lack a system CA bundle; certifi provides one
    import certifi
    _SSL_CTX = ssl.create_default_context(cafile=certifi.where())
except ImportError:
    _SSL_CTX = ssl.create_default_context()

BASE_URL = os.environ.get("LLM_BASE_URL", "https://api.synthetic.new/openai/v1")
MODEL = os.environ.get("LLM_MODEL", "syn:large:vision")
KEY_ENV = "LLM_API_KEY"
TIMEOUT = 90

PROMPT = (
    "This is a frame from a camera aimed at a birdhouse in a backyard. "
    "Answer as a single JSON object and nothing else, with exactly these keys:\n"
    '  "bird": true or false  — is any bird visible in the frame (including perched '
    "on the house, at the entrance hole, blurred, or partially occluded)?\n"
    '  "species": string or null — best species identification in lowercase if bird '
    "is true and you have a confident read, else null\n"
    '  "box": [x1, y1, x2, y2] or null — bounding box around the most prominent '
    "bird when bird is true, else null. Coordinates are relative to image "
    "width/height scaled to 0-1000, origin at top-left, x1<x2 and y1<y2\n"
    '  "confidence": number 0.0-1.0 — your confidence in the bird verdict\n'
    '  "note": string — at most 12 words describing what you saw\n'
    "No markdown, no code fences, no commentary."
)


def configured() -> bool:
    return bool(os.environ.get(KEY_ENV))


def classify(jpeg: bytes) -> dict:
    """Return {bird, species, box, confidence, note} for one frame.

    box is [x1, y1, x2, y2] normalized to 0..1 around the most prominent bird,
    or None (no bird, or the model returned an unusable box).
    """
    data_uri = "data:image/jpeg;base64," + base64.b64encode(jpeg).decode()
    body = json.dumps({
        "model": MODEL,
        "max_tokens": 500,
        "temperature": 0,
        "messages": [{
            "role": "user",
            "content": [
                {"type": "text", "text": PROMPT},
                {"type": "image_url", "image_url": {"url": data_uri}},
            ],
        }],
    }).encode()
    req = urllib.request.Request(
        f"{BASE_URL}/chat/completions",
        data=body,
        headers={
            "Content-Type": "application/json",
            "Authorization": f"Bearer {os.environ[KEY_ENV]}",
        },
    )
    with urllib.request.urlopen(req, timeout=TIMEOUT, context=_SSL_CTX) as r:
        content = json.load(r)["choices"][0]["message"]["content"]
    verdict = _parse(content)
    verdict["model"] = MODEL  # provenance: which endpoint answered this frame
    return verdict


def _parse(content: str) -> dict:
    """Defensive: models sometimes wrap JSON in fences, prose, or run out of
    tokens mid-reply — a truncated object still carries the verdict fields."""
    start, end = content.find("{"), content.rfind("}")
    if start == -1:
        raise ValueError(f"no JSON object in model reply: {content[:120]!r}")
    if end == -1:  # truncated: close any open string, then the object
        frag = content[start:]
        if frag.count('"') % 2:
            frag += '"'
        frag += "}"
    else:
        frag = content[start : end + 1]
    v = json.loads(frag)
    return {
        "bird": bool(v.get("bird")),
        "species": (v.get("species") or None),
        "box": _norm_box(v.get("box")),
        "confidence": float(v.get("confidence", 0)),
        "note": str(v.get("note", ""))[:200],
    }


def _norm_box(b) -> list[float] | None:
    """Coerce a model box to [x1,y1,x2,y2] in 0..1, or None if unusable.

    The prompt asks for 0-1000 relative coordinates (the common vision-model
    convention) but models drift; anything with a component > 1 is treated as
    that scale, everything else as 0..1.
    """
    if not isinstance(b, list) or len(b) != 4:
        return None
    try:
        f = [float(x) for x in b]
    except (TypeError, ValueError):
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
