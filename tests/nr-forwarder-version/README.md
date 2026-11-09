# NR forwarder release version

`python3 tests/nr-forwarder-version/run.py` checks that the resource uses `OptiScaler/resource.h` and is
included by MSBuild and the MSVC CMake route. It preprocesses the main and forwarder resources with
minimal SDK stubs, compares their fixed numeric file/product versions, then repeats with a synthetic
major-version bump. Four source controls reject duplicated numbers and omitted build wiring.

The forwarder's string versions are the stable release number. OptiScaler's descriptive ProductVersion
string includes its dev/build metadata; consumers comparing releases should use the fixed numeric fields.
This test does not compile an RC file or read a PE. Verify the built DLL pair separately after the Release
build. No Wine, model or GPU runs in this suite.
