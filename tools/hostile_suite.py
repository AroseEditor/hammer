import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
SERVER = os.path.join(HERE, "hostile_server.py")

NO_RESPONSE_MODES = {
    "dribble",
    "half-close",
    "headers-only",
    "garbage-prefix",
    "silent",
    "hangup",
    "endless",
    "slow-headers",
}

RESPONDING_MODES = {"tiny-chunks", "wrong-length"}

ALL_MODES = sorted(NO_RESPONSE_MODES | RESPONDING_MODES)


def start_server(mode, duration):
    process = subprocess.Popen(
        [sys.executable, SERVER, "--mode", mode, "--duration", str(duration)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    port_line = process.stdout.readline().strip()
    if not port_line.isdigit():
        process.kill()
        raise RuntimeError("server for mode %s did not report a port: %r" % (mode, port_line))
    return process, int(port_line)


def run_hammer(binary, port, wall_clock_limit):
    command = [
        binary,
        "-c", "4",
        "-t", "2",
        "-d", "2",
        "--timeout", "500",
        "http://127.0.0.1:%d/" % port,
    ]
    started = time.monotonic()
    try:
        completed = subprocess.run(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=wall_clock_limit,
        )
    except subprocess.TimeoutExpired:
        return None, time.monotonic() - started
    return completed, time.monotonic() - started


def parse_requests(output):
    match = re.search(r"^\s*(\d+) requests in ", output, re.MULTILINE)
    return int(match.group(1)) if match else None


def check_mode(binary, mode, failures):
    server, port = start_server(mode, duration=20.0)
    try:
        completed, elapsed = run_hammer(binary, port, wall_clock_limit=25.0)
    finally:
        server.kill()
        server.wait()

    if completed is None:
        failures.append("%s: hammer did not exit within 25s" % mode)
        return

    if completed.returncode != 0:
        failures.append(
            "%s: hammer exited %d\nstdout:\n%s\nstderr:\n%s"
            % (mode, completed.returncode, completed.stdout, completed.stderr)
        )
        return

    if "Socket errors" not in completed.stdout:
        failures.append("%s: hammer printed no report\n%s" % (mode, completed.stdout))
        return

    requests = parse_requests(completed.stdout)
    if requests is None:
        failures.append("%s: could not find the request count in the report" % mode)
        return

    if mode in NO_RESPONSE_MODES and requests != 0:
        failures.append(
            "%s: hammer counted %d successful requests from a server that never "
            "completes one" % (mode, requests)
        )
        return

    print("  %-14s ok  (%d requests, %.1fs)" % (mode, requests, elapsed), flush=True)


def main():
    if len(sys.argv) != 2:
        print("usage: hostile_suite.py <path to hammer binary>", file=sys.stderr)
        return 2

    binary = sys.argv[1]
    failures = []

    print("running hammer against %d hostile server modes" % len(ALL_MODES), flush=True)
    for mode in ALL_MODES:
        check_mode(binary, mode, failures)

    if failures:
        print("\nfailures:", file=sys.stderr)
        for failure in failures:
            print("  " + failure, file=sys.stderr)
        return 1

    print("all modes survived", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
