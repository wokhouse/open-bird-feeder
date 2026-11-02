"""ntfy push when a frame newly reads as bird_identified.

Enabled by NTFY_TOPIC in ingest/.env (the topic is the secret here: anyone who
knows it can publish, only subscribers with the topic string ever see it).
Optional overrides: NTFY_SERVER (default https://ntfy.sh), INGEST_PUBLIC_URL
(default this box on the tailnet; used as the message's click target).

Plain urllib JSON publish, same style as jev.py: no new dependencies. The
publish API is POST {server}/ with {"topic": ...} in the body — a JSON body
sidesteps HTTP header encoding entirely, so the emoji title is safe.

No inline image attach: ntfy.sh's base64 ?image= travels in the query string
and its nginx 414s URIs above ~5 KB (measured 2026-11-02: a 4985-byte URI
returned 200, a 9699-byte one 414 — a 160px preview of a real frame is
already at that edge). The tap instead opens the full frame served on the
tailnet, so the viewing device needs to be on the tailnet.

A failed push is printed and dropped: classification never depends on the
phone, and app.py only calls this on a real state transition to bird.
"""
import datetime
import json
import os
import urllib.parse
import urllib.request

TOPIC = os.environ.get("NTFY_TOPIC", "").strip()
SERVER = os.environ.get("NTFY_SERVER", "https://ntfy.sh").rstrip("/")
PUBLIC = os.environ.get("INGEST_PUBLIC_URL", "http://sleepy.tail643d2.ts.net:8077").rstrip("/")
TIMEOUT = 5  # a push is a side effect; give up fast rather than stall a request


def enabled() -> bool:
    return bool(TOPIC)


def photo_url(rel: str) -> str:
    """Absolute URL of a stored frame, as the ingest service itself serves it."""
    return f"{PUBLIC}/files/{urllib.parse.quote(rel)}"


def send(title: str, message: str, click: str | None = None,
        tags: tuple[str, ...] = ()) -> dict:
    """Publish one ntfy message via the JSON API. Returns the stored-message JSON."""
    body = {"topic": TOPIC, "title": title, "message": message, "priority": 3}
    if click:
        body["click"] = click
    if tags:
        body["tags"] = list(tags)
    req = urllib.request.Request(
        SERVER + "/", data=json.dumps(body).encode(),
        headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=TIMEOUT) as r:
        return json.load(r)


def bird_detected(rel: str, verdict: dict) -> None:
    """Compose and push the bird alert. Never raises: a dead ntfy (or missing
    topic config on a dev box) must not disturb the verdict that was just
    stored. app.py has already deduped — it only calls this on a flip to
    bird_identified — so one frame pushes at most once."""
    if not enabled():
        print(f"[notify] {rel}: skipped, no NTFY_TOPIC set")
        return
    p = verdict.get("p_bird")
    when = datetime.datetime.now().strftime("%H:%M")
    detail = f"p_bird={p:.2f}" if isinstance(p, (int, float)) else "bird"
    try:
        msg = send("🐦 Bird at the feeder",
                   f"{when} · {detail} · {verdict.get('model') or ''}".strip(),
                   click=photo_url(rel), tags=("bird",))
        print(f"[notify] {rel}: pushed id={msg.get('id')}")
    except Exception as e:
        print(f"[notify] {rel}: PUSH FAILED: {e}")
