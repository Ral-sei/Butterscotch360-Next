#!/usr/bin/env python3
"""Offline precompilation of a target's HLSL9 shaders into a Xenon sidecar.

Xenon has no runtime HLSL compiler, so a GameMaker title's shaders cannot be
built on the console. This host-side tool does it ahead of time:

    data.win / HLSL9 source  ->  Xbox 360 fxc  ->  data.x360shaders (sidecar)

The sidecar is loaded by src/xbox360/xbox360_shadersets.c at runtime, which
creates the D3D9 shader objects and exposes the uniform table so
`shader_get_uniform` / `shader_set_uniform_*` work. Nothing proprietary is
committed: the microcode is a build product (like the built-in shaders compiled
by compile_shaders.py).

Two input modes:

    --datawin <data.win>   Extract each SHDR entry's hlsl9_Vertex / hlsl9_Fragment
                           source and compile it, keyed by the data.win shader
                           index.
    --hlsl-dir <dir>       Compile <name>.v.hlsl / <name>.f.hlsl pairs (used by
                           the on-device shader self-test and for overrides).
                           Indices are assigned in sorted-name order.

Xenon facts this tool is built around (see docs/xbox360-renderer-design.md):

  * The Xbox 360 fxc targets `vs_3_0` / `ps_3_0` and emits a `DWORD` microcode
    array (big-endian), identical in shape to what compile_shaders.py emits.
  * The Xbox microcode compiler rejects `discard` (X3500). GameMaker's HLSL9
    alpha test is a `discard` block; this tool strips it and records that the
    runtime must use the D3DRS_ALPHATESTENABLE render state instead (which the
    port renderer already does).
  * GameMaker's generated HLSL9 binds uniforms with explicit `register(cN)` /
    `register(sN)`. fxc's listing surfaces the exact table, which this tool
    parses so the runtime can upload to the right registers without d3dx9.

Output (sidecar) is a byte-oriented, big-endian container; all integers are
big-endian so the big-endian Xenon reader needs no byte swapping.
"""

import argparse
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile

SIDECAR_MAGIC = b"BSX3SHD\0"
SIDECAR_VERSION = 1

# Uniform register classes.
REG_CONSTANT = 0
REG_SAMPLER = 1

# Uniform type ids (port-defined SeShaderUniformType).
T_INVALID = 0
T_FLOAT = 1
T_FLOAT2 = 2
T_FLOAT3 = 3
T_FLOAT4 = 4
T_FLOAT4X4 = 5
T_BOOL = 6
T_INT = 7
T_SAMPLER2D = 8

# Shader flags.
FLAG_HAS_VS = 1 << 0
FLAG_HAS_PS = 1 << 1
FLAG_ALPHATEST_STRIPPED = 1 << 2
FLAG_MATRICES_TRANSPOSED = 1 << 3

# Stage a uniform was seen in.
STAGE_VERTEX = 0
STAGE_PIXEL = 1


# ---------------------------------------------------------------------------
# data.win parsing (self-contained; mirrors src/data_win.c's SHDR reader)
# ---------------------------------------------------------------------------

def _u32(buf, off):
    return struct.unpack_from("<I", buf, off)[0]


def _cstr(buf, off):
    if off == 0:
        return None
    end = buf.find(b"\0", off)
    if end < 0:
        return None
    return buf[off:end].decode("utf-8", "replace")


def read_datawin_shaders(path: Path):
    """Return [(index, name, hlsl9_vertex, hlsl9_fragment), ...]."""
    buf = path.read_bytes()
    if buf[0:4] != b"FORM":
        raise RuntimeError(f"{path}: not a data.win (missing FORM header)")

    chunks = {}
    off = 8
    while off + 8 <= len(buf):
        name = buf[off:off + 4]
        length = _u32(buf, off + 4)
        chunks[name] = off + 8
        off += 8 + length

    shdr = chunks.get(b"SHDR")
    if shdr is None:
        return []
    count = _u32(buf, shdr)
    if count == 0:
        return []

    out = []
    for i in range(count):
        ptr = _u32(buf, shdr + 4 + 4 * i)
        if ptr == 0:
            continue
        o = ptr
        name = _cstr(buf, _u32(buf, o)); o += 4
        o += 4  # type
        o += 4 * 4  # glslES v/f, glsl v/f
        hlsl9_v = _cstr(buf, _u32(buf, o)); o += 4
        hlsl9_f = _cstr(buf, _u32(buf, o)); o += 4
        out.append((i, name or f"shader{i}", hlsl9_v, hlsl9_f))
    return out


