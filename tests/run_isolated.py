#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Spike
# Author: Spike

"""Run a Qt test with disposable user data and an offscreen display."""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def isolated_environment(scratch):
    env = os.environ.copy()
    for variable, directory in (
        ("HOME", "home"), ("XDG_CONFIG_HOME", "config"),
        ("XDG_DATA_HOME", "data"), ("XDG_CACHE_HOME", "cache"),
        ("XDG_RUNTIME_DIR", "runtime"),
    ):
        path = scratch / directory
        path.mkdir(mode=0o700, exist_ok=True)
        env[variable] = str(path)
    env["QT_QPA_PLATFORM"] = "offscreen"
    env["QTFILEMGR_CONFIG_FILE"] = str(scratch / "config" / "mgr_default_config.json")
    env["QTFILEMGR_SSH_KNOWN_HOSTS"] = str(scratch / "known_hosts")
    env.pop("SSH_AUTH_SOCK", None)
    env.pop("SSH_AGENT_PID", None)
    return env


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("arguments", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    # A separator lets application options pass through argparse unchanged.
    arguments = args.arguments[1:] if args.arguments[:1] == ["--"] else args.arguments
    binary = args.binary.resolve(strict=True)
    if not binary.is_file() or not os.access(binary, os.X_OK):
        parser.error("--binary must name an executable file")
    with tempfile.TemporaryDirectory(prefix="qtfilemgr-test-") as temporary:
        scratch = Path(temporary)
        result = subprocess.run([str(binary), *arguments],
                                env=isolated_environment(scratch), cwd=scratch,
                                timeout=240, check=False)
        return result.returncode if result.returncode >= 0 else 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, subprocess.TimeoutExpired) as error:
        print(f"RESULT: isolated runner FAIL: {error}", file=sys.stderr)
        sys.exit(1)
