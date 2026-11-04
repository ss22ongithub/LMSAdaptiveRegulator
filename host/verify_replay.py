"""
Host verification of the streaming C predictor (models/model-xgb.c + models/model-xgb-fpu.c, the
same files that are built into the kernel module) against Python.

What is checked:

  1. host/test_mlpred.c unit tests (warm-up, window boundaries, ring
     wrap-around, history reset, integer scoring, output clamping, against
     an independent naive reference).
  2. "Live" replay: each selected run of bw_dataset_all is streamed through
     replay.exe ONE 1 ms SAMPLE AT A TIME (ml_push every sample, prediction
     whenever it says one is due, history reset where the training data
     drops samples), with the exact integer samples Python trained on
     (bw1ms_features.samples_from_csv). Compared with Python's own pipeline
     for the deployed model (xgboost_mem_1ms.py: cached features +
     joblib model.predict()):
       - every example Python has is predicted by C at the same window;
       - C features == Python features, bit for bit (float32);
       - C forest outputs == model.predict(), bit for bit (float32);
       - the integer MiB/s the kernel uses == trunc(clamp(Python output));
       - the online accuracy tracker (integer, models/model-xgb.c) == the same errors
         computed in Python from the ground truth;
       - RMSE of C and Python on the run match.

Usage:  python host/verify_replay.py [--rebuild] [--runs 5] [--workloads ...]
Writes: host/build/* and host/replay_verification_report.txt
"""

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

import joblib
import numpy as np
import pandas as pd

HERE = Path(__file__).resolve().parent
ML_DIR = HERE.parent
PROJECT_DIR = ML_DIR.parent
BUILD = HERE / "build"
REPORT = HERE / "replay_verification_report.txt"

sys.path.insert(0, str(PROJECT_DIR))
import bw1ms_features as bf  # noqa: E402
import xgboost_mem_1ms as m1  # noqa: E402

EXE = ".exe" if os.name == "nt" else ""
# __USE_MINGW_ANSI_STDIO: MinGW's C99 printf/strtoul (the old msvcrt one
# does not know %llu); no effect on other platforms.
GCC_FLAGS = ["-O2", "-std=gnu99", "-Wall", "-Wextra", "-Wno-unused-parameter",
             "-msse2", "-mfpmath=sse", "-D__USE_MINGW_ANSI_STDIO=1"]
MODEL = "xgboost_mem_next_1ms"
TARGETS = ["Read BW", "Write BW"]
PRED_MAX = 1_000_000                      # ML_PRED_MAX_MBPS


# ----------------------------------------------------------------------
# Build
# ----------------------------------------------------------------------

def _newer(target, sources):
    return (not target.exists()) or target.stat().st_mtime < max(p.stat().st_mtime for p in sources)


def build(force):
    BUILD.mkdir(exist_ok=True)
    gcc = shutil.which("gcc") or sys.exit("gcc not found on PATH")
    models = list((ML_DIR / "models" / "xgb").glob("*.h"))
    hdrs = [ML_DIR / "models/model-xgb.h", ML_DIR / "models/model-xgb-fpu.h"]
    fpu_obj = BUILD / "model-xgb-fpu.o"      # all the float code + the trees (SSE)
    api_obj = BUILD / "model-xgb.o"      # integer front end with the FPU sections
    objs = [str(fpu_obj), str(api_obj)]

    def run(cmd):
        res = subprocess.run(cmd, capture_output=True, text=True)
        if res.returncode != 0 or res.stderr.strip():
            print(res.stderr)
        if res.returncode != 0:
            sys.exit("build failed: " + " ".join(cmd))

    if force or _newer(fpu_obj, [ML_DIR / "models/model-xgb-fpu.c", *hdrs, *models]):
        print("compiling models/model-xgb-fpu.c (generated trees)...")
        run([gcc, *GCC_FLAGS, "-c", str(ML_DIR / "models/model-xgb-fpu.c"), "-o", str(fpu_obj)])
    if force or _newer(api_obj, [ML_DIR / "models/model-xgb.c", *hdrs]):
        run([gcc, *GCC_FLAGS, "-c", str(ML_DIR / "models/model-xgb.c"), "-o", str(api_obj)])
    for name in ("replay", "test_mlpred", "bench_inference"):
        out = BUILD / (name + EXE)
        src = HERE / (name + ".c")
        if force or _newer(out, [src, fpu_obj, api_obj, *hdrs]):
            run([gcc, *GCC_FLAGS, str(src), *objs, "-o", str(out)])


