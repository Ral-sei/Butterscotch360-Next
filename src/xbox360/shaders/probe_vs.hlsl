// Xbox 360 fxc shader: vertex colour, clip-space position.
struct VSInput {
    float4 position : POSITION;
    float4 color : COLOR0;
    float2 texcoord : TEXCOORD0;
};
struct VSOutput {
    float4 position : POSITION;
    float4 color : COLOR0;
    float2 texcoord : TEXCOORD0;
};
VSOutput main(VSInput input) {
    VSOutput output;
    output.position = input.position;
    output.color = input.color;
    output.texcoord = input.texcoord;
    return output;
}
