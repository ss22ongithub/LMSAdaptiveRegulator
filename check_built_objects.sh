#!/bin/sh
# check_built_objects.sh -- run on the Linux box AFTER `make` (and `make probe`).
#
# Disassembles the objects the real kernel build produced and verifies that
# ONLY models/model-xgb-fpu.o contains SSE / x87 / MMX instructions. Any other object with
# one means floating-point code can run outside kernel_fpu_begin()/end(),
# which corrupts user FPU state and can crash the machine: DO NOT load such a
# module. (host/fpu_containment_check.py proves the same on the sources with a
# stand-in compiler; this checks the real compiler's output.)
#
#   sh check_built_objects.sh        # or: make check-fpu
cd "$(dirname "$0")" || exit 2
command -v objdump >/dev/null 2>&1 || { echo "objdump not found"; exit 2; }

count_fp() {
    # objdump -d lines are  "addr:<TAB>bytes<TAB>mnemonic operands"
    objdump -d "$1" | awk -F'\t' '
        NF >= 3 {
            split($3, a, " "); m = a[1]
            if (m ~ /^f/ || m ~ /^(emms|ldmxcsr|stmxcsr|xsave|xrstor)/ || $3 ~ /%(xmm|ymm|zmm|mm[0-7]|st)/)
                c++
        }
        END { print c + 0 }'
}

bad=0
checked=0
for o in ar.o ar_debugfs.o ar_perfs.o models/model-xgb.o master.o utils.o probe_main.o; do
    [ -f "$o" ] || { echo "skip   $o (not built)"; continue; }
    n=$(count_fp "$o")
    checked=$((checked + 1))
    if [ "$n" -eq 0 ]; then
        echo "ok     $o: no FP/SSE instructions"
    else
        echo "LEAK   $o: $n FP/SSE instructions -- FP code outside models/model-xgb-fpu.o"
        bad=1
    fi
done

if [ -f models/model-xgb-fpu.o ]; then
    n=$(count_fp models/model-xgb-fpu.o)
    if [ "$n" -gt 0 ]; then
        echo "ok     models/model-xgb-fpu.o: $n SSE instructions (expected: this is the one FP object)"
    else
        echo "ERROR  models/model-xgb-fpu.o has NO SSE instructions: it was not built with SSE as intended"
        bad=1
    fi
else
    echo "ERROR  models/model-xgb-fpu.o not found (run make first)"
    bad=1
fi


# ---------------------------------------------------------------------------
# FPU section order in the REAL compiled code.
#
# The disassembly above proves WHERE the FP instructions are; this proves
# WHEN they can run. In models/model-xgb.o, ml_predict() and ml_selftest()
# must call, in this order and exactly once each:
#     irq_fpu_usable -> kernel_fpu_begin[_mask] -> ml_*_fpu -> kernel_fpu_end
# and no other function in any object may call kernel_fpu_begin/end or the
# ml_*_fpu entry points (the float object itself must not call them either).
# ---------------------------------------------------------------------------

# Prints "function symbol" for every relocation that is a call/jump target.
call_targets() {
    objdump -dr "$1" | awk '
        /^[0-9a-f]+ <[^>]+>:$/ { fn = $2; gsub(/[<>:]/, "", fn); next }
        /R_X86_64_(PLT32|PC32)/ {
            sym = $NF; sub(/[-+]0x[0-9a-f]+$/, "", sym)
            print fn, sym
        }'
}

fpu_seq() {   # $1 = object, $2 = function -> relevant calls in address order
    call_targets "$1" | awk -v f="$2" '
        $1 == f && ($2 ~ /^(irq_fpu_usable|kernel_fpu_begin(_mask)?|kernel_fpu_end)$/ || $2 ~ /^ml_.*fpu/) {
            printf "%s%s", sep, $2; sep = " -> " }'
}

FPU_API=models/model-xgb.o
if [ -f "$FPU_API" ]; then
    for pair in "ml_predict ml_predict_fpu" "ml_selftest ml_selftest_fpu"; do
        set -- $pair
        got=$(fpu_seq "$FPU_API" "$1")
        want_a="irq_fpu_usable -> kernel_fpu_begin_mask -> $2 -> kernel_fpu_end"
        want_b="irq_fpu_usable -> kernel_fpu_begin -> $2 -> kernel_fpu_end"
        if [ "$got" = "$want_a" ] || [ "$got" = "$want_b" ]; then
            echo "ok     $FPU_API $1(): $got"
        else
            echo "ERROR  $FPU_API $1(): got [$got], expected [$want_a]"
            bad=1
        fi
    done
else
    echo "ERROR  $FPU_API not found (run make first)"
    bad=1
fi

# Nobody else may open an FPU section or call into the float code directly.
for o in ar.o ar_debugfs.o ar_perfs.o models/model-xgb.o models/model-xgb-fpu.o master.o utils.o probe_main.o; do
    [ -f "$o" ] || continue
    stray=$(call_targets "$o" | awk -v o="$o" '
        $2 ~ /^(kernel_fpu_begin(_mask)?|kernel_fpu_end)$/ || $2 ~ /^ml_.*_fpu$/ {
            if (o == "models/model-xgb.o" && ($1 == "ml_predict" || $1 == "ml_selftest")) next
            if (o == "models/model-xgb-fpu.o" && $2 ~ /^ml_.*_fpu$/) next   # its own internal calls
            print $1 "() calls " $2 }' | sort -u)
    if [ -n "$stray" ]; then
        echo "LEAK   $o: FPU section / float entry point used outside ml_predict()/ml_selftest():"
        echo "$stray" | sed 's/^/         /'
        bad=1
    fi
done
if [ -f models/model-xgb-fpu.o ] && call_targets models/model-xgb-fpu.o | awk '$2 ~ /^(kernel_fpu_|irq_fpu_usable)/' | grep -q .; then
    echo "ERROR  models/model-xgb-fpu.o calls kernel_fpu_*/irq_fpu_usable itself (must only be called from inside a section)"
    bad=1
fi

[ "$checked" -gt 0 ] || { echo "no objects found: run make first"; exit 2; }
if [ "$bad" -eq 0 ]; then
    echo "RESULT: OK - floating point is confined to models/model-xgb-fpu.o and only runs inside irq_fpu_usable+kernel_fpu_begin/end in ml_predict/ml_selftest"
else
    echo "RESULT: FAILED - do not insmod this build"
fi
exit "$bad"
