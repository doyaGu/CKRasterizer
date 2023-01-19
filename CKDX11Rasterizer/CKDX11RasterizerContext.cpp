#include "CKDX11Rasterizer.h"
#include "CKDX11RasterizerCommon.h"

static const char* shader = 
    "struct VOut\n"
    "{\n"
    "float4 position : SV_POSITION;\n"
    "float4 color : COLOR;\n"
    "};\n"

    "VOut VShader(float4 position : POSITION, float4 color : COLOR)\n"
    "{\n"
    "VOut output;\n"

    "output.position = position;\n"
    "output.color = color;\n"
    "return output;\n"
    "}\n"
    "float4 PShader(float4 position : SV_POSITION, float4 color : COLOR) : SV_TARGET\n"
    "{\n"
    "return color;\n"
    "}";

struct vertex
{
    float X, Y, Z;
    float Color[4];
};

vertex triangle[] = {
    {0.0f, 0.5f, 1.0f, {1.0f, 0.0f, 0.0f, 1.0f} },
    {0.45f, -0.5, 1.0f, {0.0f, 1.0f, 0.0f, 1.0f} },
    {-0.45f, -0.5f, 1.0f, {0.0f, 0.0f, 1.0f, 1.0f} }
};
ID3D11Buffer *vb;
ID3D11InputLayout *layout;

CKDX11RasterizerContext::CKDX11RasterizerContext() {}
CKDX11RasterizerContext::~CKDX11RasterizerContext() {}

static ID3DBlob *vsBlob = nullptr, *psBlob = nullptr;
static ID3D11VertexShader* vshader;
static ID3D11PixelShader *pshader;
CKBOOL CKDX11RasterizerContext::Create(WIN_HANDLE Window, int PosX, int PosY, int Width, int Height, int Bpp,
                                       CKBOOL Fullscreen, int RefreshRate, int Zbpp, int StencilBpp)
{
    HRESULT hr;

    m_InCreateDestroy = TRUE;
    SetWindowLongA((HWND)Window, GWL_STYLE, WS_OVERLAPPED | WS_SYSMENU);
    SetClassLongPtr((HWND)Window, GCLP_HBRBACKGROUND, (LONG)GetStockObject(NULL_BRUSH));

    DXGI_SWAP_CHAIN_DESC scd;
    ZeroMemory(&scd, sizeof(DXGI_SWAP_CHAIN_DESC));

    scd.BufferCount = 1;
    scd.BufferDesc.Width = Width;
    scd.BufferDesc.Height = Height;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; // just use 32-bit color here, too lazy to check if valid
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = (HWND)Window;
    scd.SampleDesc.Count = 1; // ignore multisample for now
    scd.Windowed = !Fullscreen;
    scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    UINT creationFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL featureLevels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                         D3D_FEATURE_LEVEL_10_0, D3D_FEATURE_LEVEL_9_3,  D3D_FEATURE_LEVEL_9_1};
#ifdef _DEBUG
    //creationFlags |= D3D11_CREATE_DEVICE_DEBUG;
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
    m_PosX = PosX;
    m_PosY = PosY;
    m_Fullscreen = Fullscreen;
    m_Bpp = Bpp;
    m_ZBpp = Zbpp;
    m_Width = Width;
    m_Height = Height;
    if (m_Fullscreen)
        m_Driver->m_Owner->m_FullscreenContext = this;

    D3DCall(D3DCompile(shader, strlen(shader), nullptr, nullptr, nullptr, "VShader", "vs_4_0", 0, 0, &vsBlob, nullptr));
    D3DCall(D3DCompile(shader, strlen(shader), nullptr, nullptr, nullptr, "PShader", "ps_4_0", 0, 0, &psBlob, nullptr));
    D3DCall(m_Device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vshader));
    D3DCall(m_Device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &pshader));
    m_DeviceContext->VSSetShader(vshader, nullptr, 0);
    m_DeviceContext->PSSetShader(pshader, nullptr, 0);

    D3D11_INPUT_ELEMENT_DESC desc[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    m_Device->CreateInputLayout(desc, 2, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &layout);
    m_DeviceContext->IASetInputLayout(layout);

    D3D11_BUFFER_DESC bd;
    ZeroMemory(&bd, sizeof(bd));

    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.ByteWidth = sizeof(vertex) * 3;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    D3DCall(m_Device->CreateBuffer(&bd, nullptr, &vb));
    D3D11_MAPPED_SUBRESOURCE ms;
    D3DCall(m_DeviceContext->Map(vb, NULL, D3D11_MAP_WRITE_DISCARD, NULL, &ms));
    memcpy(ms.pData, triangle, sizeof(triangle));
    m_DeviceContext->Unmap(vb, NULL);

    

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
    HRESULT hr;

    UINT stride = sizeof(vertex);
    UINT offset = 0;
    m_DeviceContext->IASetVertexBuffers(0, 1, &vb, &stride, &offset);

    m_DeviceContext->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    m_DeviceContext->Draw(3, 0);

    D3DCall(m_Swapchain->Present(vsync ? 1 : 0, 0));
    return SUCCEEDED(hr);
}

CKBOOL CKDX11RasterizerContext::BeginScene() { return CKRasterizerContext::BeginScene(); }
CKBOOL CKDX11RasterizerContext::EndScene() { return CKRasterizerContext::EndScene(); }
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
    return TRUE;
}

