#!/usr/bin/env python3
"""
Daemon to watch and auto-restart the DeepStream program.

Usage:
    python3 daemon.py [deepstream args...]

Examples:
    python3 daemon.py --config /app/configs/app2_live.ini
    python3 daemon.py --config /app/configs/app2_live.ini -s rtsp://100.64.0.153:8554/live
"""

import os
import sys
import time
import signal
import subprocess
import logging

# ── Configuration ──────────────────────────────────────────────────────────────
DEEPSTREAM_BIN   = os.environ.get("DEEPSTREAM_BIN", "./deepstream")
RESTART_DELAY    = float(os.environ.get("RESTART_DELAY", "5"))   # seconds between restarts
MAX_RESTARTS     = int(os.environ.get("MAX_RESTARTS", "0"))       # 0 = unlimited
CRASH_RESET_SECS = float(os.environ.get("CRASH_RESET_SECS", "60"))  # reset counter if running longer than this

# ── Logging ────────────────────────────────────────────────────────────────────
logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [daemon] %(levelname)s %(message)s",
    datefmt="%Y-%m-%d %H:%M:%S",
)
log = logging.getLogger("daemon")

# ── State ──────────────────────────────────────────────────────────────────────
_proc: subprocess.Popen | None = None
_running = True


def _stop_child(*_):
    global _running
    _running = False
    if _proc and _proc.poll() is None:
        log.info("Stopping child process (pid=%d)…", _proc.pid)
        _proc.send_signal(signal.SIGTERM)
        try:
            _proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            log.warning("Child did not exit; sending SIGKILL")
            _proc.kill()
    sys.exit(0)


signal.signal(signal.SIGTERM, _stop_child)
signal.signal(signal.SIGINT,  _stop_child)


def run(args: list[str]) -> None:
    global _proc

    cmd = [DEEPSTREAM_BIN] + args
    log.info("Command: %s", " ".join(cmd))

    restart_count = 0
    start_time: float | None = None

    while _running:
        if MAX_RESTARTS > 0 and restart_count >= MAX_RESTARTS:
            log.error("Reached max restarts (%d). Exiting.", MAX_RESTARTS)
            break

        log.info("Starting deepstream (attempt #%d)…", restart_count + 1)
        start_time = time.monotonic()

        try:
            _proc = subprocess.Popen(cmd)
        except FileNotFoundError:
            log.error("Binary not found: %s", DEEPSTREAM_BIN)
            sys.exit(1)

        log.info("Child started (pid=%d)", _proc.pid)
        _proc.wait()
        elapsed = time.monotonic() - start_time
        rc = _proc.returncode

        if not _running:
            break

        if rc == 0:
            log.info("Child exited cleanly (rc=0) after %.1fs — restarting.", elapsed)
        else:
            log.warning("Child crashed (rc=%d) after %.1fs.", rc, elapsed)

        # Reset restart counter if the process ran long enough (stable run)
        if elapsed >= CRASH_RESET_SECS:
            log.info("Ran for %.1fs (≥ reset threshold %.1fs) — resetting restart counter.", elapsed, CRASH_RESET_SECS)
            restart_count = 0
        else:
            restart_count += 1

        if _running:
            log.info("Restarting in %.1fs…", RESTART_DELAY)
            time.sleep(RESTART_DELAY)


if __name__ == "__main__":
    extra_args = sys.argv[1:]
    if not extra_args:
        # Default: just pass config; override with CLI args or env
        default_config = os.environ.get("DEEPSTREAM_CONFIG", "/app/configs/app2_live.ini")
        extra_args = ["--config", default_config, "-s", "rtsp://100.64.0.153:8554/live" ]

    run(extra_args)
