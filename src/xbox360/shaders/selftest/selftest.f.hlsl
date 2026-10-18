// Self-test pixel shader for the offline user-shader precompile path.
//
// `outColor` is a user uniform set from GML with shader_set_uniform_f. The
// self-test draws a full-surface quad with a 1x1 white base texture and white
// vertex colour, so the readback equals outColor, which the test sets to opaque
// green. A red readback means the uniform upload did not reach the shader.

sampler2D gm_BaseTexture : register(s0);
float4 outColor : register(c0);

struct PSInput {
    float4 color    : COLOR0;
    float2 texcoord : TEXCOORD0;
};

float4 main(PSInput input) : COLOR0 {
    return tex2D(gm_BaseTexture, input.texcoord) * input.color * outColor;
}
