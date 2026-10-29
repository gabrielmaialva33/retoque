# Release evidence

`python3 tests/release-evidence/run.py` exercises the production manifest tool with synthetic ZIPs and aggregate
host reports. It rejects partial/dirty/wrong-identity PASS claims, inconsistent counts, unsafe archive names,
symlinks and accidental model distribution; it verifies actual archive/member hashes and sanitized summaries.
CLI tests use the current checkout identity but never execute DLLs, create commits or publish releases.
This is packaging/evidence validation, not a native Windows build, a cryptographic signature or GPU qualification.
