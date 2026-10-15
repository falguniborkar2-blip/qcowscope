import json
import os
import shutil
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[1]
BIN = os.environ.get("QCOWSCOPE_BIN", str(ROOT / "qcowscope"))


class Result:
    def __init__(self, proc: subprocess.CompletedProcess):
        self.code = proc.returncode
        self.out = proc.stdout
        self.err = proc.stderr

    def json(self) -> dict:
        return json.loads(self.out)


def run(*args: str) -> Result:
    proc = subprocess.run([BIN, *map(str, args)], capture_output=True, text=True,
                          errors="replace", timeout=30)
    return Result(proc)


@pytest.fixture
def qcowscope():
    if not Path(BIN).exists():
        pytest.fail(f"{BIN} not found - run `make` first")
    return run


requires_qemu = pytest.mark.skipif(shutil.which("qemu-img") is None,
                                   reason="qemu-img is not installed")
