#! /bin/bash
# setup_2_core_env_fedora.sh -- the 2-core LMS-AR test environment, adapted from
# setup_2_core_env.sh for Fedora (cgroup v2, no cset/snapd) and for areg's CPU
# selection (it regulates CPUs 1..N, so the two test cores must be 1 and 2).
#
#   sudo ./setup_2_core_env_fedora.sh      undo with: sudo ./reset_env_fedora.sh
#
# Every change is runtime only: a reboot restores all of it. The original
# values are saved in $STATE so reset_env_fedora.sh can restore them.
#
# Differences from setup_2_core_env.sh:
#   - cores 0,1,2 stay online (not 0,1,3): areg regulates CPU 1 and 2
#   - cset (cgroup v1) -> systemd AllowedCPUs=0 for all system/user tasks;
#     benchmarks are pinned with taskset instead of cset proc -s C1/C3
#   - real-time throttling is LEFT ON (the original sets it to -1): it is what
#     keeps the machine usable if areg's throttler threads hold a core
#   - msr-tools is installed if missing; snapd is skipped if not installed

MSR_REG_HW_PREFETCHER=0x1A4
KEEP_ONLINE="0 1 2"
TAKE_OFFLINE="3 4 5"
SYSTEM_CPU=0
STATE=/run/lmsar_env
SLICES="system.slice user.slice init.scope"
SERVICES="rtkit-daemon cups snapd"

if [ "$EUID" -ne 0 ]; then
  echo "Please run as root or use sudo"
  exit 1
fi

fail=0
run() {   # run "message" cmd args...
   echo ">>> $1"
   shift
   if "$@"; then echo "    DONE"; else echo "    FAILED: $*"; fail=1; fi
}

if lsmod | grep -q "^areg "; then
   echo "areg is loaded: unload it first (sudo rmmod areg), then run this again"
   exit 1
fi
if [ -f $STATE/applied ]; then
   echo "environment already set up (run reset_env_fedora.sh first to redo it)"
   exit 1
fi
mkdir -p $STATE

##############################################################################################################
# 1. MSR access + hardware prefetchers off on all cores (bits 0-3 of 0x1A4:
#    L2 streamer, L2 adjacent line, DCU, DCU IP prefetchers)
command -v wrmsr >/dev/null || run "Install msr-tools (wrmsr/rdmsr)" dnf install -y msr-tools
run "Load msr module" modprobe msr
cat /sys/module/msr/parameters/allow_writes > $STATE/msr_allow_writes
run "Turn on the permission for writing into msr" sh -c "echo on > /sys/module/msr/parameters/allow_writes"
for c in $KEEP_ONLINE $TAKE_OFFLINE; do
   echo "$c $(rdmsr -p $c -0 $MSR_REG_HW_PREFETCHER)"
done > $STATE/prefetcher
run "Disable hardware prefetchers on all cores" wrmsr -a $MSR_REG_HW_PREFETCHER 0xF

##############################################################################################################
# 2. perf: system wide and uncore event collection
cat /proc/sys/kernel/perf_event_paranoid > $STATE/perf_event_paranoid
run "Enable system wide and uncore event collection" sh -c "echo -1 > /proc/sys/kernel/perf_event_paranoid"

# Real-time throttling is intentionally NOT disabled (see header).
echo ">>> Real-time throttling kept on: sched_rt_runtime_us=$(cat /proc/sys/kernel/sched_rt_runtime_us)"

##############################################################################################################
# 3. Retain only cores 0, 1, 2
for c in $TAKE_OFFLINE; do
   run "Disable CPU core $c" sh -c "echo 0 > /sys/devices/system/cpu/cpu$c/online"
done

##############################################################################################################
# 4. Move all system and user tasks to core 0 (what 'cset shield' / 'cset proc -m ... -t system' did)
for s in $SLICES; do
   run "Restrict $s to CPU $SYSTEM_CPU" systemctl set-property --runtime $s AllowedCPUs=$SYSTEM_CPU
done

##############################################################################################################
# 5. Stop unwanted services
: > $STATE/services
for svc in $SERVICES; do
   if ! systemctl list-unit-files "$svc.service" >/dev/null 2>&1 || \
      ! systemctl cat "$svc.service" >/dev/null 2>&1; then
      echo ">>> $svc not installed, skipped"
      continue
   fi
   if systemctl is-active --quiet "$svc"; then
      echo "$svc" >> $STATE/services
      run "Turn off $svc" systemctl stop "$svc"
   else
      echo ">>> $svc already stopped"
   fi
done

touch $STATE/applied

##############################################################################################################
# 6. Verify
echo
echo "================ verification ================"
echo "online CPUs        : $(cat /sys/devices/system/cpu/online)   (want 0-2)"
echo "prefetcher MSR 0x1A4: $(for c in $KEEP_ONLINE; do printf 'cpu%s=%s ' $c $(rdmsr -p $c -0 $MSR_REG_HW_PREFETCHER); done)  (want f)"
echo "perf_event_paranoid: $(cat /proc/sys/kernel/perf_event_paranoid)   (want -1)"
echo "sched_rt_runtime_us: $(cat /proc/sys/kernel/sched_rt_runtime_us)   (kept on)"
for s in $SLICES; do
   f=/sys/fs/cgroup/$s/cpuset.cpus.effective
   echo "$s cpus      : $( [ -f $f ] && cat $f || echo '?')   (want $SYSTEM_CPU)"
   [ -f $f ] && [ "$(cat $f)" != "$SYSTEM_CPU" ] && fail=1
done
# Every user-space task (kernel threads have an empty cmdline) must be allowed on CPU 0 only.
n=0; stray=0
for d in /proc/[0-9]*; do
   head -c1 $d/cmdline 2>/dev/null | grep -q . || continue
   a=$(awk '/Cpus_allowed_list/{print $2}' $d/status 2>/dev/null)
   [ -z "$a" ] && continue
   n=$((n+1))
   if [ "$a" != "$SYSTEM_CPU" ]; then
      stray=$((stray+1)); fail=1
      echo "   $(cat $d/comm 2>/dev/null) allowed on CPUs $a ($(cut -d: -f3 $d/cgroup 2>/dev/null))"
   fi
done
echo "user-space tasks   : $n, allowed outside CPU $SYSTEM_CPU: $stray   (want 0)"

echo
if [ $fail -eq 0 ]; then
   echo "RESULT: 2-core environment ready (system on CPU 0; CPU 1 and 2 free for the workloads)"
else
   echo "RESULT: some steps FAILED (see above). Undo with: sudo ./reset_env_fedora.sh"
fi
exit $fail
