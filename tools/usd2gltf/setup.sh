#!/usr/bin/env bash
# Creates tools/usd2gltf/.venv with usd-core (Pixar's USD reader) for usd2gltf.py.
set -euo pipefail
dir="$(cd "$(dirname "$0")" && pwd)"
[[ -x "$dir/.venv/bin/python" ]] || python3 -m venv "$dir/.venv"
# The copy of requirements.txt marks a finished install of exactly that file.
if ! cmp -s "$dir/requirements.txt" "$dir/.venv/requirements.txt"; then
  "$dir/.venv/bin/pip" install --quiet --disable-pip-version-check -r "$dir/requirements.txt"
  cp "$dir/requirements.txt" "$dir/.venv/requirements.txt"
fi
