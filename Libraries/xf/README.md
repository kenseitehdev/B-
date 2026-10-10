# XF static library for B++

This package exposes the supplied XF runtime through the B++ static-library
ABI v1.  It deliberately builds XF's core without the old embedded
LambdaScript and ByteLang bridges: those bridges target the former ByteLang
runtime and must not be linked into a modern B++ process.

Exported B++ procedures:

- `xf_version()` → owned B++ string.
- `xf_run_string(source)` → integer `0`; execution failures become normal
  B++ runtime diagnostics.
- `xf_run_file(path)` → integer `0`; execution failures become normal B++
  runtime diagnostics.

Build beside a B++ checkout:

```sh
make -C Libraries/xf BPP_ROOT=../..
./bin/release/bpp -L Libraries/xf --print-result -o xf-demo demo.bpp
```

`demo.bpp` uses `import library "xf";`.  The generated manifest pins the
archive's FNV-1a identity and asks Linux consumers for only `m` and `pthread`.
macOS needs no additional system-library argument.  This is a curated bridge,
not a general C FFI.
