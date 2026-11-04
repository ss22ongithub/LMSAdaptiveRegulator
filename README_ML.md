# LMS-AR with an ML predictor (xgboost_mem_next_1ms)

This is a copy of `LMSAdaptiveRegulator-main` in which the **LMS estimator is replaced by the 1 ms XGBoost
"next-window" model** from `simpleML_memguard` (`xgboost_mem_1ms.py`, notes in `../NOTES_1ms_model3.md`).
- The original folder is untouched and serves as the LMS baseline.
- The previous 100 ms ML version is kept in `../LMSAdaptiveRegulator-ML-100ms/`.

Enforcement works as before: per-core PMC budgets, MWAIT throttling and the global pool. The loop is now a
true 1 ms loop.

## What it does, live
Every 1 ms the master thread (CPU 0) does the following for each regulated core:

1. **Snapshot and re-arm on the core itself.** It sends an IPI through `smp_call_function_single`. The handler,
   `ar_sample_and_rearm()`, runs on that core:
   - It stops the two throttling events, reads all 6 counters and re-arms the budget (MemGuard pattern).
   - It restarts the events.
   - It takes no locks, does no FPU work and does not sleep.
2. **Integer push.** The counter deltas are normalised to exactly 1 ms and pushed with `ml_push()`, which is
   integer only.
3. **Prediction every 10th sample.** Once 20 consecutive samples exist, `ml_predict()` runs one
   `kernel_fpu_begin()/end()` section. It builds 33 features from the last two 10 ms windows and evaluates
   the two forests (400 trees each, depth 6). The output is the **minimum per-ms read and write bandwidth of
   that core over the next 10 ms**, in MiB/s.
4. **Budget.** The prediction becomes the per-class budget:
   `clamp(prediction * ml_scale_pct/100 + ml_headroom_mb, ml_initial_bw_mb, ml_max_bw_mb)`.
   It is armed on the core at the next IPI, 1 ms later.

Before the first prediction (20 ms warm-up) the budget is `ml_initial_bw_mb`. If a sample gap exceeds 2 ms,
or a counter is not running, the history restarts and the current budget is kept. Each prediction is scored
10 ms later against the realised floor, in integer arithmetic, and `ml_stats` shows the running error.

### Counters (the model's inputs = what the regulator throttles on)
| model input | perf event (generic) | PMU resource |
|---|---|---|
| Read BW (`rd`) | `cache-misses` - **read_event, throttled** | 1 GP counter |
| Write BW (`wr`) | `LLC-store-misses` - **write_event** | 1 GP counter + OFFCORE_RSP MSR |
| `ld` | `LLC-load-misses` | 1 GP counter + OFFCORE_RSP MSR |
| `cref` | `cache-references` | 1 GP counter |
| `cyc`, `ins` | `cycles`, `instructions` | fixed counters |

- **Bandwidth here means per-core "calc" bandwidth:** events × 64 B, the `calc_*_bw` columns of
  `bw_dataset_all`. The throttling events were changed from the raw OFFCORE events to these, so that
  prediction, measurement and enforcement use one unit.
- The module parameters `g_read_counter_id` and `g_write_counter_id` are gone.
- **Counter budget:** a core has 4 general-purpose counters with SMT on, 8 with SMT off. The 4 GP events
  above fit, as long as nothing else holds a GP counter.
  - The NMI watchdog normally holds the fixed cycles counter. That pushes our `cycles` event onto a GP counter,
    making 5. Run `sudo sysctl kernel.nmi_watchdog=0`, or disable SMT as in the technical report.
  - If any model counter cannot be created or scheduled, `dmesg` says so and `ml_stats` shows `counters=MISSING`.
    The predictor then refuses to switch on.

## Floating point in the kernel - how it is kept from breaking Linux

The kernel does **not** save user SSE/FPU state on kernel entry. An SSE instruction executed outside a
`kernel_fpu_begin()`/`kernel_fpu_end()` section silently corrupts a user process's registers. A section in
the wrong context (IRQ, sleeping, nested) can hang the machine.

| file | built with SSE? | contains |
|---|---|---|
| `models/model-xgb-fpu.c` (+ `models/xgb/*.h`) | **yes - the only one** | every float/double operation (features, 2 x 400 trees, self-test) |
| `models/model-xgb.c` | no | `ml_init`, `ml_push`, `ml_reset_history` (integer); `ml_predict`, `ml_selftest`: the **only** `kernel_fpu_begin/end` calls |
| `ar.c master.c ar_perfs.c ar_debugfs.c utils.c probe_main.c` | no | no floating point at all (kernel default `-mno-sse ...`) |

