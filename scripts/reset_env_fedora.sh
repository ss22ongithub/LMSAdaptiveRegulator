#! /bin/bash
# reset_env_fedora.sh -- undo setup_2_core_env_fedora.sh (a reboot does the same).
#   sudo ./reset_env_fedora.sh

MSR_REG_HW_PREFETCHER=0x1A4
STATE=/run/lmsar_env
SLICES="system.slice user.slice init.scope"

if [ "$EUID" -ne 0 ]; then
  echo "Please run as root"
  exit 1
fi

fail=0
run() {
   echo ">>> $1"
   shift
   if "$@"; then echo "    DONE"; else echo "    FAILED: $*"; fail=1; fi
}

for c in 1 2 3 4 5; do
   [ "$(cat /sys/devices/system/cpu/cpu$c/online)" = 1 ] && continue
   run "Enable CPU core $c" sh -c "echo 1 > /sys/devices/system/cpu/cpu$c/online"
done

for s in $SLICES; do
   run "Allow $s on all CPUs again" systemctl set-property --runtime $s AllowedCPUs=
done

if [ -f $STATE/prefetcher ]; then
   while read -r c v; do
      run "Restore prefetcher MSR on CPU $c (0x$v)" wrmsr -p "$c" $MSR_REG_HW_PREFETCHER "0x$v"
   done < $STATE/prefetcher
else
   run "Re-enable hardware prefetchers on all cores" wrmsr -a $MSR_REG_HW_PREFETCHER 0x0
fi
[ -f $STATE/msr_allow_writes ] && \
   run "Restore msr allow_writes" sh -c "cat $STATE/msr_allow_writes > /sys/module/msr/parameters/allow_writes"

[ -f $STATE/perf_event_paranoid ] && \
   run "Restore perf_event_paranoid" sh -c "cat $STATE/perf_event_paranoid > /proc/sys/kernel/perf_event_paranoid"

if [ -f $STATE/services ]; then
   for svc in $(cat $STATE/services); do
      run "Restart $svc" systemctl start "$svc"
   done
fi

[ $fail -eq 0 ] && rm -rf $STATE

echo
echo "online CPUs        : $(cat /sys/devices/system/cpu/online)"
echo "prefetcher MSR 0x1A4: $(rdmsr -a -0 $MSR_REG_HW_PREFETCHER 2>/dev/null | tr '\n' ' ')"
echo "perf_event_paranoid: $(cat /proc/sys/kernel/perf_event_paranoid)"
for s in $SLICES; do
   echo "$s cpus      : $(cat /sys/fs/cgroup/$s/cpuset.cpus.effective 2>/dev/null)"
done
echo "RESULT: $([ $fail -eq 0 ] && echo 'environment reset' || echo 'some steps FAILED (see above); a reboot also resets everything')"
exit $fail
