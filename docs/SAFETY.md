# Guarded undo and recoverable remote overwrite

These changes protect later edits during undo and keep recovery copies when a
remote overwrite cannot commit. They do not add crash-recovery journaling or an
exclusive lock against other applications.

## Undo

Each successful operation item supplies an opaque identity/revision proof from
the execution side through `QtFileMgrFileOperationJob::itemCompleted`. The view
does not create a new baseline later in its final GUI callback. Copy undo and
move undo both require the backend to validate that proof before changing data.

The Linux local implementation checks device/inode, node type and permissions,
owner/group, link count, size, nanosecond modification/change times and a SHA-256
digest of every regular file. A directory proof includes every descendant;
symbolic links are checked as links and their raw targets are recorded. Two
complete manifests must agree. Unknown, unreadable, changing or unsupported
nodes fail closed.

Validation is deliberately bounded: at most **8 MiB of total file contents,
4096 nodes and depth 64**, with a **50 ms soft budget per manifest pass**. Exceeding
a limit excludes that item from undo and produces an in-window warning and a
path-specific `qWarning`. The transfer itself remains successful. The budget
limits scanning work; it is not a guaranteed deadline for an individual
filesystem call on stalled storage. Large-object asynchronous undo is future work.

Before mutation, the local backend checks the current proof, moves the object
to a unique sibling quarantine using a no-replace rename and checks it again.
Move undo cannot replace a new object at the original source path. A move back
across filesystems is refused when a no-replace rename returns `EXDEV`; guarded
cross-filesystem reverse transfer is not implemented. A refused
item stays intact; if restoration is blocked, the error reports the retained
quarantine path. Successful items are tracked explicitly so retry does not infer
success from an unrelated file's existence. A new object with identical bytes
does not automatically become the original object for undo.

Consecutive moves retain execution-time provenance: the local worker captures
the source proof before a move and links it to the completed destination only
when the full retained-object proof matches across the rename. A successful
reverse move validates the restored object again and may return its new exact
proof. The view renews an older history item only when both its path and old
exact proof match that move's recorded source proof. This accounts for the
component's own root ctime changes without accepting external edits, replacement
or arbitrary rescanning as a new baseline. Cross-filesystem copied moves do not
provide same-object provenance. If the post-undo proof cannot be established,
the physically completed undo remains completed and the older proof is retained
with a warning.

An application that keeps a file descriptor open can still modify an object
concurrently after validation. These checks and quarantine reduce the ordinary
path-replacement window; they are not a cross-process transaction or a security
sandbox. Keep external writers idle when performing destructive undo.

Custom backends can implement `undoSnapshot`, `undoCopyIfUnchanged` and
`undoMoveIfUnchanged` using reliable object IDs and versions. Default
implementations reject guarded undo. The built-in SSH backend does not claim a
strong remote revision proof, so its operations do not create automatic undo
records. Returning only a path, size or timestamp is insufficient for this contract.

## Remote overwrite

The operation proceeds through explicit phases:

1. Transfer the replacement into a unique sibling staging path.
2. Rename the existing target to a unique backup path.
3. Rename staging into the now-free target path.
4. Clean the old backup only when the backend can verify its identity/revision.

Backup, install and restore use `renameNoReplace`. The default IO implementation
refuses this operation; it never emulates it with an `exists()` check followed
by a potentially replacing rename. Linux LocalIO uses `renameat2` with
`RENAME_NOREPLACE` and refuses an unavailable primitive. The SSH implementation
uses the ordinary SFTP v3 rename request; other server protocol versions are
rejected for this operation. It does not use a replacing POSIX-rename extension.
This relies on the server honoring the [SFTP v3 rename contract, section 6.5](https://datatracker.ietf.org/doc/html/draft-ietf-secsh-filexfer-02#section-6.5).
The client cannot provide stronger concurrency guarantees than its server and
filesystem implementation. See the [OpenSSH server implementation](https://github.com/openssh/openssh-portable/blob/master/sftp-server.c)
for its regular-file implementation and filesystem-dependent fallback.

If install fails, the operation attempts one guarded restoration of the backup.
If restoration is blocked by a new target, a destroyed backend or another error,
it preserves the backup and staging nodes and reports all relevant paths.
An RPC failure can mean the server acted but its reply was lost; the client does
not blindly repeat the rename or delete a node whose ownership is uncertain.

Cancellation before backup leaves the old target untouched. Cancellation after
backup attempts restoration. Cancellation after confirmed installation reports
the committed result and preserves the backup when cleanup is not safe. Cleanup
failure is reported as a warning accompanying successful installation.

For the built-in SSH backend, the absence of a strong backup revision proof
means a successful overwrite **retains the old backup and reports its path**.
This is intentional; it avoids deleting unverified remote content. Review and
remove these backups manually when no longer needed. A move may already have
removed its original source while producing staging, so a failed commit also
retains staging as a recoverable copy.

## Manual recovery

On a failed or uncertain operation, stop further changes and inspect the reported
target, backup, staging and original source paths. Identify their contents before
renaming or deleting anything. A restored target contains the old version; a
retained staging node contains the replacement when its transfer completed.
When restoration failed, the old version may be at the backup path instead.
The application reports these paths in the operation details and warnings.

See [TESTING.md](TESTING.md) for deterministic fault-injection and real loopback
SFTP checks, and [VALIDATION.md](VALIDATION.md) for the combinations actually run.
