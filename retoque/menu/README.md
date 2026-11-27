# Retoque menu additions

Fork-owned helpers used by the upstream overlay at its existing paths.

- `MenuWindowOwnerPolicy.h`: the DX overlay's one-window lease, serialized by its existing mutex.
- `NrStatusRules.h`: read-only NR Status labels from copied configuration and diagnostic state.
- `input/`: physical key-state and game-specific input helpers.

See [menu ownership and Status design](../../docs/dlssnr/design/menu-status-owner.md) and
[host contracts](../../tests/menu-status-owner/README.md). These additions do not own GPU resources.
