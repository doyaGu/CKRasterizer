#include "CKDX11Rasterizer.h"
#include "CKDX11RasterizerCommon.h"

#define LOGGING 0
#define STATUS 1

#if LOGGING
#include <conio.h>
static bool step_mode = false;
#endif

#if STATUS
static int directbat = 0;
static int vbbat = 0;
static int vbibbat = 0;
#endif

static const char *shader = R"(
struct VS_OUTPUT
{
    float4 position : SV_POSITION;
    float4 color : COLOR;
};

struct VS_INPUT_COLOR {
};

cbuffer CBuf
{
    matrix total_mat;
    //matrix viewport_mat;
};

VS_OUTPUT VShaderColor(float4 position : SV_POSITION, float4 color: COLOR, float2 texcoord: TEXCOORD)
{
    VS_OUTPUT output;
    output.position.xyzw = position.xywz;
    output.position.w = 1.0;
    //output.position = mul(output.position, viewport_mat);
    output.color = float4(texcoord, 1.0, 1.0);
    return output;
}


VS_OUTPUT VShaderNormal(float3 position : SV_POSITION, float3 normal: NORMAL, float2 texcoord: TEXCOORD)
{
    VS_OUTPUT output;
    float4 pos4 = float4(position, 1.0);
    output.position = mul(pos4, total_mat);
    output.color = float4(texcoord, 1.0, 1.0);
    return output;
}

VS_OUTPUT VShaderSpec(float3 position : SV_POSITION, float4 diffuse: COLOR, float4 specular: COLOR, float2 texcoord: TEXCOORD)
{
    VS_OUTPUT output;
    float4 pos4 = float4(position, 1.0);
    output.position = mul(pos4, total_mat);
    output.color = float4(texcoord, 1.0, 1.0);
    return output;
}

VS_OUTPUT VShader0x102(float3 position : SV_POSITION, float2 texcoord: TEXCOORD) {
    VS_OUTPUT output;
    float4 pos4 = float4(position, 1.0);
    output.position = mul(pos4, total_mat);
    output.color = float4(texcoord, 1.0, 1.0);
    return output;
}

VS_OUTPUT VShader0x142(float3 position : SV_POSITION, float4 diffuse: COLOR, float2 texcoord: TEXCOORD) {
    VS_OUTPUT output;
    float4 pos4 = float4(position, 1.0);
    output.position = mul(pos4, total_mat);
    output.color = float4(texcoord, 1.0, 1.0);
    return output;
}

VS_OUTPUT VShader0x1c4(float3 position : SV_POSITION, float4 diffuse: COLOR, float4 specular: COLOR, float2 texcoord: TEXCOORD) {
    VS_OUTPUT output;
    float4 pos4 = float4(position, 1.0);
    output.position = mul(pos4, total_mat);
    output.color = float4(texcoord, 1.0, 1.0);
    return output;
}

float4 PShader(float4 position : SV_POSITION, float4 color : COLOR) : SV_TARGET
{
    return color;
}
)";
CKDX11RasterizerContext::CKDX11RasterizerContext() {}
CKDX11RasterizerContext::~CKDX11RasterizerContext() {}

void CKDX11RasterizerContext::SetTitleStatus(const char *fmt, ...)
{
    std::string ts;
    if (fmt)
    {
        va_list args;
        va_start(args, fmt);
        va_list argsx;
        va_copy(argsx, args);
        ts.resize(vsnprintf(NULL, 0, fmt, argsx) + 1);
        va_end(argsx);
        vsnprintf(ts.data(), ts.size(), fmt, args);
        va_end(args);
        ts = m_OriginalTitle + " | " + ts;
    }
    else
        ts = m_OriginalTitle;

    SetWindowTextA(GetAncestor((HWND)m_Window, GA_ROOT), ts.c_str());
}

CKBOOL CKDX11RasterizerContext::Create(WIN_HANDLE Window, int PosX, int PosY, int Width, int Height, int Bpp,
                                       CKBOOL Fullscreen, int RefreshRate, int Zbpp, int StencilBpp)
{
#if (LOGGING)
    AllocConsole();
    freopen("CON", "w", stdout);
    freopen("CON", "w", stderr);
#endif
    HRESULT hr;

    m_InCreateDestroy = TRUE;
    SetWindowLongA((HWND)Window, GWL_STYLE, WS_OVERLAPPED | WS_SYSMENU);
    SetClassLongPtr((HWND)Window, GCLP_HBRBACKGROUND, (LONG)GetStockObject(NULL_BRUSH));

    DXGI_SWAP_CHAIN_DESC scd;
    ZeroMemory(&scd, sizeof(DXGI_SWAP_CHAIN_DESC));

    m_AllowTearing = static_cast<CKDX11Rasterizer *>(m_Owner)->m_TearingSupport;
    scd.BufferCount = 2;
    scd.BufferDesc.Width = Width;
    scd.BufferDesc.Height = Height;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; // TODO: just use 32-bit color here, too lazy to check if valid
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = (HWND)Window;
    scd.SampleDesc.Count = 1; // TODO: multisample support
    scd.Windowed = !Fullscreen;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    scd.Flags = m_AllowTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    UINT creationFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL featureLevels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                         D3D_FEATURE_LEVEL_10_0, D3D_FEATURE_LEVEL_9_3,  D3D_FEATURE_LEVEL_9_1};