- **Section shape.** Each begin/end pair is:
  `if (!irq_fpu_usable()) {skip}; kernel_fpu_begin(); ONE call into models/model-xgb-fpu.c; kernel_fpu_end();`
  There is no `return`/`goto`, nothing that sleeps, and no nesting.
- **Callers.** Only the master kthread (`ml_update_budget`, `ml_sync_enable`) and the probe's init call them.
  None of the IRQ-context functions do: the sampling IPI, the perf overflow callback and the irq_work handler.
- **Frequency.** At 1 ms the FPU is entered once per core per 10 ms, not every ms. The 1 ms path (`ml_push`,
  online scoring) is integer only.
- **No FP state outside `models/model-xgb-fpu.o`.** The predictor state (`struct ml_state`) holds integers only. The unused
  LMS `double` weight arrays were removed from `struct core_info`.
- **No sqrt or library calls.** The features need neither. The SSE entry points take only integers and pointers
  and are `noinline`. Flags are per-object (`CFLAGS_ml_fpu.o`, the amdgpu pattern), and the build refuses
  GCC < 7.1.

Run-time safety nets:
- **`ml_enable` is 0 by default.** While it is 0, the module regulates with the static warm-up budget and
  executes **no floating-point code at all**.
  - `echo 1 > /sys/module/areg/parameters/ml_enable` turns the predictor on.
  - `echo 0` turns it off instantly. This is the kill switch, and budgets return to the warm-up value.
- **Self-test on first enable.** The master thread runs `ml_selftest()`: exact feature arithmetic on known data,
  finite model output and integer conversion. The predictor stays off, with the reason in `dmesg`, if:
  - the self-test fails,
  - `irq_fpu_usable()` is false, or
  - a model counter is missing.
- **FPU unusable at some instant.** If the FPU cannot be used at some moment, `ml_predict()` executes nothing
  floating-point and counts the skip in `fpu_skip`. `ml_stats` also shows the time of the last and the slowest
  FPU section (`infer_us`, `infer_max_us`).

Machine-checked, not just reviewed:
- **`python host/fpu_containment_check.py`** (any machine with gcc) checks:
  - Makefile lint (no global SSE flags).
  - Every begin/end pair.
  - Headers shared with non-SSE objects contain no float/double.
  - IRQ-context functions never reach the FPU path, and only allowed callers call `ml_predict`/`ml_selftest`.
  - **Disassembly of every non-FPU object compiled with floating point forbidden.**
  - Controls, so it cannot pass vacuously.
- **`make check-fpu`** (on the Linux box, after `make`): the same scan on the objects the **real** kernel
  compiler produced.

## Safe first run (do these in order; stop at the first thing that looks wrong)
```
make && make probe && make check-fpu          # check-fpu must say "OK - floating point is confined to models/model-xgb-fpu.o"
sudo insmod ml_fpu_probe.ko                   # 1) FPU path ONLY: no perf events, no threads, no throttling
dmesg | tail                                  #    expect: self-test passed / ns per FPU section /
                                              #    "3 predictions from 45 samples" / FPU probe OK
sudo rmmod ml_fpu_probe
sudo sysctl kernel.nmi_watchdog=0             # frees the fixed cycles counter (see "Counter budget")
sudo insmod areg.ko                           # 2) regulator, ml_enable=0 (default): static budgets, no FPU code
dmesg | tail -30                              #    6 "configured counter" lines per core, no "unavailable"
cat /sys/kernel/debug/ar/ml_stats             #    "# ml_enable=0 selftest=not-run counters=ok"
echo 1 | sudo tee /sys/kernel/debug/ar/enable_regulation
                                              #    run a workload; static budgets, ipi_fail stays 0
echo 1 | sudo tee /sys/module/areg/parameters/ml_enable      # 3) turn the predictor on
dmesg | tail                                  #    expect: "ML self-test passed" then "ML predictor ENABLED"
cat /sys/kernel/debug/ar/ml_stats             #    ready=1 after 20 ms; fpu_skip 0; resets small; infer_max_us small
echo 0 | sudo tee /sys/module/areg/parameters/ml_enable      # kill switch
```
A disk or Timeshift snapshot before step 1 is cheap insurance. The PMU events need real hardware, so a VM
does not fully test this.

