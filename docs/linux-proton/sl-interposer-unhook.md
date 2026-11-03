# Streamline interposer unhook: detach what was attached

Date: 2026-11-03. Status: fixed, host-tested; not reproduced in a game.

## The defect (upstream)

`StreamlineHooks::unhookInterposer` runs when a second `sl.interposer.dll` is hooked. It called `DetourDetach`
for every export `hookInterposer` had looked up, whether or not a detour had been attached to it. For example,
without a Streamline frame-generation input, `slSetTag`, `slSetTagForFrame` and `slSetConstants` are looked up but
not hooked.

Detours rejects a detach of a pointer that holds no trampoline, and that error fails the whole transaction
(Detours 4.0.1, `detours.cpp`: trampoline check in `DetourDetach`, abort in `DetourTransactionCommit`). Nothing was
detached. Two consequences followed:

- `hookInterposer` went on regardless and overwrote the `o_*` pointers that the first interposer's still-attached
  hooks call;
- the DLSS-G feature hooks (`slIsFeatureSupported` and the rest) were never in the detach list at all.

The same defect is in `optiscaler/master` at `ed5708bb`. Retoque had worked around one instance of it on
2026-11-03 for the note-only tag hooks of the Streamline tag inventory (`sl-tag-inventory.md`); this replaces that
workaround.

## The fix

`retoque/hooks/DetourRecord.h` (standard library only) records each `DetourAttach` that Detours accepted, and keeps
it only if its transaction committed.

- `hookInterposer` attaches all its interposer hooks through it (`AttachInterposerHook`), in both the Streamline 2
  and the Streamline 1 transactions.
- `unhookInterposer` detaches exactly the recorded detours, newest first. That includes the DLSS-G feature hooks
  it never used to detach.
  - On success it clears each recorded original pointer, then the ones it always cleared.
  - On failure it keeps the record and reports it: `unhookInterposer` now returns whether the transaction
    committed.
- A second interposer whose predecessor's hooks could not be removed is left unhooked, with an error line,
  instead of being hooked over live detours.
- One swap runs at a time. The record, the originals and the hooked module change together, and a second
  thread arriving meanwhile is admitted without waiting: its interposer is left unhooked, with a warning. A
  blocking lock could deadlock, because one caller runs inside `DllMain` under the loader lock, and the thread
  holding the swap may need that lock.

With NR off and nothing re-hooked, nothing changes: the record is only filled and read around the same
transactions as before.

## Not covered

- **The boundary probe's own hooks** (`DlssNr_SlBoundaryProbe`, installed only with NR on and `HookMethod=2`) are
  removed by its own `Uninstall`, called first by `unhookInterposer`. `Uninstall` does not report a failed detach,
  and it clears its state before the commit: that defect predates this change and is left open. The guarantee
  here covers the interposer hooks in the record.
- Callbacks already running on other threads while a hook is removed. Only the current thread is passed to
  `DetourUpdateThread`, as before; there is no drain protocol.

## Tests

`tests/sl-interposer-unhook`:

- the record on a fake Detours, with 8 cases and 6 rule mutants. The fake refuses at once or poisons the
  transaction the way the real one does, and turns originals into trampolines and back;
- source guards with 8 mutants:
  - no raw `DetourAttach` in `hookInterposer`;
  - each transaction is bracketed by the record;
  - the unhook detaches only through the record and returns its result;
  - one swap at a time, admitted without waiting;
  - a second interposer is refused when the unhook failed.

`tests/nr-sl-tag-inventory` now checks the note-only hooks through the same record.

Not proved: a game that loads two interposers, Detours itself (the fake reproduces its documented behaviour), or
native Windows. An upstream pull request needs this as an inline struct in `OptiScaler/hooks`, because upstream has
no `retoque/` tree. It has not been sent.
