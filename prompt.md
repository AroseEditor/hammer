# hammer — HTTP load generator

Build a command-line HTTP load generator in C++20. Think `wrk`, but with correct latency
measurement. No libcurl, no boost, no HTTP library — the point of the project is that the
socket handling, the protocol parsing, and the statistics are all ours.

## Setup (do this once, then never again)

Create `.claude/commands/loop.md` containing exactly:

```
Read prompt.md. Run the loop protocol defined there until you hit a stop condition.
```

After that, `/loop` is the only command needed.

## The loop protocol

One cycle:

1. Read the task list below. Pick the first unchecked task.
2. Say in two or three lines what you're about to do. Don't write an essay.
3. Implement it.
4. `cmake --build --preset asan && ctest --preset asan`. Everything must pass.
5. Tick the checkbox in this file.
6. Commit and push.
7. Report in three lines: what shipped, anything surprising, what's next.
8. Go to step 1.

Keep looping. Don't ask permission between tasks.

Stop and ask when:

- A design decision has real trade-offs and the spec below doesn't settle it.
- The same test has failed three times running. Say what you tried.
- You want a dependency that isn't Catch2.
- A task is done and the next one looks wrong given what you just learned.

Never mark a task done with a failing or skipped test. If a task turns out bigger than it
looked, split it, commit the half that works, and say so.

## Git rules

Branch `main`, push after every green task. Never force-push.

Commit subject: imperative, lowercase, no trailing period, under 60 characters.
`add incremental http response parser`, not `feat: Added a comprehensive HTTP parser!`

Body only when the reason isn't obvious from the diff. Usually skip it.

Do not append attribution to commits. No `Co-Authored-By: Claude`, no
`Generated with Claude Code`, no emoji. Those lines are noise and they're in every commit
forever. `git commit -m "message"` and nothing else.

## Comment rules

Comments explain why, never what. If the code says it, delete the comment.

```cpp
// no
// increment the counter
count++;

// fine
// level-triggered: one read per wakeup is enough, no drain loop needed
```

No file header blocks. No `// ===== SECTION =====` banners. No doc comments that restate
the signature. No commented-out code. One line is almost always enough.

## Why this tool exists

Most load generators quietly under-report tail latency, and understanding why is the whole
point of building this.

A closed-loop generator sends a request, waits for the response, sends the next one. When
the server stalls for 200ms, the generator doesn't send anything during the stall — so the
requests that *would* have experienced that stall are never issued, never timed, and never
appear in the histogram. The tool omits exactly the samples that mattered. Gil Tene named
this coordinated omission.

The fix: decide when each request *should* depart before the test starts.

```
intended[i] = t0 + i * (1e9 / rate)
latency[i]  = response_arrival - intended[i]     // open loop, corrected
latency[i]  = response_arrival - actual_send     // closed loop, what wrk does
```

Both modes ship. The README shows the same server under the same load measured both ways,
and the p99 numbers differ by two orders of magnitude. That comparison is the deliverable.

## CLI

```
hammer [options] <url>

  -c, --connections N    total connections           (default 50)
  -t, --threads N        worker threads              (default 4)
  -d, --duration S       test duration in seconds    (default 10)
  -r, --rate N           target req/s; turns on open-loop mode
      --closed-loop      closed loop even with --rate set (for the comparison)
  -m, --method M         HTTP method                 (default GET)
  -H, --header "K: V"    extra header, repeatable
      --body FILE        request body from file
      --timeout MS       per-request timeout         (default 2000)
      --latency          print the full percentile spectrum
      --json             machine-readable output
  -h, --help
```

Hand-rolled argument parsing. Bad input exits 1 with a message naming the offending flag.
`--connections` below `--threads` is an error, not a silent clamp.

## Output

```
Running 10s test @ http://localhost:8080/
  4 threads, 50 connections, open loop @ 20000 req/s

Latency (corrected)
     50%     1.21ms
     75%     1.88ms
     90%     3.42ms
     99%    41.30ms
   99.9%   210.55ms
    max    340.11ms

  201847 requests in 10.00s, 24.10MB read
  Requests/sec   20184.70
  Transfer/sec       2.41MB
  Socket errors  connect 0, read 0, write 0, timeout 12
  Dispatch lag   mean 0.08ms, max 2.10ms, 0 requests behind schedule
```

Dispatch lag is how far behind the intended schedule we actually sent. It's the tool's
honesty check: if mean lag climbs past 10ms, print a warning that the generator itself is
saturated and the numbers describe our own bottleneck rather than the server's. A load
generator that admits when it's the problem is worth more than one that doesn't.

## Layout