CKBOOL CKDX11RasterizerContext::SetTransformMatrix(VXMATRIX_TYPE Type, const VxMatrix &Mat)
{
    return CKRasterizerContext::SetTransformMatrix(Type, Mat);
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
    return CKRasterizerContext::SetVertexShader(VShaderIndex);
}
CKBOOL CKDX11RasterizerContext::SetPixelShader(CKDWORD PShaderIndex)
{
    return CKRasterizerContext::SetPixelShader(PShaderIndex);
}
CKBOOL CKDX11RasterizerContext::SetVertexShaderConstant(CKDWORD Register, const void *Data, CKDWORD CstCount)
{
    return CKRasterizerContext::SetVertexShaderConstant(Register, Data, CstCount);
}
CKBOOL CKDX11RasterizerContext::SetPixelShaderConstant(CKDWORD Register, const void *Data, CKDWORD CstCount)
{
    return CKRasterizerContext::SetPixelShaderConstant(Register, Data, CstCount);
}
CKBOOL CKDX11RasterizerContext::DrawPrimitive(VXPRIMITIVETYPE pType, CKWORD *indices, int indexcount,
                                              VxDrawPrimitiveData *data)
{
    return CKRasterizerContext::DrawPrimitive(pType, indices, indexcount, data);
}
CKBOOL CKDX11RasterizerContext::DrawPrimitiveVB(VXPRIMITIVETYPE pType, CKDWORD VertexBuffer, CKDWORD StartIndex,
                                                CKDWORD VertexCount, CKWORD *indices, int indexcount)
{
    return CKRasterizerContext::DrawPrimitiveVB(pType, VertexBuffer, StartIndex, VertexCount, indices, indexcount);
}
CKBOOL CKDX11RasterizerContext::DrawPrimitiveVBIB(VXPRIMITIVETYPE pType, CKDWORD VB, CKDWORD IB, CKDWORD MinVIndex,
                                                  CKDWORD VertexCount, CKDWORD StartIndex, int Indexcount)
{
    return CKRasterizerContext::DrawPrimitiveVBIB(pType, VB, IB, MinVIndex, VertexCount, StartIndex, Indexcount);
}
CKBOOL CKDX11RasterizerContext::CreateObject(CKDWORD ObjIndex, CKRST_OBJECTTYPE Type, void *DesiredFormat)
{
    return CKRasterizerContext::CreateObject(ObjIndex, Type, DesiredFormat);
}
void *CKDX11RasterizerContext::LockVertexBuffer(CKDWORD VB, CKDWORD StartVertex, CKDWORD VertexCount,
                                                CKRST_LOCKFLAGS Lock)
{
    return CKRasterizerContext::LockVertexBuffer(VB, StartVertex, VertexCount, Lock);
}
CKBOOL CKDX11RasterizerContext::UnlockVertexBuffer(CKDWORD VB) { return CKRasterizerContext::UnlockVertexBuffer(VB); }
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
void *CKDX11RasterizerContext::LockIndexBuffer(CKDWORD IB, CKDWORD StartIndex, CKDWORD IndexCount, CKRST_LOCKFLAGS Lock)
{
    return CKRasterizerContext::LockIndexBuffer(IB, StartIndex, IndexCount, Lock);
}
CKBOOL CKDX11RasterizerContext::UnlockIndexBuffer(CKDWORD IB) { return CKRasterizerContext::UnlockIndexBuffer(IB); }
CKBOOL CKDX11RasterizerContext::CreateTexture(CKDWORD Texture, CKTextureDesc *DesiredFormat) { return 0; }
CKBOOL CKDX11RasterizerContext::CreateVertexShader(CKDWORD VShader, CKVertexShaderDesc *DesiredFormat) { 
    if (VShader >= m_VertexShaders.Size() || !DesiredFormat)
        return FALSE;
    CKVertexShaderDesc *desc = m_VertexShaders[VShader];
    if (DesiredFormat == desc) {
        CKDX11VertexShaderDesc *d11desc = static_cast<CKDX11VertexShaderDesc *>(desc);
        // 
    }
    return TRUE;
}
CKBOOL CKDX11RasterizerContext::CreatePixelShader(CKDWORD PShader, CKPixelShaderDesc *DesiredFormat) { return 0; }
CKBOOL CKDX11RasterizerContext::CreateVertexBuffer(CKDWORD VB, CKVertexBufferDesc *DesiredFormat)
{
    if (VB >= m_VertexBuffers.Size() || !DesiredFormat)
        return FALSE;
    HRESULT hr;
    D3D11_USAGE usage = D3D11_USAGE_DEFAULT;
    if (DesiredFormat->m_Flags & CKRST_VB_DYNAMIC)
        usage = D3D11_USAGE_DYNAMIC;
    auto *vbDesc = m_VertexBuffers[VB];
    if (vbDesc)
        delete vbDesc;
    auto *desc = new CKDX11VertexBufferDesc;
    desc->DxDesc.Usage = usage;
    desc->DxDesc.ByteWidth = DesiredFormat->m_MaxVertexCount;
    desc->DxDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    desc->DxDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    D3DCall(m_Device->CreateBuffer(&desc->DxDesc, nullptr, desc->DxBuffer.GetAddressOf()));
    m_VertexBuffers[VB] = desc;
    return SUCCEEDED(hr);
}
CKBOOL CKDX11RasterizerContext::CreateIndexBuffer(CKDWORD IB, CKIndexBufferDesc *DesiredFormat) { return 0; }