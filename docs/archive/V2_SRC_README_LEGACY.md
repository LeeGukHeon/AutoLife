# v2 Source Skeleton

This directory is reserved for Stage 15 rewrite implementation.

Guidelines:
- New runtime logic should be implemented under `src/v2/*`.
- Keep v1 code paths intact until shadow parity and gate checks pass.
- Remove v1 modules only in approved Wave B delete batches.

Current bootstrap components:
- `orchestration/DecisionKernel.cpp`
- `adapters/LegacyPolicyPlaneAdapter.cpp`
- `adapters/LegacyRiskPlaneAdapter.cpp`
- `adapters/LegacyExecutionPlaneAdapter.cpp`
