// Self-test vertex shader for the offline user-shader precompile path.
//
// Compiled by scripts/xbox360/precompile_shaders.py --hlsl-dir and loaded by
// X360Renderer_runShaderSelftest (see BS_X360_SHADER_SELFTEST). It deliberately
// mirrors a GameMaker-transpiled HLSL9 vertex shader: gm_Matrices is the
// built-in matrix block and it is read through transpose(), so the renderer must
// upload transposed matrices for the quad to land on the readback pixel.

float4x4 gm_Matrices[5] : register(c0);

struct VSInput {
    float4 position : POSITION;
    float4 color    : COLOR0;
    float2 texcoord : TEXCOORD0;
};

struct VSOutput {
    float4 position : POSITION;
    float4 color    : COLOR0;
    float2 texcoord : TEXCOORD0;
};

VSOutput main(VSInput input) {
    VSOutput output;
    output.position = mul(transpose(gm_Matrices[4]), float4(input.position.xyz, 1.0));
    output.color    = input.color;
    output.texcoord = input.texcoord;
    return output;
}
