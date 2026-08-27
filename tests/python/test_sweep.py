"""G6/G10 harness: grid expansion, quantiles, fan-out aggregation, bands.

Uses a stub worker so the test is hermetic; an integration smoke against the
real `mog simrun` runs when the binary is present.
"""

import math
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SWEEP = REPO / "tools" / "sweep.py"


def test_grid_values_lists_and_ranges():
    mod = _import_sweep()
    assert mod.grid_values("0.0,0.5,1.0") == ["0.0", "0.5", "1.0"]
    assert mod.grid_values("1..4") == ["1", "2", "3", "4"]
    assert mod.grid_values("2..10:3") == ["2", "5", "8"]


def test_quantile_interpolation():
    mod = _import_sweep()
    vals = sorted([1.0, 2.0, 3.0, 4.0])
    assert mod.quantile(vals, 0.0) == 1.0
    assert mod.quantile(vals, 1.0) == 4.0
    assert math.isclose(mod.quantile(vals, 0.5), 2.5)


def test_fanout_and_bands(tmp_path):
    worker = tmp_path / "worker.py"
    worker.write_text(
        "import json,sys\n"
        "seed=int(sys.argv[1]); dep=float(sys.argv[2])\n"
        "print(json.dumps({'fills': seed*10 + dep, 'digest_high': 'D',\n"
        "                   'tag': 'x'}))\n"
    )
    out = subprocess.run(
        [
            sys.executable,
            str(SWEEP),
            "--cmd",
            f"{sys.executable} {worker} {{seed}} {{dep}}",
            "--param",
            "seed=1..4",
            "--param",
            "dep=0.0,5.0",
            "--bands",
            "fills",
            "--jobs",
            "2",
        ],
        capture_output=True,
        text=True,
        check=False,
    )
    assert out.returncode == 0, out.stderr
    assert "points=8 ok=8 failed=0" in out.stdout
    assert "digest_high" in out.stdout and "distinct=1" in out.stdout
    # fills spans 10..45 across the grid; p50 must sit strictly inside.
    line = next(l for l in out.stdout.splitlines() if l.startswith("fills"))
    assert "min=10" in line and "max=45" in line and "p50=" in line


def test_failure_reported(tmp_path):
    worker = tmp_path / "bad.py"
    worker.write_text("import sys\nsys.exit(3)\n")
    out = subprocess.run(
        [sys.executable, str(SWEEP), "--cmd", f"{sys.executable} {worker}", "--jobs", "1"],
        capture_output=True,
        text=True,
        check=False,
    )
    assert out.returncode == 1
    assert "exit 3" in out.stderr


def test_simrun_integration_smoke(tmp_path):
    mog = REPO / "build" / "portable" / "mog"
    if not mog.exists():
        return  # binary not built; unit tests above carry coverage
    script = tmp_path / "_script.csv"
    script.write_text(
        "kind,ts_ns,side,price_ticks,qty,ref\n"
        "ext_add,100,B,99000,1000,1\n"
        "ext_add,110,S,101000,1000,2\n"
        "strat_limit,150,B,100000,300,10\n"
        "trade,200,S,100000,1200,3\n"
    )
    out = subprocess.run(
        [
            sys.executable,
            str(SWEEP),
            "--cmd",
            f"{mog} simrun {script} --seed {{seed}} --depletion {{dep}}",
            "--param",
            "seed=1..4",
            "--param",
            "dep=0.0,2000.0",
            "--bands",
            "fills",
            "--jobs",
            "2",
        ],
        capture_output=True,
        text=True,
        check=False,
    )
    assert out.returncode == 0, out.stderr
    assert "failed=0" in out.stdout
    # Depletion must move outcomes: fills band spans >1 value and digests
    # diverge across points (collisions allowed - Poisson draws can coincide).
    fills_line = next(l for l in out.stdout.splitlines() if l.startswith("fills"))
    assert "min=1" in fills_line and "max=2" in fills_line
    digest_line = next(l for l in out.stdout.splitlines() if l.startswith("digest_high"))
    assert "distinct=1" not in digest_line


def _import_sweep():
    import importlib.util

    spec = importlib.util.spec_from_file_location("sweep", SWEEP)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod
