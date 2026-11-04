"""
FPU containment check: proves, mechanically, that floating-point / SSE code
can only execute inside kernel_fpu_begin()/kernel_fpu_end() sections.

Why this matters. The Linux kernel does not save user FPU/SSE state on kernel
entry. Any SSE instruction run outside a begin/end pair silently corrupts the
registers of whichever user process was running (and begin/end in the wrong
context can hang the machine). The original LMS-AR Makefile enabled SSE for
EVERY object, so the compiler was free to emit SSE anywhere. The ML port
instead confines all FP code to models/model-xgb-fpu.c and wraps it in models/model-xgb.c.

Checks (all must pass; the script exits 1 otherwise):

  A. Makefile / ar_makefile
     - no global SSE/FP flags (ccflags-y, subdir-ccflags-y, KBUILD_CFLAGS,
       EXTRA_CFLAGS); SSE flags appear ONLY in CFLAGS_ml_fpu.o
     - the linked objects include models/model-xgb-fpu.o and models/model-xgb.o, not model.o (the old
       LMS code, which has FP in many functions)

  B. Source structure (comments stripped)
     - kernel_fpu_begin/end/irq_fpu_usable appear only in models/model-xgb.c
     - in models/model-xgb.c every begin is immediately followed by ONE call statement
       to an ml_*_fpu* function and then kernel_fpu_end(): no return / goto /
       break, no sleeping call, no nesting; begin and end counts are equal;
       irq_fpu_usable() is tested before each begin
     - the SSE entry points in models/model-xgb-fpu.c are noinline
     - ML_FPU_INTERNAL (the FP-signature API) is defined only in models/model-xgb-fpu.c
     - the headers shared with non-SSE objects (models/model-xgb.h, models/model-xgb-fpu.h, ar.h)
       contain no float/double at all
     - functions that run in IRQ context (the per-CPU sampling IPI, the
       perf overflow callback, irq_work handlers) do not call into the FPU
       path, and ml_predict()/ml_selftest() are called only from the
       allowed process-context functions (master kthread, probe init)

  C. Machine code
     - every linked source other than models/model-xgb-fpu.c is compiled with the kernel's
       "no FP at all" flags (soft-float, no SSE/MMX/x87) and its disassembly
       is scanned: any x87/SSE/MMX instruction or soft-float library call
       (__adddf3, __floatdidf, ...) means FP code leaked out of models/model-xgb-fpu.c
     - in models/model-xgb.o, ml_step and ml_selftest call, in order,
       irq_fpu_usable, kernel_fpu_begin, ONE ml_*_fpu function, kernel_fpu_end

  D. Controls (so a broken checker cannot pass silently)
     - models/model-xgb-fpu.c compiled with SSE must be flagged by the same scanner
     - a deliberately bad file (a double multiply) must be flagged
     - bad begin/end snippets (early return, two calls, missing end) must be
       flagged by the structure check

Usage: python host/fpu_containment_check.py
Writes: host/fpu_containment_report.txt
"""

import re
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ML_DIR = HERE.parent
STUB = HERE / "kstub"
BUILD = HERE / "build" / "containment"
REPORT = HERE / "fpu_containment_report.txt"

FPU_FILE = "models/model-xgb-fpu.c"
API_FILE = "models/model-xgb.c"

