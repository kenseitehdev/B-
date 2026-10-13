#!/usr/bin/env python3
"""Phase-separated native benchmarks, ABI work timing and independent results."""
import argparse
import csv
import ctypes
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import signal
import statistics
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parent.parent
SCHEMA = 2


def checksum(path):
    with Path(path).open("rb") as stream:
        digest = hashlib.sha256()
        for data in iter(lambda: stream.read(1 << 20), b""):
            digest.update(data)
        return digest.hexdigest()


def measure(argv, cwd):
    """Time one process and collect its own peak RSS (not cumulative children)."""
    with tempfile.TemporaryFile() as stdout, tempfile.TemporaryFile() as stderr:
        start = time.perf_counter_ns()
        child = subprocess.Popen([str(a) for a in argv], cwd=cwd, stdout=stdout,
                                 stderr=stderr, start_new_session=os.name == "posix")
        memory = None
        handle = None
        if os.name == "nt":
            from ctypes import wintypes
            kernel = ctypes.WinDLL("kernel32", use_last_error=True)
            kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
            kernel.OpenProcess.restype = wintypes.HANDLE
            kernel.CloseHandle.argtypes = [wintypes.HANDLE]
            handle = kernel.OpenProcess(0x0400 | 0x0010, False, child.pid)
        try:
            while True:
                if os.name == "posix":
                    pid, status, usage = os.wait4(child.pid, os.WNOHANG)
                    if pid:
                        child.returncode = os.waitstatus_to_exitcode(status)
                        memory = usage.ru_maxrss * (1 if sys.platform == "darwin" else 1024)
                        break
                elif child.poll() is not None:
                    if handle:
                        class Counters(ctypes.Structure):
                            _fields_ = [("cb", ctypes.c_ulong), ("faults", ctypes.c_ulong)] + [
                                (name, ctypes.c_size_t) for name in ("peak", "working", "peak_paged", "paged", "peak_nonpaged", "nonpaged", "pagefile", "peak_pagefile")]
                        counters = Counters()
                        counters.cb = ctypes.sizeof(counters)
                        query = ctypes.WinDLL("psapi").GetProcessMemoryInfo
                        query.argtypes = [ctypes.c_void_p, ctypes.POINTER(Counters), ctypes.c_ulong]
                        if query(handle, ctypes.byref(counters), ctypes.sizeof(counters)):
                            memory = counters.peak
                    break
                if time.perf_counter_ns() - start > 120_000_000_000:
                    if os.name == "posix":
                        os.killpg(child.pid, signal.SIGKILL)
                    else:
                        subprocess.run(["taskkill", "/PID", str(child.pid), "/T", "/F"], capture_output=True, timeout=10)
                    child.wait()
                    raise RuntimeError(f"benchmark deadline exceeded: {argv}")
                time.sleep(0.001)
        finally:
            if handle:
                kernel.CloseHandle(handle)
        wall = (time.perf_counter_ns() - start) / 1e6
        stdout.seek(0)
        stderr.seek(0)
        output, errors = stdout.read(), stderr.read()
        if child.returncode:
            raise RuntimeError(f"benchmark command failed ({child.returncode}): {argv}\n{errors.decode(errors='replace')}\n{output.decode(errors='replace')}")
        return {"wall_ms": wall, "peak_rss_bytes": memory}, output


def summary(samples):
    median = statistics.median(samples)
    return {"samples": samples, "median": median,
            "mad": statistics.median(abs(v - median) for v in samples),
            "minimum": min(samples), "maximum": max(samples)}


def sections(path):
    size = shutil.which("llvm-size") or shutil.which("size")
    if not size:
        return {"text_bytes": None, "readonly_data_bytes": None, "reason": "size tool unavailable"}
    for mode in ("-A", "-m"):
        result = subprocess.run([size, mode, str(path)], capture_output=True, text=True, timeout=30)
        records = re.findall(r"^\s*(\.[\w.$]+|__[\w.$]+)\s+(\d+)\s+", result.stdout, re.M)
        records += re.findall(r"Section (\w+):\s*(\d+)\s+bytes", result.stdout)
        if records:
            return {"text_bytes": sum(int(n) for name, n in records if name in (".text", "__text")),
                    "readonly_data_bytes": sum(int(n) for name, n in records if name in (".rodata", ".rdata", "__const", "__cstring")),
                    "source": size + " " + mode}
    return {"text_bytes": None, "readonly_data_bytes": None, "reason": "section format unavailable"}


