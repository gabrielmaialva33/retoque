# retoque-core

The shared installer engine follows `tools/deploy-to-game.sh` and `tools/install-windows.ps1`. Public entrypoints
are `plan_install`, `plan_uninstall`, `apply`, `status`, `running_processes`, `ini::Ini` and `receipt::Receipt`.
Planning reads files and processes but never creates a directory or file. Install/uninstall recheck recorded
preconditions and running processes before each step and finalization. Only paths changed by a completed engine
operation are refreshed, before progress callbacks. A later conflict stops with an incomplete receipt.

Plans retain private operation/precondition state. Obtain them from the planning functions; do not construct
or change their public steps or refusals. Clearing a refusal cannot authorize an originally refused plan. Callbacks receive zero-based indices after a step and its receipt are written.
A stale plan must be planned again. Snapshots also cover other proxy files inspected for refusals and the
resolved identity/contents of linked payload or driver sources. Copy verifies the bytes it actually read against
the planned hash before writing them. There is no force-running override.

These are step-boundary rechecks, not an OS transaction or handle-relative filesystem sandbox. Another process
can still swap paths between validation and a syscall, or start mapping a DLL between a process check and its
write. Receipt/journal writes and final cleanup recheck the resolved root/state/parents, and error handlers do
not attempt another receipt write after a failed operation. Preventing all simultaneous rename/reparse races
would require directory handles and no-follow relative operations on both platforms; those are not implemented.
There is no cross-process writer lock: do not run two installers/managers concurrently on the same game.

Default options use `dxgi.dll`, with Linux driver `_nvngx.dll` copying on and all feature/replacement switches off.
`source_dir` is a payload directory; the CLI resolves release/check-out defaults. Models named `nvngx_dlssnr*.dll`
or under `DlssNrModels` are excluded even when an older receipt lists them. Uninstall preserves empty support
folders, following Bash; only empty receipt backup folders are cleaned up.

INI editing preserves bytes and line endings, replaces every duplicate key, and inserts a missing key below the
first matching section header. Windows receipt path rules apply on both platforms. Receipt output uses the Bash
comment header and LF; both scripts can consume it. BOM/CRLF receipt input is accepted too.

Linux running-game checks read `/proc/<pid>/maps`; Windows enumerates Toolhelp process/module snapshots and probes
exclusive file opens. The Windows fallback considers the plan's observed paths (copies, edits, keeps, removals,
renames, restores and receipt dependencies), standard proxy/INI names and immediate `.retoque` state files. It does
not recursively enumerate unrelated game content or backup trees. Planned nested backup/support paths are still
checked. The public `running_processes(game_dir)` has no plan, so it uses the standard proxy/INI/state selection;
`plan_settings` adds the actual INI path and repeats that check before writes. Toolhelp still scans modules under
the whole game folder, independently of this reduced fallback. Linked/reparse paths are excluded from fallback
selection and refused by normal plan validation when used as write targets. If a protected process's module list is unavailable but a file has a sharing violation,
`running_processes` returns pid 0 with the file description: the owner cannot be identified by that probe.
An empty result does not certify inaccessible processes. Linux maps encodes newlines as `\012` but leaves
backslashes literal; names containing the literal text `\012` and a newline can be ambiguous and conservatively
refused. Windows code needs native runtime validation, including Toolhelp access, sharing conflicts, UNC paths,
junctions and ACLs.

From the workspace root:

```sh
cargo test -p retoque-core -j 4
cargo clippy -p retoque-core --all-targets -- -D warnings
cargo check --target x86_64-pc-windows-gnu -p retoque-core
```

Run Cargo inside the workstation memory scope described in the repository instructions. The repository's
`tests/app-install-parity` exercises the CLI against synthetic fixtures and cross-script receipts.

## Settings plans

`read_ini(game_dir)` returns the unique case-insensitive INI match as `(absolute_path, bytes)`, or `None` if
missing. It refuses ambiguous names and linked/non-regular INIs. It never creates an INI. Like Bash's `cd -P`,
all entrypoints resolve the selected game directory once, accepting an alias (symlink/junction) to that directory
and trusting its ancestors, including library junctions and volume mounts. Plans pin the canonical target;
changing the selected alias later cannot redirect an existing plan. The resolved game root and components below
it retain link/reparse checks, including INI files, receipt/state/backup paths and support-folder parents. A
resolved root replaced by a link after planning is refused. Ancestors above the resolved root are not policed.
The existing explicit foreign-link backup/restore behavior remains unchanged: it preserves the link itself,
never writes through it. These are boundary rechecks, not protection against concurrent filesystem races.

