#!/usr/bin/env python3
"""Assemble authored texture shaders using the separately installed compiler."""
import importlib.util
from pathlib import Path

spec=importlib.util.spec_from_file_location('assembler',Path(__file__).with_name('assemble-native-d3d9-tests.py'))
assembler=importlib.util.module_from_spec(spec)
spec.loader.exec_module(assembler)
assembler.SOURCES={
    'ps20-sample':'ps_2_0\ndcl t0.xy\ndcl_2d s0\ntexld r0, t0, s0\nmov oC0, r0\n',
    'ps30-sample':'ps_3_0\ndcl_texcoord0 v0.xy\ndcl_2d s0\ntexld r0, v0, s0\nmov oC0, r0\n',
    'ps30-sparse':'ps_3_0\ndcl_texcoord7 v9.xy\ndcl_2d s15\ntexld r0, v9, s15\nmov oC0, r0\n',
    'ps30-pair':'ps_3_0\ndcl_texcoord0 v3.xy\ndcl_texcoord7 v1.xy\ndcl_2d s0\ndcl_2d s15\ntexld r0, v3, s0\ntexld r1, v1, s15\nadd oC0, r0, r1\n',
    'ps30-dependent':'ps_3_0\ndcl_texcoord0 v0.xy\ndcl_2d s0\ndcl_2d s1\ntexld r0, v0, s0\ntexld r1, r0, s1\nmov oC0, r1\n',
    'ps30-tint':'ps_3_0\ndcl_texcoord0 v0.xy\ndcl_2d s0\ntexld r0, v0, s0\nmul oC0, r0, c5\n',
    'ps30-coordinate-swizzle':'ps_3_0\ndcl_texcoord2 v5.yw\ndcl_2d s3\ntexld r0, v5.wyzw, s3\nmov oC0, r0\n',
    'ps30-constant-coordinate':'ps_3_0\ndef c0, 0.25, 0.75, 0, 1\ndcl_2d s0\ntexld r0, c0, s0\nmov oC0, r0\n',
    'ps30-input-color':'ps_3_0\ndcl_texcoord4 v2\nmov oC0, v2\n',
    'ps30-unused-declaration':'ps_3_0\ndcl_texcoord3 v3\ndcl_2d s5\nmov oC0, c0\n',
    'ps30-dead-sample':'ps_3_0\ndcl_texcoord3 v3\ndcl_2d s5\ntexld r0, v3, s5\nmov oC0, c0\n',
    'reject-projective':'ps_3_0\ndcl_texcoord0 v0\ndcl_2d s0\ntexldp r0, v0, s0\nmov oC0, r0\n',
    'reject-bias':'ps_3_0\ndcl_texcoord0 v0\ndcl_2d s0\ntexldb r0, v0, s0\nmov oC0, r0\n',
    'reject-lod':'ps_3_0\ndcl_texcoord0 v0\ndcl_2d s0\ntexldl r0, v0, s0\nmov oC0, r0\n',
    'reject-cube':'ps_3_0\ndcl_texcoord0 v0\ndcl_cube s0\ntexld r0, v0, s0\nmov oC0, r0\n',
    'reject-volume':'ps_3_0\ndcl_texcoord0 v0\ndcl_volume s0\ntexld r0, v0, s0\nmov oC0, r0\n',
    'reject-centroid':'ps_3_0\ndcl_texcoord0_centroid v0\nmov oC0, v0\n',
    'reject-color-semantic':'ps_3_0\ndcl_color0 v0\nmov oC0, v0\n',
}
assembler.HLSL={
    'ps20-hlsl-sample':('ps_2_0','sampler2D image : register(s0); float4 tint : register(c0); float4 main(float2 uv:TEXCOORD0):COLOR0 { return tex2D(image,uv)*tint; }'),
    'ps30-hlsl-pair':('ps_3_0','sampler2D a:register(s0); sampler2D b:register(s3); float4 main(float2 uv:TEXCOORD2):COLOR0 { return tex2D(a,uv)+tex2D(b,uv); }'),
}
if __name__=='__main__': assembler.main()
