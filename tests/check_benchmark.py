#!/usr/bin/env python3
"""Validate retained benchmark data, without speed thresholds on shared CI."""
import csv
import json
import math
from pathlib import Path
import sys

prefix = Path(sys.argv[1])
report = json.loads(prefix.with_suffix(".json").read_text())
assert report["schema"] == 2 and report["release"] == "1.0.17"
assert report["host"]["sanitizers"] == "" and report["host"]["target_triple"]
assert report["host"]["compiler_sha256"] and report["host"]["runtime_objects_sha256"]
assert len(report["results"]) == 16
assert {r["selection"] for r in report["results"]} == {"optimized", "no-opt"}
assert len({r["case"] for r in report["results"]}) == 8
assert len({(r["case"],r["selection"]) for r in report["results"]}) == 16
for record in report["results"]:
    assert not record["timing_assembly_has_profile_hooks"]
    assert record["source"] and record["source_sha256"]
    assert set(record["phase_wall_ms"]) == {"compile", "assembly", "link", "startup"}
    for timing in [*record["phase_wall_ms"].values(), record["steady_abi_work_ms"]]:
        assert len(timing["samples"]) == report["sampling"]["samples"]
        assert all(math.isfinite(n) and n >= 0 for n in timing["samples"])
        assert timing["mad"] >= 0 and timing["minimum"] <= timing["median"] <= timing["maximum"]
    assert len(record["compilation_peak_rss_bytes"]) == report["sampling"]["samples"]
    assert all(n is None or n > 0 for n in record["compilation_peak_rss_bytes"])
    assert record["dynamic_profile"]["native_calls"] > 0
    assert record["dynamic_profile"]["max_native_call_depth"] >= 1
    assert record["dynamic_profile"]["native_frame_stack_span_bytes"] > 0
    assert record["dynamic_profile"]["boundary_helper_calls"] > 0
    assert record["static_spill_sites"] >= 0 and record["static_reload_sites"] >= 0
    assert record["sizes"]["text_bytes"] and record["sizes"]["executable_bytes"]
    assert all(f["frame_bytes"] > 0 for f in record["frames"].values())
    if record["case"] in ("integer", "branches", "states", "buffers", "byte_strings", "large_source"):
        assert record["dynamic_profile"]["max_native_call_depth"] == 1
        assert record["dynamic_profile"]["native_frame_stack_span_bytes"] == record["frames"]["work"]["frame_bytes"]
    if record["case"] == "recursion":
        assert record["dynamic_profile"]["max_native_call_depth"] == record["input"]+2
with prefix.with_suffix(".csv").open(newline="") as stream:
    rows = list(csv.DictReader(stream))
assert len(rows) == 16 and all(r["schema"] == "2" for r in rows)
print("Benchmark schema 2 passed: 16 independent native results, phases, sampling, memory, code/frame sizes and isolated profiling")
