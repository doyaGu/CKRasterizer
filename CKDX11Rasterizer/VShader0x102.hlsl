#include "Common.hlsl"

VS_OUTPUT VShader0x102(float3 position : SV_POSITION, float2 texcoord : TEXCOORD)
{
    VS_OUTPUT output;
    float4 pos4 = float4(position, 1.0);
    output.position = mul(pos4, total_mat);
    output.color = float4(texcoord, 1.0, 1.0);
    output.texcoord = texcoord;
    return output;
}