#if defined(DEBUG) || defined(_DEBUG)
    creationFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    hr = D3D11CreateDeviceAndSwapChain(static_cast<CKDX11RasterizerDriver *>(m_Driver)->m_Adapter.Get(),
                                       D3D_DRIVER_TYPE_UNKNOWN, nullptr, creationFlags, featureLevels,
                                       _countof(featureLevels), D3D11_SDK_VERSION,
                                       &scd, &m_Swapchain, &m_Device, nullptr, &m_DeviceContext);
    if (hr == E_INVALIDARG)
    {
        D3DCall(D3D11CreateDeviceAndSwapChain(static_cast<CKDX11RasterizerDriver *>(m_Driver)->m_Adapter.Get(),
                                              D3D_DRIVER_TYPE_UNKNOWN, nullptr, creationFlags, &featureLevels[1],
                                              _countof(featureLevels) - 1,
                                              D3D11_SDK_VERSION, &scd, &m_Swapchain, &m_Device, nullptr,
                                              &m_DeviceContext));
    }
    else
    {
        D3DCall(hr);
    }

    ID3D11Texture2D *pBuffer = nullptr;
    D3DCall(m_Swapchain->GetBuffer(0, IID_PPV_ARGS(&pBuffer)));
    D3DCall(m_Device->CreateRenderTargetView(pBuffer, nullptr, &m_BackBuffer));
    D3DCall(pBuffer->Release());
    m_DeviceContext->OMSetRenderTargets(1, m_BackBuffer.GetAddressOf(), NULL);

    m_Window = (HWND)Window;

    m_OriginalTitle.resize(GetWindowTextLengthA(GetAncestor((HWND)Window, GA_ROOT)) + 1);
    GetWindowTextA(GetAncestor((HWND)Window, GA_ROOT), m_OriginalTitle.data(), m_OriginalTitle.size());
    while (m_OriginalTitle.back() == '\0')
        m_OriginalTitle.pop_back();

    m_PosX = PosX;
    m_PosY = PosY;
    m_Fullscreen = Fullscreen;
    m_Bpp = Bpp;
    m_ZBpp = Zbpp;
    m_Width = Width;
    m_Height = Height;
    if (m_Fullscreen)
        m_Driver->m_Owner->m_FullscreenContext = this;

    CKDWORD vs_color_idx = 0, vs_normal_idx = 1, vs_spec_idx = 2, vs_0x102_idx = 3, vs_0x142_idx = 4, vs_0x1c4_idx = 5;
    CKDWORD ps_idx = 0;
    CKDX11VertexShaderDesc vs_desc;
    vs_desc.m_Function = (CKDWORD*)shader;
    vs_desc.m_FunctionSize = strlen(shader);
    vs_desc.DxEntryPoint = "VShaderColor";
    vs_desc.DxFVF = CKRST_VF_RASTERPOS | CKRST_VF_DIFFUSE | CKRST_VF_TEX1;
    CreateObject(vs_color_idx, CKRST_OBJ_VERTEXSHADER, &vs_desc);

    CKDX11PixelShaderDesc ps_desc;
    ps_desc.m_Function = (CKDWORD *)shader;
    ps_desc.m_FunctionSize = strlen(shader);
    CreateObject(ps_idx, CKRST_OBJ_PIXELSHADER, &ps_desc);

    CKDX11VertexShaderDesc vs_desc_normal;
    vs_desc_normal.m_Function = (CKDWORD *)shader;
    vs_desc_normal.m_FunctionSize = strlen(shader);
    vs_desc_normal.DxEntryPoint = "VShaderNormal";
    vs_desc_normal.DxFVF = CKRST_VF_POSITION | CKRST_VF_NORMAL | CKRST_VF_TEX1;
    CreateObject(vs_normal_idx, CKRST_OBJ_VERTEXSHADER, &vs_desc_normal);

    CKDX11VertexShaderDesc vs_spec_normal;
    vs_spec_normal.m_Function = (CKDWORD *)shader;
    vs_spec_normal.m_FunctionSize = strlen(shader);
    vs_spec_normal.DxEntryPoint = "VShaderSpec";
    vs_spec_normal.DxFVF = CKRST_VF_POSITION | CKRST_VF_SPECULAR | CKRST_VF_DIFFUSE | CKRST_VF_TEX1;
    CreateObject(vs_spec_idx, CKRST_OBJ_VERTEXSHADER, &vs_spec_normal);

    CKDX11VertexShaderDesc vs_0x102;
    vs_0x102.m_Function = (CKDWORD *)shader;
    vs_0x102.m_FunctionSize = strlen(shader);
    vs_0x102.DxEntryPoint = "VShader0x102";
    vs_0x102.DxFVF = CKRST_VF_POSITION | CKRST_VF_TEX1;
    CreateObject(vs_0x102_idx, CKRST_OBJ_VERTEXSHADER, &vs_0x102);

    CKDX11VertexShaderDesc vs_0x142;
    vs_0x142.m_Function = (CKDWORD *)shader;
    vs_0x142.m_FunctionSize = strlen(shader);
    vs_0x142.DxEntryPoint = "VShader0x142";
    vs_0x142.DxFVF = CKRST_VF_POSITION | CKRST_VF_DIFFUSE | CKRST_VF_TEX1;
    CreateObject(vs_0x142_idx, CKRST_OBJ_VERTEXSHADER, &vs_0x142);

    CKDX11VertexShaderDesc vs_0x1c4;
    vs_0x1c4.m_Function = (CKDWORD *)shader;
    vs_0x1c4.m_FunctionSize = strlen(shader);
    vs_0x1c4.DxEntryPoint = "VShader0x1c4";
    vs_0x1c4.DxFVF = CKRST_VF_RASTERPOS | CKRST_VF_DIFFUSE | CKRST_VF_SPECULAR | CKRST_VF_TEX1;
    CreateObject(vs_0x1c4_idx, CKRST_OBJ_VERTEXSHADER, &vs_0x1c4);

    m_VertexShaderMap[CKRST_VF_RASTERPOS | CKRST_VF_DIFFUSE | CKRST_VF_TEX1] = vs_color_idx;
    m_VertexShaderMap[CKRST_VF_POSITION | CKRST_VF_NORMAL | CKRST_VF_TEX1] = vs_normal_idx;
    m_VertexShaderMap[CKRST_VF_POSITION | CKRST_VF_SPECULAR | CKRST_VF_DIFFUSE | CKRST_VF_TEX1] = vs_spec_idx;
    m_VertexShaderMap[CKRST_VF_POSITION | CKRST_VF_TEX1] = vs_0x102_idx;
    m_VertexShaderMap[CKRST_VF_POSITION | CKRST_VF_DIFFUSE | CKRST_VF_TEX1] = vs_0x142_idx;
    m_VertexShaderMap[CKRST_VF_RASTERPOS | CKRST_VF_DIFFUSE | CKRST_VF_SPECULAR | CKRST_VF_TEX1] = vs_0x1c4_idx;
    // m_CurrentVShader = vs_idx;
    m_CurrentPShader = ps_idx;

    // auto *vs = static_cast<CKDX11VertexShaderDesc *>(m_VertexShaders[vs_idx]);
    auto *ps = static_cast<CKDX11PixelShaderDesc *>(m_PixelShaders[ps_idx]);
    //
    // vs->Bind(this);
    ps->Bind(this);

    m_ConstantBuffer.Create(this);

    // m_WorldMatrix = VxMatrix::Identity();
    // m_ViewMatrix = VxMatrix::Identity();
    // m_ModelViewMatrix = VxMatrix::Identity();
    // m_ProjectionMatrix = VxMatrix::Identity();
    // m_ViewProjMatrix = VxMatrix::Identity();
    // m_TotalMatrix = VxMatrix::Identity();

    m_InCreateDestroy = FALSE;

    return SUCCEEDED(hr);
}
CKBOOL CKDX11RasterizerContext::Resize(int PosX, int PosY, int Width, int Height, CKDWORD Flags)
{
    return CKRasterizerContext::Resize(PosX, PosY, Width, Height, Flags);
}
CKBOOL CKDX11RasterizerContext::Clear(CKDWORD Flags, CKDWORD Ccol, float Z, CKDWORD Stencil, int RectCount,
                                      CKRECT *rects)
{
    if (!m_BackBuffer)
        return FALSE;
    if (Flags & CKRST_CTXCLEAR_COLOR)
        m_DeviceContext->ClearRenderTargetView(m_BackBuffer.Get(), m_ClearColor);
    /* if (Flags & CKRST_CTXCLEAR_STENCIL)
        D3DCall(m_DeviceContext->ClearDepthStencilView());
    if (Flags & CKRST_)
    D3DCall(m_DeviceContext->ClearRenderTargetView())*/
    return TRUE;
}
CKBOOL CKDX11RasterizerContext::BackToFront(CKBOOL vsync) {
    if (!m_SceneBegined)
        EndScene();
#if STATUS
    // fprintf(stderr, "swap\n");
    SetTitleStatus("D3D11 | batch stats: direct %d, vb %d, vbib %d", directbat, vbbat,
                     vbibbat);

    directbat = 0;
    vbbat = 0;
    vbibbat = 0;
#endif
    HRESULT hr;
    
    D3DCall(m_Swapchain->Present(vsync ? 1 : 0, (m_AllowTearing && !m_Fullscreen && !vsync) ? DXGI_PRESENT_ALLOW_TEARING : 0));
    return SUCCEEDED(hr);
}