```
CMakeLists.txt
CMakePresets.json
.github/workflows/ci.yml
.github/workflows/release.yml
prompt.md
README.md
src/
  main.cpp
  cli.h cli.cpp
  http_parser.h http_parser.cpp
  histogram.h histogram.cpp
  conn.h conn.cpp
  loop.h loop.cpp
  stats.h stats.cpp
  net_compat.h
  poller.h
  poller_epoll.cpp
  poller_wsapoll.cpp
tests/
  test_cli.cpp
  test_parser.cpp
  test_histogram.cpp
  test_integration.cpp
  fuzz_parser.cpp
tools/
  hostile_server.py
```

C++20, CMake 3.20+, Catch2 v3 via FetchContent.

Two platforms ship: Linux x86_64 and Windows x86_64. Linux with epoll is the build that gets
benchmarked and the one the README's numbers come from; the Windows build exists so anyone
can download an exe and run it. Everything platform-specific lives in `net_compat.h` and the
two poller backends. Nothing else in the codebase may contain an `#ifdef _WIN32` — if you
find yourself wanting one somewhere else, the abstraction is in the wrong place.

No other portability work. Not macOS, not BSD, not 32-bit.

Presets: `asan` (ASan+UBSan, default for development), `tsan`, `release` (-O2, no
sanitizers, used for the real benchmark runs). CI runs asan and tsan on every push.

---

## Day 1

### [x] P0 — scaffold

CMakeLists, CMakePresets, .gitignore, CI workflow, a `main` that prints usage and exits.
Catch2 wired up with one passing dummy test so `ctest --preset asan` is green from commit
one.

Done when: `cmake --preset asan && cmake --build --preset asan && ctest --preset asan`
works from a clean clone.

`git commit -m "scaffold cmake build with sanitizer presets"`

### [ ] P1 — argument parsing

Every flag above. A `Config` struct with validated fields. URL parsing splits scheme, host,
port, path — reject anything that isn't `http://`, and say so clearly rather than failing
later at connect time.

Tests: every flag, every rejection path, the tricky ones (`-H` used four times, `--rate 0`,
port out of range, missing value after a flag).

`git commit -m "add cli parsing and url validation"`

### [ ] P2 — HTTP response parser

The hard part of day one. A resumable state machine that gets handed whatever bytes arrived
and survives a split anywhere, including mid-header-name.

```cpp
enum class State { StatusLine, Headers, BodyLength, ChunkSize, ChunkData, BodyUntilClose, Done, Error };

struct Result { size_t consumed; bool message_complete; bool error; };
Result feed(std::span<const char> bytes);
```

Handle: status line and status code, `Content-Length`, `Transfer-Encoding: chunked` with
the trailing chunk and trailers, `Connection: close` bodies that end at EOF, 204 and 304
and HEAD responses that have no body regardless of headers, case-insensitive header names,
header lines terminated by bare LF as well as CRLF.

Reject and don't crash on: `Content-Length: -5`, `Content-Length` twice with different
values, chunk sizes that overflow, header lines past 8KB, more than 100 headers.

The test that proves it works: feed a known response one byte at a time and assert the
result matches feeding it in one shot. Then do it again with random split points across
1000 seeded iterations. If both pass, the state machine is right.

Add `fuzz_parser.cpp` as an optional libFuzzer target, clang only, not part of the default
build. Run it for a few minutes and fix whatever it finds.

`git commit -m "add incremental http response parser"`

### [ ] P3 — HDR histogram

Write it, don't import it. Log-linear bucketing, O(1) record, no allocation after
construction.

Values in nanoseconds, range 1µs to 60s, 3 significant digits. Bucket index from the
position of the highest set bit, sub-bucket index from the bits below it. API: `record`,
`merge`, `percentile`, `count`, `min`, `max`, `mean`.

Test against a brute-force oracle: push 100k random values into both the histogram and a
sorted vector, then assert every percentile from 1 to 99.99 agrees within the error bound
that 3 significant digits allows. Also test the edges — empty histogram, one value, values
above the tracked maximum.

`git commit -m "add hdr histogram with oracle-based tests"`

### [ ] P4 — end-to-end on one blocking connection

Slow and single-threaded on purpose. Connect, send, parse the response with P2, record with
P3, print a report. No epoll yet.

Point it at Python's `http.server` and confirm the numbers are sane. This is the first
moment the thing is real.

`git commit -m "add blocking single connection client"`

---

## Day 2

### [ ] P5 — epoll event loop

Non-blocking sockets, level-triggered epoll. Level-triggered on purpose: edge-triggered
means draining to `EAGAIN` on every wakeup and one forgotten drain is a hang that shows up
under load a week later. Note that choice in a comment.

