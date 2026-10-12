#!/usr/bin/env python3
"""Bounded native stress, deterministic artifacts, and compiler fuzz probes."""
import argparse
import os
from pathlib import Path
import random
import shutil
import subprocess
import sys
import tempfile


def limits(cpu=20):
    if os.name == "posix":
        import resource
        resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
        resource.setrlimit(resource.RLIMIT_CPU, (cpu, cpu))
        resource.setrlimit(resource.RLIMIT_FSIZE, (32 << 20, 32 << 20))
        # ASAN/MSAN require very large virtual mappings; do not cap RLIMIT_AS.


def run(command, *, data=None, timeout=30, stack=None, cpu=20):
    def child_limits():
        limits(cpu)
        if stack and os.name == "posix":
            import resource
            resource.setrlimit(resource.RLIMIT_STACK, (stack, stack))
    try:
        return subprocess.run([str(x) for x in command], input=data,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              timeout=timeout,
                              preexec_fn=child_limits if os.name == "posix" else None)
    except subprocess.TimeoutExpired as error:
        return subprocess.CompletedProcess(command, 124, error.stdout or b"",
                                           (error.stderr or b"") + b"\nverification deadline exceeded\n")


def minimize(data, fails):
    """Deterministic byte ddmin, bounded to 100 probes; preserve failure class."""
    chunks, probes = 2, 0
    while len(data) > 1 and probes < 100:
        width = (len(data) + chunks - 1) // chunks
        reduced = False
        for start in range(0, len(data), width):
            trial = data[:start] + data[start + width:]
            probes += 1
            if trial and fails(trial):
                data, chunks, reduced = trial, max(2, chunks - 1), True
                break
            if probes >= 100:
                break
        if not reduced:
            if chunks >= len(data):
                break
            chunks = min(len(data), chunks * 2)
    return data


def failure(work, name, result, source=None, reducer=None, original=None):
    dest = Path(os.environ.get("BPP_FAILURE_DIR", "build/verification-failures")) / name
    dest.mkdir(parents=True, exist_ok=True)
    if source is not None:
        (dest / "original.bpp").write_bytes(original if original is not None else source)
        (dest / "minimal.bpp").write_bytes(minimize(source, reducer) if reducer else source)
    (dest / "stdout.txt").write_bytes(result.stdout)
    (dest / "stderr.txt").write_bytes(result.stderr)
    (dest / "reproduce.txt").write_text(f"case={name}\ncommand={result.args!r}\nexit={result.returncode}\n")
    print(f"FAIL {name}: exit={result.returncode}; retained {dest}", file=sys.stderr)
    sys.stderr.buffer.write(result.stderr[-4000:])
    raise SystemExit(1)


