#!/bin/bash
# Prepares the Pi for a benchmark session and prints its state.
# usage: sudo ./pi_check.sh

if [ "$EUID" -ne 0 ]; then
  echo "Please run as root or use sudo"
  exit 1
fi

# Fixed CPU frequency: ondemand scaling adds noise to runtime and IPC
for gov in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
  echo performance > "$gov"
done

echo "== date:      $(date)"
echo "== governor:  $(sort /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor | uniq -c | xargs)"
echo "== clock:     $(vcgencmd measure_clock arm)"
echo "== power:     $(vcgencmd get_throttled)   (anything but 0x0 = under-voltage/throttling)"
echo "== temp:      $(vcgencmd measure_temp)"
echo "== isolated:  $(cat /sys/devices/system/cpu/isolated)"
echo "== areg:      $(cat /sys/module/areg/srcversion 2>/dev/null || echo 'not loaded')"
