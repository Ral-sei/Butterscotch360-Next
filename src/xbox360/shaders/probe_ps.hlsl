// Xbox 360 fxc shader: pass interpolated colour to the render target.
sampler2D probeTexture : register(s0);

float4 main(float4 color : COLOR0, float2 texcoord : TEXCOORD0) : COLOR0 {
    return tex2D(probeTexture, texcoord) * color;
}
