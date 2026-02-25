# Workspace Structure Audit (Path/Build Reachability)
Last updated: 2026-02-25

## Scope
- Folder branching sanity (tracked workspace layout)
- Duplicate/stale source path candidates
- CMake configuration relevance vs actual workflow usage

## Findings
1. Source tree branching is consistent:
   - `src/*` and `include/*` module partitions are aligned (`analytics/app/common/...`).
2. Backtest history loader unit is still active:
   - `src/backtest/DataHistory.cpp`
   - `include/backtest/DataHistory.h`
   - Reason: referenced by `BacktestRuntime.cpp` for CSV/JSON load path.
3. CMake release-flag expression improved:
   - replaced `if(CMAKE_BUILD_TYPE STREQUAL "Release")` with config-aware expression.
   - avoids no-op behavior under VS multi-config generators.
4. Workflow/CMake option drift detected and fixed:
   - CI workflows previously relied on cached build options for test/tool binaries.
   - configure steps now explicitly set `AUTOLIFE_BUILD_*` options.
5. Strict live workflow probe gate mismatch fixed:
   - `run_ci_operational_gate.py -RunLiveProbe` now includes `-AllowLiveProbeOrder`.

## Changes Applied
- `CMakeLists.txt`
  - release compile option made config-aware
- `.github/workflows/ci-pr-gate.yml`
  - configure now forces:
    - `AUTOLIFE_BUILD_GATE_TESTS=ON`
    - `AUTOLIFE_BUILD_TOOL_BINARIES=OFF`
    - `AUTOLIFE_BUILD_EXTRA_TESTS=OFF`
- `.github/workflows/ci-strict-live-gate.yml`
  - configure now forces:
    - `AUTOLIFE_BUILD_GATE_TESTS=ON`
    - `AUTOLIFE_BUILD_TOOL_BINARIES=ON`
    - `AUTOLIFE_BUILD_EXTRA_TESTS=OFF`
  - strict gate command now includes `-AllowLiveProbeOrder`

## Remaining Notes
- `legacy_archive/manifest.json` is the only retained archive payload and is intentional.
- `build/`, `.venv/`, `.vscode/` are workspace-local operational folders (not source architecture concerns).
