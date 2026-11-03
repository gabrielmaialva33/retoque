# Streamline interposer unhook

`python3 tests/sl-interposer-unhook/run.py`

`StreamlineHooks::unhookInterposer` detaches exactly the detours `hookInterposer` attached
([sl-interposer-unhook.md](../../docs/linux-proton/sl-interposer-unhook.md)). It used to detach every export it
had looked up, and one never attached failed the whole Detours transaction.

The suite compiles `retoque/hooks/DetourRecord.h` under ASan/UBSan against a fake Detours. The fake refuses
chosen attaches at once or poisons the transaction, poisons it on a detach without a trampoline, commits all or
nothing, and turns originals into trampolines and back, as the real one does. The cases cover:

- only what was attached comes off;
- newest first;
- an attach refused at once is not recorded, the rest commits;
- an attach that poisons the transaction (an allocation failure) records nothing;
- a failed commit records nothing;
- a failed unhook keeps the record and the originals;
- two transactions accumulate;
- a new transaction drops an uncommitted one.

Six rule mutants must fail those cases.

Source guards read `Streamline_Hooks.cpp` and check:

- no raw `DetourAttach` in `hookInterposer`, and at least 19 hooks through the record;
- each of the two transactions bracketed by `Begin` and `Committed`;
- one swap at a time, admitted without waiting and released on every return;
- a second interposer refused when the unhook failed;
- the unhook detaching only through the record and returning its result;
- the old per-hook bookkeeping gone.

Eight source mutants must each fail a guard.

Not proved: Detours itself, a game that loads two interposers, a real race between two threads, MSVC compilation (the Release build covers that),
or native Windows.
