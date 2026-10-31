#!/usr/bin/env bash
# Create venv if needed and run the ingest service on 0.0.0.0:8077.
set -euo pipefail
cd "$(dirname "$0")"
[ -d .venv ] || { python3 -m venv .venv && .venv/bin/pip install -q -r requirements.txt; }
exec .venv/bin/uvicorn app:app --host 0.0.0.0 --port "${PORT:-8077}"
