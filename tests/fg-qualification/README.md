# Frame-generation qualification contracts (host)

Run `python3 tests/fg-qualification/run.py`. This invokes the real-provider fixture's
portable tests for requested versus observed x2/x3/x4 ratios, complete five-second
counter windows, capability and identity gaps, explicit INI options and process
supervision boundaries. Signals and subprocess ownership are simulated in these
controls; no Wine, GPU, game or model is launched.

The runtime fixture remains `tests/fg-provider-d3d12`. Passing these contract tests
does not qualify a GPU, driver, runtime, generated image, latency or display output.
