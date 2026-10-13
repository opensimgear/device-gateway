#!/bin/sh
# Usage: sh tools/bundle-jlcpcb.sh [version] [--output-dir directory] [--smd-only]
set -eu
tools_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
exec python3 "$tools_dir/jlcpcb.py" --board gateway "$@"