CKBOOL CKDX11RasterizerContext::BeginScene()
{
    if (m_SceneBegined)
        return FALSE;
    m_DeviceContext->OMSetRenderTargets(1, m_BackBuffer.GetAddressOf(), NULL);
    m_SceneBegined = TRUE;
    return TRUE;
}
CKBOOL CKDX11RasterizerContext::EndScene()
{
    if (!m_SceneBegined)
        return FALSE;
    m_MatrixUptodate = 0;
    m_ConstantBufferUptodate = FALSE;
    m_SceneBegined = FALSE;
    return TRUE;
}
CKBOOL CKDX11RasterizerContext::SetLight(CKDWORD Light, CKLightData *data)
{
    return CKRasterizerContext::SetLight(Light, data);
}
CKBOOL CKDX11RasterizerContext::EnableLight(CKDWORD Light, CKBOOL Enable)
{
    return CKRasterizerContext::EnableLight(Light, Enable);
}
CKBOOL CKDX11RasterizerContext::SetMaterial(CKMaterialData *mat) { return CKRasterizerContext::SetMaterial(mat); }

CKBOOL CKDX11RasterizerContext::SetViewport(CKViewportData *data) {
    ZeroMemory(&m_Viewport, sizeof(D3D11_VIEWPORT));
    m_Viewport.TopLeftX = (FLOAT)data->ViewX;
    m_Viewport.TopLeftY = (FLOAT)data->ViewY;
    m_Viewport.Width = (FLOAT)data->ViewWidth;
    m_Viewport.Height = (FLOAT)data->ViewHeight;
    m_Viewport.MaxDepth = 1.0f;
    m_Viewport.MinDepth = 0.0f;
    //viewport.MaxDepth = data->ViewZMax;
    //viewport.MinDepth = data->ViewZMin;
    m_DeviceContext->RSSetViewports(1, &m_Viewport);
    //
    // m_CBuffer.ViewportMatrix = VxMatrix::Identity();
    // float(*m)[4] = (float(*)[4]) &m_CBuffer.ViewportMatrix;
    // m[0][0] = 2. / data->ViewWidth;
    // m[1][1] = 2. / data->ViewHeight;
    // m[2][2] = 0;
    // m[3][0] = -(-2. * data->ViewX + data->ViewWidth) / data->ViewWidth;
    // m[3][1] = (-2. * data->ViewY + data->ViewHeight) / data->ViewHeight;
    return TRUE;
}

