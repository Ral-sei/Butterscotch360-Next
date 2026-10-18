// Xbox 360 precompiled pixel shader (ps_3_0) for the port renderer.
//
// Mirrors the desktop GL default fragment shader's colour math:
//   c = tex2D(s0, uv) * vertexColor;
//   c.rgb = lerp(c.rgb, fog.rgb, fog.a)
//
// Alpha testing is NOT done here: the Xenon microcode compiler rejects the
// 'discard' statement, so the renderer uses the D3D alpha-test render states
// (D3DRS_ALPHATESTENABLE / ALPHAREF / ALPHAFUNC) on the shader output instead.
//
// Untextured primitives (rectangles, lines, triangles) bind a 1x1 white
// texture rather than a NULL sampler: on Xenon, sampling a NULL texture trips
// an assertion (see docs/xbox360-renderer-design.md).

sampler2D uTexture : register(s0);

// rgb = fog colour, a = fog mix amount (0 = disabled, 1 = full)
float4 uFogColor : register(c0);

float4 main(float4 color : COLOR0, float2 texcoord : TEXCOORD0) : COLOR0 {
    float4 c = tex2D(uTexture, texcoord) * color;
    c.rgb = lerp(c.rgb, uFogColor.rgb, uFogColor.a);
    return c;
}
