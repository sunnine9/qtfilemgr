#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Spike
# Author: Spike

"""Explicit opt-in, disposable non-root loopback sshd and native SFTP client."""
import argparse
import grp
import os
from pathlib import Path
import pwd
import shutil
import socket
import subprocess
import sys
import tempfile
import time

from run_isolated import isolated_environment


def command(arguments, **kwargs):
    print("+", " ".join(map(str, arguments)), flush=True)
    return subprocess.run(arguments, check=True, **kwargs)


def executable(name):
    result = shutil.which(name)
    if not result and name == "sshd" and Path("/usr/sbin/sshd").is_file():
        result = "/usr/sbin/sshd"
    if not result:
        raise RuntimeError(f"missing required executable: {name}")
    return result


def nss_library(explicit):
    if explicit:
        library = explicit.resolve(strict=True)
        if library.is_file():
            return library
        raise RuntimeError("--nss-wrapper must name libnss_wrapper.so")
    candidates = [Path("/usr/lib/libnss_wrapper.so"), Path("/usr/lib64/libnss_wrapper.so")]
    for directory in (Path("/usr/lib"), Path("/lib")):
        candidates.extend(sorted(directory.glob("*-linux-gnu/libnss_wrapper.so")))
    for library in candidates:
        if library.is_file():
            return library.resolve()
    raise RuntimeError("missing libnss_wrapper.so: install libnss-wrapper or pass --nss-wrapper")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run", action="store_true", help="required explicit E2E opt-in")
    parser.add_argument("--binary", required=True, type=Path, help="built native SSH E2E executable")
    parser.add_argument("--nss-wrapper", type=Path, help="path to libnss_wrapper.so")
    args = parser.parse_args()
    if not args.run:
        parser.error("E2E requires --run; missing prerequisites are failures, not skips")
    if os.geteuid() == 0:
        raise RuntimeError("refusing root: sshd must run as the current unprivileged user")
    binary = args.binary.resolve(strict=True)
    if not binary.is_file() or not os.access(binary, os.X_OK):
        raise RuntimeError("--binary must name an executable file")
    sshd = executable("sshd")
    keygen = executable("ssh-keygen")
    wrapper = nss_library(args.nss_wrapper)
    sftp = next((path for path in ("/usr/lib/ssh/sftp-server", "/usr/lib/openssh/sftp-server")
                 if Path(path).is_file()), None)
    if not sftp:
        raise RuntimeError("missing host sftp-server executable")
    account = pwd.getpwuid(os.getuid())
    group = grp.getgrgid(os.getgid())

    with tempfile.TemporaryDirectory(prefix="qtfilemgr-ssh-e2e-") as temporary:
        scratch = Path(temporary)
        client_env = isolated_environment(scratch)
        home = Path(client_env["HOME"])
        (home / ".ssh").mkdir(mode=0o700)
        data = scratch / "fixture"
        data.mkdir(mode=0o700)
        hostkey = scratch / "hostkey"
        identity = home / ".ssh/id_ed25519"
        for key in (hostkey, identity):
            command([keygen, "-q", "-t", "ed25519", "-N", "", "-f", str(key)])
        authorized = scratch / "authorized_keys"
        authorized.write_bytes(identity.with_suffix(".pub").read_bytes())
        authorized.chmod(0o600)
        # libssh may use getpwuid() rather than HOME for default identities.
        # Rewrite NSS only in the client process, never the host sshd or user files.
        passwd_file = scratch / "passwd"
        passwd_file.write_text(
            f"{account.pw_name}:x:{os.getuid()}:{os.getgid()}:fixture:{home}:/bin/sh\n",
            encoding="utf-8")
        group_file = scratch / "group"
        group_file.write_text(f"{group.gr_name}:x:{os.getgid()}:\n", encoding="utf-8")
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            port = listener.getsockname()[1]
        config = scratch / "sshd_config"
        config.write_text("\n".join([
            "ListenAddress 127.0.0.1", f"Port {port}", f"HostKey {hostkey}",
            f"AuthorizedKeysFile {authorized}", f"PidFile {scratch / 'sshd.pid'}",
            f"Subsystem sftp {sftp}", "UsePAM no", "PasswordAuthentication no",
            "KbdInteractiveAuthentication no", "PubkeyAuthentication yes",
            "PermitRootLogin no", "PermitEmptyPasswords no", "AllowTcpForwarding no",
            "AllowAgentForwarding no", "X11Forwarding no", "PermitTunnel no",
            # Private fixture keys remain 0600; this relaxed ancestor check is
            # confined to the temporary server and its /tmp parent.
            "PermitUserEnvironment no", "StrictModes no", "LogLevel VERBOSE",
            "HostbasedAuthentication no", "MaxSessions 4", ""]), encoding="utf-8")
        command([sshd, "-t", "-f", str(config)])
        with (scratch / "sshd.log").open("wb") as log:
            daemon = subprocess.Popen([sshd, "-D", "-e", "-f", str(config)],
                                      stdout=log, stderr=subprocess.STDOUT)
            completed = False
            try:
                for _ in range(100):
                    if daemon.poll() is not None:
                        raise RuntimeError(f"sshd exited early ({daemon.returncode})")
                    try:
                        with socket.create_connection(("127.0.0.1", port), timeout=0.1):
                            break
                    except OSError:
                        time.sleep(0.05)
                else:
                    raise RuntimeError("loopback sshd did not start within five seconds")
                client_env.update({
                    "QTFILEMGR_SSH_KNOWN_HOSTS": str(scratch / "trusted" / "known_hosts"),
                    "QTFILEMGR_SSH_E2E": "1", "QTFILEMGR_SSH_E2E_ROOT": str(data),
                    "QTFILEMGR_SSH_E2E_KEY": str(identity),
                    "QTFILEMGR_SSH_E2E_USER": account.pw_name,
                    "QTFILEMGR_SSH_E2E_PORT": str(port),
                    "NSS_WRAPPER_PASSWD": str(passwd_file),
                    "NSS_WRAPPER_GROUP": str(group_file),
                    "LD_PRELOAD": str(wrapper),
                })
                command([str(binary)], env=client_env, cwd=scratch, timeout=180)
                completed = True
                print("RESULT: SSH E2E runner PASS")
            finally:
                daemon.terminate()
                try:
                    daemon.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    daemon.kill()
                    daemon.wait()
                if not completed or daemon.returncode not in (0, -15):
                    print("sshd diagnostics:\n" + (scratch / "sshd.log").read_text(errors="replace"),
                          file=sys.stderr)


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, OSError, KeyError, subprocess.CalledProcessError,
            subprocess.TimeoutExpired) as error:
        print(f"RESULT: SSH E2E runner FAIL: {error}", file=sys.stderr)
        sys.exit(1)