# ----------------------------------------------------------------------
# Checks
# ----------------------------------------------------------------------

def run_exe(name, *args):
    return subprocess.run([str(BUILD / (name + EXE)), *map(str, args)], capture_output=True, text=True)


def model_counters(model):
    """Counter order of the model = enum ml_counter order in models/model-xgb.h."""
    names = [str(n) for n in model.estimators_[0].feature_names_in_]
    counters = [n[: -len("_mean")] for n in names if n.endswith("_mean") and n.count("_") == 1]
    assert m1.feature_columns(counters) == names, (counters, names)
    return counters, names


def write_stream(path, samples, valid_win, counters):
    """All samples of the valid windows, in order; reset=1 on the first
    sample after a dropped window (or at the start)."""
    rows = []
    prev_valid = False
    for w in np.flatnonzero(valid_win):
        reset = np.zeros(bf.WINDOW, dtype=np.int64)
        if not (w > 0 and valid_win[w - 1] and prev_valid):
            reset[0] = 1
        prev_valid = True
        block = np.column_stack([np.full(bf.WINDOW, w), reset] +
                                [samples[c][w].astype(np.int64) for c in counters])
        rows.append(block)
    arr = np.concatenate(rows)
    assert (arr[:, 2:] >= 0).all() and (arr[:, 2:] < 2**32).all()
    header = "win,reset," + ",".join(counters)
    np.savetxt(path, arr, fmt="%d", delimiter=",", header=header, comments="")


def check_run(model, counters, names, wl, run, cache):
    csv_path = bf.DATA_DIR / wl / f"{wl}_run{run}.csv"
    samples, valid_win = bf.samples_from_csv(csv_path)
    stream = BUILD / f"{wl}_run{run}.samples.csv"
    pred_path = BUILD / f"{wl}_run{run}.replay.csv"
    write_stream(stream, samples, valid_win, counters)
    res = run_exe("replay", stream, pred_path)
    stream.unlink()
    if res.returncode != 0:
        sys.exit(f"replay failed on {wl} run {run}:\n{res.stderr}")
    st = dict(kv.split("=") for kv in res.stderr.strip().split()[1:])
    st = {k: int(v) for k, v in st.items()}

    c = pd.read_csv(pred_path)
    pred_path.unlink()
    py = cache[(cache["__workload__"] == wl) & (cache["__run__"] == run)].set_index("__win__")
    X = py[names].to_numpy(dtype=np.float32)
    y = py[TARGETS].to_numpy(dtype=np.float64)
    praw = model.predict(py[names]).astype(np.float32)        # (n, 2)

    c = c.set_index("win")
    missing = np.setdiff1d(py.index.to_numpy(), c.index.to_numpy())
    cc = c.loc[py.index]
    cf = cc[[f"f{i}" for i in range(len(names))]].to_numpy(dtype=np.float32)
    craw = cc[["raw_rd", "raw_wr"]].to_numpy(dtype=np.float32)
    cint = cc[["int_rd", "int_wr"]].to_numpy(dtype=np.int64)

    feat_ok = np.array_equal(cf, X)
    raw_same = np.all(craw == praw, axis=1)
    p64 = praw.astype(np.float64)
    exp_int = np.where(p64 > 0, np.minimum(np.floor(p64), PRED_MAX), 0).astype(np.int64)
    int_ok = np.array_equal(cint, exp_int)

    # online tracker: realised floor = (min lines * 64000) >> 20
    lines = np.rint(y / bf.MIB_PER_LINE_MS).astype(np.int64)
    assert np.allclose(lines * bf.MIB_PER_LINE_MS, y, rtol=0, atol=0)
    real = (lines * 64000) >> 20
    exp_abs = np.abs(exp_int - real).sum(axis=0)
    exp_over = (exp_int > real).sum(axis=0)
    track_ok = (st["n_eval"] == len(py) and st["abs_rd"] == exp_abs[0] and st["abs_wr"] == exp_abs[1]
                and st["over_rd"] == exp_over[0] and st["over_wr"] == exp_over[1])

    rmse_c = np.sqrt(np.mean((craw.astype(np.float64) - y) ** 2, axis=0))
    rmse_p = np.sqrt(np.mean((p64 - y) ** 2, axis=0))
    ok = (len(missing) == 0 and feat_ok and raw_same.all() and int_ok and track_ok
          and np.array_equal(rmse_c, rmse_p))
    return ok, {
        "wl": wl, "run": run, "rows": len(py), "c_preds": st["n_pred"], "missing": len(missing),
        "feat_ok": feat_ok, "raw_ident_pct": raw_same.mean() * 100, "int_ok": int_ok,
        "track_ok": track_ok, "n_eval": st["n_eval"], "resets": st["resets"],
        "rmse_rd": rmse_c[0], "rmse_wr": rmse_c[1], "sq_rd": ((craw[:, 0] - y[:, 0]) ** 2).sum(),
        "sq_wr": ((craw[:, 1] - y[:, 1]) ** 2).sum(),
    }


