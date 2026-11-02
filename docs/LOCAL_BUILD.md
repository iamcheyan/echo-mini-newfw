# Local build record

Build record for commit `6020bd6f44aa3aa6232183e165244ead33011b34` (`2026-11-01`), built locally on Debian 13.7 (amd64) on 2026-11-02. This records a local compile and host checks; it does not establish that the image works on a player.

## Dependencies

Required for the documented build and `make check`:

| Debian package | Version installed | Purpose |
|---|---|---|
| `make` | 4.4.1 | Build orchestration |
| `gcc-arm-none-eabi` | `15:14.2.rel1-1` (compiler 14.2.1) | ARM compiler and C++ driver |
| `binutils-arm-none-eabi` | `2.44-3+23+b1` | ARM assembler/linker utilities, including `size` and `nm` |
| `libnewlib-arm-none-eabi` | `4.5.0.20241231-1` | ARM C library headers used while compiling (including `string.h`) |
| `libnewlib-dev` | `4.5.0.20241231-1` | Newlib development files, installed as a dependency |
| `python3` | 3.13.5 | Font and image generation scripts |
| `cc` (Debian GCC) | 14.2.0 | Host tests in `make check` |

On a Debian system, install the project-specific cross toolchain and headers with:

```sh
sudo apt-get install make gcc-arm-none-eabi binutils-arm-none-eabi libnewlib-arm-none-eabi python3 build-essential
```

`make` and `cc` are generally provided by `build-essential`. The firmware build writes the final ELF without linking libc, but this tree includes `<string.h>` and other standard headers; in the observed Debian package set, installing `gcc-arm-none-eabi` alone was insufficient. The first build failed at `src/board.c` with `fatal error: string.h: No such file or directory`; adding `libnewlib-arm-none-eabi` supplied the missing headers. No C++ runtime package was needed by the build.

Optional checks: install `ruff` (preferred) or Python `pyflakes` for Python linting, and `codespell` for spelling checks. They were absent in this build, so `make check` explicitly skipped those two checks. No optional packages were installed for this record.

## Reproduce

From the repository root:

```sh
cd fw
make clean
make
make check
```

The normal build creates `fw/build/HIFIEC70.IMG`. `fw/build/` is ignored by Git; the image and intermediate outputs should remain local. `make check` makes a separate warnings-as-errors build under `fw/build/check/`, checks remaining BSS space and log format arguments, runs the `log` and `msc` host tests, reports linker-dropped functions, and runs optional Python/spelling checks when their tools are installed. `make check-full` additionally builds test variants and fuzzing harnesses; it was not run for this record.

## Observed result

- `make`: **PASS** on the unmodified source at the commit above.
- Output: `fw/build/HIFIEC70.IMG`; `file` identified it as generic `data`; size **2,097,156 bytes**; SHA-256 **`1e93611874f7a43cafabce9f674a0c00f78766ca7357d503473b8c26797ca1b6`**.
- `make check`: **PASS**, 17.776 seconds elapsed. Production warnings-as-errors build passed; BSS had 10,780 bytes free before the DAC ring; log format checks passed; `log_test` and `msc_test` passed; linker report listed zero dropped functions. `ruff`/`pyflakes` and `codespell` were skipped because they were not installed.
- `make check-full`: **NOT RUN**; its test-variant and fuzzing builds are outside this record.
- Emulator/simulation: **NOT RUN**.
- Physical player installation and playback: **NOT TESTED**.

The image date comes from the source commit date embedded by `fw/Makefile` (`IMG_DATE` defaults to the latest commit date), so rebuilding the same source tree is not tied to the wall-clock date. Binary identity can still depend on the toolchain version; use the recorded package versions when comparing hashes.