def stress(compiler, work):
    args = ",".join(f"a{n}" for n in range(64))
    source = f"define proc many({args}){{return {'+'.join(f'a{n}' for n in range(64))};}}\n"
    source += "define proc deep(n){if(n<=0){return 0;}return 1+deep(n-1);}\n"
    source += "define proc wide(n){\n"
    source += "".join(f"new byte v{n}=n+{n};\n" for n in range(512))
    source += "return " + "+".join(f"v{n}" for n in range(512)) + ";}\n"
    source += "define proc branches(n){new byte v=0;\n"
    source += "".join(f"if(n%2==0){{v=v+{n};}}else{{v=v-{n};}}\n" for n in range(256))
    source += "return v;}\ndefine proc main(){return many(" + ",".join(map(str, range(64)))
    source += ")+deep(300)+wide(2)+branches(2);}\n"
    path = work / "stress.bpp"
    path.write_text(source)
    expected = sum(range(64)) + 300 + 512 * 2 + sum(range(512)) + sum(range(256))
    if os.environ.get("BPP_VERIFICATION_TRACE"):
        for stage,flags in (("validate",["--check"]),("emit",["--emit-asm","-o",work/"stress-trace.s"])):
            print(f"Stress diagnostic phase: {stage}",flush=True)
            probe=run([compiler,*flags,path],timeout=180,cpu=120)
            print(probe.stderr.decode(errors="replace"),flush=True)
            if probe.returncode:failure(work,"stress-"+stage,probe,source.encode())
    for mode in ([], ["--no-opt"]):
        output = work / "stress.exe"
        result = run([compiler, *mode, "--print-result", "-o", output, path], timeout=180, cpu=120)
        if os.environ.get("BPP_VERIFICATION_TRACE"):
            print(result.stderr.decode(errors="replace"),flush=True)
        if result.returncode:
            failure(work, "stress-compile", result, source.encode())
        result = run([output])
        if result.returncode or result.stdout.strip() != str(expected).encode():
            failure(work, "stress-execute", result, source.encode())
    # Frame layouts on either side of several page-sized allocations.
    for locals_count in (16, 64, 128, 256, 512):
        boundary = "define proc frame(n){" + "".join(
            f"new byte v{k}=n+{k};" for k in range(locals_count))
        boundary += "return " + "+".join(f"v{k}" for k in range(locals_count))
        boundary += ";}define proc main(){return frame(1);}\n"
        path.write_text(boundary)
        for mode in ([], ["--no-opt"]):
            result = run([compiler, *mode, "--print-result", "-o", output, path], timeout=180, cpu=120)
            if result.returncode:
                failure(work, f"frame-{locals_count}-compile", result, boundary.encode())
            result = run([output])
            expected = locals_count + sum(range(locals_count))
            if result.returncode or result.stdout.strip() != str(expected).encode():
                failure(work, f"frame-{locals_count}-execute", result, boundary.encode())
    # Deliberate nonterminating recursion uses the host stack limit.
    # Prevent constant folding and tail-call elimination. Every frame
    # remains live while waiting for the recursive result.
    source = "define proc exhaust(n){return n+exhaust(n+1);}define proc main(){return exhaust(1);}\n"
    path.write_text(source)
    result = run([compiler, "--no-opt", "-o", output, path])
    if result.returncode:
        failure(work, "stack-exhaustion-compile", result, source.encode())
    result = run([output], timeout=15, stack=(512 << 10) if os.name == "posix" else None)
    if result.returncode not in (-11, -6, 3221225725, -1073741571) and b"stack-overflow" not in result.stderr:
        failure(work, "stack-exhaustion", result, source.encode())
    print(f"Stack exhaustion terminated under {'512 KiB' if os.name == 'posix' else 'system'} stack limit: exit={result.returncode}")
    print("Native stress passed: 64 parameters, 300 recursive frames, 512 locals, 256 branches and frame probes; both optimization modes")


def reproducible(compiler, work):
    path = work / "repeat.bpp"
    path.write_text("define proc plus(n){return ((n & -1)+1)^0;}define proc main(){return plus(41)|0;}\n")
    clang = shutil.which("clang")
    for target, triple in (("macos-arm64", "arm64-apple-macos11"),
                           ("linux-arm64", "aarch64-linux-gnu"),
                           ("linux-x86_64", "x86_64-linux-gnu"),
                           ("windows-x86_64", "x86_64-w64-windows-gnu")):
        for flags in ([], ["--no-opt"], ["--debug-info"]):
            output = work / "repeat.s"
            command = [compiler, *flags, "--emit-asm", "--target", target, "-o", output, path]
            first = run(command)
            if first.returncode:
                failure(work, f"repro-{target}-compile", first, path.read_bytes())
            assembly = output.read_bytes()
            obj = work / "repeat.o"
            assemble = [clang, "-target", triple, "-c", "-x", "assembler", output, "-o", obj]
            if clang:
                result = run(assemble)
                if result.returncode:
                    failure(work, f"repro-{target}-assemble", result)
                original_object = obj.read_bytes()
            second = run(command)
            if second.returncode or assembly != output.read_bytes():
                failure(work, f"repro-{target}-assembly", second)
            if clang:
                result = run(assemble)
                if result.returncode or original_object != obj.read_bytes():
                    failure(work, f"repro-{target}-object", result)
    print("Repeated assembly and object bytes match for all four targets, with fixed paths and toolchain")