# The only functions in models/model-xgb-fpu.c that models/model-xgb.c calls inside an FPU section.
FPU_ENTRY_POINTS = ("ml_predict_fpu", "ml_selftest_fpu")
# Integer API -> FPU entry point it wraps (call order checked in models/model-xgb.o).
FPU_WRAPPERS = (("ml_predict", "ml_predict_fpu"), ("ml_selftest", "ml_selftest_fpu"))
# Headers that objects built WITHOUT SSE include.
SHARED_HEADERS = ("models/model-xgb.h", "models/model-xgb-fpu.h", "ar.h")
# Functions that run in hard-IRQ context (IPI, perf overflow, irq_work).
IRQ_CONTEXT_FUNCS = (
    ("master.c", "ar_sample_and_rearm"),
    ("ar.c", "read_event_overflow_callback"),
    ("ar.c", "ar_handle_read_overflow"),
    ("ar_perfs.c", "llc_miss_event_irq_work_handler"),
    ("ar_perfs.c", "event_read_overflow_callback"),
)
# Process-context functions allowed to call ml_predict() / ml_selftest().
FPU_CALLERS = {
    ("master.c", "ml_update_budget"),   # master kthread, once per core per 10 ms
    ("master.c", "ml_sync_enable"),     # master kthread, self-test on first enable
    ("probe_main.c", "probe_init"),     # insmod (process context)
}

# Same flags the kernel gives every ordinary object, strengthened to
# "no floating point of any kind" so any FP use shows up as a libcall.
NOFP_FLAGS = ["-O2", "-std=gnu11", "-msoft-float", "-mno-sse", "-mno-sse2", "-mno-mmx", "-mno-80387"]
KERNEL_DEFS = ["-D__KERNEL__", "-DMODULE", '-DKBUILD_MODNAME="areg"']

results = []          # (check name, ok, detail)


def record(name, ok, detail=""):
    results.append((name, ok, detail))
    print(f"[{'PASS' if ok else 'FAIL'}] {name}" + (f"\n         {detail}" if detail and not ok else ""))


# ----------------------------------------------------------------------
# helpers
# ----------------------------------------------------------------------

def strip_comments(text):
    """Removes comments, string literals and #define lines, leaving only code
    (models/model-xgb.c defines no-op FPU macros for the host build; those are not FPU
    sections, and log messages may mention the function names)."""
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", " ", text)
    text = re.sub(r'"(?:\\.|[^"\\\n])*"', '""', text)
    return re.sub(r"^[ \t]*#[ \t]*define[^\n]*", " ", text, flags=re.M)


def linked_sources(makefile):
    text = makefile.read_text()
    m = re.search(r"-objs\s*:?=\s*(.+)", text)
    if not m:
        return None
    return [o[:-2] + ".c" for o in m.group(1).split() if o.endswith(".o")]


# ----------------------------------------------------------------------
# A. Makefiles
# ----------------------------------------------------------------------

GLOBAL_FP_FLAGS = re.compile(r"-m(?:sse|hard-float|avx|mmx|fpu|float-abi)|-mfloat")
GLOBAL_VARS = re.compile(r"^\s*(?:ccflags-y|subdir-ccflags-y|KBUILD_CFLAGS|EXTRA_CFLAGS|ccflags-)\b")


def lint_makefile(path):
    problems = []
    for n, raw in enumerate(path.read_text().splitlines(), 1):
        line = raw.split("#", 1)[0]
        if GLOBAL_VARS.match(line) and GLOBAL_FP_FLAGS.search(line):
            problems.append(f"{path.name}:{n}: global FP/SSE flag: {raw.strip()}")
        m = re.match(r"^\s*CFLAGS_(\S+?)\.o\s*[:+]?=(.*)", line)
        if m and GLOBAL_FP_FLAGS.search(m.group(2)) and m.group(1) != "models/model-xgb-fpu":
            problems.append(f"{path.name}:{n}: SSE flags on {m.group(1)}.o (only models/model-xgb-fpu.o may have them)")
    objs = linked_sources(path)
    if objs is None:
        problems.append(f"{path.name}: no -objs line found")
    else:
        for need in (FPU_FILE, API_FILE):
            if need not in objs:
                problems.append(f"{path.name}: {need[:-2]}.o is not linked")
        if "model.c" in objs:
            problems.append(f"{path.name}: model.o (old LMS code with FP) is linked")
    if not re.search(r"^\s*CFLAGS_models/model-xgb-fpu\.o\s*[:+]?=.*-msse2", path.read_text(), flags=re.M):
        problems.append(f"{path.name}: CFLAGS_ml_fpu.o does not enable -msse2")
    probe = re.search(r"ml_fpu_probe-objs\s*:?=\s*(.+)", path.read_text())
    if probe:   # the FPU-path-only probe module (Makefile only)
        want = {"probe_main.o", "models/model-xgb.o", "models/model-xgb-fpu.o"}
        if set(probe.group(1).split()) != want:
            problems.append(f"{path.name}: ml_fpu_probe-objs is {probe.group(1).split()}, expected {sorted(want)}")
    return problems