def workload(name, n):
    if name == "integer":
        return "define proc work(n){new int i=0;new int sum=0;while(i<n){sum=sum+i;i=i+1;}return sum;}", n*(n-1)//2
    if name == "branches":
        return "define proc work(n){new int i=0;new int sum=0;while(i<n){if(i%3==0){sum=sum+2;}else{sum=sum+1;}i=i+1;}return sum;}", n+(n+2)//3
    if name == "calls":
        return "define proc step(n){return n+1;}define proc work(n){new int i=0;new int sum=0;while(i<n){sum=sum+step(i);i=i+1;}return sum;}", n*(n+1)//2
    if name == "recursion":
        return "define proc rec(n){if(n<=0){return 0;}return 1+rec(n-1);}define proc work(n){return rec(n);}", n
    if name == "states":
        return "define proc work(n){new state s=0.5;new int i=0;while(i<n){s=XOR(s,-0.5);s=OR(s,0.5);i=i+1;}return faults(s);}", 0
    if name == "buffers":
        return "define proc work(n){new ptr p=malloc 64;new int i=0;new int sum=0;while(i<n){p[i%64]=i%256;sum=sum+p[i%64];i=i+1;}free p;return sum;}", sum(i%256 for i in range(n))
    if name == "byte_strings":
        return "define proc work(n){new ptr p=malloc 64;new int i=0;while(i<64){p[i]=65+i%26;i=i+1;}i=0;new int sum=0;while(i<n){sum=sum+p[i%64];i=i+1;}free p;return sum;}", sum(65+(i%64)%26 for i in range(n))
    if name == "large_source":
        return "define proc work(n){new int sum=n;" + "sum=sum+1;"*1024 + "return sum;}", n+1024
    raise ValueError(name)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", default=str(ROOT / "bin/release" / ("bpp.exe" if os.name == "nt" else "bpp")))
    parser.add_argument("--output", default="build/benchmarks/1.0.17/native")
    parser.add_argument("--samples", type=int, default=7)
    parser.add_argument("--warmup", type=int, default=3)
    parser.add_argument("--repetitions", type=int, default=8)
    parser.add_argument("--iterations", type=int, default=2048)
    parser.add_argument("--machine-id")
    args = parser.parse_args()
    if not 1 <= args.samples <= 100 or not 0 <= args.warmup <= 100 or not 1 <= args.repetitions <= 1000 or not 1 <= args.iterations <= 100000:
        parser.error("invalid sampling bounds")
    compiler = Path(args.compiler).resolve()
    info = json.loads(subprocess.check_output([str(compiler), "--build-info"], timeout=30))
    if info["sanitizers"]:
        parser.error("benchmarks require an unsanitized compiler/runtime")
    cc = os.environ.get("BPP_BENCHMARK_CC") or info["host_cc"]
    cc_path = shutil.which(cc) or str(Path(cc).resolve())
    target = info["host_target"]
    cpu = os.environ.get("PROCESSOR_IDENTIFIER", platform.processor())
    if sys.platform == "darwin":
        probe = subprocess.run(["sysctl", "-n", "machdep.cpu.brand_string"], capture_output=True, text=True, timeout=30)
        cpu = probe.stdout.strip() if probe.returncode == 0 else "unavailable (host query denied)"
    elif Path("/proc/cpuinfo").exists():
        matches = re.findall(r"^(?:model name|Hardware)\s*:\s*(.*)", Path("/proc/cpuinfo").read_text(), re.M)
        cpu = matches[0] if matches else cpu
    revision = subprocess.run(["git", "rev-parse", "HEAD"], cwd=ROOT, capture_output=True, text=True).stdout.strip()
    target_info = subprocess.check_output([str(compiler), "--target-info", target], text=True, timeout=30)
    triple = next(line.split("=",1)[1] for line in target_info.splitlines() if line.startswith("triple="))
    dirty = subprocess.run(["git", "status", "--porcelain", "--", "src", "include", "tools", "Makefile", "tests/benchmark_runner.c"], cwd=ROOT, capture_output=True, text=True).stdout
    dirty = any(not Path(line[3:]).name.startswith("._") for line in dirty.splitlines())
    host = {"os": platform.platform(), "architecture": platform.machine(), "cpu": cpu,
            "hostname_hash": hashlib.sha256(platform.node().encode()).hexdigest()[:24], "machine_id": args.machine_id or hashlib.sha256((platform.node()+platform.machine()).encode()).hexdigest()[:24],
            "machine_id_kind": "explicit" if args.machine_id else "host-fingerprint",
            "compiler_revision": revision, "compiler_worktree_dirty": dirty, "compiler_sha256": checksum(compiler),
            "host_cc": cc_path, "host_cc_identity": subprocess.check_output([cc_path, "--version"], text=True, timeout=30).splitlines()[0],
            "sanitizers": info["sanitizers"], "compiler_build_optimization": info.get("optimization"), "target": target, "target_triple": triple,
            "benchmark_driver_sha256": checksum(Path(__file__)), "fixture_source_sha256": checksum(ROOT/"tests/benchmark_runner.c"),
            "runtime_objects_sha256": {p: checksum(p) for p in info["runtime_objects"]}}
    results = []
    with tempfile.TemporaryDirectory(prefix="bpp-benchmark-") as temporary:
        directory = Path(temporary)
        runner_obj, profile_runner = directory/"runner.o", directory/"profile-runner.o"
        profile_runtime = directory/"profile-runtime.o"
        includes = ["-I", str(ROOT/"include"), "-std=c11", "-O2"]
        measure([cc_path, *includes, "-c", ROOT/"tests/benchmark_runner.c", "-o", runner_obj], ROOT)
        measure([cc_path, *includes, "-DBPP_BENCHMARK_METRICS", "-c", ROOT/"tests/benchmark_runner.c", "-o", profile_runner], ROOT)
        measure([cc_path, *includes, "-DBPP_BENCHMARK_METRICS", "-c", ROOT/"src/codegen_runtime.c", "-o", profile_runtime], ROOT)
        link_flags = ["-no-pie"] if target == "linux-x86_64" else []
        libraries = [*info["runtime_objects"]]
        profile_libraries = [str(profile_runtime) if Path(p).name=="codegen_runtime.o" else p for p in libraries]
        for name in ("integer", "branches", "calls", "recursion", "states", "buffers", "byte_strings", "large_source"):
            n = 32 if name == "recursion" else args.iterations
            source, expected = workload(name, n)
            path = directory/(name+".bpp")
            path.write_text(source+"\n")
            for optimize in (True, False):
                selection = "optimized" if optimize else "no-opt"
                flags = [] if optimize else ["--no-opt"]
                common = [str(compiler), *flags, "--target", target, "--emit-library", "--library-name", "bench"]
                assembly, obj = directory/"unit.s", directory/"unit.o"
                executable = directory/("runner.exe" if os.name=="nt" else "runner")
                phases = {key: [] for key in ("compile", "assembly", "link", "startup")}
                rss = []
                commands = {"compile": [*common, "--asm-comments", "--emit-asm", "-o", assembly.as_posix(), path.as_posix()],
                            "assembly": [cc_path, "-c", "-x", "assembler", assembly.as_posix(), "-o", obj.as_posix()],
                            "link": [cc_path, *link_flags, str(runner_obj), str(obj), *libraries, "-o", str(executable)],
                            "startup": [str(executable), "startup"]}
                for sample in range(args.warmup+args.samples):
                    for phase, command in commands.items():
                        measured, _ = measure(command, ROOT)
                        if sample >= args.warmup:
                            phases[phase].append(measured["wall_ms"])
                            if phase == "compile":
                                rss.append(measured["peak_rss_bytes"])
                assembly_text = assembly.read_text()
                assert "bpp_bench_enter" not in assembly_text and "bpp_bench_exit" not in assembly_text
                spill = re.findall(r"B\+\+ allocation-traffic function=(\S+) spills=(\d+) reloads=(\d+) rematerializations=(\d+)", assembly_text)
                _, raw = measure([*common, "--emit-map", path.as_posix()], ROOT)
                locations = json.loads(raw)
                _, raw = measure([executable, "timing", n, expected, args.warmup, args.samples, args.repetitions], ROOT)
                steady = [float(value) for value in raw.decode().splitlines()]
                assert len(steady) == args.samples and spill
                sizes = sections(obj)
                sizes.update({"executable_bytes": executable.stat().st_size, "object_bytes": obj.stat().st_size,
                              "assembly_source_bytes": assembly.stat().st_size})
                measure([*common, "--benchmark-metrics", "--emit-asm", "-o", assembly.as_posix(), path.as_posix()], ROOT)
                measure(commands["assembly"], ROOT)
                profiled = directory/("profiled.exe" if os.name=="nt" else "profiled")
                measure([cc_path, *link_flags, profile_runner, obj, *profile_libraries, "-o", profiled], ROOT)
                _, raw = measure([profiled, "metrics", n, expected], ROOT)
                dynamic = json.loads(raw)
                if name == "recursion":
                    assert dynamic["max_native_call_depth"] == n+2, dynamic
                record = {"schema": SCHEMA, "case": name, "selection": selection, "input": n,
                          "expected": expected, "source": source+"\n", "source_sha256": checksum(path), "flags": flags,
                          "phase_wall_ms": {key: summary(values) for key, values in phases.items()},
                          "timing_assembly_has_profile_hooks": False, "steady_abi_work_ms": summary(steady), "compilation_peak_rss_bytes": rss,
                          "sizes": sizes, "dynamic_profile": dynamic,
                          "static_spill_sites": sum(int(row[1]) for row in spill),
                          "static_reload_sites": sum(int(row[2]) for row in spill),
                          "static_rematerialization_sites": sum(int(row[3]) for row in spill),
                          "frames": {f["name"]: f["frame_layout"] for f in locations["functions"]},
                          "measurement_commands": {phase: [str(v) for v in command] for phase, command in commands.items()}}
                results.append(record)
                print(f"{name}/{selection}: compile={record['phase_wall_ms']['compile']['median']:.3f} ms, steady={record['steady_abi_work_ms']['median']:.6f} ms", flush=True)
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    report = {"schema": SCHEMA, "release": "1.0.17", "host": host,
              "sampling": {"warmup": args.warmup, "samples": args.samples, "repetitions": args.repetitions},
              "limitations": ["startup includes process creation, CRT initialization and immediate driver exit",
                              "steady timing includes the static ABI call, correctness check and owned-result release",
                              "native stack span excludes deeper C helper frames; profiling runs are separate from timing",
                              "spill/reload counts are actual emitted static sites, not dynamic event counts",
                              "Python process timing includes wait/poll overhead; shared CI is not a hardware baseline"],
              "unsupported_workloads": {"managed_strings": "source string constructors are retired; byte-string processing is measured",
                                        "managed_arrays": "source Array constructors are retired; byte buffers are measured",
                                        "custom_assembler_linker": "future stage; current output uses host tools"},
              "results": results}
    output.with_suffix(".json").write_text(json.dumps(report, indent=2, sort_keys=True)+"\n")
    with output.with_suffix(".csv").open("w", newline="") as stream:
        fields = ["schema", "case", "selection", "target", "machine_id", "compiler_revision", "sanitizers",
                  "compile_ms", "assembly_ms", "link_ms", "startup_ms", "steady_abi_ms", "steady_mad_ms",
                  "compile_peak_rss_bytes", "text_bytes", "readonly_data_bytes", "executable_bytes",
                  "max_native_call_depth", "native_frame_stack_span_bytes", "boundary_helper_calls",
                  "static_spill_sites", "static_reload_sites"]
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        for result in results:
            row = {k: result[k] for k in ("schema", "case", "selection", "static_spill_sites", "static_reload_sites")}
            row.update({k: host[k] for k in ("target", "machine_id", "compiler_revision", "sanitizers")})
            row.update({phase+"_ms": result["phase_wall_ms"][phase]["median"] for phase in ("compile", "assembly", "link", "startup")})
            row.update({"steady_abi_ms": result["steady_abi_work_ms"]["median"], "steady_mad_ms": result["steady_abi_work_ms"]["mad"],
                        "compile_peak_rss_bytes": max(v for v in result["compilation_peak_rss_bytes"] if v is not None) if any(v is not None for v in result["compilation_peak_rss_bytes"]) else None})
            row.update({k: result["sizes"][k] for k in ("text_bytes", "readonly_data_bytes", "executable_bytes")})
            row.update({k: result["dynamic_profile"][k] for k in ("max_native_call_depth", "native_frame_stack_span_bytes", "boundary_helper_calls")})
            writer.writerow(row)
    print(f"Saved {len(results)} independently checked native results to {output}.json/.csv")


if __name__ == "__main__":
    main()
