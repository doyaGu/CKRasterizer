struct VS_OUTPUT
{
    float4 position : SV_POSITION;
    float4 color : COLOR;
    float2 texcoord : TEXCOORD;
};

static const dword AFLG_ALPHATESTEN = 0x10u;
static const dword AFLG_ALPHAFUNCMASK = 0xFu;

cbuffer VSCBuf: register(b0)
{
    matrix total_mat;
    matrix viewport_mat;
};

cbuffer PSCBuf : register(b0)
{
    dword alpha_flags;
    float alpha_thresh;
};