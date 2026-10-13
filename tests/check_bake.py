#!/usr/bin/env python3
"""Native Bake integration and failure tests, independent of its cache code."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent
DRIVER = ROOT / "tools" / "bake.py"
COMPILER = Path(sys.argv[1]).resolve()
SANITIZED = Path(sys.argv[2]).resolve() if len(sys.argv) > 2 else None


def run(argv, cwd, code=0):
    result = subprocess.run([str(x) for x in argv], cwd=cwd, capture_output=True,
                            text=True, timeout=120)
    assert result.returncode == code, (argv, result.returncode, result.stdout, result.stderr)
    return result.stdout + result.stderr


def bake(root, *words, code=0):
    return run([sys.executable, DRIVER, *words], root, code)


with tempfile.TemporaryDirectory(prefix="bpp-bake-space-") as temporary:
    root = Path(temporary) / "project Ω with spaces"
    root.mkdir()
    (root / "helper.bpp").write_text("define proc increment(n){return n+1;}\n", encoding="utf-8")
    (root / "numbers.bpp").write_text('import "helper.bpp";\n', encoding="utf-8")
    (root / "bridge.bpp").write_text('import library "numbers";\ndefine proc twice(n){return increment(increment(n));}\n')
    (root / "main.bpp").write_text('import library "bridge";\ndefine proc main(){return twice(40);}\n')
    configuration = f'''[project]
default = app
compiler = {COMPILER.as_posix()}
build-dir = outputs

[target numbers]
kind = library
source = numbers.bpp

[target bridge]
kind = library
source = bridge.bpp
deps = ["numbers"]

[target app]
source = main.bpp
deps = ["bridge"]
options = ["--print-result"]

[target smoke]
kind = test
source = main.bpp
deps = ["bridge"]
options = ["--print-result"]
expect = "42\\n"

[target all]
kind = group
deps = ["app", "smoke"]
'''
    config = root / "Bakefile"
    config.write_text(configuration, encoding="utf-8")
    info = json.loads(run([COMPILER, "--build-info"], root))
    assert info["schema"] == 1 and info["sanitizers"] == "" and len(info["runtime_objects"]) == 5
    target = info["host_target"]
    base = root / "outputs" / target / "release"
    suffix = ".exe" if target == "windows-x86_64" else ""
    app = base / "app" / ("app" + suffix)
    output = bake(root)
    assert output.count("building: ") == 3
    assert run([app], root) == "42\n"
    times = {p: p.stat().st_mtime_ns for p in base.rglob(".bake-state.json")}
    output = bake(root, "app")
    assert output.count("up to date: ") == 3 and "building:" not in output
    assert times == {p: p.stat().st_mtime_ns for p in times}
    state = json.loads((base / "numbers" / ".bake-state.json").read_text())
    assert str((root / "helper.bpp").resolve()) in state["signature"]["inputs"]
    assert len(state["signature"]["tools"]["runtime"]) == 5
    assert "test passed: smoke" in bake(root, "test")
    assert "42\n" in bake(root, "run", "app", "--", "space argument")
    assert "all\tgroup" in bake(root, "list")
    assert not (root / "outputs" / target / "debug").exists()
    output = bake(root, "--profile", "debug", "--dry-run", "build", "all")
    assert output.count("would build") == 4
    assert not (root / "outputs" / target / "debug").exists()

    # Content must invalidate the graph even when mtime is preserved.
    helper = root / "helper.bpp"
    previous_time = helper.stat().st_mtime_ns
    helper.write_text("define proc increment(n){return n+2;}\n")
    os.utime(helper, ns=(previous_time, previous_time))
    assert bake(root, "build", "app").count("building: ") == 3
    assert run([app], root) == "44\n"
    changed = configuration.replace('options = ["--print-result"]', 'options = ["--print-result", "--no-opt"]')
    changed = changed.replace('expect = "42\\n"', 'expect = "44\\n"')
    config.write_text(changed, encoding="utf-8")
    output = bake(root, "build", "app")
    assert "building: app" in output and "building: numbers" not in output
    assert "test passed: smoke" in bake(root, "test")
    assert "test passed: smoke" in bake(root, "--profile", "debug", "test")
    debug = root / "outputs" / target / "debug"
    assert (debug / "smoke" / ("smoke" + suffix + ".bpp-source")).exists()
    assert (debug / "numbers" / "unit.s.bpp-source").exists()
    assert not (debug / ".bake-lock").exists()

    # Tampered generated archives must be repaired before consumers are probed.
    archive = base / "numbers" / "libnumbers.bpp.a"
    with archive.open("ab") as stream:
        stream.write(b"tampered")
    assert "building: numbers" in bake(root, "app")
    assert run([app], root) == "44\n"
    # The compiler, rather than Bake, owns ABI validation.
    manifest = base / "numbers" / "libnumbers.bppabi"
    original = manifest.read_text()
    manifest.write_text(original.replace("abi 2.0", "abi 1.0"))
    # External archives have no Bake producer to repair their manifest.
    external = root / "external"
    external.mkdir()
    shutil.copy2(archive, external / archive.name)
    shutil.copy2(manifest, external / manifest.name)
    (root / "external.bpp").write_text('import library "numbers";\nreturn increment(40);\n')
    bad_abi = configuration.split("[target numbers]")[0] + '''library-paths = ["external"]
[target app]
source = external.bpp
'''
    config.write_text(bad_abi)
    assert "ABI 2.0, runtime 3" in bake(root, code=1)
    manifest.write_text(original)
    config.write_text(changed)
    assert "up to date: app" in bake(root, "app")
    config.write_text(changed.replace('expect = "44\\n"', 'expect = "999\\n"'))
    assert "test failed" in bake(root, "test", code=1)
    config.write_text(changed)

    invalid = [
        ("profile = tsan\n", "profile must"),
        ("options = [\"--native-sanitizers\"]\n", "unsupported compiler options"),
        ("sanitizers = address\n", "unknown project fields"),
        ("build-dir = .\n", "dedicated directory"),
    ]
    for field, diagnostic in invalid:
        minimal = f"[project]\n{field}[target app]\nsource = main.bpp\n"
        config.write_text(minimal)
        assert diagnostic in bake(root, "list", code=2)
    config.write_text('[project]\n[target a]\nkind = group\ndeps = ["b"]\n[target b]\nkind = group\ndeps = ["a"]\n')
    assert "dependency cycle" in bake(root, "list", code=2)
    config.write_text('[project]\n[target a]\nkind = group\ndeps = ["missing"]\n')
    assert "unknown target" in bake(root, "list", code=2)
    config.write_text(changed.replace('source = main.bpp', 'source = outputs/main.bpp'))
    assert "build-dir contains input" in bake(root, "clean", code=2)
    config.write_text(changed.replace('source = main.bpp', 'source = main.bpp\noutput = ../main.bpp'))
    assert "ordinary filename" in bake(root, "clean", code=2)
    config.write_text(changed)
    if SANITIZED:
        assert "unsanitized compiler/runtime" in bake(root, "--compiler", str(SANITIZED), code=2)

    # A held lock must reject concurrent commands without touching artifacts.
    lock = base / ".bake-lock"
    lock.mkdir()
    assert "stale lock" in bake(root, "build", "app", code=2)
    lock.rmdir()
    # Clean is scoped to the chosen profile and target, preserving sources.
    assert "cleaning:" in bake(root, "clean", "app")
    assert not app.exists() and archive.exists() and (root / "main.bpp").exists()
    assert (debug / "smoke").exists()
    alternative = root / "Alternate Bakefile"
    alternative.write_text(changed)
    assert "building: app" in bake(root, "-f", str(alternative), "build", "app")
    # A failed input probe must report the error instead of using a cached artifact.
    (root / "main.bpp").write_text("define proc main(){return ;broken}\n")
    assert "command failed" in bake(root, "app", code=1)
    print("Bake native integration passed: transitive ABI archives, import/content/option invalidation, debug profiles, Unicode paths, scoped clean, errors, locks and sanitizer exclusions")
