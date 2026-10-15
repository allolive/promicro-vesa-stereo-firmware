"""Builds the firmware's clock mapping and schedule (sync_core.h) for the host
and runs the simulation in sim_sync_core.cpp against it: USB jitter, stalls,
dropped vsyncs, crystal skew, both clocks wrapping, VRR rate changes, a box
that goes quiet while the video goes on, and breaks between films."""
import os
import shutil
import subprocess
from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent
FIRMWARE = HERE.parent / "firmware" / "promicro-vesa-stereo"


@pytest.fixture(scope="module")
def sim(tmp_path_factory):
    cxx = os.environ.get("CXX") or shutil.which("g++") or shutil.which("c++")
    if not cxx:
        pytest.skip("no C++ compiler")
    out = tmp_path_factory.mktemp("sim") / "sim"
    subprocess.run([cxx, "-O2", "-Wall", "-Wextra", "-Werror", "-std=c++17",
                    "-I", str(FIRMWARE), str(HERE / "sim_sync_core.cpp"), "-o", str(out)], check=True)
    return out


def run(sim, **opts):
    args = [str(sim)]
    for k, v in opts.items():
        args += ["--" + k.replace("_", "-"), str(v)]
    r = subprocess.run(args, capture_output=True, text=True)
    assert r.returncode == 0, r.stdout + r.stderr
    return r.stdout


@pytest.mark.parametrize("rate,skew,delay", [
    (96000, 0, 0),
    (95904, 60, 0),
    (95904, -90, 3000),
    (96000, 150, -2000),
    (96000, -150, 0),
    (119880, 40, 1500),
    (120000, -60, 0),
    (100000, 20, 12000),
    (48000, -40, 0),
])
@pytest.mark.parametrize("seed", [1, 2, 3])
def test_locks(sim, rate, skew, delay, seed):
    run(sim, rate=rate, skew=skew, delay=delay, seed=seed)


@pytest.mark.parametrize("rate,rate2", [(96000, 120000), (120000, 95904), (95904, 119880)])
def test_follows_a_rate_change(sim, rate, rate2):
    """VRR moves between films: the period is exact again within a frame."""
    out = run(sim, rate=rate, rate2=rate2, skew=-70, delay=1000, seed=5)
    assert "rate_changes 1" in out


@pytest.mark.parametrize("stall", [0.5, 1.9])
@pytest.mark.parametrize("skew", [150, -150])
def test_coasts_through_a_silent_box(sim, stall, skew):
    """No messages for up to 2 s while the video goes on: the edges go on at
    the last anchor and the exact period, without a restart."""
    out = run(sim, rate=95904, skew=skew, stall=stall, seed=6)
    assert "resets 0" in out


@pytest.mark.parametrize("gap", [5, 1200])
def test_relocks_after_a_break(sim, gap):
    """A second film after a break: 1200 s crosses the 2^31-tick half range."""
    run(sim, rate=95904, skew=60, gap=gap, seconds=20 + gap + 30, seed=4)


def test_exact_period(sim):
    """The period is the box's, to the microsecond, not an average."""
    out = run(sim, rate=95904, skew=90, seed=7)
    assert "period 10427.094us" in out
