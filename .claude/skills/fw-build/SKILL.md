---
name: fw-build
description: Build the Community OS for the host, QEMU or the MK66 and check warnings, tests and flash/SRAM usage. Use whenever you need to know if the tree compiles, how many bytes a change costs, or before reporting or committing any change.
---

# Build the OS

Single CI entry point; the same script runs in the GitHub workflow and on a developer machine. It never flashes and never copies an image anywhere.

```sh
nice -n 10 tools/ci.sh host     # ASan + UBSan host build, then every ctest
nice -n 10 tools/ci.sh qemu     # unit suites cross-built for the Cortex-M4F, run on QEMU mps2-an386
nice -n 10 tools/ci.sh mk66     # MK66 libraries, the skeleton image and its static checks, then the tone image (build only)
tools/ci.sh toolchain           # unpack the pinned Arm GNU toolchain into .toolchains/ (checks its SHA-256 first)
```

- Every run starts from a fresh `build-os-ci-<mode>/` (logs `ci-configure.log` and `ci-build.log` stay inside). `O3OS_CI_KEEP=1` reuses the directory while iterating; it can hide a warning in an up-to-date file, so never use it for a final result.
- It fails on a configure or build error, on ANY warning in the configure or build output, on a failing ctest and on an empty ctest list.
- `O3OS_CI_SCOPE=changed` runs only the ctests whose area label the changed files map to (the labels it chose are printed; unmapped files run everything; docs-only changes run nothing). It is for intermediate runs. A full run (scope unset) is required before an image and before a merge into the integration branch.
- Jobs default to 4 and the script lowers its own priority; keep `nice -n 10` anyway. A lane run outside the CI lock sets `O3OS_CI_JOBS=4`. The lock holder (full run, `mk66` or `qemu`, under `flock "$(git rev-parse --git-common-dir)/../.claude/worktrees/.ci.lock"`) sets `O3OS_CI_JOBS=10`, which sets build and ctest parallelism. Run `host`, `qemu` and `mk66` as separate calls, so one failure does not hide the others.
- ccache covers every mode when installed (host, qemu, mk66). The script sets `CCACHE_BASEDIR` to the repository root so worktrees share the cache; the cache limit is 30 GB.

## Quick loop on the host

```sh
cmake -S . -B build-os-host -DO3OS_SANITIZE=ON
nice -n 10 cmake --build build-os-host -j4
ctest --test-dir build-os-host --output-on-failure -j2
```

`build-os-host/compile_commands.json` feeds the syntax-check hook (when installed). A new test must be registered in ctest and get an area label (`cmake/O3osLabels.cmake`; a test without one stops the configure).

## Reading the result

- Exit 0 means built, warning-free and every test passed. Quote the ctest summary line ("100% tests passed, 0 tests failed out of N").
- Flash and SRAM: the MK66 build prints the linker usage table (`FLASH`, `SRAM_U`, `SDRAM_STATIC`) in `build-os-ci-mk66/ci-build.log`, followed by `size -A` of the image. Quote used bytes and the delta against the previous build or the figure in the work item. The limits are strict: a "region overflowed" error is a budget failure.
- Image checks (HEX records, address range, vectors, flash configuration field) run as ctests of the MK66 build; for the release gate see skill `fw-gate-check`.

## When it fails

Read the first error in `ci-build.log`, not the last. For a size problem, get the top consumers with
`arm-none-eabi-nm --size-sort -C --radix=d build-os-ci-mk66/platform/mk66/o3os.elf | tail -40` (the toolchain's `bin/` is documented in `docs/toolchain.md`).