CKBOOL CKDX11RasterizerContext::SetTransformMatrix(VXMATRIX_TYPE Type, const VxMatrix &Mat)
{
    ZoneScopedN(__FUNCTION__);
    CKDWORD UnityMatrixMask = 0;
    // switch (Type)
    // {
    //     case VXMATRIX_WORLD:
    //         m_WorldMatrix = Mat;
    //         Vx3DMultiplyMatrix(m_ModelViewMatrix, m_ViewMatrix, m_WorldMatrix);
    //         m_MatrixUptodate &= ~0U ^ WORLD_TRANSFORM;
    //         break;
    //     case VXMATRIX_VIEW:
    //         m_ViewMatrix = Mat;
    //         Vx3DMultiplyMatrix(m_ModelViewMatrix, m_ViewMatrix, m_WorldMatrix);
    //         m_MatrixUptodate = 0;
    //         break;
    //     case VXMATRIX_PROJECTION:
    //         m_ProjectionMatrix = Mat;
    //         m_MatrixUptodate = 0;
    //         Vx3DMultiplyMatrix(m_ViewProjMatrix, m_ProjectionMatrix, m_ViewMatrix);
    //         break;
    //     case VXMATRIX_TEXTURE0:
    //     case VXMATRIX_TEXTURE1:
    //     case VXMATRIX_TEXTURE2:
    //     case VXMATRIX_TEXTURE3:
    //     case VXMATRIX_TEXTURE4:
    //     case VXMATRIX_TEXTURE5:
    //     case VXMATRIX_TEXTURE6:
    //     case VXMATRIX_TEXTURE7:
    //         UnityMatrixMask = TEXTURE0_TRANSFORM << (Type - TEXTURE1_TRANSFORM);
    //         break;
    //     default:
    //         return FALSE;
    // }
    auto ret = CKRasterizerContext::SetTransformMatrix(Type, Mat);
    if (VxMatrix::Identity() == Mat)
    {
        if ((m_UnityMatrixMask & UnityMatrixMask) != 0)
            return TRUE;
        m_UnityMatrixMask |= UnityMatrixMask;
    }
    else
    {
        m_UnityMatrixMask &= ~UnityMatrixMask;
    }
    return ret;
}
CKBOOL CKDX11RasterizerContext::SetRenderState(VXRENDERSTATETYPE State, CKDWORD Value)
{
    return CKRasterizerContext::SetRenderState(State, Value);
}
CKBOOL CKDX11RasterizerContext::GetRenderState(VXRENDERSTATETYPE State, CKDWORD *Value)
{
    return CKRasterizerContext::GetRenderState(State, Value);
}
CKBOOL CKDX11RasterizerContext::SetTexture(CKDWORD Texture, int Stage)
{
    return CKRasterizerContext::SetTexture(Texture, Stage);
}
CKBOOL CKDX11RasterizerContext::SetTextureStageState(int Stage, CKRST_TEXTURESTAGESTATETYPE Tss, CKDWORD Value)
{
    return CKRasterizerContext::SetTextureStageState(Stage, Tss, Value);
}
CKBOOL CKDX11RasterizerContext::SetVertexShader(CKDWORD VShaderIndex)
{
//     if (m_CurrentVShader == VShaderIndex)
//         return TRUE;
//     if (VShaderIndex >= m_VertexShaders.Size())
//         return FALSE;
//     auto *vs = static_cast<CKDX11VertexShaderDesc *>(m_VertexShaders[VShaderIndex]);
//     if (!vs)
//         return FALSE;
// #if LOGGING
//     fprintf(stderr, "IA: vs %s\n", vs->DxEntryPoint);
// #endif
//     vs->Bind(this);
//     m_CurrentVShader = VShaderIndex;
    return TRUE;
}
CKBOOL CKDX11RasterizerContext::SetPixelShader(CKDWORD PShaderIndex)
{
    if (m_CurrentPShader == PShaderIndex)
        return TRUE;
    if (PShaderIndex >= m_PixelShaders.Size())
        return FALSE;
    auto *ps = static_cast<CKDX11VertexShaderDesc *>(m_VertexShaders[PShaderIndex]);
    if (!ps)
        return FALSE;
    ps->Bind(this);
    m_CurrentPShader = PShaderIndex;
    return TRUE;
}
CKBOOL CKDX11RasterizerContext::SetVertexShaderConstant(CKDWORD Register, const void *Data, CKDWORD CstCount)
{
    return CKRasterizerContext::SetVertexShaderConstant(Register, Data, CstCount);
}
CKBOOL CKDX11RasterizerContext::SetPixelShaderConstant(CKDWORD Register, const void *Data, CKDWORD CstCount)
{
    return CKRasterizerContext::SetPixelShaderConstant(Register, Data, CstCount);
}


CKDWORD CKDX11RasterizerContext::GenerateIB(void *indices, int indexCount, int *startIndex)
{
    if (!indices)
        return -1;
    // m_IBCounter = (m_IBCounter + 1) % m_IndexBuffers.Size();
    CKDWORD IB = 0;
    *startIndex = 0;
    // Prepare index buffer for given IB index
    auto *ibo = m_IndexBuffers[IB];
    if (!ibo || ibo->m_MaxIndexCount < indexCount)
    {
        CKIndexBufferDesc desc;
        desc.m_MaxIndexCount = indexCount + 100 < 4096 ? 4096 : indexCount + 100;
        desc.m_CurrentICount = 0;
        desc.m_Flags = CKRST_VB_WRITEONLY | CKRST_VB_DYNAMIC;
        if (!CreateIndexBuffer(IB, &desc))
            return -1;
        ibo = m_IndexBuffers[IB];
    }
    void *pData = nullptr;
    if (indexCount + ibo->m_CurrentICount < ibo->m_MaxIndexCount)
    {
        pData = LockIndexBuffer(IB, ibo->m_CurrentICount, indexCount, CKRST_LOCK_NOOVERWRITE);
        *startIndex = ibo->m_CurrentICount;
        ibo->m_CurrentICount += indexCount;
    }
    else
    {
        pData = LockIndexBuffer(IB, 0, indexCount, CKRST_LOCK_DISCARD);
        ibo->m_CurrentICount = indexCount;
    }
    if (pData)
        memcpy(pData, indices, indexCount * sizeof(CKWORD));
    UnlockIndexBuffer(IB);
    return IB;
}

CKDWORD CKDX11RasterizerContext::TriangleFanToStrip(int VOffset, int VCount, int *startIndex)
{
    std::vector<int> strip_index;
    // Center at VOffset
    for (int i = 1; i < VCount; ++i)
    {
        strip_index.emplace_back(i + VOffset);
        strip_index.emplace_back(VOffset);
    }
    strip_index.pop_back();
    return GenerateIB(strip_index.data(), strip_index.size(), startIndex);
}

CKDWORD CKDX11RasterizerContext::TriangleFanToStrip(CKWORD *indices, int count, int *startIndex)
{
    if (!indices)
        return -1;
    std::vector<int> strip_index;
    CKWORD center = indices[0];
    for (int i = 1; i < count; ++i)
    {
        strip_index.emplace_back(indices[i]);
        strip_index.emplace_back(center);
    }
    strip_index.pop_back();
    return GenerateIB(strip_index.data(), strip_index.size(), startIndex);
}