# ---------------------------------------------------------------------------
# HLSL9 normalisation and fxc
# ---------------------------------------------------------------------------

DISCARD_RE = re.compile(r"(^|[;\s{}\n])discard\s*;", re.MULTILINE)
TRANSPOSE_RE = re.compile(r"transpose\s*\(\s*_?gm_Matrices")


def strip_discard(source: str):
    """Remove `discard;` (Xenon fxc rejects it). Returns (source, stripped?)."""
    new, n = DISCARD_RE.subn(lambda m: m.group(1), source)
    return new, n > 0


def uses_transpose(source: str) -> bool:
    return TRANSPOSE_RE.search(source) is not None


# GameMaker HLSL9 shaders define an entry point named `main`. A GMS 2.3+
# Windows data.win stores only the declaration prelude for the hlsl9 slot (the
# body ships as GLSL + compiled HLSL11 bytecode), so those entries have no
# entry point and cannot be recompiled from hlsl9.
ENTRY_RE = re.compile(r"\bmain\s*\(")


def has_entry_point(source: str) -> bool:
    return ENTRY_RE.search(source) is not None


def windows_path(path: Path) -> str:
    if os.name == "nt":
        return str(path)
    try:
        return subprocess.check_output(["wslpath", "-w", str(path)], text=True).strip()
    except (OSError, subprocess.CalledProcessError) as exc:
        raise RuntimeError("fxc on POSIX requires WSL wslpath") from exc


def compile_hlsl(fxc: Path, source_text: str, profile: str, workdir: Path,
                 tag: str, verbose: bool):
    """Compile one HLSL file; return (microcode_bytes, listing_text)."""
    src_file = workdir / f"{tag}.hlsl"
    asm_file = workdir / f"{tag}.asm"
    hdr_file = workdir / f"{tag}.h"
    src_file.write_text(source_text, encoding="utf-8", newline="\n")

    symbol = f"bs_x360_{tag.replace('.', '_')}"
    args = [str(fxc), "/nologo", f"/T{profile}", "/Emain",
            f"/Vn{symbol}", f"/Fh{windows_path(hdr_file)}",
            f"/Fc{windows_path(asm_file)}", windows_path(src_file)]
    if verbose:
        print("  fxc", " ".join(args[1:]))
    proc = subprocess.run(args, capture_output=True, text=True)
    if proc.returncode != 0:
        detail = (proc.stdout or "") + (proc.stderr or "")
        raise RuntimeError(f"fxc failed ({profile}):\n{detail.strip()}")

    microcode = parse_microcode(hdr_file, symbol)
    listing = asm_file.read_text(encoding="utf-8", errors="replace")
    return microcode, listing


def parse_microcode(header: Path, symbol: str) -> bytes:
    """Extract the DWORD array fxc wrote; return it as raw big-endian bytes."""
    text = header.read_text(encoding="utf-8", errors="replace")
    body = re.search(r"const\s+DWORD\s+" + re.escape(symbol) + r"\s*\[\s*\]\s*=\s*\{(.*?)\}",
                     text, re.DOTALL)
    if body is None:
        raise RuntimeError(f"{header}: no microcode array for {symbol}")
    words = re.findall(r"0x([0-9a-fA-F]{8})", body.group(1))
    if not words:
        raise RuntimeError(f"{header}: microcode array is empty")
    return b"".join(struct.pack(">I", int(w, 16)) for w in words)


# ---------------------------------------------------------------------------
# fxc listing -> uniform table
# ---------------------------------------------------------------------------

REG_TABLE_RE = re.compile(
    r"^//\s+(\S+)\s+([cs])(\d+)\s+(\d+)\s*$", re.MULTILINE)
DEFCONST_RE = re.compile(r"^defconst\s+(.+?),\s*(\w+),\s*(\w+),\s*(\[[^\]]*\]),\s*([cs])(\d+)",
                         re.MULTILINE)