# ----------------------------------------------------------------------
# B. source structure
# ----------------------------------------------------------------------

SECTION_RE = re.compile(r"kernel_fpu_begin\(\);\s*([^;{}]*);\s*kernel_fpu_end\(\);")
FPU_CALL_RE = re.compile(r"^(?:\w+\s*=\s*)?(ml_\w*fpu\w*)\s*\(")


def check_sections(src, name="models/model-xgb.c"):
    """Returns a list of problems in the begin/end structure of `src`."""
    code = strip_comments(src)
    probs = []
    begins = [m.start() for m in re.finditer(r"kernel_fpu_begin\s*\(", code)]
    ends = len(re.findall(r"kernel_fpu_end\s*\(", code))
    if len(begins) != ends:
        probs.append(f"{name}: {len(begins)} begin vs {ends} end")
    good = SECTION_RE.findall(code)
    if len(good) != len(begins):
        probs.append(f"{name}: {len(begins) - len(good)} section(s) are not 'begin; ONE statement; end'")
    for stmt in good:
        if not FPU_CALL_RE.match(stmt.strip()):
            probs.append(f"{name}: statement inside a section is not a call to ml_*fpu*: '{stmt.strip()[:60]}'")
        if re.search(r"\b(return|goto|break|continue)\b", stmt):
            probs.append(f"{name}: control flow inside a section: '{stmt.strip()[:60]}'")
    for b in begins:
        before = code[max(0, b - 400):b]
        if "irq_fpu_usable" not in before:
            probs.append(f"{name}: a kernel_fpu_begin() is not preceded by an irq_fpu_usable() test")
    return probs


def structure_checks(sources):
    probs = []
    for f in sources:
        code = strip_comments((ML_DIR / f).read_text())
        if f != API_FILE and re.search(r"kernel_fpu_(?:begin|end)|irq_fpu_usable", code):
            probs.append(f"{f}: uses kernel_fpu_*/irq_fpu_usable (allowed only in {API_FILE})")
        if f != FPU_FILE and "ML_FPU_INTERNAL" in code:
            probs.append(f"{f}: defines/uses ML_FPU_INTERNAL (FP-signature API) outside {FPU_FILE}")
        if f != FPU_FILE and re.search(r"\b(?:float|double)\b", code):
            # allowed only as members of struct ml_state (models/model-xgb.h), never in .c code
            probs.append(f"{f}: mentions float/double")
    probs += check_sections((ML_DIR / API_FILE).read_text())
    fpu_src = strip_comments((ML_DIR / FPU_FILE).read_text())
    for fn in FPU_ENTRY_POINTS:
        if not re.search(r"ML_NOINLINE\s+" + fn, fpu_src) and not re.search(r"ML_NOINLINE\s+\w+\s+" + fn, fpu_src):
            probs.append(f"{FPU_FILE}: {fn} is not marked ML_NOINLINE")

    # Headers included by non-SSE objects must not declare FP state either
    # (1 ms version: the predictor state is integers only).
    # (The `#ifdef ML_FPU_INTERNAL` block of models/model-xgb-fpu.h is compiled only where
    # ML_FPU_INTERNAL is defined, i.e. in models/model-xgb-fpu.c -- checked above.)
    for h in SHARED_HEADERS:
        text = strip_comments((ML_DIR / h).read_text())
        text = re.sub(r"#\s*ifdef\s+ML_FPU_INTERNAL\b.*?#\s*endif", " ", text, flags=re.S)
        if re.search(r"\b(?:float|double)\b", text):
            probs.append(f"{h}: declares float/double (included by objects built without SSE)")

    # Context: nothing that runs in IRQ context may reach the FPU path, and
    # the FPU entry points are called only from the allowed process-context
    # functions.
    for f, fn in IRQ_CONTEXT_FUNCS:
        body = function_body(strip_comments((ML_DIR / f).read_text()), fn)
        if body is None:
            probs.append(f"{f}: IRQ-context function {fn}() not found (update IRQ_CONTEXT_FUNCS)")
        elif re.search(r"\b(?:ml_predict|ml_selftest|kernel_fpu_\w+|ml_\w*fpu\w*)\s*\(", body):
            probs.append(f"{f}: {fn}() runs in IRQ context but calls into the FPU path")
    for f in sources:
        code = strip_comments((ML_DIR / f).read_text())
        for m in re.finditer(r"\b(ml_predict|ml_selftest)\s*\(", code):
            caller = enclosing_function(code, m.start())
            if caller in (None, "ml_predict", "ml_selftest") and f == API_FILE:
                continue                            # the definitions themselves
            if (f, caller) not in FPU_CALLERS:
                probs.append(f"{f}: {m.group(1)}() called from {caller}(), not an allowed "
                             f"process-context caller {sorted(FPU_CALLERS)}")
    return probs


