#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Spike
# Author: Spike

"""Render thirteen opt-in visual scenarios against disposable sample files."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

from run_isolated import isolated_environment


def fixture(scratch):
    env = isolated_environment(scratch)
    home = Path(env["HOME"])
    for name in ("Desktop", "Documents", "Downloads", "Pictures", "Projects"):
        (home / name).mkdir()
    directory = home / "Documents"
    (directory / "Design").mkdir()
    (directory / "Notes").mkdir()
    (directory / "Release").mkdir()
    (directory / "README.md").write_text("# Sample workspace\n\nA portable file manager component.\n")
    (directory / "project-notes.txt").write_text("Review the release checklist.\n")
    (directory / "budget.csv").write_text("Item,Amount\nDesign,1200\nDevelopment,4800\n")
    (directory / "Design" / "overview.txt").write_text("Sample project overview.\n")
    for path in directory.rglob("*"):
        os.utime(path, (1767225600, 1767225600))
    config = {
        "paths": {"rootPath": str(directory), "allowedPaths": []},
        "places": {
            "showDevices": False,
            "builtin": [
                {"name": "Home", "path": str(home), "icon": "user-home"},
                {"name": "Desktop", "path": str(home / "Desktop"), "icon": "user-desktop"},
                {"name": "Documents", "path": str(directory), "icon": "folder-documents"},
                {"name": "Downloads", "path": str(home / "Downloads"), "icon": "folder-downloads"},
                {"name": "Pictures", "path": str(home / "Pictures"), "icon": "folder-pictures"},
                {"name": "Projects", "path": str(home / "Projects"), "icon": "folder-development"},
            ],
        },
    }
    Path(env["QTFILEMGR_CONFIG_FILE"]).write_text(json.dumps(config), encoding="utf-8")
    return env, directory


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    binary = args.binary.resolve(strict=True)
    if not binary.is_file() or not os.access(binary, os.X_OK):
        parser.error("--binary must name the qtfilemgr_grab_shot executable")
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    failures = 0
    with tempfile.TemporaryDirectory(prefix="qtfilemgr-visual-") as temporary:
        for name in ("default", "tabs-switch", "split", "narrow", "badpath", "select-filter",
                     "views", "zoom", "dlg-settings", "dlg-search", "dlg-trash", "dlg-props",
                     "newfolder"):
            scenario_scratch = Path(temporary) / name
            scenario_scratch.mkdir()
            env, directory = fixture(scenario_scratch)
            scenario_output = output / name
            scenario_output.mkdir(exist_ok=True)
            final = scenario_output / "final.png"
            extra = []
            actions = "wait:load"
            if name == "tabs-switch":
                extra = ["--tab", str(directory / "Design"), "--switch", "0"]
            elif name == "split":
                extra = ["--split"]
            elif name == "narrow":
                extra = ["--size", "700x450"]
            elif name == "badpath":
                extra = ["--url", str(directory / "unavailable-location")]
            elif name == "select-filter":
                actions += f";select:README.md;shot:{scenario_output / 'selection.png'};key:Slash;filter:notes;shot:{scenario_output / 'filter.png'}"
            elif name == "views":
                actions += f";view:details;shot:{scenario_output / 'details.png'};view:icons;shot:{scenario_output / 'icons.png'};view:compact"
            elif name == "zoom":
                actions += ";view:icons;zoom:4"
            elif name.startswith("dlg-"):
                dialog = {"dlg-settings": "settings", "dlg-search": "search",
                          "dlg-trash": "trash", "dlg-props": "props"}[name]
                if dialog == "props":
                    actions += ";select:README.md"
                actions += f";open:{dialog};wait:700"
                if dialog == "search":
                    actions += ";search:notes;wait:1000"
                actions += f";shotw:{scenario_output / 'dialog.png'};close"
            elif name == "newfolder":
                actions += ";newfolder;wait:700"
            expected = 3 if name in ("select-filter", "views") else 2 if name.startswith("dlg-") else 1
            try:
                completed = subprocess.run(
                    [str(binary), "--url", str(directory), *extra, "--actions", actions,
                     "--out", str(final)], env=env, cwd=scenario_scratch,
                    text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                    timeout=90, check=False)
                log = completed.stdout
                (scenario_output / "render.log").write_text(log, encoding="utf-8")
                saved = log.count("saved=1")
                captures = [final]
                if name == "select-filter":
                    captures += [scenario_output / "selection.png", scenario_output / "filter.png"]
                elif name == "views":
                    captures += [scenario_output / "details.png", scenario_output / "icons.png"]
                elif name.startswith("dlg-"):
                    captures.append(scenario_output / "dialog.png")
                okay = completed.returncode == 0 and saved == expected and all(
                    path.is_file() and path.stat().st_size > 0 for path in captures)
                print(f"{'PASS' if okay else 'FAIL'}: {name} (saved={saved}, expected={expected})", flush=True)
                if not okay:
                    failures += 1
                    print(log, file=sys.stderr)
            except (OSError, subprocess.TimeoutExpired) as error:
                failures += 1
                print(f"FAIL: {name}: {error}", file=sys.stderr)
    print(f"RESULT: visual suite {'PASS' if failures == 0 else 'FAIL'} ({failures} failures); output={output}")
    return 1 if failures else 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except OSError as error:
        print(f"RESULT: visual suite FAIL: {error}", file=sys.stderr)
        sys.exit(1)