def parse_uniforms(listing: str, stage: int):
    """Parse the fxc 'Registers' table; return a list of uniform dicts.

    A matrix uniform is reported in the table as its base name with the total
    register count (e.g. `_gm_Matrices c29 20`), which is exactly what the
    runtime needs to upload each sub-matrix.
    """
    types = {}
    for m in DEFCONST_RE.finditer(listing):
        name = m.group(1).strip()
        base = re.sub(r"\[\d+\]$", "", name)
        hlsl_type = m.group(2).lower()
        shape = m.group(3).lower()
        dims = m.group(4)
        types[base] = (hlsl_type, shape, dims)

    uniforms = []
    for m in REG_TABLE_RE.finditer(listing):
        name, cls, index, size = m.group(1), m.group(2), int(m.group(3)), int(m.group(4))
        base = re.sub(r"\[\d+\]$", "", name)
        if cls == "s":
            uniforms.append({
                "name": base, "stage": stage, "reg_class": REG_SAMPLER,
                "reg_index": index, "reg_count": 0, "type": T_SAMPLER2D,
            })
            continue
        hlsl_type, shape, dims = types.get(base, ("float", "scalar", "[]"))
        uniforms.append({
            "name": base, "stage": stage, "reg_class": REG_CONSTANT,
            "reg_index": index, "reg_count": size,
            "type": _type_id(hlsl_type, shape, dims, size),
        })
    return uniforms


def _type_id(hlsl_type, shape, dims, size):
    if hlsl_type == "sampler2d":
        return T_SAMPLER2D
    if hlsl_type == "bool":
        return T_BOOL
    if hlsl_type in ("int", "uint", "dword"):
        return T_INT
    if hlsl_type == "float":
        if shape == "matrix_columns" or shape == "matrix_rows":
            return T_FLOAT4X4
        comps = size
        return {1: T_FLOAT, 2: T_FLOAT2, 3: T_FLOAT3, 4: T_FLOAT4}.get(comps, T_FLOAT4)
    return T_INVALID


# ---------------------------------------------------------------------------
# Sidecar writer
# ---------------------------------------------------------------------------

def _put_u8(out, v):
    out.append(struct.pack(">B", v))


def _put_u32(out, v):
    out.append(struct.pack(">I", v))


def _put_str(out, s):
    raw = s.encode("utf-8")
    _put_u32(out, len(raw))
    out.append(raw)


def write_sidecar(path: Path, shaders):
    """shaders: list of dicts {index,name,flags,vs,ps,uniforms}."""
    out = [SIDECAR_MAGIC]
    _put_u32(out, SIDECAR_VERSION)
    _put_u32(out, len(shaders))
    for s in shaders:
        _put_u32(out, s["index"])
        _put_str(out, s["name"])
        _put_u32(out, s["flags"])
        _put_u32(out, len(s["vs"]))
        out.append(s["vs"])
        _put_u32(out, len(s["ps"]))
        out.append(s["ps"])
        _put_u32(out, len(s["uniforms"]))
        for u in s["uniforms"]:
            _put_str(out, u["name"])
            _put_u8(out, u["stage"])
            _put_u8(out, u["reg_class"])
            _put_u32(out, u["reg_index"])
            _put_u32(out, u["reg_count"])
            _put_u32(out, u["type"])
    path.write_bytes(b"".join(out))


# ---------------------------------------------------------------------------
# Drivers
# ---------------------------------------------------------------------------

def build_from_hlsl_dir(fxc, directory: Path, workdir: Path, verbose: bool):
    pairs = {}
    for f in sorted(directory.glob("*.hlsl")):
        stem = f.name[:-len(".hlsl")]
        if stem.endswith(".v") or stem.endswith(".vs"):
            pairs.setdefault(stem[:-2], {})["v"] = f
        elif stem.endswith(".f") or stem.endswith(".ps"):
            pairs.setdefault(stem[:-2], {})["f"] = f

    shaders = []
    for index, (name, halves) in enumerate(sorted(pairs.items())):
        vs_src = halves["v"].read_text(encoding="utf-8") if "v" in halves else None
        ps_src = halves["f"].read_text(encoding="utf-8") if "f" in halves else None
        shaders.append(compile_one(fxc, index, name, vs_src, ps_src, workdir, verbose))
    return shaders


