#!/usr/bin/env python3
"""Run one CI command with a wall-clock deadline and terminate its children."""
import os
import signal
import subprocess
import sys

seconds = float(sys.argv[1])
command = sys.argv[2:]
if not command or seconds <= 0:
    raise SystemExit(2)
options = {"start_new_session": True} if os.name == "posix" else {"creationflags": subprocess.CREATE_NEW_PROCESS_GROUP}
child = subprocess.Popen(command, **options)
try:
    status = child.wait(timeout=seconds)
except subprocess.TimeoutExpired:
    print(f"verification deadline: {seconds:g}s exceeded: {command!r}", file=sys.stderr, flush=True)
    if os.name == "posix":
        os.killpg(child.pid, signal.SIGTERM)
        try:
            child.wait(timeout=3)
        except subprocess.TimeoutExpired:
            os.killpg(child.pid, signal.SIGKILL)
    else:
        subprocess.run(["taskkill", "/PID", str(child.pid), "/T", "/F"], check=False)
    child.wait()
    raise SystemExit(124)
raise SystemExit(status if 0 <= status <= 125 else 1)
