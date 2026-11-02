#!/usr/bin/env bash
# Create venv if needed and run the ingest service (reads ingest/.env first:
# OPENROUTER_API_KEY, JEV_MODEL, JEV_BIRD_P, INGEST_DATA, PORT...).
set -euo pipefail
cd "$(dirname "$0")"
[ -d .venv ] || { python3 -m venv .venv && .venv/bin/pip install -q -r requirements.txt; }
if [ -f .env ]; then set -a; . ./.env; set +a; fi
exec .venv/bin/uvicorn app:app --reload --host 0.0.0.0 --port "${PORT:-8078}"
