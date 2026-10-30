#!/usr/bin/env python3
"""Assemble our test sources with an independently installed Microsoft compiler.

Run on Windows. Only generated shader bytecode and provenance go to the output
directory; no compiler/runtime DLL is copied or distributed.
"""
import argparse
import ctypes as c
import hashlib
import json
import os
from pathlib import Path


SOURCES = {
    "vs20-position": "vs_2_0\ndcl_position v0\nm4x4 oPos, v0, c0\n",
    "ps20-texture": "ps_2_0\ndcl t0.xy\ndcl_2d s0\ntexld r0, t0, s0\nmul r0, r0, c0\nmov oC0, r0\n",
    "ps20-constants": "ps_2_0\ndef c0, 1, -2, 0.5, 0\nmov_sat r0, c0.wzyx\nmov oC0, r0\n",
    "vs30-relative": "vs_3_0\ndcl_position v0\ndcl_position o0\nmova a0.x, v0.x\nmov o0, c0[a0.x]\n",
    "ps30-predicate": "ps_3_0\ndcl_color v0\nsetp_gt p0, v0.x, c0.x\n(p0) mov r0, c0\n(!p0) mov r0, c1\nmov oC0, r0\n",
    "vs30-flow": "vs_3_0\ndcl_position v0\ndcl_position o0\ndefi i0, 2, 0, 1, 0\ndefb b0, true\nmov r0, v0\nloop aL, i0\nif b0\nadd r0, r0, c0[aL]\nelse\nmul r0, r0, c1\nendif\nendloop\nmov o0, r0\n",
    "ps30-gradient": "ps_3_0\ndcl_texcoord v0\ndcl_2d s0\ndsx r1, v0\ndsy r2, v0\ntexldd r0, v0, s0, r1, r2\nmov oC0, r0\n",
    "ps30-lod": "ps_3_0\ndcl_texcoord v0\ndcl_2d s0\ntexldl r0, v0, s0\nmov oC0, r0\n",
    "vs20-sincos": "vs_2_0\ndcl_position v0\ndef c1, -1.5500992e-6, -2.1701389e-5, 0.0026041667, 0.00026041668\ndef c2, -0.020833334, -0.125, 1, 0.5\nsincos r0.xy, v0.x, c1, c2\nmov oPos, r0\n",
    "vs30-sincos": "vs_3_0\ndcl_position v0\ndcl_position o0\nsincos r0.xy, v0.x\nmov o0, r0\n",
    "ps30-call": "ps_3_0\nmov r0, c0\ncall l0\nmov oC0, r0\nret\nlabel l0\nadd r0, r0, c1\nret\n",
    "ps30-rep": "ps_3_0\ndefi i0, 4, 0, 0, 0\nmov r0, c0\nrep i0\nadd r0, r0, c1\nbreak_gt r0.x, c2.x\nendrep\nmov oC0, r0\n",
    "vs30-sgn": "vs_3_0\ndcl_position v0\ndcl_position o0\nsgn r0, v0, r1, r2\nmov o0, r0\n",
    "vs30-relative-output": "vs_3_0\ndcl_position v0\ndcl_position o0\ndcl_texcoord0 o1\ndefi i0, 1, 1, 1, 0\nmov o0, v0\nloop aL, i0\nmov o[aL], v0\nendloop\n",
}

HLSL = {
    "vs30-hlsl": ("vs_3_0", """
float4x4 transform : register(c0);
struct V { float4 pos : POSITION; float2 uv : TEXCOORD0; };
V main(float4 pos : POSITION, float2 uv : TEXCOORD0) {
    V result; result.pos = mul(pos, transform); result.uv = uv; return result;
}
"""),
    "ps20-hlsl": ("ps_2_0", """
sampler2D image : register(s0); float4 tint : register(c0);
float4 main(float2 uv : TEXCOORD0) : COLOR0 { return tex2D(image, uv) * tint; }
"""),
}


def blob_data(blob):
    table = c.cast(blob, c.POINTER(c.POINTER(c.c_void_p))).contents
    ptr = c.WINFUNCTYPE(c.c_void_p, c.c_void_p)(table[3])(blob)
    size = c.WINFUNCTYPE(c.c_size_t, c.c_void_p)(table[4])(blob)
    return c.string_at(ptr, size)


def release_blob(blob):
    if blob:
        table = c.cast(blob, c.POINTER(c.POINTER(c.c_void_p))).contents
        c.WINFUNCTYPE(c.c_ulong, c.c_void_p)(table[2])(blob)


def assemble(dll, name, source, profile=None):
    code, errors = c.c_void_p(), c.c_void_p()
    data = source.encode("ascii")
    try:
        if profile:
            result = dll.D3DCompile(data, len(data), name.encode("ascii"), None,
                                   None, b"main", profile.encode("ascii"), 0, 0,
                                   c.byref(code), c.byref(errors))
        else:
            result = dll.D3DAssemble(data, len(data), name.encode("ascii"), None,
                                     None, 0, c.byref(code), c.byref(errors))
        if result < 0:
            detail = blob_data(errors).decode("utf-8", "replace") if errors else ""
            raise RuntimeError(f"{name}: HRESULT {result & 0xffffffff:08x}: {detail}")
        return blob_data(code)
    finally:
        release_blob(errors)
        release_blob(code)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--compiler", type=Path,
                        default=Path(os.environ.get("SystemRoot", "C:/Windows")) /
                        "System32/d3dcompiler_47.dll")
    args = parser.parse_args()
    compiler = args.compiler.resolve(strict=True)
    dll = c.WinDLL(str(compiler))
    dll.D3DAssemble.argtypes = [c.c_void_p, c.c_size_t, c.c_char_p, c.c_void_p,
                               c.c_void_p, c.c_uint, c.POINTER(c.c_void_p),
                               c.POINTER(c.c_void_p)]
    dll.D3DAssemble.restype = c.c_long
    dll.D3DCompile.argtypes = [c.c_void_p, c.c_size_t, c.c_char_p, c.c_void_p,
                              c.c_void_p, c.c_char_p, c.c_char_p, c.c_uint, c.c_uint,
                              c.POINTER(c.c_void_p), c.POINTER(c.c_void_p)]
    dll.D3DCompile.restype = c.c_long
    args.output.mkdir(parents=True, exist_ok=True)
    records = []
    header = ["/* Generated from our shader sources; no Microsoft library contents. */"]
    rows = []
    corpus = [(name, source, None) for name, source in SOURCES.items()]
    corpus += [(name, source, profile) for name, (profile, source) in HLSL.items()]
    for index, (name, source, profile) in enumerate(corpus):
        data = assemble(dll, name, source, profile)
        (args.output / (name + ".bin")).write_bytes(data)
        records.append({"name": name, "source": source, "profile": profile, "bytes": len(data),
                        "sha256": hashlib.sha256(data).hexdigest()})
        header.append(f"static const unsigned char shader_fixture_{index}[] = {{" +
                      ",".join(f"0x{value:02x}" for value in data) + "};")
        rows.append(f"{{shader_fixture_{index}, sizeof(shader_fixture_{index})}}")
        print(name, len(data), flush=True)
    header.append("static const struct { const unsigned char *data; unsigned bytes; } shader_fixtures[] = {" +
                  ",".join(rows) + "};")
    (args.output / "native_shader_fixtures.h").write_text("\n".join(header) + "\n", encoding="ascii")
    (args.output / "manifest.json").write_text(json.dumps({
        "compiler": str(compiler), "compiler_sha256": hashlib.sha256(compiler.read_bytes()).hexdigest(),
        "fixtures": records}, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
