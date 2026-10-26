# Isolated real SSH/SFTP end-to-end test

The SSH fixture is opt-in and is separate from the default local regression
suite. Missing dependencies, root execution, server startup/authentication
failures, and failed assertions return nonzero; these are never silent skips.

On Ubuntu/Debian, install the public prerequisites:

```sh
sudo apt-get install openssh-server libnss-wrapper libssh-dev python3
# Some minimal systems do not create OpenSSH's runtime directory at install time.
sudo install -d -m 0755 /run/sshd
```

Build and run as a normal user with a valid local passwd account:

```sh
cmake -S . -B build -DQTFILEMGR_WITH_SSH=ON \
  -DBUILD_TESTING=ON -DQTFILEMGR_TEST_SSH_E2E=ON
cmake --build build --parallel
(cd build && ctest -R '^qtfilemgr.ssh_e2e$' --output-on-failure)
```

The native executable can also be selected explicitly:

```sh
python3 tests/run_ssh_e2e.py --run \
  --binary build/tests/qtfilemgr_ssh_e2e_test
```

`--nss-wrapper /path/to/libnss_wrapper.so` supports distributions with a
different library layout. No private image, Docker daemon, project checkout
path, real SSH identity, agent, or personal known_hosts file is required.

The runner creates temporary Ed25519 host/client keys, isolated HOME and XDG
directories, a dedicated known_hosts path, and a disposable data root. It
starts an unprivileged sshd bound only to `127.0.0.1` on a random high port,
with password/PAM/forwarding disabled. Only the client receives an NSS wrapper
passwd/group mapping so libssh's `getpwuid()` identity search resolves inside
the disposable HOME. The host account and server's account lookup are not
modified. Temporary files and the sshd process are cleaned up even on failure.
`StrictModes no` is confined to this fixture below `/tmp`; private directories
are 0700 and private keys are 0600. Never reuse this config as a persistent
server. A raced/occupied port fails the test rather than selecting another
server.

Coverage includes unknown host rejection/acceptance, persistent known-host
reconnect, invalid configured known_hosts failure without fallback, saved-place
reconnect, real listing/mkdir, explicit `importFromLocalRequest` upload copy and
move, `exportToLocalRequest` download, backend-to-backend copy/move/delete,
existing-target preservation, atomic local download overwrite, bounded text
and image preview, upload cancellation/partial cleanup, and disconnect during
an active export with cancellation and preservation of an existing local
target. A disconnected backend must reject subsequent listing with an error.

This loopback fixture shares the host filesystem namespace; explicit import
and export calls exercise their distinct API paths, but it does not substitute
for testing an independent remote machine. It does not cover production hosts,
password/agent authentication, host-key replacement, remote directory overwrite,
network partitions, or transaction-layer queue/undo policies. Those queue and
undo policies have separate substitute-backend coverage in the local suite.

## Guarded overwrite extension

The suite also checks that `renameNoReplace` refuses an occupied real SFTP target
and succeeds for a free target. An IO decorator delegates to the final
`QtFileMgrSSHIO` transport; it selectively rejects install/restore calls to test
commit success, successful rollback and failed rollback against real disposable
server files. Transfers and successful rename calls still use SFTP. The tests
check original/new content and recovery path messages. These controlled faults
do not simulate an abrupt network partition or prove bounded shutdown.
