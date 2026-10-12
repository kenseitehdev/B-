# Codegen completion gates

Target scope: macOS ARM64, Linux x86-64, Linux ARM64, Windows x86-64.
Do not call codegen finished or move to the public ABI because cross-assembly
passes. The following separates implemented foundations from unfinished work.

| Earlier gap | This snapshot | Remaining gate/work |
|---|---|---|
| Broader allocation | 8 ARM / 4 x64 callee-saved caches | Caller-saved allocation and per-function save sets |
| Spill/reload | Overflow intervals use mapped cells; caches flush/reload | Split intervals, real register-only values and rematerialization |
| Smaller frames | Disjoint values reuse ownership cells | Local-slot reuse and elimination of unnecessary homes |
| Loop/call cost | Callee-saved caches and backedge weights | Promotion across loops and stronger call-clobber analysis |
| Allocator scaling | Dense quadratic interference graph removed | Live-range holes and larger stress limits |
| Static value kinds | Flow-sensitive value-kind/constant/truth lattice; join-proven native check elision | Language typing and deliberately-conservative interprocedural summaries later |
| Redundant accesses | Deferred payload writes, fewer flag writes | Broader redundant load/store removal |
| Constant folding | Checked folding and block-local slot propagation | Propagation across CFG edges and dead expression chains |
| Dead code | Constant branches, unreachable blocks/private procedures removed | Broader effect-aware dead instruction elimination |
| Instruction selection | Native integers and immediate arithmetic | Further measured target-specific combines |
| CFG layout | Fallthrough jumps omitted | Block layout based on hot/cold edges |
| Long branches | ARM64 internal address transfers | Range diagnostics or alternative code models for extreme sections |
| Call lowering | Host conventions implemented for all four targets | Stable public ABI is roadmap stage 2 |
| C runtime dependency | Native arithmetic/control/calls bypass generated C | Native/B++ object runtime and startup remain needed |
| Debug metadata | DWARF5/CodeView source variables/types/scopes, original imports, macOS LLDB and unwind checks | Native Linux GDB and Windows LLDB/PDB CI transcripts |
| Sanitizers | Portable native checks, guarded frames, allocation/definedness/leak tracking and native TSAN tracing; C output retired | Matching Linux/MSAN/Windows native verification |
| Verification | Unit/stress/differential/Cross-object checks | Actual updated macOS, Linux ARM64 and Windows native CI |
| Benchmarks | Portable CSV harness for loops/calls/objects | Repeatable measurements on all four actual hosts |
| More targets | Four emitters and native-host CI workflow | Three foreign-host gates must run before support is called verified |

The ABI, custom assembler/linker, buildFile, library format, debugger and complete
sanitizer implementations remain separate roadmap stages. This private boundary
can change while the codegen gates above are being completed.

## Next verification run on Apple Silicon

```sh
make BUILD_MODE=release test check-differential check-targets check-stress
make BUILD_MODE=debug test check-asan-hook
make BUILD_MODE=tsan test
make -s BUILD_MODE=release benchmark > macos-arm64.csv
```

The workflow contains matching native Linux ARM64 and Windows x64 jobs as well.
It must be placed at repository-root .github/workflows/codegen.yml when importing
this snapshot. It has not been submitted or executed remotely in this session.
Windows currently verifies release first; sanitizer runtime availability must be
established before adding those native Windows gates.
