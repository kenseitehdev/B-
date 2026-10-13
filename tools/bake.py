#!/usr/bin/env python3
"""Portable bootstrap build driver for native B++ and ABI 2 archives."""
import argparse
import configparser
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shlex
import shutil
import signal
import subprocess
import sys
import tempfile

TARGETS = {"macos-arm64": "arm64-apple-macosx13.0.0",
           "linux-x86_64": "x86_64-unknown-linux-gnu",
           "linux-arm64": "aarch64-unknown-linux-gnu",
           "windows-x86_64": "x86_64-w64-windows-gnu"}
OPTIONS = {"--no-opt", "--debug-info", "--print-result"}
PROJECT_KEYS = {"default", "compiler", "assembler", "archiver", "build-dir",
                "library-paths", "options", "target", "profile"}
TARGET_KEYS = {"kind", "source", "deps", "inputs", "options", "output",
               "args", "expect", "expect-exit"}


class BakeError(Exception):
    def __init__(self, message, code=2):
        super().__init__(message)
        self.code = code


def require(condition, message):
    if not condition:
        raise BakeError(message)


def array(section, key):
    try:
        value = json.loads(section.get(key, "[]"))
    except ValueError as error:
        raise BakeError(f"{key}: expected a JSON array: {error}") from error
    require(isinstance(value, list) and all(isinstance(x, str) and x for x in value),
            f"{key}: expected an array of nonempty strings")
    return value


def host_target():
    machine = platform.machine().lower()
    if sys.platform == "darwin" and machine in ("arm64", "aarch64"):
        return "macos-arm64"
    if sys.platform.startswith("linux"):
        if machine in ("x86_64", "amd64"):
            return "linux-x86_64"
        if machine in ("aarch64", "arm64"):
            return "linux-arm64"
    if os.name == "nt" and machine in ("x86_64", "amd64"):
        return "windows-x86_64"
    raise BakeError(f"unsupported native host: {sys.platform}/{machine}")


def digest(path):
    path = Path(path)
    if path.is_dir():
        entries = [(str(p.relative_to(path)), digest(p)) for p in sorted(path.rglob("*"))
                   if p.is_file() and not p.name.startswith("._")]
        return hashlib.sha256(json.dumps(entries).encode()).hexdigest()
    try:
        with path.open("rb") as stream:
            result = hashlib.sha256()
            for block in iter(lambda: stream.read(1 << 20), b""):
                result.update(block)
            return result.hexdigest()
    except OSError as error:
        raise BakeError(f"cannot read build input {path}: {error}") from error


def under(path, parent):
    return path == parent or parent in path.parents


def remove_tree(path):
    def onerror(function, filename, error):
        # AppleDouble sidecars can disappear when their owning file is removed.
        if not isinstance(error[1], FileNotFoundError):
            raise error[1]
    shutil.rmtree(path, onerror=onerror)


