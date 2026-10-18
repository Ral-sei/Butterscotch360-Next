#!/usr/bin/env python3
"""Generate *build-local* Xbox 360 shader headers with the installed XDK fxc.

Generic version of compile_probe_shaders.py: the caller names each shader and
its profile, so the runtime renderer and the standalone probe can share one
tool. No compiler binaries or proprietary generated microcode are shipped in
Git. Accepts Windows paths when run on Windows and /mnt/... paths in WSL.

    compile_shaders.py --fxc <fxc.exe> --source <dir> --output <dir> \\
        --shader renderer_vs:vs_3_0 --shader renderer_ps:ps_3_0

For each NAME:PROFILE, <source>/NAME.hlsl is compiled into
<output>/NAME.h with the C symbol bs_NAME.
"""

import argparse
import os
from pathlib import Path
import subprocess
import sys


def windows_path(path: Path) -> str:
    if os.name == "nt":
        return str(path)
    try:
        return subprocess.check_output(["wslpath", "-w", str(path)], text=True).strip()
    except (OSError, subprocess.CalledProcessError) as exc:
        raise RuntimeError("shader compilation on POSIX requires WSL wslpath") from exc


def parse_shader(value: str):
    if ":" not in value:
        raise argparse.ArgumentTypeError(f"expected NAME:PROFILE, got {value!r}")
    name, profile = value.split(":", 1)
    if not name or not profile:
        raise argparse.ArgumentTypeError(f"expected NAME:PROFILE, got {value!r}")
    return name, profile


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fxc", required=True, type=Path)
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--shader", required=True, action="append", type=parse_shader,
                        metavar="NAME:PROFILE")
    args = parser.parse_args()

    fxc = args.fxc.resolve()
    if not fxc.is_file():
        parser.error(f"Xbox 360 fxc not found: {fxc}")
    source = args.source.resolve()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)

    for name, profile in args.shader:
        input_file = source / f"{name}.hlsl"
        if not input_file.is_file():
            parser.error(f"missing shader: {input_file}")
        header = output / f"{name}.h"
        args_fxc = [str(fxc), "/nologo", f"/T{profile}", "/Emain",
                    f"/Vnbs_{name}", f"/Fh{windows_path(header)}",
                    windows_path(input_file)]
        subprocess.run(args_fxc, check=True)
        if not header.is_file() or header.stat().st_size == 0:
            raise RuntimeError(f"fxc did not write {header}")
        print(f"shader: {input_file.name} -> {header}")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (RuntimeError, subprocess.CalledProcessError) as exc:
        print(f"compile_shaders: {exc}", file=sys.stderr)
        sys.exit(1)