## Parameters (module parameters, `/sys/module/areg/parameters/`)
| name | default | meaning |
|---|---|---|
| `ml_enable` | 0 | predictor on/off (runtime); 0 = static budgets, no FPU use |
| `ml_initial_bw_mb` | 500 | warm-up budget and per-class floor (insmod only) |
| `ml_max_bw_mb` | 2500 | per-core cap on read+write (insmod only) |
| `ml_scale_pct` | 100 | multiplies the prediction (runtime) |
| `ml_headroom_mb` | 500 | added to the scaled prediction (runtime) |

All values are MiB/s of per-core calc bandwidth (cache-miss / store-miss lines × 64 B). The defaults were
sized from the OFFCORE-based sustainable bandwidth of the technical report. calc read bandwidth is
roughly 1.2–1.6x IMC read bandwidth for memory-heavy workloads, so **re-measure and re-tune `ml_max_bw_mb`**
in these units.

## Layout
- `models/model-xgb.c/.h`: integer front end, the counter list (`ML_COUNTER_LIST`) and the FPU sections.
- `models/model-xgb-fpu.c/.h`: all floating-point code. It is also built on the host, unchanged.
- `models/xgb/*.h`: emlearn-generated trees. Regenerate them from the project root:
  `python emlearn_c/export_xgb_to_c.py --kernel --models xgboost_mem_next_1ms --out-dir LMSAdaptiveRegulator-ML/ml_models`
- `probe_main.c`: `ml_fpu_probe.ko` (`make probe`), the FPU path only.
- `master.c`: the 1 ms loop, the per-CPU sampling/re-arm IPI and the ML budget update.
- `ar.c`, `ar.h`, `ar_perfs.*`, `ar_debugfs.c`, `Makefile`: integration.
- `model.c/h`: the old LMS code, kept for reference and not linked.
- `check_built_objects.sh`: `make check-fpu`.
- `host/`: everything that can be verified without a kernel (below).

## Verification (no kernel needed; gcc + the project's Python env)
```
python host/verify_replay.py          # unit tests + live replay of run 5 of all 20 workloads vs Python
python host/fpu_containment_check.py  # floating point confined to models/model-xgb-fpu.c, begin/end structure, contexts
python host/syntax_check.py           # kernel-side files compile against host/kstub/kstub.h
```
- `verify_replay.py` streams each run one 1 ms sample at a time through the same `models/model-xgb.c`/`models/model-xgb-fpu.c` that
  are built into the module. It checks that features, forest outputs, integer budgets and the online tracker
  are bit-identical to Python (`xgboost_mem_1ms.py` + `bw1ms_features.py`).
- `syntax_check.py` and the containment check use an *approximation* of the kernel API and a 32-bit host
  compiler. The real build and run happen on the Linux box, hence `make check-fpu` and the probe.

## Known limitations
- **Not yet built or run on a real kernel.** Everything above was checked on the host only.
- **"Write BW" is LLC store misses (RFO traffic), not DRAM write-backs.** For write-back-heavy workloads
  (fotonik3d, lbm) the real DRAM writes are 2–70x larger. No per-core write-back event was recorded in
  `bw_dataset_all`, so the model cannot use one.
- **Kernel-mode counting may differ from the dataset.** The events count user mode only
  (`exclude_kernel = 1`, as in LMS-AR). If the dataset was recorded with kernel counting on, the inputs
  differ slightly, mostly for I/O-heavy phases.
- **The prediction is a floor** (the minimum per-ms value over the next 10 ms). Used as a hard per-ms budget it
  can over-throttle. `ml_headroom_mb` / `ml_scale_pct` exist for that.
- **New budgets apply one period late.** A new budget is armed at the next IPI (1 ms later). Predictions change
  every 10 ms, so this is a 10 % lag.
- **Idle windows were not trained on.** Windows with idle samples (cycles < 100k per ms) were excluded from
  training, so predictions while a core is mostly idle are extrapolations.
- **`ML_*` constants are compile-time on purpose.** The model is tied to 1 ms samples, 10-sample windows and
  this counter list.
- **x86_64 only.** The arm64 branch (`may_use_simd()`) is untested.