`plan_settings(game_dir, edits)` accepts `(section, key, value)` tuples and builds only changed `SetIni` steps.
Each step includes `old: Option<String>`, the effective value before that step (`None` for an absent key), for
an old-to-new preview. Install `SetIni` steps carry the same field. Edits must be single-line values with valid
section/key separators; schema-specific value validation remains the caller's responsibility.

`apply` distinguishes settings from installer operations internally. Settings use exclusively created unique
temporary files beside the INI, preserve its Unix mode, and replace it atomically. On Windows the existing
exclusive-open preflight refuses readonly/access-denied destinations. ACLs and arbitrary Windows attributes
are not guaranteed to survive tempfile's replacement and have not been qualified natively. They do not read, create, rewrite, or claim ownership in a receipt. Consequently an
uninstall preserves a settings-modified INI according to the original receipt hash. Settings refuse stale
fingerprints and running games before apply and recheck the INI/processes between steps; completed edits remain
if a later step is refused. No global cross-process writer lock or multi-key rollback is provided.

Zero-step plans may be applied: empty settings plans write nothing, while uninstall can still clean up an
empty/partial receipt. Progress callbacks occur only for actual steps, with zero-based indices as before.

The CLI also exposes `retoque set [--dry-run] <game-dir> <Section> <Key> <Value>` through this same plan/apply API.

## Interrupted installs and recovery

Before an install copy, INI edit, backup or rename, the core atomically writes and syncs a prospective v1 receipt
to `.retoque/pending-receipt.txt`. The actual mutation follows, then the primary `.retoque/receipt.txt` is committed
and the pending file removed. Uninstall keeps the original record until its removal/restore/reverse-rename has
completed; if interrupted, missing source files make its next plan idempotent. The on-disk primary receipt format
is still version 1, and normal completed or `FAIL_AFTER_STEP` partial receipts remain script-compatible.

When a pending file exists, **recover with the Retoque core/CLI before using either legacy script**: the scripts
refuse any pending intent rather than interpreting it. Older script versions without that guard can lose recovery
information if used directly in that state. Keep both
state files. `status` and planning reconcile them in memory without writes. If a file still matches the previous
receipt's hash, previous ownership is retained; otherwise the intended hash records completed writes and protects
subsequent user modifications. Applying a new install/uninstall plan commits that reconciled receipt before
clearing the intent. A missing base receipt or invalid journal is refused for manual inspection.

A changed/incomplete regular backup is kept, never restored over the game. The explicit foreign-symlink backup
case restores the link object, not the external bytes. A crash during creation of a backup can leave a partial
backup or temporary `incoming.*` file for manual inspection; no later overwrite runs until that operation's
receipt commit succeeds. Modified user files and blocked restores keep the receipt incomplete as in the scripts.
This recovery is forward bookkeeping for completed steps, not rollback of the whole install.

Linux syncs files and containing directories at the journal/receipt/write boundaries; Windows has no portable
std directory durability barrier here. Recovery tests model abrupt process exit and subsequent recovery, not power
loss, storage corruption, arbitrary mid-syscall mutation or filesystem durability guarantees. Settings edits are
still receipt-free and multi-key edits are not a transaction.

The CLI recovery test requires Linux and a C compiler (`cc`) for a child-only
LD_PRELOAD rename interceptor. It also uses the explicit fault injector `RETOQUE_DEPLOY_TEST_CRASH_AT` with
`before-intent:N`, `after-intent:N`, `after-mutation:N`, `before-receipt:N`, or `after-receipt:N` (zero-based step N),
which exits 86 without cleanup. Like `RETOQUE_DEPLOY_TEST_FAIL_AFTER_STEP`, this variable is only for synthetic
subprocess tests; never set it for real installs. Neither injector is set or read from a config file by the GUI.

## User-imported sm86 components

