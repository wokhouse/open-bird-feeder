"""Kimi (Moonshot) vision client — bird/no-bird + species calls for stored frames.

API reference: https://platform.kimi.ai/docs/guide/use-kimi-vision-model
- base_url https://api.moonshot.ai/v1, key in MOONSHOT_API_KEY
- images must be base64 data URLs (URL-form images are NOT supported)

Verdicts are parsed defensively: we ask for strict JSON, then extract the first
JSON object from the reply whatever way the model wraps it.
"""
import base64
import json
import os
import re
import urllib.request

BASE_URL = os.environ.get("MOONSHOT_BASE_URL", "https://api.moonshot.ai/v1")
MODEL = os.environ.get("KIMI_MODEL", "kimi-k2.6")
KEY_ENV = "MOONSHOT_API_KEY"

PROMPT = """You are classifying a still frame from a fixed camera mounted at a birdhouse.
Respond with ONLY this JSON object, no other text:
{"bird": true or false, "species": "common bird name or null", "confidence": 0.0-1.0, "note": "short visual evidence"}
Rules:
- bird=true only if an actual bird body is visible (feathers/silhouette/legs), not stray feathers, droppings, shadows, or leaves.
- species: fill only when bird=true and visible field marks (size, color, bill) support a plausible common name; otherwise null.
- confidence: your certainty about the bird boolean.
"""


class KimiNotConfigured(RuntimeError):
    pass


def configured() -> bool:
    return bool(os.environ.get(KEY_ENV))


def classify(jpeg: bytes) -> dict:
    """Return {bird, species, confidence, note, model} for one JPEG frame."""
    if not configured():
        raise KimiNotConfigured(f"{KEY_ENV} not set — put it in ingest/.env")
    data_url = "data:image/jpeg;base64," + base64.b64encode(jpeg).decode()
    body = {
        "model": MODEL,
        "messages": [
            {
                "role": "user",
                "content": [
                    {"type": "image_url", "image_url": {"url": data_url}},
                    {"type": "text", "text": PROMPT},
                ],
            }
        ],
    }
    req = urllib.request.Request(
        BASE_URL.rstrip("/") + "/chat/completions",
        data=json.dumps(body).encode(),
        headers={
            "Content-Type": "application/json",
            "Authorization": "Bearer " + os.environ[KEY_ENV],
        },
    )
    with urllib.request.urlopen(req, timeout=90) as resp:
        reply = json.load(resp)
    content = reply["choices"][0]["message"]["content"]
    verdict = parse_verdict(content)
    verdict["model"] = MODEL
    return verdict


def parse_verdict(content: str) -> dict:
    m = re.search(r"\{.*\}", content, re.S)
    if not m:
        raise ValueError(f"no JSON object in model reply: {content[:200]!r}")
    v = json.loads(m.group())
    return {
        "bird": bool(v.get("bird")),
        "species": (str(v["species"]).strip() or None) if v.get("species") else None,
        "confidence": max(0.0, min(1.0, float(v.get("confidence", 0.0)))),
        "note": str(v.get("note", ""))[:500],
    }
