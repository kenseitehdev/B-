#!/usr/bin/env python3
"""Verify the native Windows UBSAN trap profile before the compiler suite."""
import os
import shutil
import sys
import tempfile
from pathlib import Path
from check_verification import run, failure

if os.name != "nt":
    raise SystemExit("This gate requires native Windows execution")
cc = shutil.which(sys.argv[1])
if not cc:
    raise SystemExit("Missing configured Windows compiler")
with tempfile.TemporaryDirectory(prefix="bpp-windows-ubsan-") as directory:
    work = Path(directory)
    source, output = work / "overflow.c", work / "overflow.exe"
    source.write_text("#include <limits.h>\nint main(void){volatile int n=INT_MAX;return n+1;}\n")
    result = run([cc, "-O1", "-fsanitize=undefined", "-fsanitize-trap=undefined",
                  source, "-o", output])
    if result.returncode:
        failure(work, "windows-ubsan-compile", result, source.read_bytes())
    result = run([output])
    if result.returncode not in (0xC000001D, 0xC000001D - (1 << 32)):
        failure(work, "windows-ubsan-trap", result, source.read_bytes())
print("Windows UBSAN trap instrumentation detected intentional signed overflow (STATUS_ILLEGAL_INSTRUCTION)")