`ComponentFiles { id, source_root, files }`, `plan_component_install`, `plan_component_uninstall` and
`installed_components` add a local component lifecycle. Version 1 supports one `DlssgSm86` component whose
manifest contains `version.dll`, `dlssg_sm86.ini` and files beneath `dlssg_sm86/`. The data catalog validates the
release layout; the core independently rejects unsafe paths, duplicate case-insensitive names, model paths,
linked/non-file sources and destinations, and linked destination parents. It never downloads a runtime. These
APIs do not edit LSFG settings: callers must separately review/apply `plan_settings` to disable competing FG.

The component index `.retoque/components.txt` is an INI with `[DlssgSm86]` keys `Kind=dlssg-sm86`, `Paths`
(pipe-separated safe relative paths), and `Id`. Its edits are ordinary visible `SetIni` steps; index and payload
files get normal v1 `File` records. The index is written before payload copies, making partial copies recoverable.
If interrupted before the `Id` step, use full uninstall or replan the component install. Index reads refuse an
unowned, changed or linked index. A component-only uninstall filters the receipt to its payload and associated
backups, preserving unrelated Retoque files. Changed files or blocked restores remain tracked; rerun after
resolving the conflict. Successful removal deletes the index. Component operations preserve a pre-existing incomplete base receipt status;
finishing a component alone does not certify the rest of that installation. Same-layout updates replace the identity in place;
a changed file list requires uninstalling the previous component first, preventing stranded old payloads.

An existing `version.dll` together with `dlssg_sm86.ini`, or a loader carrying `dlssg_sm86`, `dlssg_for_sm86` or
`DLSSG SM86` ASCII markers, identifies a legacy sm86 installation. Unowned files in that recognized component
layout are backed up before replacement, including old configuration; uninstall restores them. Marker recognition
is a compatibility heuristic, not cryptographic authentication. Unknown destination files are refused. An
OptiScaler-marked `version.dll` is always refused with a proxy-conflict explanation, even if its INI neighbor
looks like sm86. Other sm86-marked proxy names (including `dinput8.dll`) are refused as a second loader. Loader
refusal inputs and component sources are covered by the same stale-plan and running-process checks as installs.

Legacy Bash/PowerShell scripts intentionally know nothing about component identity or per-component uninstall.
Their full uninstall consumes the normal file/backup records and removes the index and payload, restoring old
files. Use the app/CLI for component-only removal. Legacy scripts refuse an outstanding pending intent; recover
that operation with the core first. Empty payload directories can remain, as with normal support files.

CLI commands are `retoque gpu [--json]`, `retoque games [--json] [--home DIR]`, and
`retoque component install|uninstall [--dry-run] <game-dir> <component-dir>`. Component uninstall also accepts
an installed component ID instead of a release directory (useful if the original release was removed). The CLI
inspects local releases without importing/copying a catalog; dry runs write nothing. Component apply prints the
complete plan before its first write. `games` reports per-target status, recommendations and errors; unreadable
state suppresses recommendations for that target. It uses the first detected GPU; this is not per-game adapter
selection or proof that a recommended runtime works on that GPU.

## DLSS swaps and two receipts

`plan_dlss_update(install_dir, target, catalog, files)` accepts only existing SR/RR/FG names at safe relative paths.
It backs up unowned originals, preserves the first backup on subsequent updates, refuses changed owned DLLs and
uses the ordinary sealed File/Backup/journal engine. The data layer separately pins NVIDIA hashes and versions.
Sources, all destination parents and case ambiguities are checked before planning. Whole-install running checks
are retained in the plan and repeated at mutation boundaries, including when the receipt is in a nested target.

When a DLL lies outside the target, a standalone v1 receipt lives at the known installation root, never above it.
Its owned `.retoque/dlss.txt` identity prevents treating an unrelated root installation as a DLSS receipt.
`plan_game_restore` reviews the root before the target; changed DLLs keep the root identity for later recovery.
Existing Bash/PowerShell readers need no syntax changes, but must be invoked at each receipt root separately.
`combine_install_plans` merges only same-root disjoint installs before review, retaining source observations;
`validate_plan` provides a read-only preflight for all plans before a compound apply. Multi-root applies are not
atomic: interruption can leave the root completed and the target pending; replan restoration to finish.
