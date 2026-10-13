#!/usr/bin/env python3
"""Independent signed 64-bit bitwise oracle, including native parameter paths."""
import sys
import tempfile
from pathlib import Path
from check_verification import run, failure

MASK = (1 << 64) - 1


def signed(value):
    value &= MASK
    return value if value < (1 << 63) else value - (1 << 64)


def literal(value):
    return "(-9223372036854775807-1)" if value == -(1 << 63) else str(value)


def main():
    compiler = Path(sys.argv[1]).resolve()
    numbers = [0, 1, -1, 42, -43, (1 << 63) - 1, -(1 << 63)]
    cases = []
    for a in numbers:
        cases.extend([(f"invert({literal(a)})", signed(~a)),
                      (f"~({literal(a)})", signed(~a))])
        for b in numbers:
            for name, symbol, value in (("band", "&", a & b),
                                        ("bor", "|", a | b),
                                        ("bxor", "^", a ^ b)):
                cases.extend([(f"{name}({literal(a)},{literal(b)})", signed(value)),
                              (f"({literal(a)}) {symbol} ({literal(b)})", signed(value))])
    cases.extend([("1 | 2 ^ 3 & 4", 3), ("1 & 2 == 2", 1), ("~1 + 3", 1)])
    # Each failing expression has a unique result; no aggregate checksum can
    # hide cancellation between incorrect outputs. Calls exercise parameters.
    source = "define proc invert(a){return ~a;}"
    source += "define proc band(a,b){return a & b;}"
    source += "define proc bor(a,b){return a | b;}"
    source += "define proc bxor(a,b){return a ^ b;}"
    with tempfile.TemporaryDirectory(prefix="bpp-gates-") as directory:
        work = Path(directory)
        path, output = work / "gate.bpp", work / "gate.exe"
        for begin in range(0,len(cases),64):
            batch=source+"define proc main(){"
            for index,(expression,expected) in enumerate(cases[begin:begin+64],begin+1):
                batch+=f"if (({expression}) != {literal(expected)}){{return {index};}}\n"
            batch+="return 0;}\n"
            path.write_text(batch)
            for flags in ([], ["--no-opt"]):
                result = run([compiler, *flags, "--print-result", "-o", output, path])
                if result.returncode:
                    failure(work, "bitwise-compile", result, batch.encode())
                result = run([output])
                if result.returncode or result.stdout.strip() != b"0":
                    failure(work, "bitwise-execute", result, batch.encode())
    print(f"Bitwise gates passed: {len(cases)} oracle cases, optimized and unoptimized")


if __name__ == "__main__":
    main()
