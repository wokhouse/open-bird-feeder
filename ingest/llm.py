"""Generic OpenAI-compatible vision client for bird/no-bird + species calls.

Provider-agnostic: any endpoint implementing POST {BASE}/chat/completions with
Bearer auth and OpenAI message format works. Defaults point at synthetic.new
(https://api.synthetic.new/openai/v1); its syn:*:vision aliases auto-route to
the current recommended vision model. Override everything via ingest/.env.
"""
import base64
import json
import os
import urllib.request

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
    '  "confidence": number 0.0-1.0 — your confidence in the bird verdict\n'
    '  "note": string — at most 12 words describing what you saw\n'
    "No markdown, no code fences, no commentary."
)


def configured() -> bool:
    return bool(os.environ.get(KEY_ENV))


def classify(jpeg: bytes) -> dict:
    """Return {bird, species, confidence, note} for one frame."""
    data_uri = "data:image/jpeg;base64," + base64.b64encode(jpeg).decode()
    body = json.dumps({
        "model": MODEL,
        "max_tokens": 120,
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
    with urllib.request.urlopen(req, timeout=TIMEOUT) as r:
        content = json.load(r)["choices"][0]["message"]["content"]
    return _parse(content)


def _parse(content: str) -> dict:
    """Defensive: models sometimes wrap JSON in fences or prose."""
    start, end = content.find("{"), content.rfind("}")
    if start == -1 or end == -1:
        raise ValueError(f"no JSON object in model reply: {content[:120]!r}")
    v = json.loads(content[start : end + 1])
    return {
        "bird": bool(v.get("bird")),
        "species": (v.get("species") or None),
        "confidence": float(v.get("confidence", 0)),
        "note": str(v.get("note", ""))[:200],
    }