`Conn` is a state machine: Connecting, Writing, Reading, Idle, Closed. Handle partial
writes — `send` returning less than requested is normal, not an error. Handle the server
closing a keep-alive connection between requests by reconnecting without counting it as an
error.

`TCP_NODELAY` on every socket. Nagle plus delayed ACK produces phantom 40ms latencies that
will cost a day to find.

Resolve DNS once at startup and reuse the `sockaddr`.

Put epoll behind a `Poller` interface now, in `poller.h`, even though there's only one
backend today:

```cpp
struct Event { int fd; bool readable, writable, error; };

class Poller {
public:
  void add(int fd, bool read, bool write);
  void mod(int fd, bool read, bool write);
  void del(int fd);
  std::span<const Event> wait(int timeout_ms);
};
```

The rest of the loop talks only to this. Retrofitting it in P11 after the loop has grown
epoll assumptions throughout is several hours of unpleasant work; doing it now costs
twenty minutes.

`git commit -m "add epoll event loop and connection state machine"`

### [ ] P6 — threads

One loop per thread, connections sharded at startup, no shared mutable state on the hot
path. Per-thread histograms and counters, merged once at the end. No atomics per request.

Precompute the request bytes at startup. Zero allocation in the hot path — add a debug-only
counter that asserts this, then verify it holds during a run.

Must be clean under TSan.

`git commit -m "shard connections across worker threads"`

### [ ] P7 — open-loop scheduler

The payoff. Precomputed intended departure times, latency measured from intended rather
than actual, dispatch lag tracked and reported.

`--closed-loop` runs the naive path so both numbers can be produced from one binary.

`CLOCK_MONOTONIC` / `steady_clock` everywhere. System clock plus NTP silently corrupts
timings in a way that's very hard to notice.

Test: a mock server that sleeps 200ms once mid-run. Closed loop should barely notice.
Corrected open loop should show a p99 in the hundreds of milliseconds. If both modes report
the same thing, the correction isn't wired up.

`git commit -m "add open-loop scheduling with coordinated omission correction"`

### [ ] P8 — hostile server

`tools/hostile_server.py`, one mode per flag: dribble one byte per second, chunked with
1-byte chunks, half-close mid-body, respond with headers and no body, send garbage before
the status line, accept the connection and never respond, close immediately after accept.

Integration tests run hammer against each mode. Nothing may hang, crash, or trip a
sanitizer. Wrong is fine and gets counted as an error; hanging is not.

`git commit -m "add hostile mock server and resilience tests"`

### [ ] P9 — timeouts and shutdown

Per-request timeout enforced in the loop via a timer wheel or a sorted deadline structure —
not by scanning every connection on every tick.

Socket errors counted by kind: connect, read, write, timeout. Timed-out requests are not
recorded in the latency histogram; they're counted separately, and the README says why.

SIGINT stops the test, prints the partial report, exits 0. No mutex or allocation in the
signal handler — set a flag and let the loop notice.

`git commit -m "add request timeouts and graceful shutdown"`

### [ ] P10 — JSON output and README

`--json` emits config, percentiles, throughput, errors, and dispatch lag as one object.
Hand-rolled serialisation is fine; escape strings properly.

README leads with what it is and how to run it. Then the comparison: same server, same
load, closed loop vs corrected open loop, two percentile tables side by side, and a
paragraph on why they disagree. Then a short design-notes section — level-triggered epoll,
per-thread histograms, why timeouts are excluded from latency, what isn't implemented
(HTTPS, HTTP/2, redirects).

No badges. No feature list written as marketing copy.

`git commit -m "add json output and readme"`

---

## Day 2 evening, or day 3 if day 2 ran long

### [ ] P11 — Windows backend

Windows has no epoll. `WSAPoll` is close enough to POSIX `poll` that the second backend is
small, and it's honest about its limits: O(n) scan per tick, so it won't hold up past a few
thousand connections. That's an acceptable trade for a build whose job is to be
downloadable, and the README says so plainly rather than implying parity.

`net_compat.h` holds all of it:

- `socket_t` — `int` on Linux, `SOCKET` on Windows.
- `WSAStartup` / `WSACleanup` wrapped in an RAII object constructed once in `main`. On Linux
  it's an empty struct.
- `close_socket`, `set_nonblocking` (`fcntl` vs `ioctlsocket`).
- `last_error()` and `would_block()` — `errno`/`EAGAIN` against `WSAGetLastError()`/
  `WSAEWOULDBLOCK`. Getting `EINPROGRESS` vs `WSAEWOULDBLOCK` wrong on non-blocking connect
  is the bug you'll actually hit here.
