"""G9: moment fits over synthetic logs with known latency distributions."""

import json
import random
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
CAL = REPO / "tools" / "calibrate.py"

HEADER = "type,ts_ns,ref,side,price_ticks,qty,fee_cash,mid_ticks,ledger,submit_ns"


def _log(tmp_path, rows):
    p = tmp_path / "events.csv"
    lines = [HEADER]
    for sub, fill in rows:
        lines.append(f"F,{fill},7,B,100000,10,0,100000,T,{sub}")
    p.write_text("\n".join(lines) + "\n")
    return str(p)


def _json_for(logpath):
    r = subprocess.run(
        [sys.executable, str(CAL), logpath, "--json"], capture_output=True, text=True, check=False
    )
    assert r.returncode == 0, r.stderr
    return json.loads(r.stdout)


def test_uniform_dispersion_recovers_bounded_max(tmp_path):
    rng = random.Random(7)
    rows = [(1000, 1000 + rng.randint(0, 400)) for _ in range(500)]
    out = _json_for(_log(tmp_path, rows))
    assert 0.45 < out["std_over_mean"] < 0.70
    if out["suggested_shape"] == "uniform":
        assert 300 <= out["params"]["max_ns"] <= 500
    else:  # normal fallback still estimates a sane center
        assert 150 <= out["params"]["mean_ns"] <= 350


def test_exponential_fit_detects_ratio_one(tmp_path):
    rng = random.Random(11)
    rows = [(1000, 1000 + int(rng.expovariate(1 / 250.0))) for _ in range(400)]
    out = _json_for(_log(tmp_path, rows))
    assert abs(out["std_over_mean"] - 1.0) < 0.25
    assert out["suggested_shape"] == "exponential"
    assert out["params"]["mean_ns"] > 0


def test_maker_rows_and_short_logs_rejected(tmp_path):
    p = tmp_path / "m.csv"
    p.write_text(HEADER + "\nF,5,7,B,1,1,0,1,M,0\nF,6,7,B,1,1,0,1,M,0\n")
    r = subprocess.run(
        [sys.executable, str(CAL), str(p)], capture_output=True, text=True, check=False
    )
    assert r.returncode == 1
    assert ">= 8" in r.stderr


def test_negative_latency_samples_ignored(tmp_path):
    # submit after fill is nonsense; must be dropped, not fitted.
    rng = random.Random(3)
    rows = [(1000, 1000 + rng.randint(0, 200)) for _ in range(20)]
    rows.append((5000, 4000))  # inverted pair
    out = _json_for(_log(tmp_path, rows))
    assert out["samples"] == 20
