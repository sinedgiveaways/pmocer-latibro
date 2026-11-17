# tools/run/testdata

Input for the tests of the tools in `tools/run`.

| File | What |
|---|---|
| `sample-orbitals.log` | A short synthetic `orbitals.log` in the shape of a run that ends with Unreal's render-thread timeout (an abort with a thread dump, one shader without SPIR-V, SDK imports without override, `ORBITALS_DIAG_FENCE` heartbeats that stop changing, its dump and the key counts of three waiting threads, the last wait of the GameThread and its call chain). Written for the smoke test of `shader_analyzer.exe` (`.github/workflows/log-analyzer.yml`); it holds no game file. |