def fuzz(harness, generator, work, cases):
    rejected = [b'define proc main(){return "text";}',
                b'define proc main(){return Array(1);}',
                b'define proc main(){new byte n=;return n;}',
                b'define proc main(a){return 1/0;}', b'/* unterminated',
                b'define proc main(){return 9223372036854775808;}']
    for seed in range(1, cases + 1):
        result = run([generator, str(seed)])
        if result.returncode:
            failure(work, f"generator-{seed}", result)
        valid = result.stdout
        randomizer = random.Random(seed)
        start = randomizer.randrange(len(valid))
        mutated = valid[:start] + randomizer.choice([b"}", b";", b"-", b"\x01", b"[", b"!"]) + valid[start + 1:]
        for label, source in (("valid", valid), ("mutated", mutated),
                              ("rejected", rejected[seed % len(rejected)])):
            for optimize in (0, 1):
                command = [harness, str(optimize)]
                result = run(command, data=source, timeout=10)
                if result.returncode:
                    def still_fails(trial):
                        probe = run(command, data=trial, timeout=10)
                        return probe.returncode == result.returncode
                    failure(work, f"fuzz-seed-{seed}-{label}-opt-{optimize}", result,
                            source, still_fails)
    print(f"Passed {cases} fuzz seeds: valid/mutated/rejected sources, baseline/optimized IR, corrupt-IR rejection and all native emitters")


def differential(compiler, generator, work, cases):
    for seed in range(1, cases + 1):
        for mode in ([], ["--no-opt"]):
            def evaluate_depth(depth):
                generated = run([generator, str(seed), "--depth", str(depth)])
                expected = run([generator, "--expected", str(seed), "--depth", str(depth)])
                path, output = work / "case.bpp", work / "native.exe"
                path.write_bytes(generated.stdout)
                built = run([compiler, *mode, "--print-result", "-o", output, path])
                actual = run([output]) if built.returncode == 0 else built
                matches = (generated.returncode == expected.returncode == built.returncode ==
                           actual.returncode == 0 and actual.stdout == expected.stdout)
                return matches, generated.stdout, expected.stdout, actual
            ok, source, expected, result = evaluate_depth(4)
            if not ok:
                # Preserve syntactically valid inputs and their independent
                # expectations while reducing expression depth.
                smallest = (source, expected, result)
                for depth in range(4):
                    match, trial, wanted, probe = evaluate_depth(depth)
                    if not match and len(trial) < len(smallest[0]):
                        smallest = (trial, wanted, probe)
                label = f"differential-seed-{seed}-{'baseline' if mode else 'optimized'}"
                dest = Path(os.environ.get("BPP_FAILURE_DIR", "build/verification-failures")) / label
                dest.mkdir(parents=True, exist_ok=True)
                (dest / "expected.txt").write_bytes(smallest[1])
                (dest / "original.bpp").write_bytes(original if original is not None else source)
                failure(work, label, smallest[2], smallest[0], original=source)
    print(f"Passed {cases} independently evaluated seeds in optimized and baseline native modes; arithmetic/comparisons/division/modulo/branches/loops/calls/recursion/byte buffers")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("mode", choices=("stress", "reproducible", "fuzz", "differential"))
    parser.add_argument("compiler")
    parser.add_argument("--harness")
    parser.add_argument("--generator")
    parser.add_argument("--cases", type=int, default=int(os.environ.get("BPP_FUZZ_CASES", "64")))
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="bpp-verify-") as directory:
        work = Path(directory)
        compiler = Path(args.compiler).resolve()
        if args.mode == "stress":
            stress(compiler, work)
        elif args.mode == "reproducible":
            reproducible(compiler, work)
        elif args.mode == "differential":
            differential(compiler, Path(args.generator).resolve(), work,
                         int(os.environ.get("BPP_DIFFERENTIAL_CASES", str(args.cases))))
        else:
            fuzz(Path(args.harness).resolve(), Path(args.generator).resolve(), work, args.cases)


if __name__ == "__main__":
    main()