CKBOOL CKDX11RasterizerContext::DrawPrimitive(VXPRIMITIVETYPE pType, CKWORD *indices, int indexcount,
                                              VxDrawPrimitiveData *data)
{
#if (LOGGING) || (STATUS)
    ++directbat;
#endif
    ZoneScopedN(__FUNCTION__);
    if (!m_SceneBegined)
        BeginScene();
    CKBOOL clip = 0;
    CKDWORD vertexSize;
    CKDWORD vertexFormat = CKRSTGetVertexFormat((CKRST_DPFLAGS)data->Flags, vertexSize);
    if ((data->Flags & CKRST_DP_DOCLIP))
    {
        SetRenderState(VXRENDERSTATE_CLIPPING, 1);
        clip = 1;
    }
    else
    {
        SetRenderState(VXRENDERSTATE_CLIPPING, 0);
    }

    CKDWORD VB = GetDynamicVertexBuffer(vertexFormat, data->VertexCount, vertexSize, clip);
    auto *vbo = static_cast<CKDX11VertexBufferDesc *>(m_VertexBuffers[VB]);
    // if (++m_DirectVertexBufferCounter >= DYNAMIC_VBO_COUNT)
    //     m_DirectVertexBufferCounter = 0;
    // auto* vbo = m_DynamicVertexBuffer[m_DirectVertexBufferCounter];
    if (vbo && vbo->m_MaxVertexCount < data->VertexCount)
    {
        delete vbo;
        m_VertexBuffers[VB] = nullptr;
        vbo = nullptr;
    }
    if (!vbo)
    {
        vbo = new CKDX11VertexBufferDesc;
        vbo->m_Flags = CKRST_VB_WRITEONLY | CKRST_VB_DYNAMIC;
        vbo->m_VertexFormat = vertexFormat;
        vbo->m_VertexSize = vertexSize;
        vbo->m_MaxVertexCount = (data->VertexCount + 100 > DEFAULT_VB_SIZE) ? data->VertexCount + 100 : DEFAULT_VB_SIZE;
        vbo->m_CurrentVCount = 0;
        vbo->Create(this);
    }
    m_VertexBuffers[VB] = vbo;

    assert(vertexSize == vbo->m_VertexSize);

    void *pbData = nullptr;
    CKDWORD vbase = 0;
    if (vbo->m_CurrentVCount + data->VertexCount <= vbo->m_MaxVertexCount)
    {
        TracyPlot("Lock offset", (int64_t)vertexSize * vbo->m_CurrentVCount);
        TracyPlot("Lock len", (int64_t)vertexSize * data->VertexCount);
        pbData = vbo->Lock(this, vertexSize * vbo->m_CurrentVCount, vertexSize * data->VertexCount, false);
        vbase = vbo->m_CurrentVCount;
        vbo->m_CurrentVCount += data->VertexCount;
    }
    else
    {
        TracyPlot("Lock offset", 0ll);
        TracyPlot("Lock len", (int64_t)vertexSize * data->VertexCount);
        pbData = vbo->Lock(this, 0, vertexSize * data->VertexCount, true);
        vbo->m_CurrentVCount = data->VertexCount;
    }
    {
        ZoneScopedN("CKRSTLoadVertexBuffer");
        CKRSTLoadVertexBuffer(static_cast<CKBYTE *>(pbData), vertexFormat, vertexSize, data);
    }
    vbo->Unlock(this);
    UINT stride = vbo->m_VertexSize;
    UINT offset = 0;
    m_DeviceContext->IASetVertexBuffers(0, 1, vbo->DxBuffer.GetAddressOf(), &stride, &offset);
    return InternalDrawPrimitive(pType, vbo, vbase, data->VertexCount, indices, indexcount);
}

CKBOOL CKDX11RasterizerContext::DrawPrimitiveVB(VXPRIMITIVETYPE pType, CKDWORD VB, CKDWORD StartVIndex,
                                                CKDWORD VertexCount, CKWORD *indices, int indexcount)
{
#if (LOGGING) || (STATUS)
    ++vbbat;
#endif
    ZoneScopedN(__FUNCTION__);
    if (VB >= m_VertexBuffers.Size())
        return FALSE;
    CKVertexBufferDesc *vbo = m_VertexBuffers[VB];
    if (!vbo)
        return FALSE;
    if (!m_SceneBegined)
        BeginScene();
    return InternalDrawPrimitive(pType, static_cast<CKDX11VertexBufferDesc *>(vbo), StartVIndex,
                                   VertexCount, indices, indexcount);
}

CKBOOL CKDX11RasterizerContext::InternalDrawPrimitive(VXPRIMITIVETYPE pType, CKDX11VertexBufferDesc *vbo,
                                                        CKDWORD StartVertex, CKDWORD VertexCount, CKWORD *indices,
                                                        int indexcount)
{
    ZoneScopedN(__FUNCTION__);
    D3D_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
    switch (pType & 0xf)
    {
        case VX_LINELIST:
            topology = D3D_PRIMITIVE_TOPOLOGY_LINELIST;
            break;
        case VX_LINESTRIP:
            topology = D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
            break;
        case VX_TRIANGLELIST:
            topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
            break;
        case VX_TRIANGLESTRIP:
            topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
            break;
        case VX_TRIANGLEFAN:
            // D3D11 does not support triangle fan, leave it here.
            // assert(false);
            return FALSE;
            topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
            break;
        default:
            break;
    }
    m_DeviceContext->IASetPrimitiveTopology(topology);
    UINT stride = vbo->m_VertexSize;
    UINT offset = 0;
    m_DeviceContext->IASetVertexBuffers(0, 1, vbo->DxBuffer.GetAddressOf(), &stride, &offset);
    int ibbasecnt = 0;
    if (indices)
    {
        CKDX11IndexBufferDesc *ibo = nullptr;
        void *pdata = nullptr;
        auto iboid = m_DynamicIndexBufferCounter++;
        if (m_DynamicIndexBufferCounter >= DYNAMIC_IBO_COUNT)
            m_DynamicIndexBufferCounter = 0;
        if (!m_DynamicIndexBuffer[iboid] || m_DynamicIndexBuffer[iboid]->m_MaxIndexCount < indexcount)
        {
            if (m_DynamicIndexBuffer[iboid])
                delete m_DynamicIndexBuffer[iboid];
            ibo = new CKDX11IndexBufferDesc;
            ibo->m_Flags = CKRST_VB_WRITEONLY | CKRST_VB_DYNAMIC;
            ibo->m_MaxIndexCount = indexcount + 100 < DEFAULT_VB_SIZE ? DEFAULT_VB_SIZE : indexcount + 100;
            ibo->m_CurrentICount = 0;
            if (!ibo->Create(this))
            {
                m_DynamicIndexBuffer[iboid] = nullptr;
                return FALSE;
            }
            m_DynamicIndexBuffer[iboid] = ibo;
        }
        ibo = m_DynamicIndexBuffer[iboid];
        if (indexcount + ibo->m_CurrentICount <= ibo->m_MaxIndexCount)
        {
            pdata = ibo->Lock(this, sizeof(CKWORD) * ibo->m_CurrentICount, sizeof(CKWORD) * indexcount, false);
            ibbasecnt = ibo->m_CurrentICount;
            ibo->m_CurrentICount += indexcount;
        }
        else
        {
            pdata = ibo->Lock(this, 0, sizeof(CKWORD) * indexcount, true);
            ibbasecnt = 0;
            ibo->m_CurrentICount = indexcount;
        }
        if (pdata)
            memcpy(pdata, indices, sizeof(CKWORD) * indexcount);
        ibo->Unlock(this);
        m_DeviceContext->IASetIndexBuffer(ibo->DxBuffer.Get(), DXGI_FORMAT_R16_UINT, 0);
    }

    AssemblyInput(vbo);

    if (indices)
    {
        m_DeviceContext->DrawIndexed(indexcount, ibbasecnt, StartVertex);

    } else
        m_DeviceContext->Draw(VertexCount, StartVertex);
    return TRUE;
}

