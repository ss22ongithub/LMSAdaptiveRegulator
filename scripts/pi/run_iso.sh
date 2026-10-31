#!/bin/bash
# Runs lbm, bwaves and mcf one at a time, each alone on an isolated core, and
# appends one CSV line per run to <tag>/results.csv.
# usage: ./run_iso.sh <tag> [reps] [core]
#   e.g. ./run_iso.sh baseline 1      (tag names the config: baseline / lms / mlp)

TAG=${1:?usage: run_iso.sh <tag> [reps] [core]}
REPS=${2:-1}
CORE=${3:-3}

BASE=$(cd "$(dirname "$0")" && pwd)
OUT=$BASE/$TAG
SPEC_CPU=$HOME/spec2017/benchspec/CPU
BENCHMARKS="lbm bwaves mcf"

bench_dir() {
  case $1 in
    lbm)    echo "$SPEC_CPU/519.lbm_r/run/run_base_train_mytest-64.0000" ;;
    bwaves) echo "$SPEC_CPU/503.bwaves_r/run/run_base_test_mytest-64.0000" ;;
    mcf)    echo "$SPEC_CPU/505.mcf_r/run/run_base_test_mytest-64.0000" ;;
  esac
}

# Command lines from each run directory's speccmds.cmd
bench_cmd() {
  case $1 in
    lbm)    echo './lbm_r_base.mytest-64 300 reference.dat 0 1 100_100_130_cf_b.of > lbm.out 2>> lbm.err' ;;
    bwaves) echo './bwaves_r_base.mytest-64 bwaves_1 < bwaves_1.in > bwaves_1.out 2>> bwaves_1.err; ./bwaves_r_base.mytest-64 bwaves_2 < bwaves_2.in > bwaves_2.out 2>> bwaves_2.err' ;;
    mcf)    echo './mcf_r_base.mytest-64 inp.in > inp.out 2>> inp.err' ;;
  esac
}

# Count of an event from a perf -x, file, or NA unless it was counted for >= 99%
# of the run (under a loaded module perf only gets slivers of time and scales them up)
perf_value() {
  awk -F, -v ev="$2" '$3 ~ "^"ev { if ($1 ~ /^[0-9]+$/ && $5 + 0 >= 99) print $1; else print "NA" }' "$1"
}

# The core's running totals from the module's debugfs stats file:
# "ticks cycles instructions l2_refill l2_wb reclaims throttles", or empty without a module
mod_stats() {
  sudo -n cat /sys/kernel/debug/ar/stats 2>/dev/null |
    awk -v c="$CORE" '$1 == c { print $2, $3, $4, $5, $6, $7, $8 }'
}

# Module-side counts for one run from before/after totals: ticks, cycles,
# instructions, IPC, read/write MiB/s, reclaims, throttles (NA without a module)
mod_delta() {
  awk -v a="$1" -v b="$2" -v t="$3" 'BEGIN {
    if (split(a, x) < 7 || split(b, y) < 7 || t + 0 <= 0) { print "NA,NA,NA,NA,NA,NA,NA,NA"; exit }
    for (i = 1; i <= 7; i++) d[i] = y[i] - x[i]
    ipc = d[2] > 0 ? sprintf("%.3f", d[3] / d[2]) : "NA"
    printf "%.0f,%.0f,%.0f,%s,%.0f,%.0f,%.0f,%.0f\n", d[1], d[2], d[3], ipc,
           d[4] * 64 / t / 1048576, d[5] * 64 / t / 1048576, d[6], d[7] }'
}

if grep -qv performance /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; then
  echo "CPU governor is not 'performance' on every core: run 'sudo ./pi_check.sh' first"
  exit 1
fi

MODULE=$(cat /sys/module/areg/srcversion 2>/dev/null || echo none)
case $TAG in
  baseline*)
    if [ "$MODULE" != none ]; then
      echo "Tag '$TAG' is a baseline but areg ($MODULE) is loaded: unload it first"
      exit 1
    fi ;;
esac

mkdir -p "$OUT"
CSV=$OUT/results.csv
[ -f "$CSV" ] || echo "tag,bench,run,seconds,cycles_u,instructions_u,ipc,l2_refill_u,l2_wb_u,read_mibps,write_mibps,mod_ticks,mod_cycles,mod_instructions,mod_ipc,mod_read_mibps,mod_write_mibps,mod_reclaims,mod_throttles,module,uptime_start,uptime_end,throttled_pre,temp_pre,clock_pre,throttled_post,temp_post" > "$CSV"

for b in $BENCHMARKS; do
  for r in $(seq 1 "$REPS"); do
    cd "$(bench_dir $b)" || exit 1
    perf_out=$OUT/$b.run$r.perf
    pre="$(vcgencmd get_throttled),$(vcgencmd measure_temp),$(vcgencmd measure_clock arm)"

    # Uptime matches the ftrace timestamps, so the module trace can be cut per run
    up0=$(cut -d' ' -f1 /proc/uptime)
    s0=$(mod_stats)
    t0=$(date +%s.%N)
    # r17/r18 = L2D_CACHE_REFILL / L2D_CACHE_WB: the lines the regulator counts as read/write bandwidth.
    # Under a loaded module these report <not counted> (it holds core 3's counters).
    perf stat -x, -e cycles:u,instructions:u,r17:u,r18:u -o "$perf_out" -- taskset -c "$CORE" bash -c "$(bench_cmd $b)"
    t1=$(date +%s.%N)
    up1=$(cut -d' ' -f1 /proc/uptime)
    s1=$(mod_stats)

    post="$(vcgencmd get_throttled),$(vcgencmd measure_temp)"
    cyc=$(perf_value "$perf_out" cycles)
    ins=$(perf_value "$perf_out" instructions)
    secs=$(awk -v a="$t0" -v b="$t1" 'BEGIN { printf "%.3f", b - a }')
    refill=$(perf_value "$perf_out" r17)
    wb=$(perf_value "$perf_out" r18)
    rd_bw=$(awk -v n="$refill" -v t="$secs" 'BEGIN { if (n + 0 > 0 && t + 0 > 0) printf "%.0f", n * 64 / t / 1048576; else print "NA" }')
    wr_bw=$(awk -v n="$wb" -v t="$secs" 'BEGIN { if (n + 0 > 0 && t + 0 > 0) printf "%.0f", n * 64 / t / 1048576; else print "NA" }')
    ipc=$(awk -v i="$ins" -v c="$cyc" 'BEGIN { if (c + 0 > 0) printf "%.3f", i / c; else print "NA" }')

    mod=$(mod_delta "$s0" "$s1" "$secs")

    echo "$TAG,$b,$r,$secs,$cyc,$ins,$ipc,$refill,$wb,$rd_bw,$wr_bw,$mod,$MODULE,$up0,$up1,$pre,$post" | tee -a "$CSV"
  done
done
echo "$TAG DONE"
