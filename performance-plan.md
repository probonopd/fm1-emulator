# Browser performance plan

The WebAssembly build runs at about 0.04–0.05x real time on an Apple M3 Pro
(about 4 MIPS). This plan aims to make it run in real time.

## Where the gap is

- Real time needs 125 MIPS: icount `shift=3` counts 8 ns per instruction. At
  about 4 MIPS the browser build needs a speedup of roughly 25–30x.
- Natively, the CPU core runs tight loops at 130–200 MIPS with the TCG JIT. The
  browser build uses TCI, QEMU's bytecode interpreter, because QEMU 11.1 has
  no WebAssembly code generator (`tcg/` has no wasm backend).
- 4 MIPS is slow even for an interpreter, so the browser build probably adds
  overhead on top of TCI. These suspects are unconfirmed until profiled:
  - TCI calls every helper through `ffi_call` (`tcg/tci.c`). Under Emscripten
    that call goes out to JavaScript (`ffi_call_js`) and is an Asyncify import.
  - `-sASYNCIFY=1` instruments everything that can unwind for the coroutine
    fibers, probably including the `cpu_exec`/TCI loop.
  - QEMU is built at `-O2`, without LTO.
- Felucca spends about 95% of guest time polling for LCD DMA (see
  [docs/performance.md](docs/performance.md)), so most of those 125 MIPS do no
  useful work.

## Steps

Take them cheapest first and measure after each one.

### 0. Measure (about half a day)

- Benchmark: run a fixed `FM1_POC_MAX_INSTRUCTIONS` in Node with the arguments
  from `tests/wasm_boot.mjs`, and profile it with `node --cpu-prof`. In the
  browser, record a performance trace in Chromium through the
  `chrome-devtools` MCP server.
- Build a native TCI binary (`--enable-tcg-interpreter`) and run the
  [docs/performance.md](docs/performance.md) workload on it.
- Comparing native TCI with wasm TCI separates the interpreter's cost from
  the wasm overhead (ffi, Asyncify, flags). That decides whether step 1 or
  step 3 matters more.

### 1. Remove the wasm overhead (1–2 days, estimated 2–4x)

- Replace TCI's `ffi_call` with a direct call chosen by the helper's
  signature. pi32v2 uses only a handful of signatures. Make it an anchored hook in
  `tools/integrate.py`, like the existing ones. A plain function-pointer cast
  will not work, because wasm traps when a `call_indirect` signature does not
  match.
- Use `-sASYNCIFY_REMOVE` (or `-sASYNCIFY_ONLY`) to keep Asyncify out of
  `cpu_exec`, TCI and the device code, limiting it to the coroutine paths.
- Build QEMU at `-O3` and try `-flto`.
- `mise run test_wasm` must still match the native state exactly.

### 2. Skip polling loops (2–4 days, the biggest gain for Felucca, also helps native)

- Detect a TB that loops on itself and only reads an MMIO status register
  (load, test, branch back). The targets are the SPI1, timer and GPIO polls.
- When the polled value cannot change before the next virtual-timer
  deadline, advance icount to that deadline instead of running the loop.
  mGBA and Dolphin both skip idle loops this way.
- Runs stay deterministic, but instruction counts and the `state.json`
  baselines change, so this step needs sign-off first. Validate with
  `panel_smoke`, `stress` and the LCD output instead of byte-identical state.
- This mainly helps idle firmware. Under constant input the native speed
  drops from 2.7x to 1.1x, so real work matters there too and step 3 may
  still be needed.

### 3. A real JIT (weeks, only if steps 1 and 2 fall short)

- Add a TCG backend that compiles hot TBs into small wasm modules and
  instantiates them synchronously. That is allowed because QEMU runs in a
  worker (`PROXY_TO_PTHREAD`). Cold TBs stay on TCI.
- First check whether upstream QEMU has merged a wasm TCG backend after
  11.1. A patch series was posted, and adopting it would be far cheaper than
  writing our own.

## Recommendation

Do step 0, then step 1. Choose between steps 2 and 3 from the measurements.

## Risks

- None of these gains are measured; the step 1 estimate is a guess.
- Step 2 changes the deterministic baselines that `test_wasm` and the
  [docs/performance.md](docs/performance.md) method rely on.