CKBOOL CKDX11RasterizerContext::DrawPrimitiveVBIB(VXPRIMITIVETYPE pType, CKDWORD VB, CKDWORD IB, CKDWORD MinVIndex,
                                                  CKDWORD VertexCount, CKDWORD StartIndex, int Indexcount)
{
#if (LOGGING) || (STATUS)
    ++vbibbat;
#endif
    // assert(pType != VX_TRIANGLEFAN);

    ZoneScopedN(__FUNCTION__);
    if (VB >= m_VertexBuffers.Size())
        return FALSE;

    if (IB >= m_IndexBuffers.Size())
        return FALSE;

    auto *vbo = static_cast<CKDX11VertexBufferDesc *>(m_VertexBuffers[VB]);
    if (!vbo)
        return FALSE;

    auto *ibo = static_cast<CKDX11IndexBufferDesc *>(m_IndexBuffers[IB]);
    if (!ibo)
        return FALSE;

    AssemblyInput(vbo);

    m_DeviceContext->VSSetConstantBuffers(0, 1, m_ConstantBuffer.DxBuffer.GetAddressOf());
    // UINT vertexOffsetInBytes = 0;
    // m_DeviceContext->IASetVertexBuffers(0, 1, vbo->DxBuffer.GetAddressOf(),
    //     (UINT *)&vbo->m_VertexSize, &vertexOffsetInBytes);

    m_DeviceContext->IASetIndexBuffer(ibo->DxBuffer.Get(), DXGI_FORMAT_R16_UINT, 0);
    D3D_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
    switch (pType & 0xf)
    {
        case VX_LINELIST:
            topology = D3D_PRIMITIVE_TOPOLOGY_LINELIST;
            break;
        case VX_LINESTRIP:
            topology = D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
            break;
        case VX_TRIANGLELIST:
            topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
            break;
        case VX_TRIANGLESTRIP:
            topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
            break;
        case VX_TRIANGLEFAN:
            // D3D11 does not support triangle fan, leave it here.
            // assert(false);
            return FALSE;
            topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
            break;
        default:
            break;
    }
    m_DeviceContext->IASetPrimitiveTopology(topology);

#if LOGGING
    fprintf(stderr, "IA: VB offset: %d, vsize: %d\n", MinVIndex, vbo->m_VertexSize);
    fprintf(stderr, "IA: IB offset: %d\n", StartIndex);
#endif
    assert(VertexCount <= vbo->m_MaxVertexCount - MinVIndex); // check if vb is big enough
    assert(Indexcount <= ibo->m_MaxIndexCount - StartIndex); // check if ib is big enough
    m_DeviceContext->DrawIndexed(Indexcount, StartIndex, MinVIndex);
    return TRUE;
}
CKBOOL CKDX11RasterizerContext::CreateObject(CKDWORD ObjIndex, CKRST_OBJECTTYPE Type, void *DesiredFormat)
{
    int result;

    if (ObjIndex >= m_Textures.Size())
        return FALSE;
    switch (Type)
    {
        case CKRST_OBJ_TEXTURE:
            result = CreateTexture(ObjIndex, static_cast<CKTextureDesc *>(DesiredFormat));
            break;
        case CKRST_OBJ_SPRITE:
            {
                return 0;
                result = CreateSprite(ObjIndex, static_cast<CKSpriteDesc *>(DesiredFormat));
                CKSpriteDesc *desc = m_Sprites[ObjIndex];
                fprintf(stderr, "idx: %d\n", ObjIndex);
                for (auto it = desc->Textures.Begin(); it != desc->Textures.End(); ++it)
                {
                    fprintf(stderr, "(%d,%d) WxH: %dx%d, SWxSH: %dx%d\n", it->x, it->y, it->w, it->h, it->sw, it->sh);
                }
                fprintf(stderr, "---\n");
                break;
            }
        case CKRST_OBJ_VERTEXBUFFER:
            result = CreateVertexBuffer(ObjIndex, static_cast<CKVertexBufferDesc *>(DesiredFormat));
            break;
        case CKRST_OBJ_INDEXBUFFER:
            result = CreateIndexBuffer(ObjIndex, static_cast<CKIndexBufferDesc *>(DesiredFormat));
            break;
        case CKRST_OBJ_VERTEXSHADER:
            result = CreateVertexShader(ObjIndex, static_cast<CKVertexShaderDesc *>(DesiredFormat));
            break;
        case CKRST_OBJ_PIXELSHADER:
            result = CreatePixelShader(ObjIndex, static_cast<CKPixelShaderDesc *>(DesiredFormat));
            break;
        default:
            return 0;
    }
    return result;
}

CKBOOL CKDX11RasterizerContext::LoadTexture(CKDWORD Texture, const VxImageDescEx &SurfDesc, int miplevel)
{
    return CKRasterizerContext::LoadTexture(Texture, SurfDesc, miplevel);
}
CKBOOL CKDX11RasterizerContext::CopyToTexture(CKDWORD Texture, VxRect *Src, VxRect *Dest, CKRST_CUBEFACE Face)
{
    return CKRasterizerContext::CopyToTexture(Texture, Src, Dest, Face);
}
CKBOOL CKDX11RasterizerContext::SetTargetTexture(CKDWORD TextureObject, int Width, int Height, CKRST_CUBEFACE Face,
                                                 CKBOOL GenerateMipMap)
{
    return CKRasterizerContext::SetTargetTexture(TextureObject, Width, Height, Face, GenerateMipMap);
}
CKBOOL CKDX11RasterizerContext::DrawSprite(CKDWORD Sprite, VxRect *src, VxRect *dst)
{
    return CKRasterizerContext::DrawSprite(Sprite, src, dst);
}
int CKDX11RasterizerContext::CopyToMemoryBuffer(CKRECT *rect, VXBUFFER_TYPE buffer, VxImageDescEx &img_desc)
{
    return CKRasterizerContext::CopyToMemoryBuffer(rect, buffer, img_desc);
}
int CKDX11RasterizerContext::CopyFromMemoryBuffer(CKRECT *rect, VXBUFFER_TYPE buffer, const VxImageDescEx &img_desc)
{
    return CKRasterizerContext::CopyFromMemoryBuffer(rect, buffer, img_desc);
}
CKBOOL CKDX11RasterizerContext::SetUserClipPlane(CKDWORD ClipPlaneIndex, const VxPlane &PlaneEquation)
{
    return CKRasterizerContext::SetUserClipPlane(ClipPlaneIndex, PlaneEquation);
}
CKBOOL CKDX11RasterizerContext::GetUserClipPlane(CKDWORD ClipPlaneIndex, VxPlane &PlaneEquation)
{
    return CKRasterizerContext::GetUserClipPlane(ClipPlaneIndex, PlaneEquation);
}

