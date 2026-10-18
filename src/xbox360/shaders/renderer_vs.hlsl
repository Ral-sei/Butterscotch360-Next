// Xbox 360 precompiled vertex shader (vs_3_0) for the port renderer.
//
// One transform constant (c0..c3). The matrix is a column-major float4x4,
// which is HLSL's default packing and matches src/matrix_math.h's Matrix4f
// layout (m[col*4 + row]); SetVertexShaderConstantF writes the same 16 floats
// that the desktop GL renderer uploads with glUniformMatrix4fv(..., GL_FALSE).
//
// Unlike the desktop GL shader, the port renderer does NOT flip the clip-space
// Y: D3D's render target row 0 is the top row, which already matches
// GameMaker's Y-down convention.

float4x4 uWorldViewProjection : register(c0);

struct VSInput {
    float4 position : POSITION;   // xy = world/pixel position, w = 1
    float4 color    : COLOR0;     // RGBA, 0..1
    float2 texcoord : TEXCOORD0;
};

struct VSOutput {
    float4 position : POSITION;
    float4 color    : COLOR0;
    float2 texcoord : TEXCOORD0;
};

VSOutput main(VSInput input) {
    VSOutput output;
    output.position = mul(uWorldViewProjection, float4(input.position.xyz, 1.0f));
    output.color    = input.color;
    output.texcoord = input.texcoord;
    return output;
}
