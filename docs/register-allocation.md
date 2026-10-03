# Register inventory and payload allocation

This document describes the native backends' current **selective payload cache**. It is not a general machine-register allocator: IR tags and ownership state remain in canonical memory cells, and only integer-sized payloads can occupy physical registers. The source of target register data is `src/registers.c`; allocation uses `src/regalloc.c`; frame saves and reloads are emitted by the corresponding native emitter.

## Register metadata contract

Each `BPPRegisterInfo` reports an architecture-local ID, assembler name, class mask, width, role flags, save cost, and call-clobber mask. IDs are stable only within an ISA. Floating-point and vector names can be aliases of the same physical register bank (ARM64 `vN`, x86-64 `xmmN`); the class mask records both uses. The allocator asks for the integer candidate pool and currently rejects other classes.

`BPP_REG_CALLER_SAVED` and `BPP_REG_CALLEE_SAVED` describe the selected platform ABI. `BPP_REG_FRAME`, `BPP_REG_SCRATCH`, `BPP_REG_ARGUMENT`, and `BPP_REG_RETURN` describe additional roles and may coexist with a save class. `BPP_REG_ALLOCATABLE` means the register is permitted in the payload pool after the emitter's fixed roles are excluded. `call_clobbers` is a bit set over runtime, B++ user, and external calls; ABI volatile registers are marked clobbered by all three. Callee-saved registers have no call clobber bits. `save_cost` is a relative allocator cost, not a cycle estimate: volatile integer registers cost 0, selected callee-saved integer registers cost 3, and mandatory frame registers are not candidates.

The `--target-info TARGET` output prints every inventory record. `--list-targets` retains a final count column for compatibility; it is the number of integer payload candidates exposed to this allocator, not the architectural register count.

## Target inventories and current allocation pool

| Target | Integer caller-saved candidates | Integer callee-saved candidates | Payload pool order | Floating/vector inventory |
|---|---|---|---|---|
| macOS ARM64 | `x13–x15` | `x20–x25`, `x27–x28` | `x20,x21,x22,x23,x24,x25,x27,x28,x13,x14,x15` | `v0–v7`, `v16–v31` volatile; `v8–v15` callee-saved (low 64 bits under AAPCS64) |
| Linux ARM64 | `x13–x15` | `x20–x25`, `x27–x28` | same ARM64 pool | same AAPCS64 vector rules |
| Linux x86-64 / System V | `r8–r9` | `rbx`, `r13–r15` | `rbx,r13,r14,r15,r8,r9` | `xmm0–xmm15` volatile |
| Windows x86-64 | `r8–r9` | `rbx`, `r13–r15` | `rbx,r13,r14,r15,r8,r9` | `xmm0–xmm5` volatile; `xmm6–xmm15` nonvolatile |

The complete GPR inventories also include argument/return registers, reserved registers, and scratch registers. On ARM64, `x0–x7` are argument registers, `x0` is the return register, `x8` carries an indirect-result address, `x9–x12` and `x16–x17` are emitter scratch/platform temporaries, `x13–x15` are caller-saved payload candidates, `x18` is reserved as the platform register, `x19` is the runtime-frame register, `x26` is emitter scratch, `x29` is the frame pointer, and `x30` is the caller-clobbered link register. `sp`/`xzr` are the architectural `x31` aliases and are not general payload registers. `x20–x22` and `x24` stage initial arguments before the runtime frame is established; after that setup the payload allocator may use them.

On x86-64, `rsp` is the stack pointer, `rbp` anchors the generated function frame, and `r12` is the B++ runtime-frame pointer. `rax`, `r10`, and `r11` are reserved emitter scratch/return registers. System V argument registers are `rdi,rsi,rdx,rcx,r8,r9`; Windows uses `rcx,rdx,r8,r9` and the caller-provided 32-byte shadow area. The target inventory records ABI-specific volatility, including Windows' nonvolatile `rsi/rdi` and `xmm6–xmm15`; these are currently not candidates because the emitter does not need them for the payload cache.

## Allocation and calls

The pool lists callee-saved registers first so a constrained test or future target policy can request the safe prefix. For each live interval, the allocator considers only available candidates and rejects caller-saved registers if the interval crosses a possible helper/user call or crosses a basic-block boundary. A single-block interval may use a caller-saved register when one is free; its zero save cost lets it beat a callee-saved candidate. Every non-terminator IR operation is currently treated as a possible call barrier because its fast path may fall back to a helper, including type/overflow checks and destination cleanup. This is intentionally conservative until per-op fast-path clobber summaries are available.

