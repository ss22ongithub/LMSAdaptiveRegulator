"""
Syntax/type check of the kernel-side sources against host/kstub/kstub.h
(an approximation of the Linux kernel API), because a kernel module cannot
be built on a machine without kernel headers.

    python host/syntax_check.py

Runs `gcc -fsyntax-only -Wall -D__KERNEL__ -DMODULE` on every linked source
(ar.c, ar_perfs.c, ar_debugfs.c, master.c, utils.c, models/model-xgb.c, models/model-xgb-fpu.c).
Exit code 1 on any error or warning. Passing is NOT a substitute for `make`
on the Linux box. (The floating-point containment is checked separately by
host/fpu_containment_check.py.)
"""

import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ML_DIR = HERE.parent
STUB = HERE / "kstub"

# Every header kernel_headers.h / ar_debugfs.h include. They are empty here;
# kstub.h (force-included) provides what the module uses.
STUB_HEADERS = """
linux/version.h linux/kernel.h linux/module.h linux/hrtimer.h linux/ktime.h linux/smp.h
linux/irq_work.h linux/hardirq.h linux/perf_event.h linux/delay.h linux/debugfs.h
linux/seq_file.h asm/atomic.h linux/slab.h linux/vmalloc.h linux/uaccess.h linux/notifier.h
linux/kthread.h linux/printk.h linux/interrupt.h linux/trace_events.h linux/cpumask.h
linux/topology.h linux/kfifo.h linux/init.h linux/hw_breakpoint.h linux/kstrtox.h
linux/math64.h asm/fpu/api.h uapi/linux/sched/types.h linux/sched.h asm/mwait.h
asm/cpufeature.h linux/types.h linux/string.h
""".split()

# Sources whose diagnostics must be clean (new or substantially rewritten).
SOURCES = ["models/model-xgb.c", "models/model-xgb-fpu.c", "ar_perfs.c", "ar_debugfs.c", "master.c", "ar.c", "utils.c",
           "probe_main.c"]
STRICT = set(SOURCES)


def main():
    gcc = shutil.which("gcc") or sys.exit("gcc not found on PATH")
    for h in STUB_HEADERS:
        p = STUB / h
        p.parent.mkdir(parents=True, exist_ok=True)
        if not p.exists():
            p.write_text("/* intentionally empty: see kstub.h */\n")

    base = [gcc, "-fsyntax-only", "-std=gnu11", "-Wall", "-D__KERNEL__", "-DMODULE",
            '-DKBUILD_MODNAME="areg"', f"-I{STUB}", f"-I{ML_DIR}",
            "-include", str(STUB / "kstub.h")]
    failed = False
    for src in SOURCES:
        res = subprocess.run(base + [str(ML_DIR / src)], capture_output=True, text=True)
        diag = [l for l in res.stderr.splitlines() if re.search(r"(error|warning):", l)]
        status = "ok" if res.returncode == 0 and not diag else ("FAIL" if res.returncode else "warnings")
        print(f"{src:14s} {status}")
        for line in res.stderr.splitlines():
            print("   " + line)
        if res.returncode != 0 or (diag and src in STRICT):
            failed = True
    print("\nRESULT:", "FAILED" if failed else "syntax check passed (against kstub.h, not a real kernel)")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
