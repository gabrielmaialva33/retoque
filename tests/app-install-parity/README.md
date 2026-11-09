# app-install-parity

Builds `retoque-cli` and runs every [deploy-safety](../deploy-safety/README.md) case against its `retoque`
command, with install/uninstall flags mapped to subcommands. The original Bash run is unchanged. All DLLs and
folders are synthetic; no game, Wine prefix, NVIDIA model or real app configuration is modified.

The cases cover refusals, no-write dry runs, partial receipts, reversible install/uninstall, INI byte preservation,
ReShade, driver copies and support files. The release-wrapper case instead copies the CLI beside the synthetic
payload and invokes it outside the checkout, checking executable-relative payload discovery. Shellcheck still
checks the original Bash script. The suite also cross-uninstalls complete and partial receipts and upgrades owned files between Bash and Rust,
and PowerShell when `pwsh` is available. It checks JSON status, usage errors, and recovery after an interrupted
uninstall. It also crashes the real Rust CLI immediately after its ReShade rename, through a C/LD_PRELOAD
interceptor confined to that child. Bash and PowerShell must then refuse install, uninstall and both dry runs
with exit 1 and recovery instructions. File contents, modes, inode identities and modification times (including
directories) must remain unchanged. A subsequent Rust uninstall must restore the original mod/game snapshot.
This tests both crash -> Bash -> Rust and crash -> PowerShell -> Rust; neither legacy script understands the
pending intent, so refusal is the supported compatibility boundary until the app/core recovers it.

The crash cases require Linux and a C compiler (`cc`); missing `cc` fails explicitly. PowerShell cases print an
explicit SKIP when `pwsh` is unavailable. Unit tests for the engine and Windows compilation are separate checks.

Cargo compilation uses four jobs and a memory-capped systemd scope when the host offers a user session. Cargo
missing prints an explicit `SKIP`; build and parity failures exit nonzero. This tests Linux file/process semantics,
not Windows module enumeration or exclusive opens, native Windows operation, or game/GPU compatibility.

```bash
python3 tests/app-install-parity/run.py
python3 tests/deploy-safety/run.py --impl /path/to/retoque
```