void *CKDX11RasterizerContext::LockVertexBuffer(CKDWORD VB, CKDWORD StartVertex, CKDWORD VertexCount,
                                                CKRST_LOCKFLAGS Lock)
{
    if (VB > m_VertexBuffers.Size())
        return nullptr;
    auto *desc = static_cast<CKDX11VertexBufferDesc *>(m_VertexBuffers[VB]);
    if (!desc)
        return nullptr;
    D3D11_MAP mapType = D3D11_MAP_WRITE_DISCARD;
    if (Lock == CKRST_LOCK_DISCARD)
        mapType = D3D11_MAP_WRITE_DISCARD;
    else if (Lock == CKRST_LOCK_NOOVERWRITE)
        mapType = D3D11_MAP_WRITE_NO_OVERWRITE;

    assert(StartVertex + VertexCount <= desc->m_MaxVertexCount);
    HRESULT hr;
    D3D11_MAPPED_SUBRESOURCE ms;
    D3DCall(m_DeviceContext->Map(desc->DxBuffer.Get(), NULL, mapType, NULL, &ms));
    if (SUCCEEDED(hr))
        // seems like d3d11 does not give us an option to map a portion of data...
        return (char*)(ms.pData) + StartVertex * desc->m_VertexSize;
    return nullptr;
}
CKBOOL CKDX11RasterizerContext::UnlockVertexBuffer(CKDWORD VB) {
    if (VB > m_IndexBuffers.Size())
        return FALSE;
    auto *desc = static_cast<CKDX11VertexBufferDesc *>(m_VertexBuffers[VB]);
    if (!desc)
        return FALSE;
    m_DeviceContext->Unmap(desc->DxBuffer.Get(), NULL);
    return TRUE;
}

void *CKDX11RasterizerContext::LockIndexBuffer(CKDWORD IB, CKDWORD StartIndex, CKDWORD IndexCount, CKRST_LOCKFLAGS Lock)
{
    if (IB > m_IndexBuffers.Size())
        return nullptr;
    auto *desc = static_cast<CKDX11IndexBufferDesc *>(m_IndexBuffers[IB]);
    if (!desc)
        return nullptr;
    D3D11_MAP mapType = D3D11_MAP_WRITE_DISCARD;
    if (Lock == CKRST_LOCK_DISCARD)
        mapType = D3D11_MAP_WRITE_DISCARD;
    else if (Lock == CKRST_LOCK_DEFAULT)
        mapType = D3D11_MAP_WRITE_NO_OVERWRITE;
    assert(StartIndex + IndexCount <= desc->m_MaxIndexCount);
    HRESULT hr;
    D3D11_MAPPED_SUBRESOURCE ms;
    D3DCall(m_DeviceContext->Map(desc->DxBuffer.Get(), NULL, mapType, NULL, &ms));
    if (SUCCEEDED(hr))
        // seems like d3d11 does not give us an option to map a portion of data...
        return (char*)ms.pData + StartIndex * sizeof(CKWORD);
    return nullptr;
}
CKBOOL CKDX11RasterizerContext::UnlockIndexBuffer(CKDWORD IB)
{
    if (IB > m_IndexBuffers.Size())
        return FALSE;
    auto *desc = static_cast<CKDX11IndexBufferDesc *>(m_IndexBuffers[IB]);
    if (!desc)
        return FALSE;
    m_DeviceContext->Unmap(desc->DxBuffer.Get(), NULL);
    return TRUE;
}
CKBOOL CKDX11RasterizerContext::CreateTexture(CKDWORD Texture, CKTextureDesc *DesiredFormat) { return 0; }

bool operator==(const CKVertexShaderDesc &a, const CKVertexShaderDesc& b)
{
    return a.m_FunctionSize == b.m_FunctionSize &&
        (a.m_Function == b.m_Function || 
            memcmp(a.m_Function, b.m_Function, a.m_FunctionSize) == 0);
}

CKBOOL CKDX11RasterizerContext::CreateVertexShader(CKDWORD VShader, CKVertexShaderDesc *DesiredFormat) { 
    if (VShader >= m_VertexShaders.Size() || !DesiredFormat)
        return FALSE;
    auto *desc = m_VertexShaders[VShader];
    CKDX11VertexShaderDesc *d11desc = nullptr;

    if (!desc || *DesiredFormat == *desc)
    {
        d11desc = dynamic_cast<CKDX11VertexShaderDesc *>(desc); // Check if object got from array is actually valid
        if (d11desc && d11desc->DxBlob) // A valid, while identical object already exists
            return TRUE;
    }
    delete desc;
    m_VertexShaders[VShader] = nullptr;
    d11desc = new CKDX11VertexShaderDesc;
    auto *d11fmt = dynamic_cast<CKDX11VertexShaderDesc *>(DesiredFormat);
    if (d11fmt)
        *d11desc = *d11fmt;
    else {
        d11desc->m_Function = DesiredFormat->m_Function;
        d11desc->m_FunctionSize = DesiredFormat->m_FunctionSize;
    }
    CKBOOL succeeded = d11desc->Create(this);
    if (succeeded)
        m_VertexShaders[VShader] = d11desc;
    return succeeded;
}

bool operator==(const CKPixelShaderDesc& a, const CKPixelShaderDesc& b)
{
    return a.m_FunctionSize == b.m_FunctionSize &&
        (a.m_Function == b.m_Function || memcmp(a.m_Function, b.m_Function, a.m_FunctionSize) == 0);
}