The native emitters flush dirty values to canonical payload cells before a helper, user call, branch, return, or cleanup boundary. After a helper path, they reload every allocated value live after that instruction: caller-saved registers may have been clobbered, and the helper may have written the instruction's destination cell even when its physical register is callee-saved. Generated B++ procedures preserve callee-saved payloads by the platform ABI. This applies to runtime helpers and generated B++ procedures. The call mask is therefore conservative by call category today, while the actual allocator's crossing test uses the union (`BPP_CALL_ANY`).

The allocator still gives each virtual value a reusable ownership cell. Register assignment is a cache on top of that cell, not a transfer of ownership. Spill victims, immediate literals, and other noncached values use their cell. This guarantees cleanup and error helpers can observe tags, ownership, and payloads even when a fast path keeps the newest primitive payload in a register.

## Tag/payload coherence and allocation classes

Tags and ownership bits remain memory-resident and authoritative. A cached physical register mirrors only the 64-bit payload. When an operation replaces an object value, the emitter performs the old-object destination cleanup before writing the new tag and payload. Dirty primitive payloads are written to the canonical cell before any helper or edge that can inspect or clean the value; helper-produced object payloads are written to the cell by the runtime helper. Reloads never infer a tag from a payload register.

`BPPRegPlan.alloc_class` distinguishes dead values, canonical tagged pairs, integer immediates, null immediates, and a reserved rematerializable-expression class. A dynamic tagged value is treated as one ownership unit: its tag and ownership state stay canonical in the value cell while its payload may be cached. The allocator never allocates a literal integer or null to a payload register.

Immediate-class literals still receive canonical cells and their payload is written there. That is deliberate: helper fallbacks, error cleanup, branches, and return cleanup can all inspect the canonical value frame. This is classification and register avoidance, not yet stack-home elimination or direct immediate operand selection. Those optimizations require explicit location kinds and spill/reload placement (roadmap section 1.3).

The rematerializable-expression enum is reserved but is not assigned to current IR operations. Arithmetic can overflow or divide by zero, and several other operations can call helpers or allocate objects; recomputing them at a use could change whether, when, or how an error occurs. A future pass may mark an expression rematerializable only after proving it pure, deterministic, and safe to repeat, with source/error behavior preserved.

## Per-function saves and stack invariants

Each function saves mandatory frame/runtime registers plus only the callee-saved payload registers present in its allocation plan. `bpp_reg_saved_set` derives this list from physical assignments; allocation tests compare the result with actual assignments for zero, exact-capacity, and overflowing pressure. ARM64 stores the save set below the `x29/x30` frame record and rounds the save area to 16 bytes. It restores the exact set before restoring `x29/x30`. Caller-saved payload registers require no prologue save.

x86-64 pushes `rbp` and `r12` for every generated procedure, followed by the selected callee-saved payload registers, and pops them in reverse order. The outgoing frame size is a multiple of 16 bytes plus 72 or 80 bytes depending on save-count parity. Since function entry has `rsp mod 16 = 8`, this makes `rsp` 16-byte aligned before every helper or generated-procedure call. The base area holds the 64-byte runtime header, Windows shadow space where required, and the incoming fifth-argument stash. The SysV fifth argument is stashed just below the pushed save set; the Windows fifth argument is addressed above the dynamic save and allocation areas. Large frames retain the existing stack-probe path.

This design preserves scratch ownership: allocated registers never overlap the documented scratch set, and x86-64 register names come from the target inventory rather than a second allocator pool table. The ARM64 emitter uses architecture register IDs directly after allocation. Tests exercise values across runtime and user calls, recursive calls, checked operations with error/helper paths, joins, and loop backedges. `check-targets` cross-assembles the generated code for all four targets; `check-msabi` executes the Windows x64 calling convention on Linux x86-64; the ordinary native suite executes on the current host. Cross-assembly checks syntax and frame directives, not OS-level preservation behavior. A native execution run is still required on macOS ARM64 and Linux ARM64 hosts before claiming runtime validation on those platforms.

## Current limits

- Float/vector registers are inventoried but not allocated.
- Integer and null literals are explicitly classified as immediates but still retain canonical stack cells; immediate operands and stack-home elimination are not implemented.
- The rematerializable-expression class is reserved; no current IR operation is marked rematerializable because repeating checked/side-effecting operations could change failure behavior.
- The caller-saved choice is a conservative interval heuristic; it does not split a range around a call or compare dynamic save/restore costs.
- Live-range intervals are conservative across CFG joins and loop backedges. They are not a full interference graph or a spill-placement solution.
- Tags, object ownership, and cleanup remain memory-based; every IR value still has a reusable canonical cell.
- Platform assembly and ABI execution tests are required for each host/toolchain. Linux x86-64 and Windows x64 ABI execution are covered where their test environments are available; macOS ARM64 and Linux ARM64 still need native-host execution runs. Cross-assembly checks validate syntax and unwind directives but do not replace running on each target OS.
