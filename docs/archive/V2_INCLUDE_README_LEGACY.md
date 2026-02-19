# v2 Include Skeleton

This directory is reserved for Stage 15 rewrite interfaces and models.

Guidelines:
- Place new contracts/types under `include/v2/*`.
- Avoid changing existing public CLI behavior during migration.
- Keep compatibility with existing report JSON keys until final cutover.

Current bootstrap components:
- `model/KernelTypes.h`
- `contracts/IPolicyPlane.h`
- `contracts/IRiskPlane.h`
- `contracts/IExecutionPlane.h`
- `contracts/ILearningStateStore.h`
- `orchestration/DecisionKernel.h`
- `adapters/Legacy*Adapter.h`