def main():
    parser = argparse.ArgumentParser(description="Verify streaming C predictor vs Python")
    parser.add_argument("--rebuild", action="store_true")
    parser.add_argument("--runs", type=int, nargs="+", default=[5])
    parser.add_argument("--workloads", nargs="+", default=None)
    args = parser.parse_args()

    build(args.rebuild)
    lines = [f"Streaming C predictor (models/model-xgb.c + models/model-xgb-fpu.c) vs Python  [model: {MODEL}]", ""]
    ok_all = True

    res = run_exe("test_mlpred")
    ok = res.returncode == 0
    ok_all &= ok
    lines += ["Unit tests (host/test_mlpred.c): " + ("PASS" if ok else "FAIL")]
    lines += ["  " + l for l in res.stdout.strip().splitlines()] + [""]

    model = joblib.load(PROJECT_DIR / f"{MODEL}_model.joblib")
    counters, names = model_counters(model)
    lines.append(f"Model counters (must equal enum ml_counter in models/model-xgb.h): {counters}; "
                 f"{len(names)} features")
    cache = bf.load_windows()
    wls = args.workloads or sorted(cache["__workload__"].astype(str).unique())

    lines.append(f"  {'workload':16s} run {'examples':>8s} {'C preds':>8s} {'resets':>6s} "
                 f"{'features':>8s} {'raw bit-ident':>13s} {'int':>4s} {'tracker':>7s} "
                 f"{'RMSE rd':>8s} {'RMSE wr':>8s}  result")
    tot = {"rows": 0, "sq_rd": 0.0, "sq_wr": 0.0}
    for wl in wls:
        for run in args.runs:
            ok, r = check_run(model, counters, names, wl, run, cache)
            ok_all &= ok
            for k in tot:
                tot[k] += r[k]
            lines.append(f"  {wl:16s} {run:3d} {r['rows']:8d} {r['c_preds']:8d} {r['resets']:6d} "
                         f"{'same' if r['feat_ok'] else 'DIFF':>8s} {r['raw_ident_pct']:12.3f}% "
                         f"{'ok' if r['int_ok'] else 'DIFF':>4s} {'ok' if r['track_ok'] else 'DIFF':>7s} "
                         f"{r['rmse_rd']:8.2f} {r['rmse_wr']:8.2f}  {'ok' if ok else 'MISMATCH'}")
            print(lines[-1], flush=True)
    lines.append(f"  All: {tot['rows']} examples, RMSE (C) read={np.sqrt(tot['sq_rd'] / tot['rows']):.3f} "
                 f"write={np.sqrt(tot['sq_wr'] / tot['rows']):.3f} MiB/s")
    lines += ["", "RESULT: " + ("ALL PASS" if ok_all else "FAILURES PRESENT")]
    text = "\n".join(lines)
    print(text)
    REPORT.write_text(text + "\n", encoding="utf-8")
    sys.exit(0 if ok_all else 1)


if __name__ == "__main__":
    main()
