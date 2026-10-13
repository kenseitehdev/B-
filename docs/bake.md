# Bake and Bakefile (1.0.17)

Bake builds native B++ executables and ABI 2 static libraries. The initial
bootstrap implementation uses Python 3.9 or newer and its standard library.
Run `./bin/bake` on Unix, or `python tools/bake.py` on Windows. The B++ compiler, assembler
and archiver are separate tools. Add the repository's `bin` directory to PATH
to invoke the command as `bake`. Build the compiler first with
`make BUILD_MODE=release`; Bake never inherits Make's sanitizer-enabled default.

## Bakefile syntax

A Bakefile is UTF-8 INI with one `[project]` section and named `[target NAME]`
sections. Names are ASCII identifiers. Values are literal strings, except
list fields and `expect`, which use JSON. No shell expansion or shell recipes
are performed. Unknown fields and duplicate sections/keys are errors.

```ini
[project]
default = app
compiler = bin/release/bpp
build-dir = build/bake

[target numbers]
kind = library
source = examples/bake/numbers.bpp

[target app]
kind = executable
source = examples/bake/main.bpp
deps = ["numbers"]
options = ["--print-result"]

[target smoke]
kind = test
source = examples/bake/main.bpp
deps = ["numbers"]
options = ["--print-result"]
expect = "42\n"
```

Project fields: `default`, `compiler`, `assembler`, `archiver`, `build-dir`,
`library-paths` (JSON array), `options` (JSON array), `target` (default `host`),
and `profile` (default `release`). The default compiler
is `bin/release/bpp` (`.exe` on Windows); the assembler defaults to that
compiler's recorded host driver. The archiver defaults to `llvm-ar` when
available, otherwise `ar`. Windows Unicode paths require an archiver that
supports them; matching UCRT64 CI uses LLVM's archiver. Windows executable
linking uses LLD for the same UTF-8 path guarantee in release and debug output.

Target fields: `kind` (`executable`, `library`, `test`, or `group`), `source`,
`deps`, `inputs`, `options`, `output`, `args`, `expect`, and `expect-exit`.
Each compiled target has one source entry point; textual imports provide
additional source files. A group has only dependencies. `inputs` lists extra
files whose content should invalidate the build. `args` and expectations apply
only to tests. `expect` is optional; without it only the exit status is checked.
The default expected exit is zero. Executable `output` is a filename within its
target directory, not an arbitrary path. Library NAME always produces
`libNAME.bpp.a` and `libNAME.bppabi`. Dependency names order builds; the source's
`import library` directives determine the actual ABI imports and link order.

Supported compiler options are `--no-opt`, `--debug-info`, and
`--print-result`. A debug profile adds ordinary debug metadata while using the
same unsanitized compiler/runtime. There are no sanitizer profiles, commands
or options. Bake queries `bpp --build-info` and rejects a sanitizer-enabled
compiler, including one supplied through `--compiler`.

## Commands

```sh
./bin/bake                         # build the default target and its dependencies
./bin/bake app                     # make-like named target selection
./bin/bake build app smoke
./bin/bake test                    # build and run every test target
./bin/bake run app -- argument
./bin/bake list
./bin/bake clean                   # clean all targets in this target/profile
./bin/bake clean app
./bin/bake -f path/to/Bakefile --profile debug build app
./bin/bake --rebuild build app
./bin/bake --dry-run build app
```

`--target` accepts the compiler's four target IDs. Executables/tests require
a matching native host; a library can be cross-assembled with a suitable Clang
driver. `--compiler`, `--assembler`, and `--archiver` override individual tool
paths. Paths are relative to the selected Bakefile's directory. Arguments are
passed directly as an argv array, preserving spaces and Unicode paths.

Outputs live under `build-dir/TARGET/PROFILE/NAME`. The build directory must
be inside the project, must not be the project root, and must not contain the
Bakefile or any declared source/input. Clean removes only configured target
directories in the selected target/profile. Symlink escapes are rejected.

Bake asks `bpp --emit-dependencies` for the exact expanded sources and resolved
static manifests/archives. It hashes their content, declared inputs, dependency
outputs, compiler/runtime objects, tool identities, target, profile and options.
It also checks artifact hashes before accepting a cached build. A content
change rebuilds the affected target and its consumers; timestamps alone do not.
Manifest generation follows archive creation so identity checks remain valid.
The compiler owns ABI validation and dependant-before-dependency link ordering.

Cache records are replaced atomically after a successful build. Failed input
validation cannot use a cached artifact. An artifact rebuild invalidates its
old cache and can leave partial artifacts on failure; the next build repairs them. Debug snapshots use stable target paths. Dry-run lists the full planned
target order without generating artifacts or claiming cache validity for
libraries that have not yet been built. Commands have 120-second deadlines;
test processes have 60-second deadlines. A second Bake invocation against the
same target/profile is rejected by a lock; stale locks are reported explicitly.

Exit codes: 0 success, 1 build/test failure, 2 invalid configuration/invocation.
This tool does not implement broad FFI, shared libraries, dynamic loading or
the separate NBL/self-hosted operating-system project.

Windows compiler builds embed a UTF-8 process-code-page manifest so the narrow
CRT/file APIs receive Unicode paths consistently on Windows 10 version 1903
and newer. See [Microsoft's UTF-8 process documentation](https://learn.microsoft.com/en-us/windows/apps/design/globalizing/use-utf8-code-page). Native CI uses Windows Server 2022.