def function_body(code, name):
    """Text of the body of function `name` (brace matched), or None."""
    m = re.search(r"\b" + re.escape(name) + r"\s*\([^;{)]*\)\s*\{", code)
    if not m:
        return None
    depth, i = 0, m.end() - 1
    for j in range(i, len(code)):
        if code[j] == "{":
            depth += 1
        elif code[j] == "}":
            depth -= 1
            if depth == 0:
                return code[i:j + 1]
    return None


FUNC_DEF_RE = re.compile(r"^[A-Za-z_][\w \t\*]*?\b(\w+)\s*\([^;{)]*\)\s*\{", re.M)


def enclosing_function(code, pos):
    """Name of the top-level function definition whose body contains
    offset `pos` (definitions start in column 0 in these sources)."""
    for m in FUNC_DEF_RE.finditer(code):
        depth, start = 0, m.end() - 1
        for j in range(start, len(code)):
            if code[j] == "{":
                depth += 1
            elif code[j] == "}":
                depth -= 1
                if depth == 0:
                    break
        if m.start() <= pos <= j:
            return m.group(1)
    return None


# ----------------------------------------------------------------------
# C. machine code
# ----------------------------------------------------------------------

INSN_RE = re.compile(r"^\s*[0-9a-f]+:\t[0-9a-f ]+?\s*\t(\S+)\s*(.*)$")
RELOC_RE = re.compile(r"^\s+[0-9a-f]+:\s+([A-Za-z][A-Za-z0-9_]*)\s+(\S+)\s*$")
FUNC_RE = re.compile(r"^[0-9a-f]+ <(.+)>:$")
# libgcc soft-float helpers (with all leading underscores stripped): __adddf3,
# __ltdf2, __floatsidf, __floatundidf, __fixdfdi, __fixunsdfsi, __extendsfdf2, ...
SOFTFP_NAME = re.compile(
    r"^(?:(?:add|sub|mul|div)[sd]f3|(?:neg|eq|ne|lt|le|gt|ge|unord|cmp)[sd]f2"
    r"|float(?:un)?[sdt]i[sdt]f|fix(?:uns)?[sd]f[sd]i|extendsfdf2|truncdfsf2)$")
FP_OPERANDS = re.compile(r"%(?:xmm|ymm|zmm|mm[0-7]|st\b|st\()")
FP_MNEMONIC = re.compile(r"^(?:f\w+|emms|ldmxcsr|stmxcsr|xsave\w*|xrstor\w*)$")