- Ignoring `SIGPIPE` becomes a no-op; Windows has no such signal.

`poller_wsapoll.cpp` implements the same `Poller` interface as the epoll backend. CMake
picks one file by platform — no `#ifdef` inside either backend.

MSVC needs `WIN32_LEAN_AND_MEAN` and `NOMINMAX` defined before including `windows.h`, and
`ws2_32` linked. Build with `/W4` and fix the warnings rather than silencing them; MSVC
catches a few real things gcc lets through.

Add a Windows job to `ci.yml`: configure, build Release, run ctest. No sanitizers there.
The full test suite passes on both platforms or the task isn't done.

`git commit -m "add wsapoll backend and windows build"`

### [ ] P12 — release workflow

`.github/workflows/release.yml`, triggered on `push` of tags matching `v*` plus
`workflow_dispatch` so it can be tested without inventing a tag.

Needs `permissions: contents: write` at the workflow level, or creating the release fails
with a 403 that reads like an auth problem and isn't.

Three jobs:

**build-linux** on `ubuntu-22.04`, not `ubuntu-latest`. The glibc on the runner sets the
floor for what the binary will run on, and pinning an older image means it works on more
machines. Configure the release preset, build, run ctest, link with `-static-libstdc++
-static-libgcc` so it doesn't need a matching toolchain at runtime, `strip` it, upload as
`hammer-linux-x86_64`.

**build-windows** on `windows-latest` with MSVC. Configure with `-DCMAKE_BUILD_TYPE=Release`
directly rather than the sanitizer presets, which carry gcc-only flags. Build, run ctest,
upload `build/Release/hammer.exe` as `hammer-windows-x86_64.exe`.

Both jobs emit a `.sha256` next to their binary.

**release** runs after both, downloads the artifacts, and calls
`softprops/action-gh-release@v2` with `draft: true` and the four files. Draft, not
published — that leaves a review step before anything goes public, and a bad draft can be
deleted without burning the tag.

GitHub attaches "Source code (zip)" and "Source code (tar.gz)" to every release on its own,
built from the tag. Nothing in the workflow creates them and nothing should try to remove or
replace them. Leave that behaviour alone — a release with the binaries *and* the source zip
is exactly the intended result.

While you're here: bake the tag into the binary. Pass `-DHAMMER_VERSION=${{ github.ref_name
}}` at configure time, default it to `dev` for local builds, and add a `--version` flag that
prints it. A binary that can't tell you which build it is causes confusion the first time
someone reports a bug.

Test the whole thing by pushing `v0.0.1-test`. Check that both binaries land on the draft,
that the source zip is sitting there beside them, and that the Linux one actually runs on a
different machine than the one that built it. Then delete the draft and the tag.

`git commit -m "add release workflow for linux and windows builds"`

---

## Things that will cost a day if ignored

- `ulimit -n` defaults to 1024. Above that many connections you'll get confusing `EMFILE`
  errors. Raise it in the test scripts and mention it in the README.
- `Connection: close` testing burns through the ~28k ephemeral port range in seconds and
  then blocks on TIME_WAIT. Know why. Say so in the README rather than pretending it's a
  bug.
- `connect()` on a non-blocking socket returns `EINPROGRESS`. Success is signalled by the
  socket becoming writable, and you still have to check `SO_ERROR` — a writable socket can
  be a failed connect.
- `SIGPIPE` will kill the process when writing to a closed socket. Ignore it at startup.
- `epoll_wait` returns `EINTR` and that's not an error.
- Timing under ASan is 2-3x slower. Real benchmark numbers come from the release preset
  only, and the README should say which build produced them.
- On Windows, sockets are `SOCKET` (an unsigned handle), not small ints, and the invalid
  value is `INVALID_SOCKET`, not `-1`. Code that checks `fd < 0` compiles fine and is wrong.
- `WSAPoll` has a documented bug where a non-blocking connect that fails never reports
  `POLLERR` — it just never becomes writable. Use a connect timeout, and don't wait forever
  on a socket that will never signal.
- `send`/`recv` on Windows take `char*` and `int`, not `void*` and `size_t`. Casting in
  `net_compat.h` keeps the mess in one file.
- Windows has no `SIGPIPE`, so the Linux workaround has no counterpart. Writing to a closed
  socket returns an error instead of killing the process — a difference in behaviour worth
  one comment where it matters.

## Standing rules

Correctness first, speed second. A load generator that reports wrong numbers confidently is
worse than a slow one.

Prefer the specific over the adjective, in code and in prose. "cuts p99 from 41ms to 3ms"
beats "improves performance."

Don't claim something is tested if it isn't. If a path is untested, say which one and move
on.
