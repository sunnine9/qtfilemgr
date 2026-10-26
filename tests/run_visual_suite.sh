#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Spike
# Author: Spike

# Compatibility entry point; the portable runner owns fixture/environment setup.
set -euo pipefail
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
exec python3 "$script_dir/run_visual_suite.py" "$@"