def disassemble(obj):
    res = subprocess.run(["objdump", "-dr", str(obj)], capture_output=True, text=True)
    if res.returncode != 0:
        sys.exit(f"objdump failed on {obj}: {res.stderr}")
    return res.stdout.splitlines()


def scan_fp(lines):
    """Returns (fp instructions, soft-float libcalls) found in a disassembly."""
    insns, calls = [], []
    for ln in lines:
        m = INSN_RE.match(ln)
        if m:
            mnem, ops = m.group(1), m.group(2)
            if FP_MNEMONIC.match(mnem) or FP_OPERANDS.search(ops):
                insns.append(f"{mnem} {ops}".strip())
            continue
        r = RELOC_RE.match(ln)
        if r and not re.fullmatch(r"[0-9a-f]{2}", r.group(1)):   # a relocation, not an instruction
            if SOFTFP_NAME.match(r.group(2).lstrip("_")):
                calls.append(r.group(2))
    return insns, calls


def compile_obj(gcc, src, out, flags):
    cmd = [gcc, "-c", *flags, *KERNEL_DEFS, f"-I{STUB}", f"-I{ML_DIR}",
           "-include", str(STUB / "kstub.h"), str(src), "-o", str(out)]
    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode != 0:
        sys.exit(f"compile failed: {' '.join(cmd)}\n{res.stderr}")
    return out


def call_sequence(lines, func):
    """Relocation targets (calls) inside `func`, in address order."""
    seq, inside = [], False
    for ln in lines:
        f = FUNC_RE.match(ln)
        if f:
            inside = f.group(1).lstrip("_") == func
            continue
        if inside:
            r = RELOC_RE.match(ln)
            if r and not re.fullmatch(r"[0-9a-f]{2}", r.group(1)):
                seq.append(r.group(2).lstrip("_"))
    return seq


# ----------------------------------------------------------------------

