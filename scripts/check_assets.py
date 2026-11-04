#!/usr/bin/env python3
"""Check local deployment assets without opening or displaying credentials."""
import hashlib
import json
from pathlib import Path

root = Path(__file__).resolve().parents[1]
manifest = json.loads((root / 'assets/manifest.json').read_text())
failures = []
for entry in manifest['files']:
    path = root / entry['file']
    ok = path.is_file() and path.stat().st_size == entry['bytes']
    if ok:
        ok = hashlib.sha256(path.read_bytes()).hexdigest() == entry['sha256']
    print(('OK ' if ok else 'MISSING_OR_CHANGED ') + entry['file'])
    if not ok:
        failures.append(entry['file'])
raise SystemExit(1 if failures else 0)
