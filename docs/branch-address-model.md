# Native branch and address model

This document defines the revision-1 code model used before B++ owns an
assembler and linker. It applies only to compiler-generated internal labels;
the private C runtime boundary still relies on the host toolchain's normal
platform stubs and relocations.

## Range inventory

| ISA / format | Form | Relocation | Signed byte range | Current use |
|---|---|---|---:|---|
| ARM64 Mach-O | `ADRP` + `ADD @PAGEOFF` | `ARM64_RELOC_PAGE21` / `ARM64_RELOC_PAGEOFF12` | -4 GiB to +4 GiB - 4 KiB | Internal jumps, calls, data and layout labels |
| ARM64 ELF | `ADRP` + `ADD :lo12:` | `R_AARCH64_ADR_PREL_PG_HI21` / `R_AARCH64_ADD_ABS_LO12_NC` | -4 GiB to +4 GiB - 4 KiB | Same internal references |
| ARM64 Mach-O / ELF | literal `LDR` | `ARM64_RELOC_BRANCH19` / `R_AARCH64_LD_PREL_LO19` | -1 MiB to +1 MiB - 4 | Not emitted by the current backend; recorded for future literal pools |
| ARM64 Mach-O / ELF | `B` / `BL` | `ARM64_RELOC_BR26` / `R_AARCH64_JUMP26`, `R_AARCH64_CALL26` | -128 MiB to +128 MiB - 4 | Host-runtime calls may use toolchain stubs; B++ internal transfers do not |
| ARM64 Mach-O / ELF | conditional branch | `ARM64_RELOC_BRANCH19` / `R_AARCH64_CONDBR19` | -1 MiB to +1 MiB - 4 | Only skip-over-long-transfer branches |
| x86-64 ELF | PC-relative branch/call/data | `R_X86_64_PC32` / `R_X86_64_PLT32` | -2 GiB to +2 GiB - 1 | Runtime/host toolchain boundary and local emitter labels |
| x86-64 COFF | PC-relative branch/call/data | `IMAGE_REL_AMD64_REL32` | -2 GiB to +2 GiB - 1 | Same role on Windows |
| x86-64 ELF / COFF | absolute internal target | `R_X86_64_64` / `IMAGE_REL_AMD64_ADDR64` | full 64-bit address | B++ internal calls and cross-block jumps |

`--target-info TARGET` prints the registered relocation/range table used for
that target, so scripts do not need a copied target matrix.

## Current lowering policy

ARM64 uses the **small ADRP-page code model** for internal B++ labels:

```asm
adrp x16, target@PAGE       // Mach-O spelling; ELF uses target
add  x16, x16, target@PAGEOFF
br   x16                    // or blr x16 for a procedure call
```

The conditional instruction branches only around this three-instruction
transfer, so it stays local even when the destination is distant. The
post-layout guard rejects an estimated source/target separation beyond the
ADRP page range with the source location, source label, target label, measured
distance, relocation name, and permitted range. There are no ARM64 islands yet:
the supported model is one generated text/data image whose internal labels are
within the ADRP range. A future B++ linker will own section placement, veneer
islands, literal pools, and final relocation checks.

x86-64 uses the **large internal-label model** for B++ procedures and CFG
edges:

```asm
movabsq $target, %r11
jmp *%r11                   # call *%r11 for a procedure call
```

A conditional cross-block transfer is inverted to a short `jcc` over that
sequence. Thus no B++ internal edge depends on `rel32`; only nearby local
emitter labels and the host runtime/toolchain boundary do. On ELF, generated
native x86-64 executables deliberately link with `-no-pie`, because internal
`R_X86_64_64` references are part of this pre-linker static code model.

## Validation and tests

`bpp_target_check_module_ranges` runs after optimization and before native
assembly emission. It models generated block/procedure/data labels using a
conservative per-IR-instruction size, checks ARM64 `ADRP` references, and
reports a source-positioned error before invoking the host assembler. The
public single-site checker is also used by `test_ranges` to exercise exact
positive limits and one-byte-over-limit layouts for ARM64 page relocations and
x86-64 `rel32`, without a multi-gigabyte fixture.

`tests/run_tests.sh` emits all four target assemblies and checks that ARM64
retains page-relative transfers while both x86-64 targets emit the large-model
marker and short skip labels. `make check-targets` independently assembles the
Mach-O ARM64, ELF ARM64, ELF x86-64, and COFF x86-64 outputs when a Clang
cross-assembler is available. Final linked-image range validation remains a
future assembler/linker responsibility.