def main():
    gcc = shutil.which("gcc") or sys.exit("gcc not found on PATH")
    shutil.which("objdump") or sys.exit("objdump not found on PATH")
    BUILD.mkdir(parents=True, exist_ok=True)
    from syntax_check import STUB_HEADERS                  # the same empty kernel-header stubs
    for h in STUB_HEADERS:
        p = STUB / h
        p.parent.mkdir(parents=True, exist_ok=True)
        if not p.exists():
            p.write_text("/* intentionally empty: see kstub.h */\n")

    # ---- A ----
    for mk in ("Makefile", "ar_makefile"):
        probs = lint_makefile(ML_DIR / mk)
        record(f"A. {mk}: SSE only for models/model-xgb-fpu.o, right objects linked", not probs, "; ".join(probs))

    # every .c that goes into areg.ko, plus the probe module's own file
    sources = linked_sources(ML_DIR / "Makefile") + ["probe_main.c"]

    # ---- B ----
    probs = structure_checks(sources)
    record("B. source structure (begin / ONE call / end, guards, no FP outside models/model-xgb-fpu.c)", not probs, "; ".join(probs))

    # ---- C ----
    leaks = []
    for f in sources:
        if f == FPU_FILE:
            continue
        obj = compile_obj(gcc, ML_DIR / f, BUILD / (Path(f).stem + ".nofp.o"), NOFP_FLAGS)
        insns, calls = scan_fp(disassemble(obj))
        if insns or calls:
            leaks.append(f"{f}: {len(insns)} FP/SSE instructions {insns[:3]}, soft-float calls {calls[:3]}")
    record(f"C. no FP/SSE instruction or soft-float call in any of the {len(sources) - 1} non-FPU objects",
           not leaks, "; ".join(leaks))

    api_lines = disassemble(BUILD / "model-xgb.nofp.o")
    order_probs = []
    for fn, expect_mid in FPU_WRAPPERS:
        seq = [s for s in call_sequence(api_lines, fn)
               if s in ("irq_fpu_usable", "kernel_fpu_begin", "kernel_fpu_end") or re.match(r"ml_\w*fpu\w*", s)]
        want = ["irq_fpu_usable", "kernel_fpu_begin", expect_mid, "kernel_fpu_end"]
        if seq != want:
            order_probs.append(f"{fn}: call order {seq}, expected {want}")
    record("C. models/model-xgb.o: irq_fpu_usable -> kernel_fpu_begin -> ONE ml_*_fpu call -> kernel_fpu_end",
           not order_probs, "; ".join(order_probs))

    # ---- D (controls) ----
    fpu_obj = compile_obj(gcc, ML_DIR / FPU_FILE, BUILD / "ml_fpu.sse.o",
                          ["-O2", "-std=gnu11", "-msse2", "-mfpmath=sse"])
    insns, _ = scan_fp(disassemble(fpu_obj))
    record(f"D. control: scanner flags models/model-xgb-fpu.c built with SSE ({len(insns)} SSE instructions found)", len(insns) > 100)

    bad = BUILD / "bad_example.c"
    bad.write_text("double bad_mul(double a, double b) { return a * b + 1.5; }\n")
    bad_obj = compile_obj(gcc, bad, BUILD / "bad_example.o", NOFP_FLAGS)
    insns, calls = scan_fp(disassemble(bad_obj))
    record(f"D. control: scanner flags a deliberate double multiply (soft-float calls: {calls})", bool(calls or insns))

    samples = {
        "early return inside section": "if (!irq_fpu_usable()) return 0; kernel_fpu_begin(); if (x) return 0; ml_predict_fpu(a); kernel_fpu_end();",
        "two statements inside section": "if (!irq_fpu_usable()) return 0; kernel_fpu_begin(); ml_predict_fpu(a); ml_predict_fpu(b); kernel_fpu_end();",
        "missing end": "if (!irq_fpu_usable()) return 0; kernel_fpu_begin(); ml_predict_fpu(a);",
        "sleeping call inside section": "if (!irq_fpu_usable()) return 0; kernel_fpu_begin(); msleep(1); kernel_fpu_end();",
        "no irq_fpu_usable guard": "kernel_fpu_begin(); ml_predict_fpu(a); kernel_fpu_end();",
    }
    missed = [k for k, v in samples.items() if not check_sections(v, k)]
    record(f"D. control: structure check rejects {len(samples) - len(missed)}/{len(samples)} bad begin/end snippets",
           not missed, "not rejected: " + ", ".join(missed))

    # controls for the context checks: an IRQ function that predicts, and
    # a caller lookup on known code
    irq_bad = ("static void ar_sample_and_rearm(void *info)\n{\n    struct core_info *c = info;\n"
               "    if (x) {\n        ml_predict(&c->ml, &a, &b);\n    }\n}\n")
    body = function_body(irq_bad, "ar_sample_and_rearm")
    flagged = body is not None and re.search(r"\bml_predict\s*\(", body) is not None
    caller = enclosing_function(irq_bad, irq_bad.index("ml_predict"))
    record("D. control: context check finds ml_predict() inside an IRQ-context function "
           f"(caller resolved as {caller}())", flagged and caller == "ar_sample_and_rearm")

    ok = all(r[1] for r in results)
    lines = [f"[{'PASS' if r[1] else 'FAIL'}] {r[0]}" + ("" if r[1] or not r[2] else f"\n       {r[2]}") for r in results]
    lines += ["", "RESULT: " + ("ALL PASS - floating point is confined to models/model-xgb-fpu.c and only runs inside "
                                "kernel_fpu_begin()/end()" if ok else "FAILED")]
    text = "\n".join(lines)
    REPORT.write_text(text + "\n", encoding="utf-8")
    print("\n" + lines[-1])
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
