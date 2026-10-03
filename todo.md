0. CG
    1. Broader register allocation. Four registers currently hold payloads. Tags stay in memory, and other registers have fixed roles.
    2. Actual spill management. Every IR value still has a stack cell. Add live-range splitting, spill/reload placement, and selective spilling.
    3. Smaller stack frames. Reuse cells whose lifetimes don’t overlap and avoid allocating cells for values that can remain entirely in registers.
    4. Better allocation decisions. Consider loop frequency, call boundaries, and spill cost. Current allocation uses a simple greedy heuristic.
    5. Scalable liveness/allocation. The interference graph has quadratic worst-case memory use.
    6. Static value information. Track known integer/null/object values to eliminate unnecessary tag checks and fallback branches.
    7. More redundant-access elimination. Remove repeated tag loads, slot loads/stores, initialization checks, and conservative payload flushes.
    8. Constant folding and propagation. Evaluate known expressions during compilation while preserving overflow and error behavior.
    9. Dead-code elimination. Remove unused computations, unreachable blocks, and unnecessary cleanup for proven primitive values.
    10. Better instruction selection. Use immediate operands, combined addressing, and other ARM64 instruction forms where appropriate.
    11. Control-flow optimization. Improve block layout, use fall-through paths, simplify branches, and remove redundant jumps.
    12. Large-program branch handling. Handle branches and calls that exceed instruction displacement limits, coordinated with the future assembler/linker.
    13. Formal ABI call lowering. Once stage 2 defines the ABI, implement its argument placement, returns, stack rules, external calls, and symbol conventions.
    14. Further runtime independence. Decide which remaining C helpers to replace with native lowering or a B++ runtime. Allocations and object management can still remain runtime operations.
    15. Debug metadata emission. Emit source locations and variable/register locations for the future debugger.
    16. Native sanitizer instrumentation. Add checks to emitted instructions. Host sanitizer flags currently instrument the C components, not hand-emitted assembly.
    17. Broader verification. Add differential tests, randomized programs, pressure tests, large frames, deep recursion, and automated native Mac testing.
    18. Performance benchmarks. Measure actual execution time, code size, stack use, and compile time—not just instruction-model store counts.
    19. Additional targets, if wanted. The current backend targets Apple Silicon macOS only.
1. assembler
2. linker
3. buildFile
4. xf static lib for bl
5. debugger
6. sanitizers
