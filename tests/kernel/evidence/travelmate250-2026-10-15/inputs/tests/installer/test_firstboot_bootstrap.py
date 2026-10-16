#!/usr/bin/env python3
"""Regression: Vision must exist before the BKL first-boot wizard can run."""
from __future__ import annotations

import importlib.util
import pathlib
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location(
    "cd_bkl", ROOT / "tools/build_cd_bkl_installer.py"
)
assert spec and spec.loader
cd = importlib.util.module_from_spec(spec)
spec.loader.exec_module(cd)


def put(root: pathlib.Path, rel: str, content: bytes = b"BEX demo\n") -> None:
    p = root / rel
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_bytes(content)


def check() -> None:
    assert cd.classify(pathlib.Path("SYSTEM/SERVICES/VISION.BEX")) is None
    assert cd.classify(pathlib.Path("SYSTEM/SERVICES/PRINTSPL.BEX")) == "PROGRAMS"
    assert cd.classify(pathlib.Path("SYSTEM/DRIVERS/VESA.DVR")) is None
    with tempfile.TemporaryDirectory(prefix="bk-installer-firstboot-") as tmp:
        td = pathlib.Path(tmp)
        source, out, setup = td / "root", td / "bootstrap", td / "SETUP.BEX"
        put(source, "SYSTEM/SERVICES/VISION.BEX", b"vision-service")
        put(source, "SYSTEM/DRIVERS/VESA.DVR")
        put(source, "SYSTEM/WALLPAPR/NOCHE.BMP")
        put(source, "SYSTEM/PROGRAMS/EXAMPLE.BEX")
        setup.write_bytes(b"setup-wizard")
        import subprocess
        run = subprocess.run([
            "python3", str(ROOT / "tools/build_cd_bkl_installer.py"),
            "--source-root", str(source), "--output", str(out),
            "--setup-exe", str(setup),
        ], capture_output=True, text=True)
        assert run.returncode == 0, run.stderr
        assert (out / "SYSTEM/SERVICES/VISION.BEX").read_bytes() == b"vision-service"
        assert (out / "SYSTEM/PROGRAMS/SETUP.BEX").read_bytes() == b"setup-wizard"
        group = {p.name: cd.classify(p.relative_to(source))
                 for p in source.rglob("*") if p.is_file()}
        assert group["VISION.BEX"] is None
        assert group["EXAMPLE.BEX"] == "PROGRAMS"
        (source / "SYSTEM/SERVICES/VISION.BEX").unlink()
        run = subprocess.run([
            "python3", str(ROOT / "tools/build_cd_bkl_installer.py"),
            "--source-root", str(source), "--output", str(out),
            "--setup-exe", str(setup),
        ], capture_output=True, text=True)
        assert run.returncode != 0, "Missing Vision must fail before ISO creation"
        assert "VISION.BEX" in run.stderr
    print("PASS: first-boot Vision bootstrap / fail-closed on missing Vision")


if __name__ == "__main__":
    check()