class Project:
    def __init__(self, filename, args):
        self.file = Path(filename).resolve()
        self.root = self.file.parent
        config = configparser.ConfigParser(interpolation=None, delimiters=("=",),
                                           strict=True, empty_lines_in_values=False)
        try:
            with self.file.open(encoding="utf-8") as stream:
                config.read_file(stream)
        except (OSError, configparser.Error) as error:
            raise BakeError(f"{self.file}: {error}") from error
        require(not config.defaults(), "DEFAULT fields are not supported; use [project]")
        require("project" in config, "Bakefile requires [project]")
        self.settings = dict(config["project"])
        require(not (self.settings.keys() - PROJECT_KEYS), "unknown project fields: " +
                ", ".join(sorted(self.settings.keys() - PROJECT_KEYS)))
        self.host = host_target()
        self.target = args.target or self.settings.get("target", "host")
        if self.target == "host":
            self.target = self.host
        require(self.target in TARGETS, f"unsupported target: {self.target}")
        self.profile = args.profile or self.settings.get("profile", "release")
        require(self.profile in ("release", "debug"), "profile must be release or debug")
        self.build = (self.root / self.settings.get("build-dir", "build/bake")).resolve()
        require(under(self.build, self.root) and self.build != self.root,
                "build-dir must be a dedicated directory inside the project")
        require(not under(self.file, self.build), "build-dir contains the Bakefile")
        self.base = (self.build / self.target / self.profile).resolve()
        require(under(self.base, self.build), "target/profile directory escapes build-dir")
        self.targets = {}
        for section in config.sections():
            if section == "project":
                continue
            require(section.startswith("target "), f"unknown section: {section}")
            name = section[7:]
            require(re.fullmatch(r"[A-Za-z_][A-Za-z_0-9]*", name),
                    f"target must be an ASCII identifier: {name}")
            target = dict(config[section])
            original = set(target)
            require(not (original - TARGET_KEYS), f"{name}: unknown fields: " +
                    ", ".join(sorted(original - TARGET_KEYS)))
            kind = target.setdefault("kind", "executable")
            require(kind in ("executable", "library", "test", "group"),
                    f"{name}: unsupported kind {kind}")
            for field in ("deps", "inputs", "options", "args"):
                target[field] = array(target, field)
            self.validate_options(target["options"])
            if kind == "group":
                require(original <= {"kind", "deps"}, f"{name}: a group only has kind and deps")
            else:
                require(target.get("source"), f"{name}: source is required")
                target["source"] = self.input(target["source"])
                target["inputs"] = [self.input(p) for p in target["inputs"]]
                for p in [target["source"], *target["inputs"]]:
                    require(not under(p, self.build), f"build-dir contains input: {p}")
                output = target.get("output", name + (".exe" if self.target == "windows-x86_64" else ""))
                require(output not in ("", ".", "..", ".bake-state.json", "unit.s", "unit.o")
                        and Path(output).name == output and "/" not in output and "\\" not in output,
                        f"{name}: output must be an ordinary filename")
                target["output"] = output
                require(kind != "library" or "output" not in original,
                        f"{name}: libraries use libNAME.bpp.a")
            require(kind == "test" or not ({"args", "expect", "expect-exit"} & original),
                    f"{name}: test fields require kind=test")
            try:
                target["expect-exit"] = int(target.get("expect-exit", "0"))
                if "expect" in target:
                    target["expect"] = json.loads(target["expect"])
                    require(isinstance(target["expect"], str), f"{name}: expect must be a JSON string")
            except ValueError as error:
                raise BakeError(f"{name}: invalid test expectation: {error}") from error
            self.targets[name] = target
        require(self.targets, "Bakefile has no targets")
        self.default = self.settings.get("default", next(iter(self.targets)))
        self.order(list(self.targets))
        require(self.default in self.targets, f"unknown default target: {self.default}")
        self.options = array(self.settings, "options")
        self.validate_options(self.options)
        self.library_paths = [self.input(p) for p in array(self.settings, "library-paths")]
        self.args = args
        self.tools = None

    def input(self, path):
        return (self.root / path).resolve()

    @staticmethod
    def validate_options(options):
        require(set(options) <= OPTIONS, "unsupported compiler options: " +
                ", ".join(sorted(set(options) - OPTIONS)))

    def directory(self, name):
        raw = self.base / name
        require(not raw.is_symlink(), f"target directory is a symlink: {raw}")
        path = raw.resolve()
        require(under(path, self.build) and path != self.build,
                f"target directory escapes build-dir: {path}")
        return path

    def order(self, names):
        visiting, done, result = [], set(), []
        def visit(name):
            require(name in self.targets, f"unknown target: {name}")
            require(name not in visiting, "dependency cycle: " + " -> ".join([*visiting, name]))
            if name in done:
                return
            visiting.append(name)
            for dep in self.targets[name]["deps"]:
                visit(dep)
            visiting.pop()
            done.add(name)
            result.append(name)
        for name in names:
            visit(name)
        return result

    def tool(self, value):
        candidate = self.input(value)
        if os.name == "nt" and not candidate.is_file() and Path(str(candidate) + ".exe").is_file():
            candidate = Path(str(candidate) + ".exe")
        found = str(candidate) if candidate.is_file() else shutil.which(value)
        require(found, f"tool not found: {value}")
        return str(Path(found).resolve())

    def command(self, argv, capture=False, timeout=120, expected=0):
        if not capture:
            print("+ " + (subprocess.list2cmdline(argv) if os.name == "nt" else shlex.join(argv)), flush=True)
        environment = dict(os.environ)
        environment.pop("BPP_ISEL_FORCE_GENERIC", None)
        if self.tools:
            environment["BPP_CC"] = self.tools["assembler"]
        kwargs = {"start_new_session": True} if os.name == "posix" else {
            "creationflags": subprocess.CREATE_NEW_PROCESS_GROUP}
        child = subprocess.Popen(argv, cwd=self.root, env=environment,
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE, **kwargs)
        try:
            stdout, stderr = child.communicate(timeout=timeout)
        except subprocess.TimeoutExpired:
            if os.name == "posix":
                os.killpg(child.pid, signal.SIGKILL)
            else:
                subprocess.run(["taskkill", "/PID", str(child.pid), "/T", "/F"],
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=10)
            child.kill()
            child.communicate()
            raise BakeError(f"command deadline exceeded ({timeout}s): {argv[0]}", 1)
        if expected is not None and child.returncode != expected:
            raise BakeError(f"command failed ({child.returncode}, expected {expected}): {argv[0]}\n" +
                            stderr.decode("utf-8", errors="replace") +
                            stdout.decode("utf-8", errors="replace"), 1)
        if not capture:
            sys.stdout.write(stdout.decode("utf-8", errors="replace"))
            sys.stderr.write(stderr.decode("utf-8", errors="replace"))
        return stdout if expected is not None else stdout + stderr

    def prepare_tools(self):
        if self.tools:
            return
        compiler = self.tool(self.args.compiler or self.settings.get("compiler",
                    "bin/release/bpp.exe" if os.name == "nt" else "bin/release/bpp"))
        try:
            info = json.loads(self.command([compiler, "--build-info"], capture=True))
        except ValueError as error:
            raise BakeError("compiler must support bpp --build-info (1.0.17 or newer)") from error
        require(info.get("schema") == 1 and info.get("host_supported") is True,
                "compiler has an unsupported native host")
        require(info.get("sanitizers") == "", "Bake requires an unsanitized compiler/runtime; build B++ with BUILD_MODE=release")
        require(info.get("host_target") == self.host, "compiler does not match the native host")
        assembler = self.tool(self.args.assembler or self.settings.get("assembler", info["host_cc"]))
        archiver = self.tool(self.args.archiver or self.settings.get("archiver", "ar"))
        self.tools = {"compiler": compiler, "assembler": assembler, "archiver": archiver}
        self.tool_identity = {k: {"path": p, "sha256": digest(p)} for k, p in self.tools.items()}
        for key in ("assembler", "archiver"):
            self.tool_identity[key]["version"] = self.command([self.tools[key], "--version"], capture=True, expected=None).decode(errors="replace")
        self.tool_identity["runtime"] = {str(Path(p).resolve()): digest(p) for p in info["runtime_objects"]}

    def flags(self, name):
        target = self.targets[name]
        flags = ["--target", self.target, *self.options, *target["options"]]
        if self.profile == "debug" and "--debug-info" not in flags:
            flags.append("--debug-info")
        if target["kind"] == "library":
            flags += ["--emit-library", "--library-name", name]
        paths = list(self.library_paths)
        for dep in reversed(self.order(target["deps"])):
            if self.targets[dep]["kind"] == "library":
                paths.append(self.directory(dep))
        for p in dict.fromkeys(paths):
            flags += ["-L", p.as_posix()]
        return flags

    def artifacts(self, name):
        directory, target = self.directory(name), self.targets[name]
        if target["kind"] == "library":
            outputs = [directory / f"lib{name}.bpp.a", directory / f"lib{name}.bppabi", directory / "unit.s", directory / "unit.o"]
        elif target["kind"] == "group":
            return []
        else:
            outputs = [directory / target["output"]]
        if "--debug-info" in self.flags(name):
            base = directory / ("unit.s" if target["kind"] == "library" else target["output"])
            outputs.append(Path(str(base) + ".bpp-source"))
            if target["kind"] != "library":
                if self.target == "macos-arm64":
                    outputs.append(Path(str(base) + ".dSYM"))
                if self.target == "windows-x86_64":
                    outputs.append(Path(str(base) + ".pdb"))
        return outputs

    def build_target(self, name):
        target = self.targets[name]
        if target["kind"] == "group":
            return
        require(target["kind"] == "library" or self.target == self.host,
                "executables and tests require a matching native target")
        directory, flags, compiler = self.directory(name), self.flags(name), self.tools["compiler"]
        if self.args.dry_run:
            print(f"would build {name} ({target['kind']}) -> {directory}")
            return
        dependencies = json.loads(self.command([compiler, *flags, "--emit-dependencies",
                                                target["source"].as_posix()], capture=True))
        require(dependencies.get("schema") == 1, "unsupported compiler dependency schema")
        inputs = list(target["inputs"])
        for kind in ("sources", "manifests", "archives"):
            inputs += [Path(p).resolve() for p in dependencies[kind]]
        for dep in self.order(target["deps"]):
            inputs += self.artifacts(dep)
        signature = {"schema": 1, "bake": digest(Path(__file__).resolve()), "target": self.target,
                     "profile": self.profile, "settings": {**target, "source": str(target["source"]),
                     "inputs": [str(p) for p in target["inputs"]]}, "flags": flags,
                     "tools": self.tool_identity, "inputs": {str(p): digest(p) for p in sorted(set(inputs))}}
        outputs = self.artifacts(name)
        require(all(not p.is_symlink() for p in outputs), "artifact output is a symlink")
        state_path = directory / ".bake-state.json"
        try:
            state = json.loads(state_path.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            state = {}
        if not self.args.rebuild and state.get("signature") == signature and all(p.exists() for p in outputs):
            if state.get("outputs") == {str(p): digest(p) for p in outputs}:
                print(f"up to date: {name}")
                return
        directory.mkdir(parents=True, exist_ok=True)
        state_path.unlink(missing_ok=True)
        print(f"building: {name}", flush=True)
        if target["kind"] == "library":
            assembly, obj = directory / "unit.s", directory / "unit.o"
            archive, manifest = outputs[:2]
            self.command([compiler, *flags, "--emit-asm", "-o", assembly.as_posix(), target["source"].as_posix()])
            assemble = [self.tools["assembler"]]
            if self.target != self.host:
                assemble += ["-target", TARGETS[self.target]]
            self.command([*assemble, "-c", "-x", "assembler", assembly.as_posix(), "-o", obj.as_posix()])
            archive.unlink(missing_ok=True)
            self.command([self.tools["archiver"], "rcs", archive.as_posix(), obj.as_posix()])
            manifest_flags = [f for f in flags if f != "--emit-library"]
            self.command([compiler, *manifest_flags, "--emit-library-manifest", "-o", manifest.as_posix(), target["source"].as_posix()])
        else:
            self.command([compiler, *flags, "-o", outputs[0].as_posix(), target["source"].as_posix()])
        state = {"signature": signature, "outputs": {str(p): digest(p) for p in outputs}}
        with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", dir=directory, delete=False) as stream:
            json.dump(state, stream, sort_keys=True, indent=2)
            temporary = Path(stream.name)
        try:
            os.replace(temporary, state_path)
        finally:
            temporary.unlink(missing_ok=True)

    def execute(self, names, command):
        if command == "list":
            require(not names, "list does not take target names")
            for name, target in self.targets.items():
                print(f"{name}\t{target['kind']}\tdeps={','.join(target['deps']) or '-'}")
            return
        if command == "clean":
            selected = names or list(self.targets)
            for name in selected:
                require(name in self.targets, f"unknown target: {name}")
                path = self.directory(name)
                print(("would clean: " if self.args.dry_run else "cleaning: ") + str(path))
                if path.exists() and not self.args.dry_run:
                    remove_tree(path)
            return
        if command == "run":
            selected, run_args = ([names[0]], names[1:]) if names else ([self.default], [])
        elif command == "test":
            selected = names or [n for n, t in self.targets.items() if t["kind"] == "test"]
            require(selected and all(n in self.targets and self.targets[n]["kind"] == "test" for n in selected),
                    "test requires test targets")
        else:
            selected = names or [self.default]
        order = self.order(selected)
        self.prepare_tools()
        for name in order:
            self.build_target(name)
        if self.args.dry_run:
            return
        for name in selected:
            target = self.targets[name]
            if command == "run":
                require(target["kind"] in ("executable", "test"), "run requires an executable or test target")
                self.command([str(self.artifacts(name)[0]), *run_args], timeout=60)
            elif command == "test":
                stdout = self.command([str(self.artifacts(name)[0]), *target["args"]],
                                      capture=True, timeout=60, expected=target["expect-exit"])
                output = stdout.decode("utf-8", errors="replace").replace("\r\n", "\n")
                if "expect" in target and output != target["expect"]:
                    raise BakeError(f"test failed: {name}: stdout={output!r}, expected={target['expect']!r}", 1)
                print(f"test passed: {name}")


def main():
    if os.name == "nt":
        sys.stdout.reconfigure(encoding="utf-8")
        sys.stderr.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser(description="Bake native B++ programs and static ABI libraries")
    parser.add_argument("--version", action="version", version="Bake 1.0.17")
    parser.add_argument("-f", "--file", default="Bakefile")
    parser.add_argument("--profile", choices=("release", "debug"))
    parser.add_argument("--target", choices=["host", *TARGETS])
    for name in ("compiler", "assembler", "archiver"):
        parser.add_argument("--" + name)
    parser.add_argument("-n", "--dry-run", action="store_true")
    parser.add_argument("--rebuild", action="store_true")
    parser.add_argument("words", nargs="*")
    args = parser.parse_args()
    words = args.words
    command = words[0] if words and words[0] in ("build", "clean", "test", "run", "list") else "build"
    names = words[1:] if words and words[0] == command else words
    try:
        project = Project(args.file, args)
        if command == "list" or args.dry_run:
            project.execute(names, command)
            return 0
        project.base.mkdir(parents=True, exist_ok=True)
        lock = project.base / ".bake-lock"
        try:
            lock.mkdir()
        except FileExistsError:
            raise BakeError(f"another Bake invocation or stale lock exists: {lock}")
        try:
            (lock / "owner.json").write_text(json.dumps({"pid": os.getpid(), "Bakefile": str(project.file)}))
            project.execute(names, command)
        finally:
            remove_tree(lock)
        return 0
    except BakeError as error:
        print("Bake: " + str(error), file=sys.stderr)
        return error.code
    except (OSError, ValueError, KeyError, subprocess.TimeoutExpired) as error:
        print("Bake: " + str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
