# CLAUDE.md

claude-statusline is a status line command for Claude Code: it reads the session JSON
on stdin and prints one line (clock, context gauge, 5 h rate limit with a depletion
forecast, path, account, model, disk/CPU/RAM/GPU/VRAM). `--simple` trims the line,
`--bar` prints nothing and feeds a window-manager bar instead. Single-file C11, POSIX
only (Linux, macOS), no dependencies. Users download release binaries or `make install`;
nothing deploys automatically. The README documents output format, glyph/colour
thresholds and the forecast maths in detail; read it instead of re-deriving.

## Commands

```sh
make            # ./statusline; uses musl-gcc + -static when musl-gcc is on PATH
make test       # ./tests: regression, blending, harvesting, compaction, parsers
make lint       # strict warning set as errors, no binary; CI runs CC=gcc and CC=clang
make sanitize   # unit tests under ASan + UBSan with the system compiler (SAN_CC=clang)
make install    # copy to ~/.claude/claude-statusline/statusline (PREFIX= to override)
make clean
echo '{"cwd":"/tmp","context_window":{"used_percentage":12.5}}' | ./statusline   # CI smoke test
```

- CI (`.github/workflows/ci.yml`): `make lint` with gcc and clang; on Linux and macOS
  `make test`, `make sanitize` (Linux), then a rebuild with `-Werror` added to CFLAGS,
  `./statusline --help` and the smoke test. The default CFLAGS have no `-Werror`, so a
  warning that passes locally can still fail CI: run `make lint` before pushing.
- Release binaries (x86_64/aarch64 Linux musl, aarch64/x86_64 macOS) are built only by
  the Release workflow on minor/major releases (`release.yml`, job `binaries`); patch
  tags get none.

## Architecture

- `src/statusline.c` is the whole program: one translation unit of file-scope `static`
  functions in banner-comment sections (utilities, number scanning, output line, json,
  time, state files, gpu, samples, host metrics, gauges, forecast, main, --bar).
- `src/tests.c` `#include`s `statusline.c` and is built with `-DSL_TEST` to reach the
  statics; tests are plain functions called from `main()` with a `CHECK` macro. No
  header, no library: keep it one TU, a second source file breaks this arrangement.
- State in `~/.cache/claude-statusline/` (`$XDG_CACHE_HOME` honoured): `samples.tsv`,
  `rates.tsv`, `cpu.tsv`, `gpu.csv`. A legacy `~/.cache/statusline-rs` from the old Rust
  implementation is adopted on first run.
- `--bar` sets the terminal title via OSC 2 and writes the weekly limit as Pango markup
  to `$XDG_RUNTIME_DIR/claude-bar/limits`; the swaybar in the `tuios-fedora` repo reads it.

## Cost model (why the code looks like this)

Claude Code spawns a fresh process on every UI refresh, so the budget is about 1 ms wall
and 0.6 MB RSS per call. Compiler and optimisation level barely matter (gcc -O2, -O3
-march=native -flto, clang and PGO land within 3 %); the wins were I/O volume, static
musl linking and C, in that order.

- Never add a synchronous subprocess to the hot path. `nvidia-smi` runs at most every
  10 s in a detached child that refreshes `gpu.csv`; invocations only read the cache.
- The sample log keeps only what the estimators read (last 45 min of the current window
  plus its first sample). Do not keep more history: parsing 12 h cost 5-13 ms per call.
  Compaction is amortised (grow to ~2x, then rewrite); normal refreshes append one line.
- Without `musl-gcc`, `make` silently produces a slower glibc-dynamic build (about 2x
  wall time, 5x RSS). Check which build you measured before quoting numbers.
- Benchmarks belong to an otherwise idle machine; numbers from a loaded one are not
  comparable to the README table.

## Rules and pitfalls

- The forecast is a shrinkage blend: exponentially weighted least squares over 45 min
  (15 min half-life) against the median rate of the last 20 closed 5 h windows, the prior
  worth ~20 min of evidence and fading to zero at 45 min. Change it only with tests in
  `src/tests.c`.
- `make install` copies the binary; editing the source changes nothing for a running
  Claude Code until `make && make install`. With `--bar` the full-line layout is never
  visible, so check output by piping JSON into `./statusline` directly.
- Fields whose source is missing are omitted, never printed as zero or placeholder; keep
  that contract for new fields.
- 4-space indentation, comments explain why. Keep the help text in `USAGE` in sync with
  new flags and the README.

## Workflow

- Every change starts with a GitHub issue. Branch `<issue>-<slug>` from `main`,
  open a PR with `Closes #<issue>`. Never push to `main` directly.
- PRs auto-merge (squash) once CI and the "Linked ticket" check pass; drafts are not merged.
- Versions exist only as git tags `vX.Y.Z`. Every merge to `main` tags the next patch;
  never edit version numbers in files. Minor/major releases: Actions -> Release (also
  builds and attaches the binaries).
- Nothing deploys automatically; install locally with `make install`.
- Details: CONTRIBUTING.md.
