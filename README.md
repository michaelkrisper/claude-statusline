# claude-statusline

[![CI](https://github.com/michaelkrisper/claude-statusline/actions/workflows/ci.yml/badge.svg)](https://github.com/michaelkrisper/claude-statusline/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/michaelkrisper/claude-statusline)](https://github.com/michaelkrisper/claude-statusline/releases/latest)
[![License: MIT](https://img.shields.io/github/license/michaelkrisper/claude-statusline)](LICENSE)
[![Platforms](https://img.shields.io/badge/platform-linux%20%7C%20macos-blue)](https://github.com/michaelkrisper/claude-statusline/releases/latest)
[![Written in C](https://img.shields.io/badge/C-c11-blue?logo=c)](https://en.wikipedia.org/wiki/C11_(C_standard_revision))

A fast, single-binary status line for [Claude Code](https://claude.com/claude-code) that
doubles as a tiny system monitor: clock, CPU, RAM, GPU/VRAM, free disk space, the active
account, rate-limit consumption — and **predicts when your tokens will run out**, based on
your live burn rate and your usage history.

```
09:48 | CTX ◑ 78k SESSION ◕ 66% (~20:29 / 23:52) ~/projects/foo you Fable 5 high | DISK 21G CPU ○ RAM ◔ GPU ○ VRAM ◔
```

The clock leads, followed by two ` | `-separated blocks: what this session is spending
and where it runs (context, rate limit, path, account, model), then what the host has
left (disk, CPU, RAM, GPU, VRAM). Fields inside a block are space-separated; a field
whose source is unavailable is simply omitted, and a block whose fields are all
unavailable disappears together with its separator.

| Field | Source |
|---|---|
| `09:48` | current local time |
| `CTX ◑ 78k` | context window usage of the current session, with the token count behind it — always shown; turns yellow at 25%, red at 40% |
| `SESSION ◕ 66% (…)` | 5 h rate-limit usage with depletion forecast (see below) |
| `~/projects/foo` | project directory |
| `you` | active Claude account — the part before the `@` of the signed-in email |
| `Fable 5 high` | model and effort level |
| `DISK 21G` | free space on the filesystem holding the project directory (`statvfs`) |
| `CPU ○` | CPU usage since the previous refresh (`/proc/stat` delta; appears from the second invocation on) |
| `RAM ◔` | used RAM, `MemAvailable` vs `MemTotal` from `/proc/meminfo` |
| `GPU ○` | GPU utilization (first GPU, via `nvidia-smi`, cached — see below) |
| `VRAM ◔` | used VRAM (memory.used vs memory.total) |

Load percentages are shown as a gauge glyph rather than digits — a quarter-filled circle
plus a color, encoding the value twice so it reads at a glance. Only `SESSION`, where the
exact number matters for pacing, keeps its digits alongside the glyph.

| Glyph | `○` | `◔` | `◑` | `◕` | `●` |
|---|---|---|---|---|---|
| Value | 0–12% | 13–37% | 38–62% | 63–87% | 88–100% |

| Color | green | yellow | red |
|---|---|---|---|
| Value | below 60% | 60–84% | 85% and above |

Colour elsewhere on the line is deliberately sparse, so that the gauges keep their
signal value: the path is a muted steel blue (256-colour index 110), `SESSION` is bold,
and a depletion ETA less than 15 minutes away turns bright red. Everything else stays in
the terminal's default foreground.

The host metrics (`cpu`, `ram`) come from `/proc` and are shown on Linux; `disk` on any
Unix. Fields whose source is unavailable are simply omitted. The program is POSIX-only
— Linux and macOS.

`--simple` keeps what belongs to the session and drops the rest — no clock, no account,
no model, and of the host block only GPU/VRAM:

```
CTX ◑ 78k SESSION ◕ 66% (~20:29 / 23:52) ~/projects/foo | GPU ○ VRAM ◔
```

`nvidia-smi` takes hundreds of milliseconds (notably on WSL2), so the GPU fields are
never queried inline: at most every 10 s a **detached** background child refreshes
`gpu.csv` in the state dir, and invocations only ever read the cached value — no
status-line refresh ever blocks on the GPU. Without `nvidia-smi` the fields stay
hidden.

Reading the `SESSION` (5 h) segment:

| Display | Meaning |
|---|---|
| `SESSION 66% (~20:29 / 23:52)` | at the current burn rate you hit 100% at ~20:29, window resets 23:52 |
| `SESSION 22% (+8h / 23:52)` | you have headroom: depletion would land ~8 h *past* the reset |
| `SESSION 22% (23:52)` | no rate estimate yet (fresh install, no history) |

The account segment is read live from `~/.claude.json` on every invocation so it
reflects the current login immediately after an account switch. It is omitted when
that file is absent or holds no signed-in account.

## How the prediction works

The status line is invoked by Claude Code on every refresh. Each invocation appends a
usage sample (timestamp, 5h/7d percentage, reset timestamps) to a small log — at most
one sample per 10 s. From that log it estimates the burn rate as a
**shrinkage blend** of two estimators:

1. **Live rate** — exponentially weighted least-squares regression over the last 45 min
   of samples (15 min half-life, so recent activity dominates). Under 5 min of history
   it falls back to the window average, which is exact early on because a 5 h window
   starts at 0% with your first message.
2. **Personal prior** — whenever a 5 h window closes, its observed average rate is
   harvested into a per-window history (last 20 windows). The prior is the **median**
   of those rates, so a single burst session can't skew it.

The prior starts with the weight of ~20 min of live evidence and fades linearly to zero
once a full 45 min of live data exists: right after you start a session you get a
sensible estimate from your typical behavior; once there is real data, only the live
regression counts. The projected depletion time is then `now + remaining / rate`.

No prediction is shown when the rate is zero (idle) and no prior exists yet.

The log only keeps what those two estimators read: the last 45 min of the current
window, plus its very first sample, which anchors the full-window rate harvested at
the next rollover. Compaction is amortized — the log may grow to twice that before it
is rewritten, and ordinary refreshes just append one line.

## Why C?

A status line runs on *every* UI refresh — easily thousands of times per session — and
Claude Code spawns a fresh process each time (`"statusLine": {"type": "command"}`),
so per-invocation cost is the whole game. Measured on Linux x86_64, best of six runs
of 300 invocations each, pinned to one core:

| | full line | `--simple` |
|---|---|---|
| wall clock | 1.04 ms | 0.57 ms |
| peak RSS | 0.57 MB | 0.57 MB |
| syscalls | 61 | 36 |

Those were taken on a busy machine (load average ~5); with the cores free the same
build measures 0.69 ms and 0.43 ms. For reference, a bare `int main(){}` linked the
same way costs 0.24 ms — most of what is left is the kernel's process-spawn floor, not
this program. An interpreter would pay
30–100 ms *before executing its first line* (Python/Node startup), i.e. 50–150× the
entire budget, on every refresh.

Three things buy that, in order of impact:

1. **The log holds only what is read.** Retaining 12 h of samples and parsing all of
   them to use 45 min of them cost 5–13 ms per invocation. See above.
2. **Static musl linking.** No dynamic loader, no relocation processing: 1.6 ms → 0.8 ms
   and 3.0 MB → 0.6 MB peak RSS against a glibc-dynamic build of the same code.
3. **C.** ~95 KB of binary with no runtime to initialize, worth a further ~0.2 ms and
   ~0.1 MB over an equivalent statically linked Rust build.

Everything else is below the noise floor: JSON scanning, the weighted regression, the
median over 20 rates. Compiler choice and optimization level are, too — gcc `-O2`,
gcc `-O3 -march=native -flto`, clang `-O3` and a PGO build all land within 3 % of each
other, because there is barely any compute to optimize.

## Install (Claude Code)

### 1. Get the binary

Download the binary for your platform from [Releases](../../releases):

| Platform | Asset |
|---|---|
| Linux x86_64 (static) | `statusline-x86_64-linux-musl` |
| Linux arm64 (static) | `statusline-aarch64-linux-musl` |
| macOS Apple Silicon | `statusline-aarch64-macos` |
| macOS Intel | `statusline-x86_64-macos` |

Or build from source. `make` picks up `musl-gcc` automatically and links statically;
without it, it falls back to `cc` and a dynamically linked binary:

```sh
make
```

### 2. Put it somewhere stable

```sh
make install                       # -> ~/.claude/claude-statusline/statusline
make install PREFIX=/usr/local/bin # or anywhere else
```

### 3. Point Claude Code at it

In `~/.claude/settings.json`:

```json
{
  "statusLine": {
    "type": "command",
    "command": "/home/YOU/.claude/claude-statusline/statusline"
  }
}
```

That's it — the prediction appears automatically once enough samples exist, and gets
sharper after your first completed 5 h window.

## State files

| File | Content |
|---|---|
| `~/.cache/claude-statusline/samples.tsv` | rolling usage samples of the current 5 h window |
| `~/.cache/claude-statusline/rates.tsv` | per-window burn rates of the last 20 closed 5 h windows |
| `~/.cache/claude-statusline/cpu.tsv` | `/proc/stat` jiffies baseline for the CPU-usage delta |
| `~/.cache/claude-statusline/gpu.csv` | cached `nvidia-smi` sample, refreshed in the background every 10 s |

(`$XDG_CACHE_HOME` is honored.) Delete them to reset all learned history. A
`~/.cache/statusline-rs` left over from the Rust implementation is adopted on first
run, so the history carries over.

## Versioning & releases

[SemVer](https://semver.org/): breaking output-format changes bump minor (pre-1.0) /
major, everything else patch. A release is cut by pushing a tag — CI builds the
portable binary and attaches it:

```sh
git tag v0.1.1 && git push origin v0.1.1
```

## Development

```sh
make test    # unit tests for regression, blending, harvesting, compaction, parsing
make         # build; uses musl-gcc when available
```

The whole program is `src/statusline.c`; `src/tests.c` includes it with `-DSL_TEST` so
the tests can reach the static functions.

## License

MIT
