# NGX shutdown adapter

`python3 tests/nr-shutdown/run.py` compiles the production core-shutdown typedefs,
`ShutdownDx12Device` adapter and public-signature getter with host fakes under ASan/UBSan.

The fake core writes to the mandatory second argument, verifies its initial value and the forwarded
device, and returns success/failure codes. Tests verify that results are preserved, uninitialized or
missing entries are not exposed, and a null device selects the all-devices API instead of dereferencing
null inside the core. The public callable retains the SDK's one-argument type.

The same harness compiles `CustomNgxModule` and `ShutdownCustomDx12` from production:
uninitialized runtimes are inert, all-devices shutdown takes priority, the fallback calls the
one-argument SDK entry, missing entries are tolerated, and the private core slot is never used.
This preserves upstream's public-SDK contract for custom DLLs; it does not inspect arbitrary DLLs.

These are host behavior checks; the driver ABI is established separately through binary inspection
and the real Proton loopback. See `docs/dlssnr/design/ngx-shutdown-order.md` for that evidence.