CKBOOL CKDX11RasterizerContext::CreatePixelShader(CKDWORD PShader, CKPixelShaderDesc *DesiredFormat) {
    if (PShader >= m_PixelShaders.Size() || !DesiredFormat)
        return FALSE;
    auto *desc = m_PixelShaders[PShader];
    CKDX11PixelShaderDesc *d11desc = nullptr;
    if (!desc || *DesiredFormat == *desc)
    {
        d11desc = dynamic_cast<CKDX11PixelShaderDesc *>(desc); // Check if object got from array is actually valid
        if (d11desc && d11desc->DxBlob) // A valid, while identical object already exists
            return TRUE;
    }
    delete desc;
    m_PixelShaders[PShader] = nullptr;
    d11desc = new CKDX11PixelShaderDesc;
    d11desc->m_Function = DesiredFormat->m_Function;
    d11desc->m_FunctionSize = DesiredFormat->m_FunctionSize;
    CKBOOL succeeded = d11desc->Create(this);
    if (succeeded)
        m_PixelShaders[PShader] = d11desc;
    return succeeded;
}

bool operator==(const CKVertexBufferDesc& a, const CKVertexBufferDesc& b){
    return a.m_CurrentVCount == b.m_CurrentVCount && a.m_Flags == b.m_Flags &&
        a.m_MaxVertexCount == b.m_MaxVertexCount && a.m_VertexFormat == b.m_VertexFormat &&
        a.m_VertexSize == b.m_VertexSize;
}

CKBOOL CKDX11RasterizerContext::CreateVertexBuffer(CKDWORD VB, CKVertexBufferDesc *DesiredFormat)
{
    if (VB >= m_VertexBuffers.Size() || !DesiredFormat)
        return FALSE;
    
    auto *vbo = m_VertexBuffers[VB];
    CKDX11VertexBufferDesc *dx11vb = nullptr;
    if (vbo && *DesiredFormat == *vbo)
    {
        dx11vb = dynamic_cast<CKDX11VertexBufferDesc *>(vbo);
        if (dx11vb && dx11vb->DxBuffer)
            return TRUE;
    }
    delete vbo;
    m_VertexBuffers[VB] = nullptr;
    
    dx11vb = new CKDX11VertexBufferDesc;
    dx11vb->m_CurrentVCount = DesiredFormat->m_CurrentVCount;
    dx11vb->m_Flags = DesiredFormat->m_Flags;
    dx11vb->m_MaxVertexCount = DesiredFormat->m_MaxVertexCount;
    dx11vb->m_VertexFormat = DesiredFormat->m_VertexFormat;
    dx11vb->m_VertexSize = FVF::ComputeVertexSize(DesiredFormat->m_VertexFormat);

    CKBOOL succeeded = dx11vb->Create(this);
    if (succeeded)
        m_VertexBuffers[VB] = dx11vb;
    return succeeded;
}

bool operator==(const CKIndexBufferDesc &a, const CKIndexBufferDesc &b)
{
    return a.m_Flags == b.m_Flags && a.m_CurrentICount == b.m_CurrentICount &&
        a.m_MaxIndexCount == b.m_MaxIndexCount;
}

CKBOOL CKDX11RasterizerContext::CreateIndexBuffer(CKDWORD IB, CKIndexBufferDesc *DesiredFormat) {
    if (IB >= m_IndexBuffers.Size() || !DesiredFormat)
        return FALSE;

    auto *vbDesc = m_IndexBuffers[IB];
    CKDX11IndexBufferDesc *dx11ib = nullptr;
    if (!vbDesc || *DesiredFormat == *vbDesc)
    {
        dx11ib = dynamic_cast<CKDX11IndexBufferDesc *>(vbDesc);
        if (dx11ib && dx11ib->DxBuffer)
            return TRUE;
    }
    delete vbDesc;
    m_IndexBuffers[IB] = nullptr;

    dx11ib = new CKDX11IndexBufferDesc;
    dx11ib->m_CurrentICount = DesiredFormat->m_CurrentICount;
    dx11ib->m_Flags = DesiredFormat->m_Flags;
    dx11ib->m_MaxIndexCount = DesiredFormat->m_MaxIndexCount;

    CKBOOL succeeded = dx11ib->Create(this);
    if (succeeded)
        m_IndexBuffers[IB] = dx11ib;
    return succeeded;
}

void CKDX11RasterizerContext::AssemblyInput(CKDX11VertexBufferDesc *vbo)
{
    HRESULT hr;
    
    // if (m_FVF == vbo->m_VertexFormat)
    //     return; // no need to re-set input layout
    if (!vbo)
        return;
    CKDWORD VShader;
#if defined(DEBUG) || defined(_DEBUG)
    try
    {
#endif
        VShader = m_VertexShaderMap.at(vbo->m_VertexFormat);
#if defined(DEBUG) || defined(_DEBUG)
    } catch (...)
    {
#if LOGGING
        fprintf(stderr, "FVF: 0x%x\n", vbo->m_VertexFormat);
#endif
        assert(false);
        return;
    }
#endif
    auto *vs = static_cast<CKDX11VertexShaderDesc *>(m_VertexShaders[VShader]);
#if LOGGING
    fprintf(stderr, "IA: Layout: ");
    for (auto item: vbo->DxInputElementDesc)
    {
        fprintf(stderr, "%s | ", item.SemanticName);
    }
    fprintf(stderr, ", Size: %d\n", vbo->m_VertexSize);
#endif
    // D3D11_INPUT_ELEMENT_DESC desc[] = {
    //     {"SV_POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
    //     {"COLOR", 0, DXGI_FORMAT_B8G8R8A8_UNORM, 0, ~0U, D3D11_INPUT_PER_VERTEX_DATA, 0},
    //     {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, ~0U, D3D11_INPUT_PER_VERTEX_DATA, 0},
    // };
    // m_DeviceContext->IASetInputLayout(m_InputLayout.Get());
    {
        Vx3DTransposeMatrix(m_CBuffer.TotalMatrix, m_TotalMatrix);
        D3D11_MAPPED_SUBRESOURCE ms;
        D3DCall(m_DeviceContext->Map(m_ConstantBuffer.DxBuffer.Get(), NULL, D3D11_MAP_WRITE_DISCARD, NULL, &ms));
        memcpy(ms.pData, &m_CBuffer, sizeof(ConstantBufferStruct));
        m_DeviceContext->Unmap(m_ConstantBuffer.DxBuffer.Get(), NULL);
        m_DeviceContext->VSSetConstantBuffers(0, 1, m_ConstantBuffer.DxBuffer.GetAddressOf());
        m_ConstantBufferUptodate = TRUE;
    }
#if LOGGING
    fprintf(stderr, "IA: vs %s\n", vs->DxEntryPoint);
#endif
    vs->Bind(this);
    m_FVF = vbo->m_VertexFormat;
}