def compile_one(fxc, index, name, vs_src, ps_src, workdir, verbose):
    flags = 0
    uniforms = []
    vs_bytes = b""
    ps_bytes = b""

    if vs_src is not None:
        flags |= FLAG_HAS_VS
        if uses_transpose(vs_src):
            flags |= FLAG_MATRICES_TRANSPOSED
        src, stripped = strip_discard(vs_src)
        if stripped:
            flags |= FLAG_ALPHATEST_STRIPPED
        vs_bytes, listing = compile_hlsl(fxc, src, "vs_3_0", workdir,
                                         f"{index}_{name}_vs", verbose)
        uniforms.extend(parse_uniforms(listing, STAGE_VERTEX))

    if ps_src is not None:
        flags |= FLAG_HAS_PS
        if uses_transpose(ps_src):
            flags |= FLAG_MATRICES_TRANSPOSED
        src, stripped = strip_discard(ps_src)
        if stripped:
            flags |= FLAG_ALPHATEST_STRIPPED
        ps_bytes, listing = compile_hlsl(fxc, src, "ps_3_0", workdir,
                                         f"{index}_{name}_ps", verbose)
        uniforms.extend(parse_uniforms(listing, STAGE_PIXEL))

    return {
        "index": index, "name": name, "flags": flags,
        "vs": vs_bytes, "ps": ps_bytes, "uniforms": uniforms,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--fxc", required=True, type=Path,
                        help="installed Xbox 360 fxc.exe")
    src = parser.add_mutually_exclusive_group(required=True)
    src.add_argument("--datawin", type=Path)
    src.add_argument("--hlsl-dir", type=Path)
    parser.add_argument("--output", required=True, type=Path,
                        help="sidecar file to write, e.g. data.x360shaders")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()

    fxc = args.fxc.resolve()
    if not fxc.is_file():
        parser.error(f"Xbox 360 fxc not found: {fxc}")
    out_path = args.output.resolve()
    out_path.parent.mkdir(parents=True, exist_ok=True)

    shaders = []
    skipped = 0
    with tempfile.TemporaryDirectory(prefix="bs_x360_shaders_") as tmp:
        workdir = Path(tmp)
        if args.datawin is not None:
            entries = read_datawin_shaders(args.datawin.resolve())
            print(f"data.win shaders: {len(entries)}")
            for index, name, vs_src, ps_src in entries:
                if not vs_src and not ps_src:
                    print(f"  [{index}] {name}: skipped (no hlsl9 source in this data.win)")
                    skipped += 1
                    continue
                if (vs_src and not has_entry_point(vs_src)) and (ps_src and not has_entry_point(ps_src)):
                    print(f"  [{index}] {name}: skipped (hlsl9 is declarations only, "
                          f"no entry point - GMS 2.3+ stores the body as GLSL/HLSL11)")
                    skipped += 1
                    continue
                # A half whose stored source is declarations-only (no entry point)
                # is treated as absent; the runtime then falls back for that half.
                if vs_src and not has_entry_point(vs_src):
                    print(f"  [{index}] {name}: hlsl9 vertex is declarations only; using default VS")
                    vs_src = None
                if ps_src and not has_entry_point(ps_src):
                    print(f"  [{index}] {name}: hlsl9 fragment is declarations only; using default PS")
                    ps_src = None
                try:
                    rec = compile_one(fxc, index, name, vs_src, ps_src, workdir, args.verbose)
                except RuntimeError as exc:
                    print(f"  [{index}] {name}: FAILED\n{exc}")
                    skipped += 1
                    continue
                print(f"  [{index}] {name}: vs={len(rec['vs'])}B ps={len(rec['ps'])}B "
                      f"uniforms={len(rec['uniforms'])}")
                for u in rec["uniforms"]:
                    cls = "s" if u["reg_class"] == REG_SAMPLER else "c"
                    stage = "vs" if u["stage"] == STAGE_VERTEX else "ps"
                    print(f"      {stage} {u['name']} -> {cls}{u['reg_index']} "
                          f"count={u['reg_count']} type={u['type']}")
                shaders.append(rec)
        else:
            shaders = build_from_hlsl_dir(fxc, args.hlsl_dir.resolve(), workdir, args.verbose)
            for rec in shaders:
                print(f"  [{rec['index']}] {rec['name']}: vs={len(rec['vs'])}B "
                      f"ps={len(rec['ps'])}B uniforms={len(rec['uniforms'])}")

    write_sidecar(out_path, shaders)
    print(f"wrote {out_path} ({out_path.stat().st_size} bytes, "
          f"{len(shaders)} shader(s), {skipped} skipped)")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (RuntimeError, subprocess.CalledProcessError) as exc:
        print(f"precompile_shaders: {exc}", file=sys.stderr)
        sys.exit(1)
