#include "CKDX9Rasterizer.h"
#include "XUtil.h"

#if defined(_MSC_VER)
#include <intrin.h>
#endif

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>

#define LOGGING 0
#define STEP 0
#define LOG_LOADTEXTURE 0
#define LOG_CREATETEXTURE 0
#define LOG_DRAWPRIMITIVE 0
#define LOG_SETTEXURESTAGESTATE 0
#define LOG_FLUSHCACHES 0
#define LOG_BATCHSTATS 0

#define USE_D3DSTATEBLOCKS 0

#if STEP
#include <conio.h>
static bool step_mode = false;
#endif

#if LOG_BATCHSTATS
static int directbat = 0;
static int vbbat = 0;
static int vbibbat = 0;
#endif

namespace {

const size_t DX9_SIZE_MAX = static_cast<size_t>(-1);

struct DX9FrameStats
{
    unsigned int frameIndex;
    unsigned int drawSprites;
    unsigned int loadTextures;
};

DX9FrameStats g_dx9Frame = {0, 0, 0};
volatile LONG g_dx9LogInit = 0;
int g_dx9LogEnabled = 0;
FILE *g_dx9Log = NULL;
CRITICAL_SECTION g_dx9LogLock;
bool g_dx9LogLockInit = false;
unsigned int g_dx9TextureStageLogCount = 0;
unsigned int g_dx9RenderStateLogCount = 0;
unsigned int g_dx9StageBlendValidateLogCount = 0;

const char *DX9BufferTypeName(VXBUFFER_TYPE buffer)
{
    switch (buffer) {
        case VXBUFFER_BACKBUFFER: return "BACKBUFFER";
        case VXBUFFER_ZBUFFER: return "ZBUFFER";
        case VXBUFFER_STENCILBUFFER: return "STENCILBUFFER";
        default: return "UNKNOWN";
    }
}

void DX9EnsureLogLockInit()
{
    if (!g_dx9LogLockInit) {
        InitializeCriticalSection(&g_dx9LogLock);
        g_dx9LogLockInit = true;
    }
}

void DX9BuildLogPath(char *out, size_t outSize)
{
    char exePath[MAX_PATH];
    DWORD len = GetModuleFileNameA(NULL, exePath, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        snprintf(out, outSize, "Player.log.dx9");
        return;
    }

    for (int i = (int)len - 1; i >= 0; --i) {
        if (exePath[i] == '\\' || exePath[i] == '/') {
            exePath[i + 1] = '\0';
            break;
        }
    }
    snprintf(out, outSize, "%sPlayer.log.dx9", exePath);
}

int DX9LogEnvEnabled()
{
    char buf[16];
    DWORD n = GetEnvironmentVariableA("CKDX9_LOG", buf, sizeof(buf));
    if (n == 0) {
        n = GetEnvironmentVariableA("CKGL_LOG", buf, sizeof(buf));
    }
    if (n == 0 || n >= sizeof(buf)) {
        return 0;
    }
    if (n == 1 && buf[0] == '0') {
        return 0;
    }
    if (_stricmp(buf, "false") == 0 || _stricmp(buf, "no") == 0 || _stricmp(buf, "off") == 0) {
        return 0;
    }
    return 1;
}

void DX9LogInit()
{
    if (InterlockedCompareExchange(&g_dx9LogInit, 1, 0) != 0) {
        return;
    }

    DX9EnsureLogLockInit();
    if (!DX9LogEnvEnabled()) {
        return;
    }

    char path[MAX_PATH + 32];
    DX9BuildLogPath(path, sizeof(path));
    g_dx9Log = fopen(path, "w");
    if (!g_dx9Log) {
        return;
    }

    g_dx9LogEnabled = 1;
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(g_dx9Log,
            "=== CKDX9Rasterizer diagnostic log opened %04u-%02u-%02u %02u:%02u:%02u.%03u ===\n",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    fflush(g_dx9Log);
}

bool DX9ValidateShaderFunction(const CKDWORD *function, CKDWORD byteSize)
{
    if (!function || byteSize < sizeof(CKDWORD) * 2 ||
        byteSize % sizeof(CKDWORD) != 0)
    {
        return false;
    }

    const size_t tokenCount = static_cast<size_t>(byteSize) / sizeof(CKDWORD);
    return function[tokenCount - 1] == D3DSIO_END;
}

bool DX9CopyShaderFunction(const CKDWORD *source, CKDWORD byteSize,
                           CKDWORD **destination)
{
    if (!destination || !DX9ValidateShaderFunction(source, byteSize))
        return false;

    const size_t tokenCount = static_cast<size_t>(byteSize) / sizeof(CKDWORD);
    if (tokenCount > DX9_SIZE_MAX / sizeof(CKDWORD))
        return false;

    CKDWORD *copy = new CKDWORD[tokenCount];
    if (!copy)
        return false;

    memcpy(copy, source, static_cast<size_t>(byteSize));
    *destination = copy;
    return true;
}

bool DX9IsBlockCompressedFormat(D3DFORMAT format)
{
    return format == D3DFMT_DXT1 || format == D3DFMT_DXT2 ||
           format == D3DFMT_DXT3 || format == D3DFMT_DXT4 ||
           format == D3DFMT_DXT5;
}

bool DX9ValidateImageDesc(const VxImageDescEx &desc)
{
    if (!desc.Image || desc.Width <= 0 || desc.Height <= 0 ||
        desc.BytesPerLine <= 0)
    {
        return false;
    }

    const size_t width = static_cast<size_t>(desc.Width);
    const size_t height = static_cast<size_t>(desc.Height);
    if (width > static_cast<size_t>(INT_MAX) ||
        height > static_cast<size_t>(INT_MAX))
    {
        return false;
    }

    const D3DFORMAT format = VxPixelFormatToD3DFormat(VxImageDesc2PixelFormat(desc));
    size_t minimumPitch = 0;
    size_t rowCount = height;

    if (DX9IsBlockCompressedFormat(format))
    {
        const size_t blockBytes = format == D3DFMT_DXT1 ? 8u : 16u;
        if (width > DX9_SIZE_MAX - 3u ||
            height > DX9_SIZE_MAX - 3u)
        {
            return false;
        }
        const size_t blocksPerRow = (width + 3u) / 4u;
        if (blocksPerRow > DX9_SIZE_MAX / blockBytes)
            return false;
        minimumPitch = blocksPerRow * blockBytes;
        rowCount = (height + 3u) / 4u;
    }
    else
    {
        if (desc.BitsPerPixel <= 0 ||
            width > (DX9_SIZE_MAX - 7u) /
                        static_cast<size_t>(desc.BitsPerPixel))
        {
            return false;
        }
        minimumPitch = (width * static_cast<size_t>(desc.BitsPerPixel) + 7u) / 8u;
    }

    const size_t sourcePitch = static_cast<size_t>(desc.BytesPerLine);
    return sourcePitch >= minimumPitch &&
           rowCount <= DX9_SIZE_MAX / sourcePitch;
}

CKBOOL DX9FailSetTargetTexture(CKDX9RasterizerContext *context,
                               CKDWORD textureObject,
                               CKDX9TextureDesc *desc,
                               CKBOOL createdDesc,
                               IDirect3DBaseTexture9 *originalTexture,
                               int textureStageCount,
                               IDirect3DSurface9 *surface,
                               IDirect3DTexture9 *replacementTexture,
                               IDirect3DCubeTexture9 *replacementCubeTexture)
{
    if (context->m_DefaultBackBuffer)
        context->m_Device->SetRenderTarget(0, context->m_DefaultBackBuffer);
    if (context->m_DefaultDepthBuffer)
        context->m_Device->SetDepthStencilSurface(context->m_DefaultDepthBuffer);

    for (int stage = 0; stage < textureStageCount; ++stage)
    {
        if ((context->m_RenderTargetTextureStageMask & (1u << stage)) != 0)
            context->m_Device->SetTexture(stage, originalTexture);
    }
    context->m_RenderTargetTextureStageMask = 0;

    SAFERELEASE(surface);
    SAFERELEASE(replacementTexture);
    SAFERELEASE(replacementCubeTexture);
    SAFERELEASE(context->m_DefaultBackBuffer);
    SAFERELEASE(context->m_DefaultDepthBuffer);
    if (createdDesc)
    {
        delete desc;
        context->m_Textures[textureObject] = NULL;
    }
    return FALSE;
}

struct DX9SpriteState
{
    CKRenderStateData stateCache[VXRENDERSTATE_MAXSTATE];
    CKBOOL inverseWinding;
    CKDWORD vertexShaderCache;
    CKDWORD vertexFormatCache;
    LPDIRECT3DVERTEXBUFFER9 vertexBufferCache;
    CKDWORD vertexSizeCache;
    CKDWORD pixelShaderCache;
    LPDIRECT3DSTATEBLOCK9 stateBlock;
    IDirect3DBaseTexture9 *texture0;
    IDirect3DVertexBuffer9 *stream0;
    UINT streamOffset;
    UINT streamStride;
    HRESULT textureResult;
    HRESULT streamResult;
    CKDX9TextureStageStateCacheEntry
        textureStageStateCache[8][CKRST_TSS_MAXSTATE];
    CKDX9TextureStageStateCacheEntry
        d3dSamplerStateCache[8][D3DSAMP_DMAPOFFSET + 1];
    CKDX9TextureStageStateCacheEntry
        d3dTextureStageStateCache[8][D3DTSS_CONSTANT + 1];
    CKDWORD textureBindingCache[8];
    CKBOOL textureBindingCacheValid[8];
};

CKBOOL DX9RestoreSpriteState(CKDX9RasterizerContext *context,
                             DX9SpriteState &state)
{
    HRESULT restoreResult = state.stateBlock->Apply();
    SAFERELEASE(state.stateBlock);
    HRESULT textureResult = S_OK;
    HRESULT streamResult = S_OK;
    if (SUCCEEDED(state.textureResult))
        textureResult = context->m_Device->SetTexture(0, state.texture0);
    if (SUCCEEDED(state.streamResult))
    {
        streamResult = context->m_Device->SetStreamSource(
            0,
            state.stream0,
            state.stream0 ? state.streamOffset : 0,
            state.stream0 ? state.streamStride : 0);
    }
    SAFERELEASE(state.texture0);
    SAFERELEASE(state.stream0);
    memcpy(context->m_StateCache, state.stateCache, sizeof(state.stateCache));
    context->m_InverseWinding = state.inverseWinding;
    context->m_CurrentVertexShaderCache = state.vertexShaderCache;
    context->m_CurrentVertexFormatCache = state.vertexFormatCache;
    context->m_CurrentVertexBufferCache = state.vertexBufferCache;
    context->m_CurrentVertexSizeCache = state.vertexSizeCache;
    context->m_CurrentPixelShaderCache = state.pixelShaderCache;
    memcpy(context->m_TextureStageStateCache, state.textureStageStateCache,
           sizeof(state.textureStageStateCache));
    memcpy(context->m_D3DSamplerStateCache, state.d3dSamplerStateCache,
           sizeof(state.d3dSamplerStateCache));
    memcpy(context->m_D3DTextureStageStateCache, state.d3dTextureStageStateCache,
           sizeof(state.d3dTextureStageStateCache));
    memcpy(context->m_TextureBindingCache, state.textureBindingCache,
           sizeof(state.textureBindingCache));
    memcpy(context->m_TextureBindingCacheValid, state.textureBindingCacheValid,
           sizeof(state.textureBindingCacheValid));
    return SUCCEEDED(restoreResult) && SUCCEEDED(textureResult) &&
           SUCCEEDED(streamResult);
}

void DX9CaptureFrontBufferRect(const CKRECT &requestedRect,
                               const POINT &windowOrigin,
                               const D3DDISPLAYMODE &displayMode,
                               const D3DSURFACE_DESC &backBufferDesc,
                               const D3DLOCKED_RECT &frontLock,
                               const D3DLOCKED_RECT &backupLock,
                               const VxImageDescEx &sourceTemplate,
                               const VxImageDescEx &targetTemplate,
                               int targetBytesPerPixel)
{
    int left = requestedRect.left < 0 ? 0 : requestedRect.left;
    int top = requestedRect.top < 0 ? 0 : requestedRect.top;
    int right = requestedRect.right > static_cast<int>(backBufferDesc.Width)
                    ? static_cast<int>(backBufferDesc.Width)
                    : requestedRect.right;
    int bottom = requestedRect.bottom > static_cast<int>(backBufferDesc.Height)
                     ? static_cast<int>(backBufferDesc.Height)
                     : requestedRect.bottom;
    if (right <= left || bottom <= top)
        return;

    int sourceX = windowOrigin.x + left;
    int sourceY = windowOrigin.y + top;
    if (sourceX < 0)
    {
        left -= sourceX;
        sourceX = 0;
    }
    if (sourceY < 0)
    {
        top -= sourceY;
        sourceY = 0;
    }
    if (sourceX >= static_cast<int>(displayMode.Width) ||
        sourceY >= static_cast<int>(displayMode.Height))
    {
        return;
    }

    int width = right - left;
    int height = bottom - top;
    const int availableWidth = static_cast<int>(displayMode.Width) - sourceX;
    const int availableHeight = static_cast<int>(displayMode.Height) - sourceY;
    if (width > availableWidth)
        width = availableWidth;
    if (height > availableHeight)
        height = availableHeight;
    if (width <= 0 || height <= 0)
        return;

    VxImageDescEx source = sourceTemplate;
    VxImageDescEx target = targetTemplate;
    source.Width = width;
    source.Height = height;
    source.Image = static_cast<CKBYTE *>(frontLock.pBits) +
                   static_cast<size_t>(sourceY) * frontLock.Pitch +
                   static_cast<size_t>(sourceX) * 4;
    target.Width = width;
    target.Height = height;
    target.Image = static_cast<CKBYTE *>(backupLock.pBits) +
                   static_cast<size_t>(top) * backupLock.Pitch +
                   static_cast<size_t>(left) * targetBytesPerPixel;
    VxDoBlit(source, target);
}

bool DX9ReplaceBoundTexture(IDirect3DDevice9 *device, int stageCount,
                            IDirect3DBaseTexture9 *oldTexture,
                            IDirect3DBaseTexture9 *newTexture)
{
    if (!oldTexture)
        return true;
    if (!device || stageCount < 0 || stageCount > 32)
        return false;

    CKDWORD reboundStages = 0;
    for (int stage = 0; stage < stageCount; ++stage)
    {
        IDirect3DBaseTexture9 *boundTexture = NULL;
        const HRESULT getResult = device->GetTexture(stage, &boundTexture);
        if (FAILED(getResult))
        {
            for (int rollbackStage = 0; rollbackStage < stage; ++rollbackStage)
            {
                if (reboundStages & (1u << rollbackStage))
                    device->SetTexture(rollbackStage, oldTexture);
            }
            return false;
        }

        const bool replace = boundTexture == oldTexture;
        SAFERELEASE(boundTexture);
        if (!replace)
            continue;

        if (FAILED(device->SetTexture(stage, newTexture)))
        {
            for (int rollbackStage = 0; rollbackStage < stage; ++rollbackStage)
            {
                if (reboundStages & (1u << rollbackStage))
                    device->SetTexture(rollbackStage, oldTexture);
            }
            return false;
        }
        reboundStages |= 1u << stage;
    }
    return true;
}

HRESULT DX9DrawSurfaceToMultisampledBackBuffer(IDirect3DDevice9 *device,
                                                IDirect3DSurface9 *source,
                                                IDirect3DSurface9 *backBuffer,
                                                const D3DSURFACE_DESC &backBufferDesc,
                                                CKBOOL sceneBegun)
{
    if (!device || !source || !backBuffer ||
        backBufferDesc.MultiSampleType == D3DMULTISAMPLE_NONE)
    {
        return D3DERR_INVALIDCALL;
    }

    IDirect3DSurface9 *currentRenderTarget = NULL;
    HRESULT hr = device->GetRenderTarget(0, &currentRenderTarget);
    const bool targetsBackBuffer = SUCCEEDED(hr) && currentRenderTarget == backBuffer;
    SAFERELEASE(currentRenderTarget);
    if (!targetsBackBuffer)
        return D3DERR_INVALIDCALL;

    IDirect3DTexture9 *backgroundTexture = NULL;
    IDirect3DSurface9 *textureSurface = NULL;
    hr = device->CreateTexture(backBufferDesc.Width, backBufferDesc.Height, 1, 0,
                               backBufferDesc.Format, D3DPOOL_DEFAULT,
                               &backgroundTexture, NULL);
    if (SUCCEEDED(hr) && backgroundTexture)
        hr = backgroundTexture->GetSurfaceLevel(0, &textureSurface);
    if (SUCCEEDED(hr) && textureSurface)
        hr = device->UpdateSurface(source, NULL, textureSurface, NULL);
    SAFERELEASE(textureSurface);
    if (FAILED(hr))
    {
        SAFERELEASE(backgroundTexture);
        return hr;
    }

    IDirect3DStateBlock9 *savedState = NULL;
    IDirect3DBaseTexture9 *savedTexture0 = NULL;
    IDirect3DVertexBuffer9 *savedStream0 = NULL;
    UINT savedStreamOffset = 0;
    UINT savedStreamStride = 0;
    const HRESULT savedTextureResult = device->GetTexture(0, &savedTexture0);
    const HRESULT savedStreamResult = device->GetStreamSource(
        0, &savedStream0, &savedStreamOffset, &savedStreamStride);
    hr = device->CreateStateBlock(D3DSBT_ALL, &savedState);
    if (FAILED(hr) || !savedState)
    {
        SAFERELEASE(savedTexture0);
        SAFERELEASE(savedStream0);
        SAFERELEASE(backgroundTexture);
        return FAILED(hr) ? hr : E_FAIL;
    }

    bool beganScene = false;
    if (!sceneBegun)
    {
        hr = device->BeginScene();
        beganScene = SUCCEEDED(hr);
    }

    struct ScreenVertex
    {
        float x;
        float y;
        float z;
        float rhw;
        float u;
        float v;
    };
    const ScreenVertex vertices[4] = {
        {-0.5f, -0.5f, 0.0f, 1.0f, 0.0f, 0.0f},
        {static_cast<float>(backBufferDesc.Width) - 0.5f, -0.5f, 0.0f, 1.0f, 1.0f, 0.0f},
        {-0.5f, static_cast<float>(backBufferDesc.Height) - 0.5f, 0.0f, 1.0f, 0.0f, 1.0f},
        {static_cast<float>(backBufferDesc.Width) - 0.5f,
         static_cast<float>(backBufferDesc.Height) - 0.5f, 0.0f, 1.0f, 1.0f, 1.0f}
    };

    if (sceneBegun || beganScene)
    {
        const D3DVIEWPORT9 viewport = {
            0, 0, backBufferDesc.Width, backBufferDesc.Height, 0.0f, 1.0f};

        hr = device->SetViewport(&viewport);
        if (SUCCEEDED(hr)) hr = device->SetVertexShader(NULL);
        if (SUCCEEDED(hr)) hr = device->SetPixelShader(NULL);
        if (SUCCEEDED(hr)) hr = device->SetVertexDeclaration(NULL);
        if (SUCCEEDED(hr)) hr = device->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
        if (SUCCEEDED(hr)) hr = device->SetTexture(0, backgroundTexture);
        if (SUCCEEDED(hr)) hr = device->SetTexture(1, NULL);
        if (SUCCEEDED(hr)) hr = device->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        if (SUCCEEDED(hr)) hr = device->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        if (SUCCEEDED(hr)) hr = device->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        if (SUCCEEDED(hr)) hr = device->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        if (SUCCEEDED(hr)) hr = device->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        if (SUCCEEDED(hr)) hr = device->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        if (SUCCEEDED(hr)) hr = device->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        if (SUCCEEDED(hr)) hr = device->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        if (SUCCEEDED(hr)) hr = device->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        if (SUCCEEDED(hr)) hr = device->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        if (SUCCEEDED(hr)) hr = device->SetRenderState(D3DRS_ZENABLE, FALSE);
        if (SUCCEEDED(hr)) hr = device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        if (SUCCEEDED(hr)) hr = device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        if (SUCCEEDED(hr)) hr = device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        if (SUCCEEDED(hr)) hr = device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        if (SUCCEEDED(hr)) hr = device->SetRenderState(D3DRS_FOGENABLE, FALSE);
        if (SUCCEEDED(hr)) hr = device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        if (SUCCEEDED(hr)) hr = device->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
        if (SUCCEEDED(hr)) hr = device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
        if (SUCCEEDED(hr))
        {
            hr = device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, vertices,
                                         sizeof(ScreenVertex));
        }
    }

    if (beganScene)
    {
        const HRESULT endResult = device->EndScene();
        if (SUCCEEDED(hr) && FAILED(endResult))
            hr = endResult;
    }

    const HRESULT applyResult = savedState->Apply();
    if (SUCCEEDED(savedTextureResult))
        device->SetTexture(0, savedTexture0);
    if (SUCCEEDED(savedStreamResult))
    {
        device->SetStreamSource(0, savedStream0,
                                savedStream0 ? savedStreamOffset : 0,
                                savedStream0 ? savedStreamStride : 0);
    }
    SAFERELEASE(savedState);
    SAFERELEASE(savedTexture0);
    SAFERELEASE(savedStream0);
    SAFERELEASE(backgroundTexture);

    if (SUCCEEDED(hr) && FAILED(applyResult))
        hr = applyResult;
    return hr;
}

} // namespace

void CKDX9LogPrintf(const char *fmt, ...)
{
    DX9LogInit();
    if (!g_dx9LogEnabled || !g_dx9Log) {
        return;
    }

    DX9EnsureLogLockInit();
    EnterCriticalSection(&g_dx9LogLock);

    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(g_dx9Log, "[%02u:%02u:%02u.%03u][tid=%lu] ",
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
            (unsigned long)GetCurrentThreadId());

    va_list args;
    va_start(args, fmt);
    vfprintf(g_dx9Log, fmt, args);
    va_end(args);
    fputc('\n', g_dx9Log);
    fflush(g_dx9Log);

    LeaveCriticalSection(&g_dx9LogLock);
}

static inline CKDWORD GetMsb(CKDWORD num, CKDWORD max)
{
    if (num == 0)
        return 0;

#if defined(_MSC_VER)
    unsigned long index = 0;
    _BitScanReverse(&index, static_cast<unsigned long>(num));
    CKDWORD i = static_cast<CKDWORD>(index);
    return (i > max) ? max : i;
#else
    CKDWORD i = 0;
    while (num >>= 1)
        ++i;
    return (i > max) ? max : i;
#endif
}

CKBOOL CKDX9VertexShaderDesc::Create(CKDX9RasterizerContext *Ctx, CKVertexShaderDesc *Format)
{
    if (!Ctx || !Ctx->m_Device)
        return FALSE;

    if (!DX9ValidateShaderFunction(m_Function, m_FunctionSize))
        return FALSE;

    // Update ownership
    Owner = Ctx;

    // Re-create the shader if device is available
    SAFERELEASE(DxShader);
    HRESULT hr = Ctx->m_Device->CreateVertexShader(reinterpret_cast<const DWORD *>(m_Function), &DxShader);

    return SUCCEEDED(hr);
}

CKDX9VertexShaderDesc::~CKDX9VertexShaderDesc()
{
    SAFERELEASE(DxShader);

    if (m_Function)
    {
        delete[] m_Function;
        m_Function = NULL;
    }
}

CKBOOL CKDX9PixelShaderDesc::Create(CKDX9RasterizerContext *Ctx, CKPixelShaderDesc *Format)
{
    // Validate parameters
    if (!Ctx || !Ctx->m_Device)
        return FALSE;

    if (!DX9ValidateShaderFunction(m_Function, m_FunctionSize))
        return FALSE;

    // Update ownership
    Owner = Ctx;

    // Re-create the shader if device is available
    SAFERELEASE(DxShader);
    HRESULT hr = Ctx->m_Device->CreatePixelShader(reinterpret_cast<const DWORD *>(m_Function), &DxShader);

    return SUCCEEDED(hr);
}

CKDX9PixelShaderDesc::~CKDX9PixelShaderDesc()
{
    SAFERELEASE(DxShader);

    if (m_Function)
    {
        delete[] m_Function;
        m_Function = NULL;
    }
}

CKDX9RasterizerContext::CKDX9RasterizerContext(CKDX9RasterizerDriver *driver) :
    CKRasterizerContext(),
    m_Device(NULL),
    m_PresentParams(),
    m_DirectXData(),
    m_SoftwareVertexProcessing(FALSE),
    m_CurrentTextureIndex(0),
    m_RenderTargetTextureStageMask(0),
    m_DefaultBackBuffer(NULL),
    m_DefaultDepthBuffer(NULL),
    m_InCreateDestroy(TRUE),
    m_ScreenBackup(NULL),
    m_CurrentVertexShaderCache(0),
    m_CurrentVertexFormatCache(0),
    m_CurrentVertexBufferCache(NULL),
    m_CurrentVertexSizeCache(0),
    m_CurrentPixelShaderCache(0),
    m_CurrentIndexBufferCache(NULL),
    m_CurrentIndexBufferCacheValid(FALSE),
    m_MaterialCacheValid(FALSE),
    m_ViewportCacheValid(FALSE),
    m_LightCacheValidMask(0),
    m_LightEnableCacheValidMask(0),
    m_LightEnableCacheMask(0),
    m_UserClipPlaneValidMask(0),
    m_StateCacheHitMask(),
    m_StateCacheMissMask(),
    m_SpriteStateBlock(NULL),
    m_TextureStageStateCacheEnabled(FALSE),
    m_Owner(NULL)
{
    // Keep destruction safe even if construction is given an invalid driver.
    memset(m_IndexBuffer, 0, sizeof(m_IndexBuffer));
    memset(m_TranslatedRenderStates, 0, sizeof(m_TranslatedRenderStates));
    memset(m_TempZBuffers, 0, sizeof(m_TempZBuffers));
    memset(m_TextureMinFilterStateBlocks, 0, sizeof(m_TextureMinFilterStateBlocks));
    memset(m_TextureMagFilterStateBlocks, 0, sizeof(m_TextureMagFilterStateBlocks));
    memset(m_TextureMapBlendStateBlocks, 0, sizeof(m_TextureMapBlendStateBlocks));
    ResetTextureStageStateCache();
    memset(m_UserClipPlanes, 0, sizeof(m_UserClipPlanes));
    ResetDirtyRects();

    if (!driver)
    {
        return; // Early return in case driver is null
    }

    // Set driver and owner
    m_Driver = driver;
    m_Owner = static_cast<CKDX9Rasterizer *>(driver->m_Owner);

}

CKDX9RasterizerContext::~CKDX9RasterizerContext()
{
    // Set flag to prevent recursive calls during destruction
    m_InCreateDestroy = TRUE;

    // If we're the fullscreen context, clear the reference
    if (m_Owner && m_Owner->m_FullscreenContext == this)
        m_Owner->m_FullscreenContext = NULL;

    // Release anything bound to the device
    if (m_Device)
    {
        // End scene if it was active
        if (m_SceneBegined)
            EndScene();

        // Clear all texture stages
        for (int i = 0; i < 8; i++)
        {
            m_Device->SetTexture(i, NULL);
        }

        // Clear index buffer binding
        SetD3DIndices(NULL);

        // Clear vertex stream bindings
        for (UINT i = 0; i < 8; i++)
        {
            m_Device->SetStreamSource(i, NULL, 0, 0);
        }

        // Clear shader bindings
        SetVertexShader(NULL);
        SetPixelShader(NULL);

        // Clear render target binding
        if (m_DefaultBackBuffer)
        {
            m_Device->SetRenderTarget(0, m_DefaultBackBuffer);
            if (m_DefaultDepthBuffer)
                m_Device->SetDepthStencilSurface(m_DefaultDepthBuffer);
        }
    }

    // Free state-specific resources
    ReleaseStateBlocks();
    ReleaseIndexBuffers();

    // Release temporary and cached resources that do not depend on Create/Destroy guards.
    ReleaseTempZBuffers();
    SAFERELEASE(m_ScreenBackup);
    if (m_Device)
    {
        m_Device->SetVertexDeclaration(NULL);
    }
    for (XNHashTable<LPDIRECT3DVERTEXDECLARATION9, DWORD>::Iterator it = m_VertexDeclarations.Begin(); it != m_VertexDeclarations.End(); ++it)
    {
        LPDIRECT3DVERTEXDECLARATION9 decl = *it;
        SAFERELEASE(decl);
    }
    m_VertexDeclarations.Clear();
    m_CurrentVertexFormatCache = 0;

    // Release major objects
    FlushObjects(CKRST_OBJ_ALL);

    // Release device-specific resources
    SAFERELEASE(m_DefaultBackBuffer);
    SAFERELEASE(m_DefaultDepthBuffer);

    // Finally release the device itself
    SAFERELEASE(m_Device);

    m_InCreateDestroy = FALSE;
}

int DepthBitPerPixelFromFormat(D3DFORMAT Format, CKDWORD *StencilSize)
{
    int result;
    switch (Format)
    {
        case D3DFMT_D16_LOCKABLE:
        case D3DFMT_D16:
            result = 16;
            *StencilSize = 0;
            break;
        case D3DFMT_D15S1:
            *StencilSize = 1;
            result = 16;
            break;
        case D3DFMT_D24S8:
            result = 24;
            *StencilSize = 8;
            break;
        case D3DFMT_D24X8:
            result = 24;
            *StencilSize = 0;
            break;
        case D3DFMT_D32:
            *StencilSize = 0;
            result = 32;
            break;
        case D3DFMT_D24X4S4:
            *StencilSize = 4;
            result = 24;
            break;
        default:
            result = 32;
            *StencilSize = 0;
            break;
    }
    return result;
}

CKBOOL CKDX9RasterizerContext::Create(WIN_HANDLE Window, int PosX, int PosY, int Width, int Height, int Bpp,
                                      CKBOOL Fullscreen, int RefreshRate, int Zbpp, int StencilBpp)
{
#ifdef TRACY_ENABLE
    ZoneScopedN(__FUNCTION__);
#endif
    CKDX9_LOG("Context::Create begin this=%p window=%p pos=(%d,%d) size=%dx%d bpp=%d fullscreen=%d refresh=%d z=%d stencil=%d",
              this, Window, PosX, PosY, Width, Height, Bpp, Fullscreen, RefreshRate, Zbpp, StencilBpp);
#if (STEP) || (LOGGING)
    AllocConsole();
    freopen("CON", "w", stdout);
    freopen("CON", "w", stderr);
#endif
    m_InCreateDestroy = TRUE;

    // Store original window style for restoring later if needed
    LONG originalStyle = 0;
    CKRECT windowRect = {0, 0, 0, 0};

    // Get window position and handle parent relationship
    if (Window)
    {
        // Store original window style
        originalStyle = GetWindowLongA((HWND)Window, GWL_STYLE);

        // Get window position relative to parent
        VxGetWindowRect(Window, &windowRect);
        WIN_HANDLE parent = VxGetParent(Window);
        if (parent)
        {
            VxScreenToClient(parent, reinterpret_cast<CKPOINT *>(&windowRect));
            VxScreenToClient(parent, reinterpret_cast<CKPOINT *>(&windowRect.right));
        }

        // For fullscreen mode, remove child window style
        if (Fullscreen)
        {
            SetWindowLongA((HWND)Window, GWL_STYLE, originalStyle & ~WS_CHILDWINDOW);
        }
    }

    CKDX9RasterizerDriver *driver = static_cast<CKDX9RasterizerDriver *>(m_Driver);
    if (!driver || !driver->m_Inited)
    {
        CKDX9_LOG("Context::Create failed: driver=%p inited=%d", driver, driver ? driver->m_Inited : 0);
        m_InCreateDestroy = FALSE;
        return FALSE;
    }

    memset(&m_PresentParams, 0, sizeof(m_PresentParams));
    m_PresentParams.hDeviceWindow = (HWND)Window;
    m_PresentParams.BackBufferWidth = Width;
    m_PresentParams.BackBufferHeight = Height;
#ifdef ENABLE_TRIPLE_BUFFER
    m_PresentParams.BackBufferCount = 2; // Triple buffering
#else
    m_PresentParams.BackBufferCount = 1; // Double buffering
#endif
    m_PresentParams.Windowed = !Fullscreen;
#ifdef USE_D3D9EX
    m_PresentParams.SwapEffect = D3DSWAPEFFECT_FLIPEX;
#else
    m_PresentParams.SwapEffect = D3DSWAPEFFECT_DISCARD;
#endif
    m_PresentParams.EnableAutoDepthStencil = TRUE;
    m_PresentParams.FullScreen_RefreshRateInHz = Fullscreen ? RefreshRate : D3DPRESENT_RATE_DEFAULT;
    
    // Use immediate presentation by default
    m_PresentParams.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    // m_PresentParams.PresentationInterval = Fullscreen ? D3DPRESENT_INTERVAL_IMMEDIATE : D3DPRESENT_INTERVAL_DEFAULT;
    
    // Find appropriate formats for rendering
    m_PresentParams.BackBufferFormat = driver->FindNearestRenderTargetFormat(Bpp, !Fullscreen);
    if (m_PresentParams.BackBufferFormat == D3DFMT_UNKNOWN)
    {
        CKDX9_LOG("Context::Create failed: no backbuffer format bpp=%d windowed=%d", Bpp, !Fullscreen);
        RestoreWindowStyle((HWND)Window, originalStyle, Fullscreen);
        m_InCreateDestroy = FALSE;
        return FALSE;
    }

    m_PresentParams.AutoDepthStencilFormat = driver->FindNearestDepthFormat(m_PresentParams.BackBufferFormat, Zbpp, StencilBpp);
    if (m_PresentParams.AutoDepthStencilFormat == D3DFMT_UNKNOWN)
    {
        CKDX9_LOG("Context::Create failed: no depth format backFmt=0x%08X z=%d stencil=%d",
                  (unsigned)m_PresentParams.BackBufferFormat, Zbpp, StencilBpp);
        RestoreWindowStyle((HWND)Window, originalStyle, Fullscreen);
        m_InCreateDestroy = FALSE;
        return FALSE;
    }

    // Configure multisampling
    ConfigureMultisampling();

    D3DDISPLAYMODEEX displayMode = {
        sizeof(D3DDISPLAYMODEEX),
        (UINT)Width,
        (UINT)Height,
        (UINT)RefreshRate,
        m_PresentParams.BackBufferFormat,
        D3DSCANLINEORDERING_PROGRESSIVE
    };

    // Configure device behavior flags
    DWORD behaviorFlags = D3DCREATE_ENABLE_PRESENTSTATS;
    if (!driver->m_IsHTL || driver->m_D3DCaps.VertexShaderVersion < D3DVS_VERSION(1, 0) && m_EnsureVertexShader)
    {
        m_SoftwareVertexProcessing = TRUE;
        behaviorFlags |= D3DCREATE_SOFTWARE_VERTEXPROCESSING;
    }
    else
    {
        behaviorFlags |= D3DCREATE_HARDWARE_VERTEXPROCESSING;
        if (driver->m_D3DCaps.DevCaps & D3DDEVCAPS_PUREDEVICE)
            behaviorFlags |= D3DCREATE_PUREDEVICE;
        m_SoftwareVertexProcessing = FALSE;
    }

    HRESULT hr = S_OK;
    CKDX9_LOG("Context::Create params back=%ux%u fmt=0x%08X depthFmt=0x%08X windowed=%d swap=%u interval=%u msaa=%u/%lu behavior=0x%08lX",
              m_PresentParams.BackBufferWidth, m_PresentParams.BackBufferHeight,
              (unsigned)m_PresentParams.BackBufferFormat,
              (unsigned)m_PresentParams.AutoDepthStencilFormat,
              m_PresentParams.Windowed, (unsigned)m_PresentParams.SwapEffect,
              (unsigned)m_PresentParams.PresentationInterval,
              (unsigned)m_PresentParams.MultiSampleType,
              (unsigned long)m_PresentParams.MultiSampleQuality,
              (unsigned long)behaviorFlags);

    // Create the Direct3D device
#ifdef USE_D3D9EX
    hr = m_Owner->m_D3D9->CreateDeviceEx(
        driver->m_AdapterIndex, 
        D3DDEVTYPE_HAL, 
        (HWND)m_Owner->m_MainWindow,
        behaviorFlags, 
        &m_PresentParams, 
        Fullscreen ? &displayMode : NULL, 
        &m_Device);
    if (FAILED(hr) && m_PresentParams.MultiSampleType != D3DMULTISAMPLE_NONE)
    {
        m_PresentParams.MultiSampleType = D3DMULTISAMPLE_NONE;
        hr = m_Owner->m_D3D9->CreateDeviceEx(
            driver->m_AdapterIndex, 
            D3DDEVTYPE_HAL, 
            (HWND)m_Owner->m_MainWindow,
            behaviorFlags, 
            &m_PresentParams, 
            Fullscreen ? &displayMode : NULL,
            &m_Device);
    }
#else
    hr = m_Owner->m_D3D9->CreateDevice(
        driver->m_AdapterIndex, 
        D3DDEVTYPE_HAL, 
        (HWND)m_Owner->m_MainWindow,
        behaviorFlags, 
        &m_PresentParams, 
        &m_Device);
    if (FAILED(hr) && m_PresentParams.MultiSampleType != D3DMULTISAMPLE_NONE)
    {
        m_PresentParams.MultiSampleType = D3DMULTISAMPLE_NONE;
        hr = m_Owner->m_D3D9->CreateDevice(
            driver->m_AdapterIndex, 
            D3DDEVTYPE_HAL, 
            (HWND)m_Owner->m_MainWindow,
            behaviorFlags, 
            &m_PresentParams, 
            &m_Device);
    }
#endif
    CKDX9_LOG("Context::Create CreateDevice result hr=0x%08lX device=%p", (unsigned long)hr, m_Device);

    // Restore window style if device creation failed
    if (FAILED(hr))
    {
        RestoreWindowStyle((HWND)Window, originalStyle, Fullscreen);
        m_InCreateDestroy = FALSE;
        return FALSE;
    }

    // Position the window appropriately
    if (Window)
    {
        if (Fullscreen)
        {
            // For fullscreen, restore window as a child if it was a child window
            if (originalStyle & WS_CHILDWINDOW)
            {
                SetWindowLongA((HWND)Window, GWL_STYLE, originalStyle);
            }
        }
        else
        {
            // For windowed mode, position the window correctly
            VxMoveWindow(Window, windowRect.left, windowRect.top,
                         windowRect.right - windowRect.left,
                         windowRect.bottom - windowRect.top, FALSE);
        }
    }

    // Store context configuration
    m_Window = (HWND)Window;
    m_PosX = PosX;
    m_PosY = PosY;
    m_Fullscreen = Fullscreen;

    // Update internal dimensions and formats from created device
    if (!UpdateDeviceProperties())
    {
        DestroyDevice();
        m_InCreateDestroy = FALSE;
        return FALSE;
    }

    // Set initial rendering states
    if (!InitializeDeviceStates())
    {
        DestroyDevice();
        m_InCreateDestroy = FALSE;
        return FALSE;
    }

    // Update DirectX data for external access
    UpdateDirectXData();
    
    // Initialize caches and object arrays
    FlushCaches();
    UpdateObjectArrays(m_Owner);
    ClearStreamCache();

    // Register as fullscreen context if in fullscreen mode
    if (m_Fullscreen)
        m_Owner->m_FullscreenContext = this;

    m_InCreateDestroy = FALSE;
    CKDX9_LOG("Context::Create success this=%p device=%p size=%ux%u back=%p depth=%p",
              this, m_Device, m_Width, m_Height, m_DefaultBackBuffer, m_DefaultDepthBuffer);
    return TRUE;
}

CKBOOL CKDX9RasterizerContext::Resize(int PosX, int PosY, int Width, int Height, CKDWORD Flags)
{
#ifdef TRACY_ENABLE
    ZoneScopedN(__FUNCTION__);
#endif
    CKDX9_LOG("Context::Resize begin this=%p pos=(%d,%d) size=%dx%d flags=0x%08X current=%ux%u scene=%d",
              this, PosX, PosY, Width, Height, Flags, m_Width, m_Height, m_SceneBegined);
    if (m_InCreateDestroy || !m_Device)
    {
        CKDX9_LOG("Context::Resize rejected inCreateDestroy=%d device=%p", m_InCreateDestroy, m_Device);
        return FALSE;
    }

    // End any active scene
    if (m_SceneBegined)
        EndScene();

    ReleaseScreenBackup();

    // Update position if allowed
    if ((Flags & VX_RESIZE_NOMOVE) == 0)
    {
        m_PosX = PosX;
        m_PosY = PosY;
    }

    // Skip size changes if requested
    if ((Flags & VX_RESIZE_NOSIZE) != 0)
    {
        CKDX9_LOG("Context::Resize no-size success");
        return TRUE;
    }

    // Calculate dimensions if not specified
    RECT rect;
    if (Width == 0 || Height == 0)
    {
        if (!m_Window || !GetClientRect((HWND)m_Window, &rect))
            return FALSE;

        Width = rect.right; // Don't subtract m_PosX - client rect is already relative
        Height = rect.bottom;
    }

    // Validate minimum dimensions
    if (Width < 1 || Height < 1)
        return FALSE;

    // Store old dimensions for fallback
    UINT oldWidth = m_Width;
    UINT oldHeight = m_Height;

    // Update presentation parameters with new size
    m_PresentParams.BackBufferWidth = Width;
    m_PresentParams.BackBufferHeight = Height;

    // Clean up resources that will be invalidated by Reset
    PrepareDeviceReset();

    // Configure multisampling
    ConfigureMultisampling();

    // Try to reset the device
    HRESULT hr = ResetDevice();
    CKDX9_LOG("Context::Resize ResetDevice primary hr=0x%08lX requested=%dx%d", (unsigned long)hr, Width, Height);
    if (SUCCEEDED(hr))
    {
        m_Width = Width;
        m_Height = Height;
    }
    else
    {
        // Reset failed - try to recover with original dimensions
        m_PresentParams.BackBufferWidth = oldWidth;
        m_PresentParams.BackBufferHeight = oldHeight;

        // Try without multisampling as a fallback
        m_PresentParams.MultiSampleType = D3DMULTISAMPLE_NONE;
        hr = ResetDevice();
        CKDX9_LOG("Context::Resize ResetDevice fallback hr=0x%08lX old=%ux%u", (unsigned long)hr, oldWidth, oldHeight);

        if (FAILED(hr))
        {
            // Device is truly lost and can't be recovered yet
            // We'll need to wait for TestCooperativeLevel to return D3DERR_DEVICENOTRESET
            return FALSE;
        }
    }

    // Update DirectX data and state caches
    UpdateDirectXData();
    FlushCaches();

    CKDX9_LOG("Context::Resize end hr=0x%08lX size=%ux%u", (unsigned long)hr, m_Width, m_Height);
    return SUCCEEDED(hr);
}

CKBOOL CKDX9RasterizerContext::Clear(CKDWORD Flags, CKDWORD Ccol, float Z, CKDWORD Stencil, int RectCount, CKRECT *rects)
{
#ifdef TRACY_ENABLE
    ZoneScopedN(__FUNCTION__);
#endif
    if (m_InCreateDestroy || !m_Device)
        return FALSE;

    DWORD flags = 0;

    // In transparent mode, we skip clearing the color buffer
    if (!m_TransparentMode && (Flags & CKRST_CTXCLEAR_COLOR) && m_Bpp != 0)
        flags |= D3DCLEAR_TARGET;

    // Always handle depth and stencil clearing normally
    if ((Flags & CKRST_CTXCLEAR_STENCIL) && m_StencilBpp != 0)
        flags |= D3DCLEAR_STENCIL;

    if ((Flags & CKRST_CTXCLEAR_DEPTH) && m_ZBpp != 0)
        flags |= D3DCLEAR_ZBUFFER;

    if (flags == 0)
        return TRUE;

    // Validate rect count and pointers
    if (RectCount < 0)
        RectCount = 0;

    if (RectCount > 0 && !rects)
        RectCount = 0;

    // Perform the clear operation
    HRESULT hr = m_Device->Clear(RectCount, (D3DRECT *)rects, flags, Ccol, Z, Stencil);

    // If this was a full clear, reset the dirty rects
    if (!m_TransparentMode && RectCount == 0 && (Flags & CKRST_CTXCLEAR_COLOR))
    {
        ResetDirtyRects();
    }

    return SUCCEEDED(hr);
}

#if LOGGING && LOG_LOADTEXTURE
static int texture_used[100] = {0};
#endif
CKBOOL CKDX9RasterizerContext::BackToFront(CKBOOL vsync)
{
#ifdef TRACY_ENABLE
    ZoneScopedN(__FUNCTION__);
#endif
    if ((g_dx9Frame.frameIndex & 1023u) == 0)
    {
        CKDX9_LOG("BackToFront[frame=%u] vsync=%d drawS=%u loadTex=%u",
                  g_dx9Frame.frameIndex, vsync, g_dx9Frame.drawSprites,
                  g_dx9Frame.loadTextures);
    }
    ++g_dx9Frame.frameIndex;
    g_dx9Frame.drawSprites = 0;
    g_dx9Frame.loadTextures = 0;

    if (m_InCreateDestroy || !m_Device)
        return FALSE;

    if (m_SceneBegined)
        EndScene();

    HRESULT hr = S_OK;

    if (vsync && m_CurrentTextureIndex == 0)
    {
        #ifdef TRACY_ENABLE
            ZoneScopedN("VSync");
        #endif
        D3DRASTER_STATUS status = {};
        hr = m_Device->GetRasterStatus(0, &status);

        // With an immediate swap chain, emulate the per-call CK vsync flag by
        // waiting for the next vblank in both windowed and fullscreen modes.
        // Leave an already-active vblank first so repeated calls cannot return
        // immediately in the same interval. Bound polling to avoid a driver
        // that reports a frozen raster status hanging the render thread.
        const ULONGLONG waitStart = GetTickCount64();
        while (SUCCEEDED(hr) && status.InVBlank && GetTickCount64() - waitStart < 100)
        {
            Sleep(0);
            hr = m_Device->GetRasterStatus(0, &status);
        }
        while (SUCCEEDED(hr) && !status.InVBlank && GetTickCount64() - waitStart < 100)
        {
            Sleep(0);
            hr = m_Device->GetRasterStatus(0, &status);
        }
    }

    m_CurrentPresentInterval = vsync ? 1 : 0;

#ifdef USE_D3D9EX
    hr = m_Device->PresentEx(NULL, NULL, NULL, NULL, 0);
#else
    hr = m_Device->Present(NULL, NULL, NULL, NULL);
#endif
    if (FAILED(hr))
    {
        if (hr == D3DERR_DEVICELOST)
        {
            // Check if device can be reset
            hr = m_Device->TestCooperativeLevel();
            if (hr == D3DERR_DEVICENOTRESET)
            {
                // Device can be reset now
                PrepareDeviceReset();
                hr = ResetDevice();
                if (SUCCEEDED(hr))
                {
                    UpdateDirectXData();
                    FlushCaches();
                    return TRUE; // Successfully reset
                }
            }

            // Device is lost and can't be reset yet
            return FALSE;
        }
    }

#if LOGGING && LOG_LOADTEXTURE
    int count = 0;
    for (int i = 0; i < 100; ++i)
    {
        if (texture_used[i])
        {
            fprintf(stderr, "%d ", i);
            ++count;
        }
    }
    fprintf(stderr, "\n count %d\n", count);
    fprintf(stderr, "buffer swap\n");
    ZeroMemory(texture_used, 100 * sizeof(int));
#endif
#if LOGGING && LOG_BATCHSTATS
    fprintf(stderr, "batch stats: direct %d, vb %d, vbib %d\r", directbat, vbbat, vbibbat);
    directbat = 0;
    vbbat = 0;
    vbibbat = 0;
#endif
#if STEP
    int x = _getch();
    if (x == 'z')
        step_mode = true;
    else if (x == 'x')
        step_mode = false;
#endif

    return SUCCEEDED(hr);
}

CKBOOL CKDX9RasterizerContext::BeginScene()
{
#ifdef TRACY_ENABLE
    FrameMark;
#endif
    if (m_SceneBegined)
        return TRUE;

    if (!m_Device)
        return FALSE;

    // Begin the scene
    HRESULT hr = m_Device->BeginScene();
    if (SUCCEEDED(hr))
    {
        m_SceneBegined = TRUE;
        return TRUE;
    }
    else
    {
        return FALSE;
    }
}

CKBOOL CKDX9RasterizerContext::EndScene()
{
#ifdef TRACY_ENABLE
    ZoneScopedN(__FUNCTION__);
#endif
    if (!m_SceneBegined)
        return TRUE;

    if (!m_Device)
    {
        // Force consistency even without a valid device
        m_SceneBegined = FALSE;
        return FALSE;
    }

    // End the scene
    HRESULT hr = m_Device->EndScene();
    // Update scene state regardless of success/failure
    // This ensures state consistency even if EndScene fails
    m_SceneBegined = FALSE;
    return SUCCEEDED(hr);
}

CKBOOL CKDX9RasterizerContext::SetLight(CKDWORD Light, CKLightData *data)
{
#ifdef TRACY_ENABLE
    ZoneScopedN(__FUNCTION__);
#endif
    if (!m_Device || !data || Light >= RST_MAX_LIGHT)
        return FALSE;

    const CKDWORD lightMask = 1u << Light;
    if ((m_LightCacheValidMask & lightMask) != 0 &&
        memcmp(&m_CurrentLightData[Light], data, sizeof(*data)) == 0)
    {
        return TRUE;
    }

    D3DLIGHT9 light;
    light.Type = (D3DLIGHTTYPE)data->Type;
    light.Range = data->Range;
    light.Attenuation0 = data->Attenuation0;
    light.Attenuation1 = data->Attenuation1;
    light.Attenuation2 = data->Attenuation2;
    light.Ambient.a = data->Ambient.a;
    light.Ambient.r = data->Ambient.r;
    light.Ambient.g = data->Ambient.g;
    light.Ambient.b = data->Ambient.b;
    light.Diffuse.a = data->Diffuse.a;
    light.Diffuse.r = data->Diffuse.r;
    light.Diffuse.g = data->Diffuse.g;
    light.Diffuse.b = data->Diffuse.b;
    light.Position.x = data->Position.x;
    light.Position.y = data->Position.y;
    light.Position.z = data->Position.z;
    light.Direction.x = data->Direction.x;
    light.Direction.y = data->Direction.y;
    light.Direction.z = data->Direction.z;
    light.Falloff = data->Falloff;
    light.Specular.a = data->Specular.a;
    light.Specular.r = data->Specular.r;
    light.Specular.g = data->Specular.g;
    light.Specular.b = data->Specular.b;
    light.Theta = data->InnerSpotCone;
    light.Phi = data->OuterSpotCone;

    if (data->Type == VX_LIGHTPARA)
    {
        light.Type = D3DLIGHT_POINT;
    }
    else if (data->Type == VX_LIGHTSPOT)
    {
        if (light.Theta > PI)
            light.Theta = PI;
        if (light.Phi < light.Theta)
            light.Phi = light.Theta;
    }

    ConvertAttenuationModelFromDX5(light.Attenuation0, light.Attenuation1, light.Attenuation2, data->Range);
    if (FAILED(m_Device->SetLight(Light, &light)))
        return FALSE;

    m_CurrentLightData[Light] = *data;
    m_LightCacheValidMask |= lightMask;
    return TRUE;
}

CKBOOL CKDX9RasterizerContext::EnableLight(CKDWORD Light, CKBOOL Enable)
{
#ifdef TRACY_ENABLE
    ZoneScopedN(__FUNCTION__);
#endif
    if (!m_Device || Light >= RST_MAX_LIGHT)
        return FALSE;

    const CKDWORD lightMask = 1u << Light;
    const CKBOOL normalizedEnable = Enable != FALSE;
    if ((m_LightEnableCacheValidMask & lightMask) != 0 &&
        (((m_LightEnableCacheMask & lightMask) != 0) ==
         (normalizedEnable != FALSE)))
    {
        return TRUE;
    }

    const HRESULT hr = m_Device->LightEnable(Light, normalizedEnable);
    if (FAILED(hr))
    {
        m_LightEnableCacheValidMask &= ~lightMask;
        return FALSE;
    }

    if (normalizedEnable)
        m_LightEnableCacheMask |= lightMask;
    else
        m_LightEnableCacheMask &= ~lightMask;
    m_LightEnableCacheValidMask |= lightMask;
    return TRUE;
}

CKBOOL CKDX9RasterizerContext::SetMaterial(CKMaterialData *mat)
{
#ifdef TRACY_ENABLE
    ZoneScopedN(__FUNCTION__);
#endif
    if (!m_Device)
        return FALSE;

    if (!mat)
        return FALSE;

    if (m_MaterialCacheValid &&
        memcmp(&m_CurrentMaterialData, mat, sizeof(*mat)) == 0)
    {
        return TRUE;
    }

    D3DMATERIAL9 d3dMat;
    d3dMat.Diffuse.r = mat->Diffuse.r;
    d3dMat.Diffuse.g = mat->Diffuse.g;
    d3dMat.Diffuse.b = mat->Diffuse.b;
    d3dMat.Diffuse.a = mat->Diffuse.a;

    d3dMat.Ambient.r = mat->Ambient.r;
    d3dMat.Ambient.g = mat->Ambient.g;
    d3dMat.Ambient.b = mat->Ambient.b;
    d3dMat.Ambient.a = mat->Ambient.a;

    d3dMat.Specular.r = mat->Specular.r;
    d3dMat.Specular.g = mat->Specular.g;
    d3dMat.Specular.b = mat->Specular.b;
    d3dMat.Specular.a = mat->Specular.a;

    d3dMat.Emissive.r = mat->Emissive.r;
    d3dMat.Emissive.g = mat->Emissive.g;
    d3dMat.Emissive.b = mat->Emissive.b;
    d3dMat.Emissive.a = mat->Emissive.a;

    d3dMat.Power = mat->SpecularPower;

    if (FAILED(m_Device->SetMaterial(&d3dMat)))
        return FALSE;

    m_CurrentMaterialData = *mat;
    m_MaterialCacheValid = TRUE;
    return TRUE;
}

CKBOOL CKDX9RasterizerContext::SetViewport(CKViewportData *data)
{
#ifdef TRACY_ENABLE
    ZoneScopedN(__FUNCTION__);
#endif
    if (!m_Device || !data)
        return FALSE;

    if (data->ViewX < 0 || data->ViewY < 0 ||
        data->ViewWidth < 0 || data->ViewHeight < 0)
    {
        return FALSE;
    }

    CKViewportData appliedViewport = *data;

    // A zero dimension requests the full context viewport. Keep the software
    // cache in the same normalized form that is sent to Direct3D.
    if (appliedViewport.ViewWidth == 0 || appliedViewport.ViewHeight == 0)
    {
        appliedViewport.ViewWidth = static_cast<int>(m_Width);
        appliedViewport.ViewHeight = static_cast<int>(m_Height);
    }

    if (static_cast<CKDWORD>(appliedViewport.ViewX) > m_Width ||
        static_cast<CKDWORD>(appliedViewport.ViewY) > m_Height ||
        static_cast<CKDWORD>(appliedViewport.ViewWidth) > m_Width - static_cast<CKDWORD>(appliedViewport.ViewX) ||
        static_cast<CKDWORD>(appliedViewport.ViewHeight) > m_Height - static_cast<CKDWORD>(appliedViewport.ViewY))
    {
        return FALSE;
    }

    if (appliedViewport.ViewZMin > appliedViewport.ViewZMax)
    {
        const float temp = appliedViewport.ViewZMin;
        appliedViewport.ViewZMin = appliedViewport.ViewZMax;
        appliedViewport.ViewZMax = temp;
    }

    // These comparisons also reject NaN values.
    if (!(appliedViewport.ViewZMin >= 0.0f && appliedViewport.ViewZMin <= 1.0f &&
          appliedViewport.ViewZMax >= 0.0f && appliedViewport.ViewZMax <= 1.0f))
    {
        return FALSE;
    }

    if (m_ViewportCacheValid &&
        memcmp(&m_ViewportData, &appliedViewport,
               sizeof(appliedViewport)) == 0)
    {
        return TRUE;
    }

    D3DVIEWPORT9 d3dViewport;
    d3dViewport.X = static_cast<DWORD>(appliedViewport.ViewX);
    d3dViewport.Y = static_cast<DWORD>(appliedViewport.ViewY);
    d3dViewport.Width = static_cast<DWORD>(appliedViewport.ViewWidth);
    d3dViewport.Height = static_cast<DWORD>(appliedViewport.ViewHeight);
    d3dViewport.MinZ = appliedViewport.ViewZMin;
    d3dViewport.MaxZ = appliedViewport.ViewZMax;

    if (FAILED(m_Device->SetViewport(&d3dViewport)))
        return FALSE;

    m_ViewportData = appliedViewport;
    m_ViewportCacheValid = TRUE;
    return TRUE;
}

CKBOOL CKDX9RasterizerContext::SetTransformMatrix(VXMATRIX_TYPE Type, const VxMatrix &Mat)
{
    if (!m_Device)
        return FALSE;

    CKDWORD matrixMask = 0;
    D3DTRANSFORMSTATETYPE type = (D3DTRANSFORMSTATETYPE)Type;
    switch (Type)
    {
        case VXMATRIX_WORLD:
            matrixMask = WORLD_TRANSFORM;
            type = D3DTS_WORLD;
            break;

        case VXMATRIX_VIEW:
            matrixMask = VIEW_TRANSFORM;
            type = D3DTS_VIEW;
            break;

        case VXMATRIX_PROJECTION:
            matrixMask = PROJ_TRANSFORM;
            type = D3DTS_PROJECTION;
            break;

        case VXMATRIX_TEXTURE0:
            matrixMask = TEXTURE0_TRANSFORM;
            type = D3DTS_TEXTURE0;
            break;

        case VXMATRIX_TEXTURE1:
        case VXMATRIX_TEXTURE2:
        case VXMATRIX_TEXTURE3:
        case VXMATRIX_TEXTURE4:
        case VXMATRIX_TEXTURE5:
        case VXMATRIX_TEXTURE6:
        case VXMATRIX_TEXTURE7:
            matrixMask = TEXTURE0_TRANSFORM << (Type - VXMATRIX_TEXTURE0);
            type = (D3DTRANSFORMSTATETYPE)(D3DTS_TEXTURE0 + (Type - VXMATRIX_TEXTURE0));
            break;

        default:
            return FALSE;
    }

    const CKBOOL isIdentity = VxMatrix::Identity() == Mat;
    if (isIdentity && (m_UnityMatrixMask & matrixMask) != 0)
        return TRUE;

    HRESULT hr = m_Device->SetTransform(type, (const D3DMATRIX *)&Mat);
    if (FAILED(hr))
        return FALSE;

    // Commit the software-side state only after the device accepted it.
    switch (Type)
    {
        case VXMATRIX_WORLD:
            m_WorldMatrix = Mat;
            Vx3DMultiplyMatrix(m_ModelViewMatrix, m_ViewMatrix, m_WorldMatrix);
            m_MatrixUptodate &= ~WORLD_TRANSFORM;
            break;
        case VXMATRIX_VIEW:
            m_ViewMatrix = Mat;
            Vx3DMultiplyMatrix(m_ModelViewMatrix, m_ViewMatrix, m_WorldMatrix);
            m_MatrixUptodate = 0;
            break;
        case VXMATRIX_PROJECTION:
            m_ProjectionMatrix = Mat;
            m_MatrixUptodate = 0;
            break;
        default:
            break;
    }

    if (isIdentity)
        m_UnityMatrixMask |= matrixMask;
    else
        m_UnityMatrixMask &= ~matrixMask;

    return TRUE;
}

CKBOOL CKDX9RasterizerContext::SetRenderState(VXRENDERSTATETYPE State, CKDWORD Value)
{
    if (!m_Device)
    {
        CKDX9_LOG("SetRenderState rejected: null device state=%u value=%u", (unsigned)State, Value);
        return FALSE;
    }

    if ((int)State < 0 || State >= VXRENDERSTATE_MAXSTATE)
    {
        CKDX9_LOG("SetRenderState rejected: invalid state=%u value=%u max=%u",
                  (unsigned)State, Value, (unsigned)VXRENDERSTATE_MAXSTATE);
        return FALSE;
    }

    const bool criticalState = (State == VXRENDERSTATE_CULLMODE) ||
                               (State == VXRENDERSTATE_INVERSEWINDING);
    const bool logState = (g_dx9RenderStateLogCount < 256) &&
                          (criticalState || g_dx9Frame.frameIndex < 3);
    if (logState)
    {
        ++g_dx9RenderStateLogCount;
        CKDX9_LOG("SetRenderState request frame=%u state=%u value=%u valid=%d cached=%u hitMask=%d missMask=%d inverse=%d",
                  g_dx9Frame.frameIndex, (unsigned)State, Value,
                  m_StateCache[State].Valid, m_StateCache[State].Value,
                  m_StateCacheHitMask.IsSet(State), m_StateCacheMissMask.IsSet(State),
                  m_InverseWinding);
    }

    // Check if this state is locked or disabled
    if (m_StateCache[State].Flags != 0)
        return TRUE;

    // Cache hit check
    if (m_StateCache[State].Valid && m_StateCache[State].Value == Value)
    {
        ++m_RenderStateCacheHit;
        return TRUE;
    }

    // Cache miss. Do not make the entry valid until the operation succeeds.
    ++m_RenderStateCacheMiss;

    if (State == VXRENDERSTATE_CULLMODE)
    {
        static const D3DCULL VXCullModes[4] = {D3DCULL_NONE, D3DCULL_NONE, D3DCULL_CW, D3DCULL_CCW};
        static const D3DCULL VXCullModesInverted[4] = {D3DCULL_NONE, D3DCULL_NONE, D3DCULL_CCW, D3DCULL_CW};

        if (Value >= 4)
            Value = 0; // Default to VXCULL_NONE
        HRESULT hr = m_Device->SetRenderState(D3DRS_CULLMODE, m_InverseWinding ? VXCullModesInverted[Value] : VXCullModes[Value]);
        if (FAILED(hr) || logState)
        {
            CKDX9_LOG("SetRenderState cullmode state=%u value=%u inverse=%d hr=0x%08lX",
                      (unsigned)State, Value, m_InverseWinding, (unsigned long)hr);
        }
        if (FAILED(hr))
            return FALSE;

        InvalidateStageBlendCache();
        m_StateCache[State].Value = Value;
        m_StateCache[State].Valid = TRUE;
        return TRUE;
    }

    if (State == VXRENDERSTATE_INVERSEWINDING ||
        State == VXRENDERSTATE_TEXTURETARGET)
    {
        if (State == VXRENDERSTATE_INVERSEWINDING)
        {
            m_InverseWinding = Value != 0;
            InvalidateStateCache(VXRENDERSTATE_CULLMODE);
            if (logState)
                CKDX9_LOG("SetRenderState inverseWinding=%d invalidatedCull", m_InverseWinding);
        }
        m_StateCache[State].Value = Value;
        m_StateCache[State].Valid = TRUE;
        return TRUE;
    }

    // Check if this state is in the miss mask (states to ignore)
    if (m_StateCacheMissMask.IsSet(State))
    {
        if (logState)
            CKDX9_LOG("SetRenderState ignored by miss mask state=%u value=%u", (unsigned)State, Value);
        return FALSE;
    }

    // Check if this state is in the hit mask (states with special handling)
    if (m_StateCacheHitMask.IsSet(State))
    {
        m_StateCache[State].Value = Value;
        m_StateCache[State].Valid = TRUE;
        return TRUE;
    }

    // Set the actual state in D3D (using translated values if needed)
    // CKDWORD translatedValue = Value;
    // if (m_TranslatedRenderStates[State])
    //     translatedValue = m_TranslatedRenderStates[State][Value];

    HRESULT hr = m_Device->SetRenderState((D3DRENDERSTATETYPE)State, Value);
    if (FAILED(hr) || logState)
    {
        CKDX9_LOG("SetRenderState d3d state=%u value=%u hr=0x%08lX",
                  (unsigned)State, Value, (unsigned long)hr);
    }
    if (FAILED(hr))
        return FALSE;

    InvalidateStageBlendCache();
    m_StateCache[State].Value = Value;
    m_StateCache[State].Valid = TRUE;
    return TRUE;
}

CKBOOL CKDX9RasterizerContext::GetRenderState(VXRENDERSTATETYPE State, CKDWORD *Value)
{
    if (!Value || (int)State < 0 || State >= VXRENDERSTATE_MAXSTATE)
        return FALSE;

    // If the cache is valid, return the cached value
    if (m_StateCache[State].Valid)
    {
        *Value = m_StateCache[State].Value;
        return TRUE;
    }

    // Special handling for INVERSEWINDING which is handled internally
    if (State == VXRENDERSTATE_INVERSEWINDING)
    {
        *Value = m_InverseWinding;
        return TRUE;
    }

    // For states that are in the StateCacheHitMask but don't have special handling in SetRenderState,
    // we need to add them here if needed

    // Return default value for any other state
    *Value = m_StateCache[State].DefaultValue;
    return FALSE;
}

CKBOOL CKDX9RasterizerContext::SetTexture(CKDWORD Texture, int Stage)
{
#ifdef TRACY_ENABLE
    ZoneScopedN(__FUNCTION__);
#endif
#if LOGGING && LOG_SETTEXTURE
    fprintf(stderr, "settexture %d %d\n", Texture, Stage);
#endif
    if (!m_Driver || Stage < 0 || Stage >= m_Driver->m_3DCaps.MaxNumberTextureStage)
    {
        CKDX9_LOG("SetTexture rejected: stage=%u maxStages=%u texture=%u",
                  (unsigned)Stage, m_Driver ? m_Driver->m_3DCaps.MaxNumberTextureStage : 0, Texture);
        return FALSE;
    }

    if (!m_Device)
    {
        CKDX9_LOG("SetTexture rejected: null device stage=%u texture=%u", Stage, Texture);
        return FALSE;
    }

    HRESULT hr = S_OK;
    const bool logTexture = (g_dx9Frame.frameIndex < 3) ||
                            (Stage == 0 && (g_dx9Frame.frameIndex % 200 == 0));

    // Only texture index 0 has the interface-defined "no texture" meaning.
    // Invalid non-zero handles must not silently mutate the current binding.
    if (Texture != 0 &&
        (Texture >= m_Textures.Size() || m_Textures[Texture] == NULL ||
         !(m_Textures[Texture]->Flags & CKRST_TEXTURE_VALID)))
    {
        CKDX9_LOG("SetTexture rejected: invalid texture stage=%u texture=%u size=%d desc=%p flags=0x%08X",
                  Stage, Texture, m_Textures.Size(),
                  Texture < m_Textures.Size() ? m_Textures[Texture] : NULL,
                  (Texture < m_Textures.Size() && m_Textures[Texture]) ? m_Textures[Texture]->Flags : 0);
        return FALSE;
    }

    if (m_TextureStageStateCacheEnabled && Stage < 8 &&
        m_TextureBindingCacheValid[Stage] &&
        m_TextureBindingCache[Stage] == Texture)
    {
        return TRUE;
    }

    // Case 1: Disable texturing for this stage.
    if (Texture == 0)
    {
        DWORD colorOpBefore = 0;
        DWORD colorArg1Before = 0;
        DWORD colorArg2Before = 0;
        DWORD alphaOpBefore = 0;
        DWORD alphaArg1Before = 0;
        DWORD alphaArg2Before = 0;
        if (logTexture)
        {
            if (Stage == 0)
            {
                m_Device->GetTextureStageState(0, D3DTSS_COLOROP, &colorOpBefore);
                m_Device->GetTextureStageState(0, D3DTSS_COLORARG1, &colorArg1Before);
                m_Device->GetTextureStageState(0, D3DTSS_COLORARG2, &colorArg2Before);
                m_Device->GetTextureStageState(0, D3DTSS_ALPHAOP, &alphaOpBefore);
                m_Device->GetTextureStageState(0, D3DTSS_ALPHAARG1, &alphaArg1Before);
                m_Device->GetTextureStageState(0, D3DTSS_ALPHAARG2, &alphaArg2Before);
            }
            CKDX9_LOG("SetTexture disable frame=%u stage=%u texture=%u size=%d validPtr=%p flags=0x%08X",
                      g_dx9Frame.frameIndex, Stage, Texture, m_Textures.Size(),
                      Texture < m_Textures.Size() ? m_Textures[Texture] : NULL,
                      (Texture < m_Textures.Size() && m_Textures[Texture]) ? m_Textures[Texture]->Flags : 0);
            if (Stage == 0)
            {
                CKDX9_LOG("SetTexture disable stage0 before colorOp=%lu colorArg1=0x%08lX colorArg2=0x%08lX alphaOp=%lu alphaArg1=0x%08lX alphaArg2=0x%08lX",
                          (unsigned long)colorOpBefore, (unsigned long)colorArg1Before, (unsigned long)colorArg2Before,
                          (unsigned long)alphaOpBefore, (unsigned long)alphaArg1Before, (unsigned long)alphaArg2Before);
            }
        }
        // Binding and texture-stage configuration are independent parts of
        // the CKRasterizer contract.  Unbinding must not replace the caller's
        // combiner state, because a later SetTexture call is expected to use
        // the previously selected TEXTUREMAPBLEND/STAGEBLEND configuration.
        hr = m_Device->SetTexture(Stage, NULL);
        if (FAILED(hr) || logTexture)
        {
            CKDX9_LOG("SetTexture disable result stage=%u texture=%u hr=0x%08lX", Stage, Texture, (unsigned long)hr);
            if (logTexture && Stage == 0)
            {
                DWORD colorOpAfter = 0;
                DWORD colorArg1After = 0;
                DWORD colorArg2After = 0;
                DWORD alphaOpAfter = 0;
                DWORD alphaArg1After = 0;
                DWORD alphaArg2After = 0;
                m_Device->GetTextureStageState(0, D3DTSS_COLOROP, &colorOpAfter);
                m_Device->GetTextureStageState(0, D3DTSS_COLORARG1, &colorArg1After);
                m_Device->GetTextureStageState(0, D3DTSS_COLORARG2, &colorArg2After);
                m_Device->GetTextureStageState(0, D3DTSS_ALPHAOP, &alphaOpAfter);
                m_Device->GetTextureStageState(0, D3DTSS_ALPHAARG1, &alphaArg1After);
                m_Device->GetTextureStageState(0, D3DTSS_ALPHAARG2, &alphaArg2After);
                CKDX9_LOG("SetTexture disable stage0 after colorOp=%lu colorArg1=0x%08lX colorArg2=0x%08lX alphaOp=%lu alphaArg1=0x%08lX alphaArg2=0x%08lX",
                          (unsigned long)colorOpAfter, (unsigned long)colorArg1After, (unsigned long)colorArg2After,
                          (unsigned long)alphaOpAfter, (unsigned long)alphaArg1After, (unsigned long)alphaArg2After);
            }
        }
        if (SUCCEEDED(hr) && Stage < 8)
        {
            m_TextureBindingCache[Stage] = 0;
            m_TextureBindingCacheValid[Stage] = TRUE;
            InvalidateStageBlendCache();
        }
        return SUCCEEDED(hr);
    }

    // Case 2: Setting a valid texture
    CKDX9TextureDesc *desc = static_cast<CKDX9TextureDesc *>(m_Textures[Texture]);

    IDirect3DBaseTexture9 *dxTexture = NULL;
    if (desc->Flags & CKRST_TEXTURE_CUBEMAP)
        dxTexture = desc->DxCubeTexture;
    else if (desc->Flags & CKRST_TEXTURE_VOLUMEMAP)
        dxTexture = desc->DxVolumeTexture;
    else
        dxTexture = desc->DxTexture;

    // Validate the resource selected by the descriptor type. Checking only
    // DxTexture made every valid cube/volume texture impossible to bind.
    if (!dxTexture)
    {
        CKDX9_LOG("SetTexture rejected: desc has no bindable resource stage=%u texture=%u flags=0x%08X cube=%p volume=%p render=%p",
                  Stage, Texture, desc->Flags, desc->DxCubeTexture, desc->DxVolumeTexture, desc->DxRenderTexture);
        return FALSE;
    }

    if (logTexture)
    {
        CKDX9_LOG("SetTexture bind frame=%u stage=%u texture=%u desc=%p flags=0x%08X fmt=%ux%u bpp=%d dx=%p cube=%p volume=%p render=%p",
                  g_dx9Frame.frameIndex, Stage, Texture, desc, desc->Flags,
                  desc->Format.Width, desc->Format.Height, desc->Format.BitsPerPixel,
                  desc->DxTexture, desc->DxCubeTexture, desc->DxVolumeTexture, desc->DxRenderTexture);
    }

    hr = m_Device->SetTexture(Stage, dxTexture);

    if (FAILED(hr))
        return FALSE;

    if (Stage < 8)
    {
        m_TextureBindingCache[Stage] = Texture;
        m_TextureBindingCacheValid[Stage] = TRUE;
        InvalidateStageBlendCache();
    }

    if (logTexture)
        CKDX9_LOG("SetTexture success stage=%u texture=%u", Stage, Texture);
    return TRUE;
}

void CKDX9RasterizerContext::ResetTextureStageStateCache()
{
    memset(m_TextureStageStateCache, 0, sizeof(m_TextureStageStateCache));
    memset(m_D3DSamplerStateCache, 0, sizeof(m_D3DSamplerStateCache));
    memset(m_D3DTextureStageStateCache, 0,
           sizeof(m_D3DTextureStageStateCache));
    memset(m_TextureBindingCache, 0, sizeof(m_TextureBindingCache));
    memset(m_TextureBindingCacheValid, 0, sizeof(m_TextureBindingCacheValid));
    m_CurrentIndexBufferCache = NULL;
    m_CurrentIndexBufferCacheValid = FALSE;
    m_MaterialCacheValid = FALSE;
    m_ViewportCacheValid = FALSE;
    m_LightCacheValidMask = 0;
    m_LightEnableCacheValidMask = 0;
    m_LightEnableCacheMask = 0;
}

HRESULT CKDX9RasterizerContext::SetD3DSamplerState(
    int Stage, D3DSAMPLERSTATETYPE State, CKDWORD Value)
{
    if (!m_Device || Stage < 0 || Stage >= 8 ||
        State < D3DSAMP_ADDRESSU || State > D3DSAMP_DMAPOFFSET)
    {
        return D3DERR_INVALIDCALL;
    }

    CKDX9TextureStageStateCacheEntry &entry =
        m_D3DSamplerStateCache[Stage][State];
    if (m_TextureStageStateCacheEnabled && entry.Valid &&
        entry.Value == Value)
    {
        return S_OK;
    }

    const HRESULT hr = m_Device->SetSamplerState(Stage, State, Value);
    if (SUCCEEDED(hr) && m_TextureStageStateCacheEnabled)
    {
        entry.Value = Value;
        entry.Valid = TRUE;
    }
    else if (FAILED(hr))
    {
        entry.Valid = FALSE;
        CKDX9_LOG("SetD3DSamplerState failed stage=%u state=%u value=%u hr=0x%08lX",
                  Stage, (unsigned)State, Value, (unsigned long)hr);
    }
    return hr;
}

HRESULT CKDX9RasterizerContext::SetD3DTextureStageState(
    int Stage, D3DTEXTURESTAGESTATETYPE State, CKDWORD Value)
{
    if (!m_Device || Stage < 0 || Stage >= 8 ||
        State < D3DTSS_COLOROP || State > D3DTSS_CONSTANT)
    {
        return D3DERR_INVALIDCALL;
    }

    CKDX9TextureStageStateCacheEntry &entry =
        m_D3DTextureStageStateCache[Stage][State];
    if (m_TextureStageStateCacheEnabled && entry.Valid &&
        entry.Value == Value)
    {
        return S_OK;
    }

    const HRESULT hr = m_Device->SetTextureStageState(Stage, State, Value);
    if (SUCCEEDED(hr) && m_TextureStageStateCacheEnabled)
    {
        entry.Value = Value;
        entry.Valid = TRUE;
    }
    else if (FAILED(hr))
    {
        entry.Valid = FALSE;
        CKDX9_LOG("SetD3DTextureStageState failed stage=%u state=%u value=%u hr=0x%08lX",
                  Stage, (unsigned)State, Value, (unsigned long)hr);
    }
    return hr;
}

HRESULT CKDX9RasterizerContext::SetD3DIndices(
    LPDIRECT3DINDEXBUFFER9 Buffer)
{
    if (!m_Device)
        return D3DERR_INVALIDCALL;

    if (m_CurrentIndexBufferCacheValid &&
        m_CurrentIndexBufferCache == Buffer)
    {
        return S_OK;
    }

    const HRESULT hr = m_Device->SetIndices(Buffer);
    if (SUCCEEDED(hr))
    {
        m_CurrentIndexBufferCache = Buffer;
        m_CurrentIndexBufferCacheValid = TRUE;
    }
    else
    {
        m_CurrentIndexBufferCache = NULL;
        m_CurrentIndexBufferCacheValid = FALSE;
    }
    return hr;
}

void CKDX9RasterizerContext::InvalidateStageBlendCache()
{
    for (int stage = 0; stage < 8; ++stage)
        m_TextureStageStateCache[stage][CKRST_TSS_STAGEBLEND].Valid = FALSE;
}

void CKDX9RasterizerContext::RecordTextureStageState(
    int Stage, CKRST_TEXTURESTAGESTATETYPE Tss, CKDWORD Value)
{
    if (!m_TextureStageStateCacheEnabled || Stage < 0 || Stage >= 8 ||
        Tss >= CKRST_TSS_MAXSTATE)
    {
        return;
    }

    // A successful STAGEBLEND validation is reusable only while every texture
    // stage remains unchanged. Any other stage-state mutation invalidates the
    // cached validation across all stages.
    if (Tss != CKRST_TSS_STAGEBLEND)
        InvalidateStageBlendCache();

    if (Tss == CKRST_TSS_TEXTUREMAPBLEND || Tss == CKRST_TSS_STAGEBLEND)
    {
        for (int state = CKRST_TSS_OP; state <= CKRST_TSS_AARG2; ++state)
            m_TextureStageStateCache[Stage][state].Valid = FALSE;
        m_TextureStageStateCache[Stage][CKRST_TSS_TEXTUREMAPBLEND].Valid = FALSE;
        m_TextureStageStateCache[Stage][CKRST_TSS_STAGEBLEND].Valid = FALSE;
    }
    else if (Tss >= CKRST_TSS_OP && Tss <= CKRST_TSS_AARG2)
    {
        m_TextureStageStateCache[Stage][CKRST_TSS_TEXTUREMAPBLEND].Valid = FALSE;
        m_TextureStageStateCache[Stage][CKRST_TSS_STAGEBLEND].Valid = FALSE;
    }
    else if (Tss == CKRST_TSS_ADDRESS)
    {
        // ADDRESS writes U, V and W at once. Remember U/V as well so the
        // common ADDRESS + ADDRESSU + ADDRESSV sequence does not submit the
        // same sampler values three times for every draw.
        m_TextureStageStateCache[Stage][CKRST_TSS_ADDRESSU].Value = Value;
        m_TextureStageStateCache[Stage][CKRST_TSS_ADDRESSU].Valid = TRUE;
        m_TextureStageStateCache[Stage][CKRST_TSS_ADDRESSV].Value = Value;
        m_TextureStageStateCache[Stage][CKRST_TSS_ADDRESSV].Valid = TRUE;
    }
    else if (Tss == CKRST_TSS_ADDRESSU || Tss == CKRST_TSS_ADDRESSV ||
             Tss == CKRST_TSS_ADDRESW)
    {
        m_TextureStageStateCache[Stage][CKRST_TSS_ADDRESS].Valid = FALSE;
    }

    m_TextureStageStateCache[Stage][Tss].Value = Value;
    m_TextureStageStateCache[Stage][Tss].Valid = TRUE;
}

CKBOOL CKDX9RasterizerContext::SetTextureStageState(int Stage, CKRST_TEXTURESTAGESTATETYPE Tss, CKDWORD Value)
{
#ifdef TRACY_ENABLE
    ZoneScopedN(__FUNCTION__);
#endif
    if (!m_Device || !m_Driver || Stage < 0 || Stage >= 8 ||
        Stage >= m_Driver->m_3DCaps.MaxNumberTextureStage)
    {
        CKDX9_LOG("SetTextureStageState rejected: device=%p stage=%u maxStages=%u tss=%u value=%u",
                  m_Device, Stage, m_Driver ? m_Driver->m_3DCaps.MaxNumberTextureStage : 0, (unsigned)Tss, Value);
        return FALSE;
    }

    if (Tss >= CKRST_TSS_MAXSTATE)
    {
        CKDX9_LOG("SetTextureStageState rejected: invalid tss stage=%u tss=%u value=%u",
                  Stage, (unsigned)Tss, Value);
        return FALSE;
    }

    const bool suspiciousFilter = (Tss == CKRST_TSS_MAGFILTER || Tss == CKRST_TSS_MINFILTER) && Value > VXTEXTUREFILTER_ANISOTROPIC;
    const bool suspiciousBlend = Tss == CKRST_TSS_TEXTUREMAPBLEND && Value >= VXTEXTUREBLEND_MAX;
    const bool keyTss = Tss == CKRST_TSS_TEXTUREMAPBLEND || Tss == CKRST_TSS_STAGEBLEND ||
                        Tss == CKRST_TSS_MAGFILTER || Tss == CKRST_TSS_MINFILTER;
    const bool logTss = suspiciousFilter || suspiciousBlend ||
                        (keyTss && g_dx9TextureStageLogCount < 128);

    if (suspiciousFilter)
    {
        CKDX9_LOG("SetTextureStageState suspicious filter value stage=%u tss=%u value=%u maxValid=%u",
                  Stage, (unsigned)Tss, Value, (unsigned)VXTEXTUREFILTER_ANISOTROPIC);
    }
    if (suspiciousBlend)
    {
        CKDX9_LOG("SetTextureStageState suspicious blend value stage=%u value=%u maxExclusive=%u",
                  Stage, Value, (unsigned)VXTEXTUREBLEND_MAX);
    }
    if (logTss)
    {
        ++g_dx9TextureStageLogCount;
        CKDX9_LOG("SetTextureStageState request frame=%u stage=%u tss=%u value=%u",
                  g_dx9Frame.frameIndex, Stage, (unsigned)Tss, Value);
    }

    if (suspiciousFilter || suspiciousBlend)
        return FALSE;

    if (m_TextureStageStateCacheEnabled && Stage < 8 &&
        m_TextureStageStateCache[Stage][Tss].Valid &&
        m_TextureStageStateCache[Stage][Tss].Value == Value)
    {
        return TRUE;
    }

    HRESULT hr = S_OK;
    const CKDX9RasterizerDriver *dxDriver = static_cast<CKDX9RasterizerDriver *>(m_Driver);
    CKDWORD maxAnisotropy = dxDriver ? dxDriver->m_D3DCaps.MaxAnisotropy : 1;
    if (maxAnisotropy == 0)
        maxAnisotropy = 1;

    // Preserve the first device failure instead of overwriting it with the
    // result of a later state call.
#define setSamplerState(state, value) \
    do { if (SUCCEEDED(hr)) hr = SetD3DSamplerState(Stage, state, value); } while (0)
#define setTextureStageState(state, value) \
    do { if (SUCCEEDED(hr)) hr = SetD3DTextureStageState(Stage, state, value); } while (0)

    switch (Tss)
    {
        case CKRST_TSS_ADDRESS:
            setSamplerState(D3DSAMP_ADDRESSU, Value);
            setSamplerState(D3DSAMP_ADDRESSV, Value);
            setSamplerState(D3DSAMP_ADDRESSW, Value);
            break;
        case CKRST_TSS_ADDRESSU:
            setSamplerState(D3DSAMP_ADDRESSU, Value);
            break;
        case CKRST_TSS_ADDRESSV:
            setSamplerState(D3DSAMP_ADDRESSV, Value);
            break;
        case CKRST_TSS_BORDERCOLOR:
        case CKRST_TSS_MIPMAPLODBIAS:
        case CKRST_TSS_MAXMIPMLEVEL:
        case CKRST_TSS_MAXANISOTROPY:
            setSamplerState((D3DSAMPLERSTATETYPE)(Tss - (CKRST_TSS_BORDERCOLOR - D3DSAMP_BORDERCOLOR)), Value);
            break;
        case CKRST_TSS_MAGFILTER:
            {
                LPDIRECT3DSTATEBLOCK9 block = m_TextureMagFilterStateBlocks[Value][Stage];
                if (block)
                {
                    hr = block->Apply();
                    if (SUCCEEDED(hr))
                        RecordTextureStageState(Stage, Tss, Value);
                    else
                        memset(m_TextureStageStateCache[Stage], 0,
                               sizeof(m_TextureStageStateCache[Stage]));
                    return SUCCEEDED(hr);
                }

                switch (Value)
                {
                    case VXTEXTUREFILTER_NEAREST:
                    case VXTEXTUREFILTER_MIPNEAREST:
                        setSamplerState(D3DSAMP_MAGFILTER, D3DTEXF_POINT);
                        break;
                    case VXTEXTUREFILTER_LINEAR:
                    case VXTEXTUREFILTER_MIPLINEAR:
                    case VXTEXTUREFILTER_LINEARMIPNEAREST:
                    case VXTEXTUREFILTER_LINEARMIPLINEAR:
                        setSamplerState(D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
                        break;
                    case VXTEXTUREFILTER_ANISOTROPIC:
                        setSamplerState(D3DSAMP_MAXANISOTROPY, maxAnisotropy);
                        setSamplerState(D3DSAMP_MAGFILTER, D3DTEXF_ANISOTROPIC);
                        break;
                    default:
                        return FALSE;
                }
            }
            break;
        case CKRST_TSS_MINFILTER:
            {
                LPDIRECT3DSTATEBLOCK9 block = m_TextureMinFilterStateBlocks[Value][Stage];
                if (block)
                {
                    hr = block->Apply();
                    if (SUCCEEDED(hr))
                        RecordTextureStageState(Stage, Tss, Value);
                    else
                        memset(m_TextureStageStateCache[Stage], 0,
                               sizeof(m_TextureStageStateCache[Stage]));
                    return SUCCEEDED(hr);
                }

                switch (Value)
                {
                    case VXTEXTUREFILTER_NEAREST:
                        setSamplerState(D3DSAMP_MINFILTER, D3DTEXF_POINT);
                        setSamplerState(D3DSAMP_MIPFILTER, D3DTEXF_NONE);
                        break;
                    case VXTEXTUREFILTER_LINEAR:
                        setSamplerState(D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
                        setSamplerState(D3DSAMP_MIPFILTER, D3DTEXF_NONE);
                        break;
                    case VXTEXTUREFILTER_MIPNEAREST:
                        setSamplerState(D3DSAMP_MINFILTER, D3DTEXF_POINT);
                        setSamplerState(D3DSAMP_MIPFILTER, D3DTEXF_POINT);
                        break;
                    case VXTEXTUREFILTER_MIPLINEAR:
                        setSamplerState(D3DSAMP_MINFILTER, D3DTEXF_POINT);
                        setSamplerState(D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
                        break;
                    case VXTEXTUREFILTER_LINEARMIPNEAREST:
                        setSamplerState(D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
                        setSamplerState(D3DSAMP_MIPFILTER, D3DTEXF_POINT);
                        break;
                    case VXTEXTUREFILTER_LINEARMIPLINEAR:
                        setSamplerState(D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
                        setSamplerState(D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
                        break;
                    case VXTEXTUREFILTER_ANISOTROPIC:
                        setSamplerState(D3DSAMP_MAXANISOTROPY, maxAnisotropy);
                        setSamplerState(D3DSAMP_MINFILTER, D3DTEXF_ANISOTROPIC);
                        setSamplerState(D3DSAMP_MIPFILTER, D3DTEXF_LINEAR);
                        break;
                    default:
                        return FALSE;
                }
            }
            break;
        case CKRST_TSS_TEXTUREMAPBLEND:
            {
                LPDIRECT3DSTATEBLOCK9 block = m_TextureMapBlendStateBlocks[Value][Stage];
                if (block)
                {
                    hr = block->Apply();
#if LOGGING && LOG_SETTEXURESTAGESTATE
                    fprintf(stderr, "Applying TextureMapBlendStateBlocks Value %d Stage %d -> 0x%x\n", Value, Stage, hr);
#endif
                    if (SUCCEEDED(hr))
                        RecordTextureStageState(Stage, Tss, Value);
                    else
                        memset(m_TextureStageStateCache[Stage], 0,
                               sizeof(m_TextureStageStateCache[Stage]));
                    return SUCCEEDED(hr);
                }
            }

            switch (Value)
            {
                case 0:
                    setTextureStageState(D3DTSS_COLOROP, D3DTOP_DISABLE);
                    setTextureStageState(D3DTSS_ALPHAOP, D3DTOP_DISABLE);
                    break;
                case VXTEXTUREBLEND_DECAL:
                case VXTEXTUREBLEND_COPY:
                    setTextureStageState(D3DTSS_COLOROP, D3DTOP_SELECTARG1);
                    setTextureStageState(D3DTSS_COLORARG1, D3DTA_TEXTURE);
                    setTextureStageState(D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
                    setTextureStageState(D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
                    break;
                case VXTEXTUREBLEND_MODULATE:
                case VXTEXTUREBLEND_MODULATEALPHA:
                case VXTEXTUREBLEND_MODULATEMASK:
                    setTextureStageState(D3DTSS_COLOROP, D3DTOP_MODULATE);
                    setTextureStageState(D3DTSS_COLORARG1, D3DTA_TEXTURE);
                    setTextureStageState(D3DTSS_COLORARG2, D3DTA_CURRENT);
                    setTextureStageState(D3DTSS_ALPHAOP, D3DTOP_MODULATE);
                    setTextureStageState(D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
                    setTextureStageState(D3DTSS_ALPHAARG2, D3DTA_CURRENT);
                    break;
                case VXTEXTUREBLEND_DECALALPHA:
                case VXTEXTUREBLEND_DECALMASK:
                    setTextureStageState(D3DTSS_COLOROP, D3DTOP_BLENDTEXTUREALPHA);
                    setTextureStageState(D3DTSS_COLORARG1, D3DTA_TEXTURE);
                    setTextureStageState(D3DTSS_COLORARG2, D3DTA_CURRENT);
                    setTextureStageState(D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
                    setTextureStageState(D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
                    break;
                case VXTEXTUREBLEND_ADD:
                    setTextureStageState(D3DTSS_COLOROP, D3DTOP_ADD);
                    setTextureStageState(D3DTSS_COLORARG1, D3DTA_TEXTURE);
                    setTextureStageState(D3DTSS_COLORARG2, D3DTA_CURRENT);
                    setTextureStageState(D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
                    setTextureStageState(D3DTSS_ALPHAARG1, D3DTA_CURRENT);
                    break;
                case VXTEXTUREBLEND_DOTPRODUCT3:
                    setTextureStageState(D3DTSS_COLOROP, D3DTOP_DOTPRODUCT3);
                    setTextureStageState(D3DTSS_COLORARG1, D3DTA_TEXTURE);
                    setTextureStageState(D3DTSS_COLORARG2, D3DTA_TFACTOR);
                    setTextureStageState(D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
                    setTextureStageState(D3DTSS_ALPHAARG1, D3DTA_CURRENT);
                    break;
                default:
                    return FALSE;
            }
            break;
        case CKRST_TSS_STAGEBLEND:
            if (Value <= 0xFF)
            {
                if (Value == 0)
                {
                    setTextureStageState(D3DTSS_COLOROP, D3DTOP_DISABLE);
                    setTextureStageState(D3DTSS_ALPHAOP, D3DTOP_DISABLE);
                    if (SUCCEEDED(hr))
                        RecordTextureStageState(Stage, Tss, Value);
                    else
                        memset(m_TextureStageStateCache[Stage], 0,
                               sizeof(m_TextureStageStateCache[Stage]));
                    return SUCCEEDED(hr);
                }

                CKStageBlend *blend = m_Owner->m_BlendStages[Value];
                if (!blend)
                {
                    CKDX9_LOG("SetTextureStageState stageblend missing blend stage=%u value=%u", Stage, Value);
                    return FALSE;
                }

                setTextureStageState(D3DTSS_COLOROP, blend->Cop);
                setTextureStageState(D3DTSS_COLORARG1, blend->Carg1);
                setTextureStageState(D3DTSS_COLORARG2, blend->Carg2);
                setTextureStageState(D3DTSS_ALPHAOP, blend->Aop);
                setTextureStageState(D3DTSS_ALPHAARG1, blend->Aarg1);
                setTextureStageState(D3DTSS_ALPHAARG2, blend->Aarg2);
                if (FAILED(hr))
                {
                    memset(m_TextureStageStateCache[Stage], 0,
                           sizeof(m_TextureStageStateCache[Stage]));
                    return FALSE;
                }

                const CKDWORD stageBeforeValidate = Stage;
                DWORD passes = 0;
                HRESULT validateHr = m_Device->ValidateDevice(&passes);
                if (FAILED(validateHr) || g_dx9StageBlendValidateLogCount < 64)
                {
                    ++g_dx9StageBlendValidateLogCount;
                    CKDX9_LOG("SetTextureStageState stageblend ValidateDevice stage=%u passes=%lu value=%u hr=0x%08lX",
                              stageBeforeValidate, (unsigned long)passes, Value, (unsigned long)validateHr);
                }
                if (FAILED(validateHr))
                {
                    SetD3DTextureStageState(Stage, D3DTSS_COLOROP, D3DTOP_DISABLE);
                    SetD3DTextureStageState(Stage, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
                    memset(m_TextureStageStateCache[Stage], 0,
                           sizeof(m_TextureStageStateCache[Stage]));
                    return FALSE;
                }
            }
            else
            {
                return FALSE;
            }
            break;
        default:
            hr = SetD3DTextureStageState(
                Stage, (D3DTEXTURESTAGESTATETYPE)Tss, Value);
            if (FAILED(hr))
            {
                CKDX9_LOG("SetTextureStageState default failed stage=%u tss=%u value=%u hr=0x%08lX",
                          Stage, (unsigned)Tss, Value, (unsigned long)hr);
            }
            if (SUCCEEDED(hr))
                RecordTextureStageState(Stage, Tss, Value);
            else
                memset(m_TextureStageStateCache[Stage], 0,
                       sizeof(m_TextureStageStateCache[Stage]));
            return SUCCEEDED(hr);
    }

#undef setTextureStageState
#undef setSamplerState
    if (SUCCEEDED(hr))
        RecordTextureStageState(Stage, Tss, Value);
    else
        memset(m_TextureStageStateCache[Stage], 0,
               sizeof(m_TextureStageStateCache[Stage]));
    return SUCCEEDED(hr);
}

CKBOOL CKDX9RasterizerContext::SetVertexShader(CKDWORD VShaderIndex)
{
#ifdef TRACY_ENABLE
    ZoneScopedN(__FUNCTION__);
#endif
    if (!m_Device)
        return FALSE;

    // Handle the case of setting null shader (disabling programmable pipeline)
    if (VShaderIndex == 0)
    {
        if (m_CurrentVertexShaderCache == 0)
            return TRUE;

        HRESULT hr = m_Device->SetVertexShader(NULL);
        if (FAILED(hr))
            return FALSE;
        InvalidateStageBlendCache();
        m_CurrentVertexShaderCache = 0;
        m_CurrentVertexFormatCache = 0;
        return TRUE;
    }

    if (VShaderIndex >= m_VertexShaders.Size())
        return FALSE;

    // Get and validate shader descriptor
    CKDX9VertexShaderDesc *desc = static_cast<CKDX9VertexShaderDesc *>(m_VertexShaders[VShaderIndex]);
    if (!desc || !desc->DxShader)
        return FALSE;

    // Cache current shader for optimizing redundant calls
    if (m_CurrentVertexShaderCache == VShaderIndex)
        return TRUE; // Already set

    HRESULT hr = m_Device->SetVertexShader(desc->DxShader);
    if (FAILED(hr))
        return FALSE;

    InvalidateStageBlendCache();
    m_CurrentVertexShaderCache = VShaderIndex;
    m_CurrentVertexFormatCache = 0;
    return TRUE;
}

CKBOOL CKDX9RasterizerContext::SetPixelShader(CKDWORD PShaderIndex)
{
#ifdef TRACY_ENABLE
    ZoneScopedN(__FUNCTION__);
#endif
    if (!m_Device)
        return FALSE;

    // Handle the case of setting null shader (disabling programmable pipeline)
    if (PShaderIndex == 0)
    {
        if (m_CurrentPixelShaderCache == 0)
            return TRUE; // Already disabled

        HRESULT hr = m_Device->SetPixelShader(NULL);
        if (FAILED(hr))
            return FALSE;
        InvalidateStageBlendCache();
        m_CurrentPixelShaderCache = 0;
        return TRUE;
    }

    if (PShaderIndex >= m_PixelShaders.Size())
        return FALSE;

    // Get and validate shader
    CKDX9PixelShaderDesc *desc = static_cast<CKDX9PixelShaderDesc *>(m_PixelShaders[PShaderIndex]);
    if (!desc || !desc->DxShader)
        return FALSE;

    // Cache current shader for optimizing redundant calls
    if (m_CurrentPixelShaderCache == PShaderIndex)
        return TRUE; // Already set

    HRESULT hr = m_Device->SetPixelShader(desc->DxShader);
    if (FAILED(hr))
        return FALSE;

    InvalidateStageBlendCache();
    m_CurrentPixelShaderCache = PShaderIndex;
    return TRUE;
}

CKBOOL CKDX9RasterizerContext::SetVertexShaderConstant(CKDWORD Register, const void *Data, CKDWORD CstCount)
{
    const CKDWORD registerLimit = 256;
    if (!m_Device || !Data || CstCount == 0 || Register >= registerLimit)
        return FALSE;

    // Use subtraction so malformed unsigned inputs cannot wrap the range check.
    if (CstCount > registerLimit - Register)
        return FALSE;

    // Safe casting - ensure alignment requirement for float data
    if ((reinterpret_cast<uintptr_t>(Data) & 0x3) != 0)
    {
        // Data is not properly aligned for float
        // Consider creating an aligned copy before setting
        float *alignedData = new float[static_cast<size_t>(CstCount) * 4];
        if (!alignedData)
            return FALSE;
        memcpy(alignedData, Data, CstCount * 4 * sizeof(float));
        HRESULT hr = m_Device->SetVertexShaderConstantF(Register, alignedData, CstCount);
        delete[] alignedData;
        return SUCCEEDED(hr);
    }

    return SUCCEEDED(m_Device->SetVertexShaderConstantF(Register, static_cast<const float *>(Data), CstCount));
}

CKBOOL CKDX9RasterizerContext::SetPixelShaderConstant(CKDWORD Register, const void *Data, CKDWORD CstCount)
{
    const CKDWORD registerLimit = 224;
    if (!m_Device || !Data || CstCount == 0 || Register >= registerLimit)
        return FALSE;

    // Use subtraction so malformed unsigned inputs cannot wrap the range check.
    if (CstCount > registerLimit - Register)
        return FALSE;

    // Safe casting - ensure alignment requirement for float data
    if ((reinterpret_cast<uintptr_t>(Data) & 0x3) != 0)
    {
        // Data is not properly aligned for float
        // Consider creating an aligned copy before setting
        float *alignedData = new float[static_cast<size_t>(CstCount) * 4];
        if (!alignedData)
            return FALSE;
        memcpy(alignedData, Data, CstCount * 4 * sizeof(float));
        HRESULT hr = m_Device->SetPixelShaderConstantF(Register, alignedData, CstCount);
        delete[] alignedData;
        return SUCCEEDED(hr);
    }

    return SUCCEEDED(m_Device->SetPixelShaderConstantF(Register, static_cast<const float *>(Data), CstCount));
}

CKBOOL CKDX9RasterizerContext::DrawPrimitive(VXPRIMITIVETYPE pType, WORD *indices, int indexcount, VxDrawPrimitiveData *data)
{
#if LOGGING && LOG_DRAWPRIMITIVE
    fprintf(stderr, "drawprimitive ib %d\n", indexcount);
#endif
#if STEP
    if (step_mode)
    {
        BackToFront(false);
        _getch();
    }
#endif
#if LOG_BATCHSTATS
    ++directbat;
#endif

#ifdef TRACY_ENABLE
    ZoneScopedN(__FUNCTION__);
#endif

    if (!data || data->VertexCount <= 0)
        return FALSE;

    if (!m_SceneBegined && !BeginScene())
        return FALSE;

    // Calculate vertex format and size
    CKDWORD vertexSize;
    CKDWORD vertexFormat = CKRSTGetVertexFormat((CKRST_DPFLAGS)data->Flags, vertexSize);

    // Set clipping state based on flags
    CKBOOL clip = FALSE;
    if (data->Flags & CKRST_DP_DOCLIP)
    {
        if (!SetRenderState(VXRENDERSTATE_CLIPPING, TRUE))
            return FALSE;
        clip = TRUE;
    }
    else
    {
        if (!SetRenderState(VXRENDERSTATE_CLIPPING, FALSE))
            return FALSE;
    }

    // Get a suitable dynamic vertex buffer
    CKDWORD index = GetDynamicVertexBuffer(vertexFormat, data->VertexCount, vertexSize, clip);
    CKDX9VertexBufferDesc *vertexBufferDesc = (index < m_VertexBuffers.Size()) ? static_cast<CKDX9VertexBufferDesc *>(m_VertexBuffers[index]) : NULL;
    if (!vertexBufferDesc || !vertexBufferDesc->DxBuffer)
        return FALSE;

    // Lock the vertex buffer with proper strategy
    void *ppbData = NULL;
    HRESULT hr = E_FAIL;
    CKDWORD startIndex = 0;
    DWORD lockFlags = 0;
    CKBOOL appending = FALSE;

    const CKDWORD vertexCount = static_cast<CKDWORD>(data->VertexCount);

    // Try to append to existing buffer if there's room. Use subtraction so a
    // malformed count cannot wrap the unsigned addition and pass the check.
    if (vertexBufferDesc->m_CurrentVCount <= vertexBufferDesc->m_MaxVertexCount &&
        vertexCount <= vertexBufferDesc->m_MaxVertexCount - vertexBufferDesc->m_CurrentVCount)
    {
        hr = vertexBufferDesc->DxBuffer->Lock(vertexSize * vertexBufferDesc->m_CurrentVCount, vertexSize * vertexCount, &ppbData, D3DLOCK_NOOVERWRITE);
        if (SUCCEEDED(hr) && ppbData)
        {
            startIndex = vertexBufferDesc->m_CurrentVCount;
            vertexBufferDesc->m_CurrentVCount += vertexCount;
            appending = TRUE;
        }
    }

    // If the append failed or there's not enough space, discard and start fresh
    if (!appending)
    {
        hr = vertexBufferDesc->DxBuffer->Lock(0, vertexSize * vertexCount, &ppbData, D3DLOCK_DISCARD);
        if (SUCCEEDED(hr) && ppbData)
        {
            startIndex = 0;
            vertexBufferDesc->m_CurrentVCount = vertexCount;
        }
    }

    // If all locking attempts failed, return error
    if (FAILED(hr) || !ppbData)
        return FALSE;

    // Copy vertex data to the buffer
    CKRSTLoadVertexBuffer(reinterpret_cast<CKBYTE *>(ppbData), vertexFormat, vertexSize, data);

    // Unlock the buffer
    hr = vertexBufferDesc->DxBuffer->Unlock();
    if (FAILED(hr))
        return FALSE;

    return InternalDrawPrimitiveVB(pType, vertexBufferDesc, startIndex, data->VertexCount, indices, indexcount, clip);
}

CKBOOL CKDX9RasterizerContext::DrawPrimitiveVB(VXPRIMITIVETYPE pType, CKDWORD VertexBuffer, CKDWORD StartIndex,
                                               CKDWORD VertexCount, WORD *indices, int indexcount)
{
#if LOGGING && LOG_DRAWPRIMITIVEVB
    fprintf(stderr, "drawprimitive vb %d %d\n", VertexCount, indexcount);
#endif
#if STEP
    if (step_mode)
        _getch();
#endif
#if LOG_BATCHSTATS
    ++vbbat;
#endif

#ifdef TRACY_ENABLE
    ZoneScopedN(__FUNCTION__);
#endif

    if (VertexCount == 0)
        return FALSE;

    if (VertexBuffer >= m_VertexBuffers.Size())
        return FALSE;

    CKVertexBufferDesc *vertexBufferDesc = m_VertexBuffers[VertexBuffer];
    if (!vertexBufferDesc || !(vertexBufferDesc->m_Flags & CKRST_VB_VALID))
        return FALSE;

    if (indices && indexcount <= 0)
        return FALSE;

    // Calculate total vertices needed and ensure buffer has enough
    if (StartIndex > vertexBufferDesc->m_MaxVertexCount ||
        VertexCount > vertexBufferDesc->m_MaxVertexCount - StartIndex)
        return FALSE;

    // Begin the scene if needed
    if (!m_SceneBegined && !BeginScene())
        return FALSE;

    // Cast buffer to DX9-specific implementation
    CKDX9VertexBufferDesc *dxVertexBufferDesc = static_cast<CKDX9VertexBufferDesc *>(vertexBufferDesc);
    if (!dxVertexBufferDesc->DxBuffer)
        return FALSE;

    // Draw the primitive
    return InternalDrawPrimitiveVB(pType, dxVertexBufferDesc, StartIndex, VertexCount, indices, indexcount, TRUE);
}

CKBOOL CKDX9RasterizerContext::DrawPrimitiveVBIB(VXPRIMITIVETYPE pType, CKDWORD VB, CKDWORD IB, CKDWORD MinVIndex,
                                                 CKDWORD VertexCount, CKDWORD StartIndex, int Indexcount)
{
#if LOGGING && LOG_DRAWPRIMITIVEVBIB
    fprintf(stderr, "drawprimitive vbib %d %d\n", VertexCount, Indexcount);
#endif
#if STEP
    if (step_mode)
        _getch();
#endif
#if LOG_BATCHSTATS
    ++vbibbat;
#endif

#ifdef TRACY_ENABLE
    ZoneScopedN(__FUNCTION__);
#endif

    if (VertexCount == 0 || Indexcount <= 0)
        return FALSE;

    // Check if indices are valid
    if (VB >= m_VertexBuffers.Size() || IB >= m_IndexBuffers.Size())
        return FALSE;

    // Get and validate both buffers
    CKDX9VertexBufferDesc *vertexBufferDesc = static_cast<CKDX9VertexBufferDesc *>(m_VertexBuffers[VB]);
    if (!vertexBufferDesc || !(vertexBufferDesc->m_Flags & CKRST_VB_VALID) || !vertexBufferDesc->DxBuffer)
        return FALSE;

    CKDX9IndexBufferDesc *indexBufferDesc = static_cast<CKDX9IndexBufferDesc *>(m_IndexBuffers[IB]);
    if (!indexBufferDesc || !(indexBufferDesc->m_Flags & CKRST_VB_VALID) || !indexBufferDesc->DxBuffer)
        return FALSE;

    // Validate buffer sizes
    if (MinVIndex > vertexBufferDesc->m_MaxVertexCount ||
        VertexCount > vertexBufferDesc->m_MaxVertexCount - MinVIndex)
        return FALSE;

    const CKDWORD indexCount = static_cast<CKDWORD>(Indexcount);
    if (StartIndex > indexBufferDesc->m_MaxIndexCount ||
        indexCount > indexBufferDesc->m_MaxIndexCount - StartIndex)
        return FALSE;

    // Begin scene if needed
    if (!m_SceneBegined && !BeginScene())
        return FALSE;

    if (!SetupStreams(vertexBufferDesc->DxBuffer, vertexBufferDesc->m_VertexFormat, 0, vertexBufferDesc->m_VertexSize))
        return FALSE;

    // Calculate primitive count based on primitive type
    int primitiveCount = Indexcount;
    switch (pType)
    {
        case VX_LINELIST:
            primitiveCount = primitiveCount / 2;
            break;
        case VX_LINESTRIP:
            primitiveCount = primitiveCount - 1;
            break;
        case VX_TRIANGLELIST:
            primitiveCount = primitiveCount / 3;
            break;
        case VX_TRIANGLESTRIP:
        case VX_TRIANGLEFAN:
            primitiveCount = primitiveCount - 2;
            break;
        default:
            break;
    }

    // Make sure we have valid primitive count
    if (primitiveCount <= 0)
        return FALSE;

    // Set index buffer
    HRESULT hr = SetD3DIndices(indexBufferDesc->DxBuffer);
    if (FAILED(hr))
        return FALSE;

    // Draw the primitive
    hr = m_Device->DrawIndexedPrimitive(
        (D3DPRIMITIVETYPE)pType,
        MinVIndex,    // Base vertex index offset
        0,            // Minimum vertex index
        VertexCount,  // Number of vertices
        StartIndex,   // Start index in index buffer
        primitiveCount // Number of primitives
    );

    return SUCCEEDED(hr);
}

CKBOOL CKDX9RasterizerContext::CreateObject(CKDWORD ObjIndex, CKRST_OBJECTTYPE Type, void *DesiredFormat)
{
#ifdef TRACY_ENABLE
    ZoneScopedN(__FUNCTION__);
#endif
    CKBOOL result = FALSE;

    if (ObjIndex >= m_Textures.Size())
        return FALSE;

    switch (Type)
    {
        case CKRST_OBJ_TEXTURE:
            result = CreateTexture(ObjIndex, static_cast<CKTextureDesc *>(DesiredFormat));
            break;
        case CKRST_OBJ_SPRITE:
            {
                CKSpriteDesc *sd = static_cast<CKSpriteDesc *>(DesiredFormat);
                CKDX9_LOG("CreateObject SPRITE idx=%u desired=%p desired.Format=%dx%d bpp=%d flags=0x%08X",
                          ObjIndex, sd,
                          sd ? sd->Format.Width : -1, sd ? sd->Format.Height : -1,
                          sd ? sd->Format.BitsPerPixel : -1,
                          sd ? sd->Flags : 0);
                result = CreateSprite(ObjIndex, sd);
                if (result && ObjIndex < m_Sprites.Size() && m_Sprites[ObjIndex])
                {
                    CKSpriteDesc *stored = m_Sprites[ObjIndex];
                    CKDX9_LOG("CreateObject SPRITE idx=%u -> stored Format=%dx%d bpp=%d flags=0x%08X textures=%d",
                              ObjIndex, stored->Format.Width, stored->Format.Height,
                              stored->Format.BitsPerPixel, stored->Flags,
                              (int)stored->Textures.Size());
                    int subIndex = 0;
                    for (CKSPRTextInfo *it = stored->Textures.Begin(); it != stored->Textures.End() && subIndex < 8; ++it, ++subIndex)
                    {
                        CKDX9_LOG("CreateObject SPRITE idx=%u sub[%d] tex=%u pos=(%d,%d) wh=%dx%d storage=%dx%d",
                                  ObjIndex, subIndex, it->IndexTexture,
                                  it->x, it->y, it->w, it->h, it->sw, it->sh);
                    }
                }
#if LOGGING
                CKSpriteDesc *desc = m_Sprites[ObjIndex];
                fprintf(stderr, "idx: %d\n", ObjIndex);
                for (CKSPRTextInfo *it = desc->Textures.Begin(); it != desc->Textures.End(); ++it)
                {
                    fprintf(stderr, "(%d,%d) WxH: %dx%d, SWxSH: %dx%d\n", it->x, it->y, it->w, it->h, it->sw, it->sh);
                }
                fprintf(stderr, "---\n");
#endif
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
            return FALSE;
    }

    return result;
}

CKBOOL CKDX9RasterizerContext::LoadTexture(CKDWORD Texture, const VxImageDescEx &SurfDesc, int miplevel)
{
#ifdef TRACY_ENABLE
    ZoneScopedN(__FUNCTION__);
#endif
    ++g_dx9Frame.loadTextures;
    if (g_dx9Frame.loadTextures <= 8)
    {
        CKDX9_LOG("LoadTexture[frame=%u,#%u] tex=%u src=%dx%d bpp=%d mip=%d flags=0x%08X image=%p bpl=%d amask=0x%08X rmask=0x%08X",
                  g_dx9Frame.frameIndex, g_dx9Frame.loadTextures, Texture,
                  SurfDesc.Width, SurfDesc.Height, SurfDesc.BitsPerPixel, miplevel,
                  SurfDesc.Flags, SurfDesc.Image, SurfDesc.BytesPerLine,
                  SurfDesc.AlphaMask, SurfDesc.RedMask);
    }

#if LOGGING && LOG_LOADTEXTURE
    texture_used[Texture] = 1;
    fprintf(stderr, "load texture %d %dx%d %d\n", Texture, SurfDesc.Width, SurfDesc.Height, miplevel);
#endif
    if (Texture >= m_Textures.Size())
        return FALSE;

    CKDX9TextureDesc *desc = static_cast<CKDX9TextureDesc *>(m_Textures[Texture]);
    if (!desc || !desc->DxTexture)
        return FALSE;

    // Skip special texture types (cubemaps and render targets are handled separately)
    if ((desc->Flags & (CKRST_TEXTURE_CUBEMAP | CKRST_TEXTURE_RENDERTARGET)) != 0)
        return TRUE;

    if (miplevel < -1 || !DX9ValidateImageDesc(SurfDesc))
        return FALSE;

    // Determine target mip level
    int actualMipLevel = (miplevel < 0) ? 0 : miplevel;

    D3DSURFACE_DESC surfaceDesc;
    HRESULT hr = desc->DxTexture->GetLevelDesc(actualMipLevel, &surfaceDesc);
    if (FAILED(hr))
    {
        CKDX9_LOG("LoadTexture GetLevelDesc failed texture=%u mip=%d hr=0x%08lX", Texture, actualMipLevel, (unsigned long)hr);
        return FALSE;
    }

    if (g_dx9Frame.loadTextures <= 8)
    {
        CKDX9_LOG("LoadTexture[%u] dstLevel=%d descFmt=%dx%d bpp=%d flags=0x%08X dxFmt=0x%08X surface=%ux%u",
                  Texture, actualMipLevel,
                  desc->Format.Width, desc->Format.Height, desc->Format.BitsPerPixel,
                  desc->Flags, (unsigned)surfaceDesc.Format,
                  surfaceDesc.Width, surfaceDesc.Height);
    }

    // Special handling for compressed textures (DXT formats)
    bool isCompressedFormat = 
        (surfaceDesc.Format == D3DFMT_DXT1 || 
         surfaceDesc.Format == D3DFMT_DXT2 || 
         surfaceDesc.Format == D3DFMT_DXT3 ||
         surfaceDesc.Format == D3DFMT_DXT4 || 
         surfaceDesc.Format == D3DFMT_DXT5);

    if (isCompressedFormat)
    {
        if (!D3DXLoadSurfaceFromSurface || !D3DXLoadSurfaceFromMemory)
            return FALSE;

        // Use D3DX functions for compressed textures
        IDirect3DSurface9 *pSurfaceLevel = NULL;
        hr = desc->DxTexture->GetSurfaceLevel(actualMipLevel, &pSurfaceLevel);
        if (FAILED(hr) || !pSurfaceLevel)
        {
            SAFERELEASE(pSurfaceLevel);
            return FALSE;
        }

        // Prepare source rect
        RECT srcRect{0, 0, (LONG)SurfDesc.Width, (LONG)SurfDesc.Height};
        D3DFORMAT format = VxPixelFormatToD3DFormat(VxImageDesc2PixelFormat(SurfDesc));
        if (format == D3DFMT_UNKNOWN)
        {
            SAFERELEASE(pSurfaceLevel);
            return FALSE;
        }

        // Load texture data
        hr = D3DXLoadSurfaceFromMemory(
            pSurfaceLevel,      // Destination surface
            NULL,               // No palette
            NULL,               // Full destination surface
            SurfDesc.Image,     // Source data
            format,             // Source format
            SurfDesc.BytesPerLine, // Source pitch
            NULL,               // No palette
            &srcRect,           // Source rect
            D3DX_FILTER_LINEAR, // Filter
            0);                 // No color key
        if (FAILED(hr))
        {
            SAFERELEASE(pSurfaceLevel);
            return FALSE;
        }

         // Generate mipmaps if requested
         if (miplevel == -1 && desc->MipMapCount > 0)
         {
            // Generate each mipmap level from the base level using box filter
            for (int i = 1; i <= desc->MipMapCount; ++i)
            {
                IDirect3DSurface9 *pMipSurface = NULL;
                hr = desc->DxTexture->GetSurfaceLevel(i, &pMipSurface);
                if (FAILED(hr) || !pMipSurface)
                {
                    SAFERELEASE(pMipSurface);
                    SAFERELEASE(pSurfaceLevel);
                    return FALSE;
                }

                // Generate this mip level from the base level.
                hr = D3DXLoadSurfaceFromSurface(
                    pMipSurface,        // Destination
                    NULL,               // No palette
                    NULL,               // Full surface
                    pSurfaceLevel,      // Source (base level)
                    NULL,               // No palette
                    NULL,               // Full surface
                    D3DX_FILTER_BOX,    // Box filter for better quality
                    0);                 // No color key

                SAFERELEASE(pMipSurface);
                if (FAILED(hr))
                {
                    SAFERELEASE(pSurfaceLevel);
                    return FALSE;
                }
            }
        }

        SAFERELEASE(pSurfaceLevel);
        return TRUE;
    }

    // Standard texture loading path (non-compressed)

    // Mipmap generation always uses two distinct 32-bit working buffers. This
    // avoids format-dependent stride errors and never aliases the source and
    // destination passed to VxGenerateMipMap.
    VxImageDescEx src = SurfDesc;
    XArray<CKBYTE> mipCurrent;
    XArray<CKBYTE> mipNext;
    bool needMipmapGen = (miplevel == -1 && desc->MipMapCount > 0);

    if (needMipmapGen)
    {
        if (surfaceDesc.Width == 0 || surfaceDesc.Height == 0 ||
            surfaceDesc.Height > DX9_SIZE_MAX / surfaceDesc.Width ||
            surfaceDesc.Width > static_cast<UINT>(INT_MAX / 4))
        {
            return FALSE;
        }
        const size_t pixelCount = static_cast<size_t>(surfaceDesc.Width) * surfaceDesc.Height;
        if (pixelCount > static_cast<size_t>(INT_MAX) / 4)
            return FALSE;

        const int bufferSize = static_cast<int>(pixelCount * 4);
        mipCurrent.Resize(bufferSize);
        mipNext.Resize(bufferSize);

        VxImageDescEx working;
        working.Width = surfaceDesc.Width;
        working.Height = surfaceDesc.Height;
        working.BitsPerPixel = 32;
        working.BytesPerLine = 4 * working.Width;
        working.AlphaMask = A_MASK;
        working.RedMask = R_MASK;
        working.GreenMask = G_MASK;
        working.BlueMask = B_MASK;
        working.Image = mipCurrent.Begin();
        VxDoBlit(src, working);
        src = working;
    }

    // Lock and load base level
    D3DLOCKED_RECT lockRect;
    hr = desc->DxTexture->LockRect(actualMipLevel, &lockRect, NULL, 0);
    if (FAILED(hr))
    {
        return FALSE;
    }

    if (desc->Format.BytesPerLine == 0)
        desc->Format.BytesPerLine = lockRect.Pitch;

    // Copy data to texture surface
    if (!LoadSurface(surfaceDesc, lockRect, src))
    {
        // Unlock before returning
        desc->DxTexture->UnlockRect(actualMipLevel);
        return FALSE;
    }

    // Unlock the base level
    hr = desc->DxTexture->UnlockRect(actualMipLevel);
    if (FAILED(hr))
    {
        return FALSE;
    }

    // Generate mipmaps if needed
    if (needMipmapGen && !mipCurrent.IsEmpty() && !mipNext.IsEmpty())
    {
        VxImageDescEx current = src;

        // Generate each mip level
        for (int i = 1; i <= desc->MipMapCount; ++i)
        {
            VxGenerateMipMap(current, mipNext.Begin());

            VxImageDescEx next = current;
            if (next.Width > 1)
                next.Width >>= 1;
            if (next.Height > 1)
                next.Height >>= 1;
            next.BytesPerLine = 4 * next.Width;
            next.Image = mipNext.Begin();

            // Check if surface exists at this mip level
            if (FAILED(desc->DxTexture->GetLevelDesc(i, &surfaceDesc)))
            {
                return FALSE;
            }

            // Lock the mip level
            hr = desc->DxTexture->LockRect(i, &lockRect, NULL, 0);
            if (FAILED(hr))
            {
                return FALSE;
            }

            // Copy data to the mip level
            if (!LoadSurface(surfaceDesc, lockRect, next))
            {
                desc->DxTexture->UnlockRect(i);
                return FALSE;
            }

            // Unlock the mip level
            hr = desc->DxTexture->UnlockRect(i);
            if (FAILED(hr))
            {
                return FALSE;
            }

            // Stop if we've reached smallest mip level
            if (next.Width <= 1 && next.Height <= 1)
            {
                break;
            }

            current = next;
            mipCurrent.Swap(mipNext);
        }
    }

    return TRUE;
}

CKBOOL CKDX9RasterizerContext::CopyToTexture(CKDWORD Texture, VxRect *Src, VxRect *Dest, CKRST_CUBEFACE Face)
{
#ifdef TRACY_ENABLE
    ZoneScopedN(__FUNCTION__);
#endif
    CKDX9_LOG("CopyToTexture begin frame=%u texture=%u face=%u src=%s dest=%s",
              g_dx9Frame.frameIndex, Texture, (unsigned)Face,
              Src ? "set" : "full", Dest ? "set" : "full");
#if LOGGING && LOG_COPYTEXTURE
    fprintf(stderr, "copy to texture %d (%f,%f,%f,%f) (%f,%f,%f,%f)\n", Texture,
            Src->left, Src->top, Src->right, Src->bottom,
            Dest->left, Dest->top, Dest->right, Dest->bottom);
#endif
    if (!m_Device || !m_Driver || Texture >= m_Textures.Size())
    {
        CKDX9_LOG("CopyToTexture rejected: texture=%u textureCount=%d", Texture, m_Textures.Size());
        return FALSE;
    }

    CKDX9TextureDesc *desc = static_cast<CKDX9TextureDesc *>(m_Textures[Texture]);
    if (!desc)
    {
        CKDX9_LOG("CopyToTexture rejected: desc=%p dxTexture=%p", desc, desc ? desc->DxTexture : NULL);
        return FALSE;
    }

    CKBOOL cubemap = (desc->Flags & CKRST_TEXTURE_CUBEMAP) != 0;
    if (cubemap)
    {
        if (!desc->DxCubeTexture || static_cast<int>(Face) < 0 || Face > CKRST_CUBEFACE_ZNEG)
        {
            CKDX9_LOG("CopyToTexture rejected: cube=%p face=%u", desc->DxCubeTexture, (unsigned)Face);
            return FALSE;
        }
    }
    else if (!desc->DxTexture)
    {
        CKDX9_LOG("CopyToTexture rejected: desc=%p dxTexture=%p", desc, desc->DxTexture);
        return FALSE;
    }

    // Create destination rectangle
    RECT destRect;
    if (Dest)
        SetRect(&destRect, (int)Dest->left, (int)Dest->top, (int)Dest->right, (int)Dest->bottom);
    else
        SetRect(&destRect, 0, 0, desc->Format.Width, desc->Format.Height);

    // Create source rectangle
    RECT srcRect;
    if (Src)
        SetRect(&srcRect, (int)Src->left, (int)Src->top, (int)Src->right, (int)Src->bottom);
    else
        SetRect(&srcRect, 0, 0, desc->Format.Width, desc->Format.Height);

    HRESULT hr = S_OK;
    IDirect3DSurface9 *backBuffer = NULL;
    IDirect3DSurface9 *textureSurface = NULL;
    CKBOOL success = FALSE;

    // Get back buffer
    hr = m_Device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer);
    if (FAILED(hr) || !backBuffer)
    {
        CKDX9_LOG("CopyToTexture GetBackBuffer failed hr=0x%08lX backBuffer=%p", (unsigned long)hr, backBuffer);
        return FALSE;
    }

    // Get texture surface
    if (cubemap)
        hr = desc->DxCubeTexture->GetCubeMapSurface((D3DCUBEMAP_FACES)Face, 0, &textureSurface);
    else
        hr = desc->DxTexture->GetSurfaceLevel(0, &textureSurface);
    if (FAILED(hr) || !textureSurface)
    {
        CKDX9_LOG("CopyToTexture GetSurfaceLevel failed texture=%u hr=0x%08lX surface=%p",
                  Texture, (unsigned long)hr, textureSurface);
        SAFERELEASE(backBuffer);
        return FALSE;
    }

    int copyWidth = destRect.right - destRect.left;
    int copyHeight = destRect.bottom - destRect.top;
    int sourceWidth = srcRect.right - srcRect.left;
    int sourceHeight = srcRect.bottom - srcRect.top;
    if (sourceWidth < copyWidth && sourceHeight < copyHeight)
    {
        copyWidth = sourceWidth;
        copyHeight = sourceHeight;
    }

    RECT copyDestRect;
    SetRect(&copyDestRect,
            destRect.left,
            destRect.top,
            destRect.left + copyWidth,
            destRect.top + copyHeight);

    // Copy from back buffer to texture surface
    hr = m_Device->StretchRect(backBuffer, &srcRect, textureSurface, &copyDestRect, D3DTEXF_NONE);
    CKDX9_LOG("CopyToTexture StretchRect backBuffer=%p textureSurface=%p src=(%ld,%ld,%ld,%ld) dest=(%ld,%ld) hr=0x%08lX",
              backBuffer, textureSurface,
              srcRect.left, srcRect.top, srcRect.right, srcRect.bottom,
              destRect.left, destRect.top, (unsigned long)hr);
    if (SUCCEEDED(hr))
    {
        success = TRUE;
    }
    // Managed textures cannot be StretchRect destinations. Build a replacement
    // render-target texture transactionally and only publish it after the copy
    // succeeds, so a bad rectangle/format or allocation failure is non-destructive.
    else if (desc->Flags & CKRST_TEXTURE_MANAGED)
    {
        SAFERELEASE(textureSurface);
        int textureWidth = desc->Format.Width;
        int textureHeight = desc->Format.Height;
        LPDIRECT3DTEXTURE9 replacementTexture = NULL;
        LPDIRECT3DCUBETEXTURE9 replacementCubeTexture = NULL;

        if (cubemap)
        {
            hr = m_Device->CreateCubeTexture(
                textureWidth,
                1,
                D3DUSAGE_RENDERTARGET,
                m_PresentParams.BackBufferFormat,
                D3DPOOL_DEFAULT,
                &replacementCubeTexture,
                NULL
            );
            if (SUCCEEDED(hr) && replacementCubeTexture)
                hr = replacementCubeTexture->GetCubeMapSurface((D3DCUBEMAP_FACES)Face, 0, &textureSurface);
        }
        else
        {
            hr = m_Device->CreateTexture(
                textureWidth,
                textureHeight,
                1,
                D3DUSAGE_RENDERTARGET,
                m_PresentParams.BackBufferFormat,
                D3DPOOL_DEFAULT,
                &replacementTexture,
                NULL
            );
            if (SUCCEEDED(hr) && replacementTexture)
                hr = replacementTexture->GetSurfaceLevel(0, &textureSurface);
        }

        if (SUCCEEDED(hr) && textureSurface)
        {
            // Try copy again
            hr = m_Device->StretchRect(backBuffer, &srcRect, textureSurface, &copyDestRect, D3DTEXF_NONE);
            CKDX9_LOG("CopyToTexture retry StretchRect texture=%u hr=0x%08lX newDxTexture=%p surface=%p",
                      Texture, (unsigned long)hr,
                      cubemap ? (void *)replacementCubeTexture : (void *)replacementTexture,
                      textureSurface);
            success = SUCCEEDED(hr);
        }

        if (success)
        {
            IDirect3DBaseTexture9 *oldTexture = cubemap
                ? static_cast<IDirect3DBaseTexture9 *>(desc->DxCubeTexture)
                : static_cast<IDirect3DBaseTexture9 *>(desc->DxTexture);
            IDirect3DBaseTexture9 *newTexture = cubemap
                ? static_cast<IDirect3DBaseTexture9 *>(replacementCubeTexture)
                : static_cast<IDirect3DBaseTexture9 *>(replacementTexture);

            // Keep stages that referenced the logical CK texture attached to
            // the newly committed D3D resource. Do not publish the replacement
            // if any stage cannot be changed; the helper rolls prior stages back.
            if (!DX9ReplaceBoundTexture(m_Device, m_Driver->m_3DCaps.MaxNumberTextureStage,
                                        oldTexture, newTexture))
            {
                success = FALSE;
            }

            if (success)
            {
                if (desc->DxRenderTexture == desc->DxTexture)
                    desc->DxRenderTexture = NULL;
                if (cubemap)
                {
                    SAFERELEASE(desc->DxCubeTexture);
                    desc->DxCubeTexture = replacementCubeTexture;
                    replacementCubeTexture = NULL;
                }
                else
                {
                    SAFERELEASE(desc->DxTexture);
                    desc->DxTexture = replacementTexture;
                    replacementTexture = NULL;
                }
                SAFERELEASE(desc->DxRenderTexture);

                D3DFormatToTextureDesc(m_PresentParams.BackBufferFormat, desc);
                desc->Format.Width = textureWidth;
                desc->Format.Height = textureHeight;
                desc->MipMapCount = 0;
                desc->Flags &= ~CKRST_TEXTURE_MANAGED;
                desc->Flags |= CKRST_TEXTURE_RENDERTARGET | CKRST_TEXTURE_VALID;
                if (cubemap)
                    desc->Flags |= CKRST_TEXTURE_CUBEMAP;
            }
        }

        SAFERELEASE(replacementTexture);
        SAFERELEASE(replacementCubeTexture);
    }

    // Clean up resources
    SAFERELEASE(textureSurface);
    SAFERELEASE(backBuffer);

    return success;
}

CKBOOL CKDX9RasterizerContext::SetTargetTexture(CKDWORD TextureObject, int Width, int Height, CKRST_CUBEFACE Face)
{
#ifdef TRACY_ENABLE
    ZoneScopedN(__FUNCTION__);
#endif
    CKDX9_LOG("SetTargetTexture begin frame=%u texture=%u size=%dx%d face=%u current=%u defaultBack=%p defaultDepth=%p",
              g_dx9Frame.frameIndex, TextureObject, Width, Height, (unsigned)Face,
              m_CurrentTextureIndex, m_DefaultBackBuffer, m_DefaultDepthBuffer);
    if (!m_Device || !m_Driver)
    {
        CKDX9_LOG("SetTargetTexture rejected: device=%p driver=%p", m_Device, m_Driver);
        return FALSE;
    }

    // Render-target changes are only legal outside an active scene.
    if (!EndScene())
        return FALSE;

    // Restoring the default render target if TextureObject is 0
    if (TextureObject == 0)
    {
        if (!m_DefaultBackBuffer)
        {
            CKDX9_LOG("SetTargetTexture restore rejected: no default backbuffer current=%u", m_CurrentTextureIndex);
            return FALSE;
        }

        HRESULT hr = m_Device->SetRenderTarget(0, m_DefaultBackBuffer);
        if (FAILED(hr))
        {
            CKDX9_LOG("SetTargetTexture restore SetRenderTarget failed hr=0x%08lX back=%p",
                      (unsigned long)hr, m_DefaultBackBuffer);
            return FALSE;
        }

        CKBOOL restored = TRUE;

        // Restore default depth buffer
        if (m_DefaultDepthBuffer)
        {
            restored = SUCCEEDED(m_Device->SetDepthStencilSurface(m_DefaultDepthBuffer));
        }

        // Restore only the stages that referenced the render-target texture
        // before redirection. Other texture bindings were never disturbed.
        if (m_CurrentTextureIndex < m_Textures.Size())
        {
            CKDX9TextureDesc *desc = static_cast<CKDX9TextureDesc *>(m_Textures[m_CurrentTextureIndex]);
            if (desc)
            {
                IDirect3DBaseTexture9 *texture = (desc->Flags & CKRST_TEXTURE_CUBEMAP)
                    ? static_cast<IDirect3DBaseTexture9 *>(desc->DxCubeTexture)
                    : static_cast<IDirect3DBaseTexture9 *>(desc->DxTexture);
                if (!texture && m_RenderTargetTextureStageMask != 0)
                {
                    restored = FALSE;
                }
                else
                {
                    const int stageCount = XMin((int)m_Driver->m_3DCaps.MaxNumberTextureStage, 32);
                    for (int stage = 0; stage < stageCount; ++stage)
                    {
                        if ((m_RenderTargetTextureStageMask & (1u << stage)) != 0 &&
                            FAILED(m_Device->SetTexture(stage, texture)))
                        {
                            restored = FALSE;
                        }
                    }
                }
            }
            else if (m_RenderTargetTextureStageMask != 0)
            {
                restored = FALSE;
            }
        }
        else if (m_RenderTargetTextureStageMask != 0)
        {
            restored = FALSE;
        }

        if (!restored)
        {
            CKDX9_LOG("SetTargetTexture restore incomplete; keeping backup state for retry");
            return FALSE;
        }

        m_RenderTargetTextureStageMask = 0;
        m_CurrentTextureIndex = 0;

        SAFERELEASE(m_DefaultBackBuffer);
        SAFERELEASE(m_DefaultDepthBuffer);
        CKDX9_LOG("SetTargetTexture restore result=%d", restored);
        return restored;
    }

    if (TextureObject >= m_Textures.Size() || m_DefaultBackBuffer)
    {
        CKDX9_LOG("SetTargetTexture rejected: texture=%u textureCount=%d defaultBack=%p",
                  TextureObject, m_Textures.Size(), m_DefaultBackBuffer);
        return FALSE;
    }

    // Handle cube map case
    CKBOOL cubemap = FALSE;
    if (Height < 0)
    {
        cubemap = TRUE;
        Height = Width;
    }
    if (cubemap && (static_cast<int>(Face) < 0 || Face > CKRST_CUBEFACE_ZNEG))
    {
        CKDX9_LOG("SetTargetTexture rejected: invalid cube face=%u", (unsigned)Face);
        return FALSE;
    }
    if (Width < 0)
    {
        CKDX9_LOG("SetTargetTexture rejected: negative width=%d", Width);
        return FALSE;
    }

    // Get or create the texture descriptor
    CKDX9TextureDesc *desc = static_cast<CKDX9TextureDesc *>(m_Textures[TextureObject]);
    CKBOOL createdDesc = FALSE;
    if (!desc)
    {
        desc = new CKDX9TextureDesc;
        desc->Format.Width = (Width != 0) ? Width : 256;
        desc->Format.Height = (Height != 0) ? Height : 256;
        m_Textures[TextureObject] = desc;
        createdDesc = TRUE;
        CKDX9_LOG("SetTargetTexture created desc texture=%u desc=%p fmt=%ux%u",
                  TextureObject, desc, desc->Format.Width, desc->Format.Height);
    }
    CKDX9_LOG("SetTargetTexture desc texture=%u desc=%p flags=0x%08X fmt=%ux%u bpp=%d dx=%p render=%p cube=%p locked=%p cubemap=%d",
              TextureObject, desc, desc->Flags, desc->Format.Width, desc->Format.Height,
              desc->Format.BitsPerPixel, desc->DxTexture, desc->DxRenderTexture,
              desc->DxCubeTexture, desc->DxLockedSurface, cubemap);

    if ((Width == 0 && desc->Format.Width > static_cast<CKDWORD>(INT_MAX)) ||
        (Height == 0 && desc->Format.Height > static_cast<CKDWORD>(INT_MAX)))
    {
        if (createdDesc)
        {
            delete desc;
            m_Textures[TextureObject] = NULL;
        }
        return FALSE;
    }

    int targetWidth = (Width != 0) ? Width : desc->Format.Width;
    int targetHeight = (Height != 0) ? Height : desc->Format.Height;
    if (targetWidth <= 0)
        targetWidth = 256;
    if (targetHeight <= 0)
        targetHeight = 256;
    if (cubemap)
        targetHeight = targetWidth;
    const CKDX9RasterizerDriver *dxDriver = static_cast<CKDX9RasterizerDriver *>(m_Driver);
    if (static_cast<UINT>(targetWidth) > dxDriver->m_D3DCaps.MaxTextureWidth ||
        static_cast<UINT>(targetHeight) > dxDriver->m_D3DCaps.MaxTextureHeight)
    {
        CKDX9_LOG("SetTargetTexture rejected: size=%ux%u caps=%ux%u",
                  static_cast<unsigned>(targetWidth), static_cast<unsigned>(targetHeight),
                  dxDriver->m_D3DCaps.MaxTextureWidth, dxDriver->m_D3DCaps.MaxTextureHeight);
        if (createdDesc)
        {
            delete desc;
            m_Textures[TextureObject] = NULL;
        }
        return FALSE;
    }

    // Capture current render target and depth buffer
    HRESULT hr = m_Device->GetRenderTarget(0, &m_DefaultBackBuffer);
    if (FAILED(hr) || !m_DefaultBackBuffer)
    {
        CKDX9_LOG("SetTargetTexture GetRenderTarget failed hr=0x%08lX back=%p",
                  (unsigned long)hr, m_DefaultBackBuffer);
        if (createdDesc)
        {
            delete desc;
            m_Textures[TextureObject] = NULL;
        }
        SAFERELEASE(m_DefaultBackBuffer);
        return FALSE;
    }

    hr = m_Device->GetDepthStencilSurface(&m_DefaultDepthBuffer);
    if (FAILED(hr))
    {
        CKDX9_LOG("SetTargetTexture GetDepthStencilSurface failed/nonfatal hr=0x%08lX", (unsigned long)hr);
        // No depth buffer is ok, but we should clear the pointer
        SAFERELEASE(m_DefaultDepthBuffer);
    }

    // A render target cannot simultaneously be sampled. Unbind only stages
    // that reference this texture and remember them for the return to the
    // default target; unrelated stage bindings must survive the transition.
    IDirect3DBaseTexture9 *originalTexture = (desc->Flags & CKRST_TEXTURE_CUBEMAP)
        ? static_cast<IDirect3DBaseTexture9 *>(desc->DxCubeTexture)
        : static_cast<IDirect3DBaseTexture9 *>(desc->DxTexture);
    m_RenderTargetTextureStageMask = 0;
    CKBOOL unbindSucceeded = TRUE;
    const int textureStageCount = XMin((int)m_Driver->m_3DCaps.MaxNumberTextureStage, 32);
    for (int i = 0; i < textureStageCount; ++i)
    {
        IDirect3DBaseTexture9 *boundTexture = NULL;
        if (SUCCEEDED(m_Device->GetTexture(i, &boundTexture)))
        {
            if (boundTexture && boundTexture == originalTexture)
            {
                if (SUCCEEDED(m_Device->SetTexture(i, NULL)))
                    m_RenderTargetTextureStageMask |= (1u << i);
                else
                    unbindSucceeded = FALSE;
            }
            SAFERELEASE(boundTexture);
        }
    }

    // Try to use existing texture as render target
    CKBOOL surfaceSuccess = FALSE;
    IDirect3DSurface9 *surface = NULL;
    LPDIRECT3DTEXTURE9 replacementTexture = NULL;
    LPDIRECT3DCUBETEXTURE9 replacementCubeTexture = NULL;

    if (!unbindSucceeded)
        return DX9FailSetTargetTexture(
            this, TextureObject, desc, createdDesc, originalTexture,
            textureStageCount, surface, replacementTexture, replacementCubeTexture);
    
    if ((cubemap && (desc->Flags & CKRST_TEXTURE_CUBEMAP) && desc->DxCubeTexture) ||
        (!cubemap && !(desc->Flags & CKRST_TEXTURE_CUBEMAP) && desc->DxTexture))
    {
        if (cubemap)
        {
            D3DRESOURCETYPE type = desc->DxCubeTexture->GetType();
            if (type == D3DRTYPE_CUBETEXTURE)
            {
                hr = desc->DxCubeTexture->GetCubeMapSurface((D3DCUBEMAP_FACES)Face, 0, &surface);
                if (SUCCEEDED(hr) && surface)
                {
                    D3DSURFACE_DESC surfaceDesc;
                    hr = surface->GetDesc(&surfaceDesc);
                    if (SUCCEEDED(hr) && (surfaceDesc.Usage & D3DUSAGE_RENDERTARGET) &&
                        surfaceDesc.Width == (UINT)targetWidth && surfaceDesc.Height == (UINT)targetHeight)
                    {
                        surfaceSuccess = TRUE;
                    }
                    else
                    {
                        SAFERELEASE(surface);
                    }
                }
            }
        }
        else
        {
            hr = desc->DxTexture->GetSurfaceLevel(0, &surface);
            if (SUCCEEDED(hr) && surface)
            {
                D3DSURFACE_DESC surfaceDesc;
                hr = surface->GetDesc(&surfaceDesc);
                if (SUCCEEDED(hr) && (surfaceDesc.Usage & D3DUSAGE_RENDERTARGET) &&
                    surfaceDesc.Width == (UINT)targetWidth && surfaceDesc.Height == (UINT)targetHeight)
                {
                    surfaceSuccess = TRUE;
                }
                else
                {
                    SAFERELEASE(surface);
                }
            }
        }
    }
    
    // Create a replacement if the existing texture cannot be used. Keep the
    // descriptor and old resource untouched until the complete target/depth
    // transition succeeds.
    if (!surfaceSuccess)
    {
        SAFERELEASE(surface);

        if (cubemap)
        {
            hr = m_Device->CreateCubeTexture(
                targetWidth,
                1, 
                D3DUSAGE_RENDERTARGET, 
                m_PresentParams.BackBufferFormat,
                D3DPOOL_DEFAULT, 
                &replacementCubeTexture,
                NULL
            );
            if (FAILED(hr) || !replacementCubeTexture)
            {
                CKDX9_LOG("SetTargetTexture CreateCubeTexture failed texture=%u fmt=%ux%u hr=0x%08lX",
                          TextureObject, targetWidth, targetHeight, (unsigned long)hr);
                return DX9FailSetTargetTexture(
                    this, TextureObject, desc, createdDesc, originalTexture,
                    textureStageCount, surface, replacementTexture, replacementCubeTexture);
            }

            hr = replacementCubeTexture->GetCubeMapSurface((D3DCUBEMAP_FACES)Face, 0, &surface);
            if (FAILED(hr) || !surface)
            {
                CKDX9_LOG("SetTargetTexture GetCubeMapSurface failed texture=%u face=%u hr=0x%08lX surface=%p",
                          TextureObject, (unsigned)Face, (unsigned long)hr, surface);
                return DX9FailSetTargetTexture(
                    this, TextureObject, desc, createdDesc, originalTexture,
                    textureStageCount, surface, replacementTexture, replacementCubeTexture);
            }
        }
        else
        {
            hr = m_Device->CreateTexture(
                targetWidth,
                targetHeight,
                1, 
                D3DUSAGE_RENDERTARGET,
                m_PresentParams.BackBufferFormat, 
                D3DPOOL_DEFAULT, 
                &replacementTexture,
                NULL
            );
            if (FAILED(hr) || !replacementTexture)
            {
                CKDX9_LOG("SetTargetTexture CreateTexture sample failed texture=%u fmt=%ux%u hr=0x%08lX",
                          TextureObject, targetWidth, targetHeight, (unsigned long)hr);
                return DX9FailSetTargetTexture(
                    this, TextureObject, desc, createdDesc, originalTexture,
                    textureStageCount, surface, replacementTexture, replacementCubeTexture);
            }

            hr = replacementTexture->GetSurfaceLevel(0, &surface);
            if (FAILED(hr) || !surface)
            {
                CKDX9_LOG("SetTargetTexture GetSurfaceLevel render failed texture=%u hr=0x%08lX surface=%p dx=%p",
                          TextureObject, (unsigned long)hr, surface, replacementTexture);
                return DX9FailSetTargetTexture(
                    this, TextureObject, desc, createdDesc, originalTexture,
                    textureStageCount, surface, replacementTexture, replacementCubeTexture);
            }
        }
    }

    D3DSURFACE_DESC targetSurfaceDesc = {};
    hr = surface->GetDesc(&targetSurfaceDesc);
    if (FAILED(hr))
    {
        CKDX9_LOG("SetTargetTexture GetDesc failed texture=%u hr=0x%08lX",
                  TextureObject, (unsigned long)hr);
        return DX9FailSetTargetTexture(
            this, TextureObject, desc, createdDesc, originalTexture,
            textureStageCount, surface, replacementTexture, replacementCubeTexture);
    }

    // Get appropriate depth buffer
    IDirect3DSurface9 *zbuffer = GetTempZBuffer(targetWidth, targetHeight);
    if (!zbuffer)
    {
        CKDX9_LOG("SetTargetTexture failed: no compatible depth surface texture=%u size=%ux%u",
                  TextureObject, targetWidth, targetHeight);
        return DX9FailSetTargetTexture(
            this, TextureObject, desc, createdDesc, originalTexture,
            textureStageCount, surface, replacementTexture, replacementCubeTexture);
    }
    
    // A texture render target is non-multisampled. Detach the current depth
    // surface before changing render targets so the old MSAA/depth dimensions
    // cannot make SetRenderTarget fail compatibility validation.
    hr = m_Device->SetDepthStencilSurface(NULL);
    if (FAILED(hr))
    {
        CKDX9_LOG("SetTargetTexture failed to detach depth surface hr=0x%08lX", (unsigned long)hr);
        return DX9FailSetTargetTexture(
            this, TextureObject, desc, createdDesc, originalTexture,
            textureStageCount, surface, replacementTexture, replacementCubeTexture);
    }

    // Set render target and depth buffer
    hr = m_Device->SetRenderTarget(0, surface);
    if (FAILED(hr))
    {
        CKDX9_LOG("SetTargetTexture SetRenderTarget failed texture=%u surface=%p hr=0x%08lX surfaceSuccess=%d",
                  TextureObject, surface, (unsigned long)hr, surfaceSuccess);
        return DX9FailSetTargetTexture(
            this, TextureObject, desc, createdDesc, originalTexture,
            textureStageCount, surface, replacementTexture, replacementCubeTexture);
    }

    // Set the depth-stencil surface if available
    if (zbuffer)
    {
        hr = m_Device->SetDepthStencilSurface(zbuffer);
        CKDX9_LOG("SetTargetTexture SetDepthStencilSurface z=%p hr=0x%08lX", zbuffer, (unsigned long)hr);
        if (FAILED(hr))
        {
            return DX9FailSetTargetTexture(
                this, TextureObject, desc, createdDesc, originalTexture,
                textureStageCount, surface, replacementTexture, replacementCubeTexture);
        }
    }

    SAFERELEASE(surface);

    if (replacementTexture || replacementCubeTexture)
    {
        if (desc->DxRenderTexture == desc->DxTexture)
            desc->DxRenderTexture = NULL;
        if (cubemap)
        {
            SAFERELEASE(desc->DxCubeTexture);
            desc->DxCubeTexture = replacementCubeTexture;
            replacementCubeTexture = NULL;
        }
        else
        {
            SAFERELEASE(desc->DxTexture);
            desc->DxTexture = replacementTexture;
            replacementTexture = NULL;
        }
        SAFERELEASE(desc->DxRenderTexture);
    }

    // Update texture properties
    D3DFormatToTextureDesc(targetSurfaceDesc.Format, desc);
    desc->Format.Width = targetSurfaceDesc.Width;
    desc->Format.Height = targetSurfaceDesc.Height;
    desc->Flags &= ~CKRST_TEXTURE_MANAGED;
    desc->Flags |= CKRST_TEXTURE_VALID | CKRST_TEXTURE_RENDERTARGET;
    if (cubemap)
        desc->Flags |= CKRST_TEXTURE_CUBEMAP;
    else
        desc->Flags &= ~CKRST_TEXTURE_CUBEMAP;

    m_CurrentTextureIndex = TextureObject;
    CKDX9_LOG("SetTargetTexture success texture=%u desc=%p dx=%p render=%p cube=%p surface=%p flags=0x%08X surfaceSuccess=%d",
              TextureObject, desc, desc->DxTexture, desc->DxRenderTexture,
              desc->DxCubeTexture, surface, desc->Flags, surfaceSuccess);
    return TRUE;
}

CKBOOL CKDX9RasterizerContext::DrawSprite(CKDWORD Sprite, VxRect *src, VxRect *dst)
{
#ifdef TRACY_ENABLE
    ZoneScopedN(__FUNCTION__);
#endif
    ++g_dx9Frame.drawSprites;
    if (!src || !dst || Sprite >= m_Sprites.Size())
        return FALSE;

    CKSpriteDesc *sprite = m_Sprites[Sprite];
    if (!sprite || sprite->Textures.IsEmpty())
        return FALSE;

    const bool spriteLog = (g_dx9Frame.frameIndex < 3) ||
                           (g_dx9Frame.frameIndex % 200 == 0 && g_dx9Frame.drawSprites <= 6);
    if (spriteLog)
    {
        CKDX9_LOG("DrawSprite[frame=%u,#%u] idx=%u src=(%.1f,%.1f %.1fx%.1f) dst=(%.1f,%.1f %.1fx%.1f) fmt=%ux%u bpp=%d textures=%d tex0idx=%d ctx=%ux%u",
                  g_dx9Frame.frameIndex, g_dx9Frame.drawSprites, Sprite,
                  src->left, src->top, src->right - src->left, src->bottom - src->top,
                  dst->left, dst->top, dst->right - dst->left, dst->bottom - dst->top,
                  sprite->Format.Width, sprite->Format.Height, sprite->Format.BitsPerPixel,
                  (int)sprite->Textures.Size(),
                  sprite->Textures.Size() > 0 ? (int)sprite->Textures[0].IndexTexture : -1,
                  m_Width, m_Height);
    }

    // Basic boundary checks with early exit
    if (src->GetWidth() <= 0.0f || src->GetHeight() <= 0.0f ||
        dst->GetWidth() <= 0.0f || dst->GetHeight() <= 0.0f ||
        src->right < 0.0f || src->bottom < 0.0f ||
        sprite->Format.Width <= src->left || sprite->Format.Height <= src->top ||
        dst->right < 0.0f || dst->bottom < 0.0f ||
        m_Width <= dst->left || m_Height <= dst->top)
    {
        if (spriteLog)
        {
            CKDX9_LOG("DrawSprite REJECTED: fmt=%ux%u ctx=%ux%u src=(%g %g %g %g) dst=(%g %g %g %g)",
                      sprite->Format.Width, sprite->Format.Height, m_Width, m_Height,
                      src->left, src->top, src->right, src->bottom,
                      dst->left, dst->top, dst->right, dst->bottom);
        }
        return FALSE;
    }

    // Begin scene if needed
    if (!m_SceneBegined && !BeginScene())
        return FALSE;

    // Sprite rendering touches render, sampler, texture-stage, shader, stream,
    // texture and viewport state. Capture the complete device state and the
    // matching software caches so every exit path can restore both sides.
    DX9SpriteState savedSpriteState;
    memcpy(savedSpriteState.stateCache, m_StateCache, sizeof(savedSpriteState.stateCache));
    memcpy(savedSpriteState.textureStageStateCache, m_TextureStageStateCache,
           sizeof(savedSpriteState.textureStageStateCache));
    memcpy(savedSpriteState.d3dSamplerStateCache, m_D3DSamplerStateCache,
           sizeof(savedSpriteState.d3dSamplerStateCache));
    memcpy(savedSpriteState.d3dTextureStageStateCache,
           m_D3DTextureStageStateCache,
           sizeof(savedSpriteState.d3dTextureStageStateCache));
    memcpy(savedSpriteState.textureBindingCache, m_TextureBindingCache,
           sizeof(savedSpriteState.textureBindingCache));
    memcpy(savedSpriteState.textureBindingCacheValid, m_TextureBindingCacheValid,
           sizeof(savedSpriteState.textureBindingCacheValid));
    savedSpriteState.inverseWinding = m_InverseWinding;
    savedSpriteState.vertexShaderCache = m_CurrentVertexShaderCache;
    savedSpriteState.vertexFormatCache = m_CurrentVertexFormatCache;
    savedSpriteState.vertexBufferCache = m_CurrentVertexBufferCache;
    savedSpriteState.vertexSizeCache = m_CurrentVertexSizeCache;
    savedSpriteState.pixelShaderCache = m_CurrentPixelShaderCache;
    savedSpriteState.stateBlock = NULL;
    savedSpriteState.texture0 = NULL;
    savedSpriteState.stream0 = NULL;
    savedSpriteState.streamOffset = 0;
    savedSpriteState.streamStride = 0;

    HRESULT stateHr = S_OK;
    if (m_SpriteStateBlock)
    {
        stateHr = m_SpriteStateBlock->Capture();
    }
    else
    {
        stateHr = m_Device->CreateStateBlock(D3DSBT_ALL, &m_SpriteStateBlock);
    }
    if (FAILED(stateHr) || !m_SpriteStateBlock)
    {
        SAFERELEASE(m_SpriteStateBlock);
        return FALSE;
    }
    savedSpriteState.stateBlock = m_SpriteStateBlock;
    savedSpriteState.stateBlock->AddRef();

    savedSpriteState.textureResult =
        m_Device->GetTexture(0, &savedSpriteState.texture0);
    savedSpriteState.streamResult =
        m_Device->GetStreamSource(0, &savedSpriteState.stream0,
                                  &savedSpriteState.streamOffset,
                                  &savedSpriteState.streamStride);

    // Save render states to restore later
    CKDWORD oldZEnable, oldZWriteEnable, oldLighting, oldCullMode, oldClipping, oldAlphaBlend, oldSrcBlend, oldDestBlend;
    GetRenderState(VXRENDERSTATE_ZENABLE, &oldZEnable);
    GetRenderState(VXRENDERSTATE_ZWRITEENABLE, &oldZWriteEnable);
    GetRenderState(VXRENDERSTATE_LIGHTING, &oldLighting);
    GetRenderState(VXRENDERSTATE_CULLMODE, &oldCullMode);
    GetRenderState(VXRENDERSTATE_CLIPPING, &oldClipping);
    GetRenderState(VXRENDERSTATE_ALPHABLENDENABLE, &oldAlphaBlend);
    GetRenderState(VXRENDERSTATE_SRCBLEND, &oldSrcBlend);
    GetRenderState(VXRENDERSTATE_DESTBLEND, &oldDestBlend);

    // Save original viewport
    D3DVIEWPORT9 oldViewport = {};
    if (FAILED(m_Device->GetViewport(&oldViewport)))
    {
        DX9RestoreSpriteState(this, savedSpriteState);
        return FALSE;
    }

    // Sprite vertices and texture combiners use the fixed-function pipeline.
    // Leaving an application shader bound would either reinterpret CKVertex
    // with the wrong declaration or bypass the texture-stage setup entirely.
    if (!SetVertexShader(0) || !SetPixelShader(0))
    {
        DX9RestoreSpriteState(this, savedSpriteState);
        return FALSE;
    }

    // Set sprite rendering states once.
    const CKBOOL renderStateReady =
        SetRenderState(VXRENDERSTATE_ZENABLE, FALSE) &&
        SetRenderState(VXRENDERSTATE_ZWRITEENABLE, FALSE) &&
        SetRenderState(VXRENDERSTATE_LIGHTING, FALSE) &&
        SetRenderState(VXRENDERSTATE_CULLMODE, VXCULL_NONE) &&
        SetRenderState(VXRENDERSTATE_CLIPPING, FALSE) &&
        SetRenderState(VXRENDERSTATE_TEXTUREPERSPECTIVE, FALSE) &&
        SetRenderState(VXRENDERSTATE_FILLMODE, VXFILL_SOLID) &&
        SetRenderState(VXRENDERSTATE_ALPHABLENDENABLE, TRUE) &&
        SetRenderState(VXRENDERSTATE_SRCBLEND, VXBLEND_SRCALPHA) &&
        SetRenderState(VXRENDERSTATE_DESTBLEND, VXBLEND_INVSRCALPHA);
    if (!renderStateReady)
    {
        DX9RestoreSpriteState(this, savedSpriteState);
        return FALSE;
    }

    // Set sprite viewport
    D3DVIEWPORT9 viewport = {0, 0, m_Width, m_Height, 0.0f, 1.0f};
    if (FAILED(m_Device->SetViewport(&viewport)))
    {
        DX9RestoreSpriteState(this, savedSpriteState);
        return FALSE;
    }

    // Set up texture stage states once
    const CKBOOL textureStateReady =
        SetTextureStageState(0, CKRST_TSS_TEXTUREMAPBLEND, VXTEXTUREBLEND_COPY) &&
        SetTextureStageState(0, CKRST_TSS_MINFILTER, VXTEXTUREFILTER_NEAREST) &&
        SetTextureStageState(0, CKRST_TSS_MAGFILTER, VXTEXTUREFILTER_NEAREST) &&
        SetTextureStageState(0, CKRST_TSS_ADDRESSU, VXTEXTURE_ADDRESSCLAMP) &&
        SetTextureStageState(0, CKRST_TSS_ADDRESSV, VXTEXTURE_ADDRESSCLAMP);
    if (!textureStateReady)
    {
        DX9RestoreSpriteState(this, savedSpriteState);
        return FALSE;
    }

    // Calculate scaling ratios
    const float widthRatio = dst->GetWidth() / src->GetWidth();
    const float heightRatio = dst->GetHeight() / src->GetHeight();
    if (spriteLog)
    {
        CKDX9_LOG("DrawSprite SCALE widthRatio=%.6f heightRatio=%.6f",
                  widthRatio, heightRatio);
    }

    // ----- BATCH RENDERING IMPLEMENTATION -----

    // Step 1: Define a structure to hold fragment data
    struct SpriteFragment
    {
        CKDWORD TextureId;
        float ScreenLeft, ScreenTop, ScreenRight, ScreenBottom;
        float TexU1, TexV1, TexU2, TexV2;
    };

    // Step 2: Pre-process all visible fragments to collect and sort them
    XArray<SpriteFragment> visibleFragments;

    // First pass: Collect visible fragments
    for (XArray<CKSPRTextInfo>::Iterator it = sprite->Textures.Begin(); it != sprite->Textures.End(); ++it)
    {
        const float tx = it->x;
        const float ty = it->y;
        const float tw = it->w;
        const float th = it->h;
        const float tr = tx + tw;
        const float tb = ty + th;

        // Skip textures completely outside view
        if (tx > src->right || ty > src->bottom || tr < src->left || tb < src->top)
            continue;

        // Create fragment for this texture piece
        SpriteFragment fragment;
        fragment.TextureId = it->IndexTexture;

        // Default full texture coordinates
        fragment.TexU1 = 0.0f;
        fragment.TexV1 = 0.0f;
        fragment.TexU2 = static_cast<float>(tw) / it->sw;
        fragment.TexV2 = static_cast<float>(th) / it->sh;

        // Default screen coordinates
        fragment.ScreenLeft = (tx - src->left) * widthRatio + dst->left;
        fragment.ScreenTop = (ty - src->top) * heightRatio + dst->top;
        fragment.ScreenRight = (tr - src->left) * widthRatio + dst->left;
        fragment.ScreenBottom = (tb - src->top) * heightRatio + dst->top;

        // Handle clipping on right/bottom
        if (src->right < tr)
        {
            const float clippedWidth = src->right - tx;
            fragment.ScreenRight = (tx + clippedWidth - src->left) * widthRatio + dst->left;
            fragment.TexU2 = clippedWidth / it->sw;
        }

        if (src->bottom < tb)
        {
            const float clippedHeight = src->bottom - ty;
            fragment.ScreenBottom = (ty + clippedHeight - src->top) * heightRatio + dst->top;
            fragment.TexV2 = clippedHeight / it->sh;
        }

        // Handle clipping on left/top
        if (src->left > tx)
        {
            const float clippedLeft = src->left - tx;
            fragment.TexU1 = clippedLeft / it->sw;
            fragment.ScreenLeft = dst->left;
        }

        if (src->top > ty)
        {
            const float clippedTop = src->top - ty;
            fragment.TexV1 = clippedTop / it->sh;
            fragment.ScreenTop = dst->top;
        }

        if (spriteLog && visibleFragments.Size() < 8)
        {
            CKDX9_LOG("DrawSprite SUB idx=%u tex=%u pos=(%d,%d) wh=%dx%d storage=%dx%d screen=(%.3f,%.3f %.3fx%.3f) uv=(%.6f,%.6f %.6fx%.6f)",
                      Sprite, it->IndexTexture,
                      it->x, it->y, it->w, it->h, it->sw, it->sh,
                      fragment.ScreenLeft, fragment.ScreenTop,
                      fragment.ScreenRight - fragment.ScreenLeft,
                      fragment.ScreenBottom - fragment.ScreenTop,
                      fragment.TexU1, fragment.TexV1,
                      fragment.TexU2 - fragment.TexU1,
                      fragment.TexV2 - fragment.TexV1);
        }

        // Add to visible fragments list
        visibleFragments.PushBack(fragment);
    }

    // Return if nothing to render
    if (visibleFragments.IsEmpty())
    {
        m_Device->SetViewport(&oldViewport);
        SetRenderState(VXRENDERSTATE_ZENABLE, oldZEnable);
        SetRenderState(VXRENDERSTATE_ZWRITEENABLE, oldZWriteEnable);
        SetRenderState(VXRENDERSTATE_LIGHTING, oldLighting);
        SetRenderState(VXRENDERSTATE_CULLMODE, oldCullMode);
        SetRenderState(VXRENDERSTATE_CLIPPING, oldClipping);
        SetRenderState(VXRENDERSTATE_ALPHABLENDENABLE, oldAlphaBlend);
        SetRenderState(VXRENDERSTATE_SRCBLEND, oldSrcBlend);
        SetRenderState(VXRENDERSTATE_DESTBLEND, oldDestBlend);
        return DX9RestoreSpriteState(this, savedSpriteState);
    }

    // Step 3: Sort fragments by texture ID to minimize texture switches
    // Simple insertion sort (efficient for small arrays)
    for (int i = 1; i < visibleFragments.Size(); ++i)
    {
        SpriteFragment key = visibleFragments[i];
        int j = i - 1;

        while (j >= 0 && visibleFragments[j].TextureId > key.TextureId)
        {
            visibleFragments[j + 1] = visibleFragments[j];
            --j;
        }

        visibleFragments[j + 1] = key;
    }

    // Step 4: Allocate a vertex buffer for the entire sprite
    if (visibleFragments.Size() > INT_MAX / 4)
    {
        DX9RestoreSpriteState(this, savedSpriteState);
        return FALSE;
    }
    const CKDWORD totalVertices = static_cast<CKDWORD>(visibleFragments.Size()) * 4u;

    CKDWORD vertexBufferIndex = GetDynamicVertexBuffer(CKRST_VF_TLVERTEX, totalVertices, sizeof(CKVertex), 1);
    CKDX9VertexBufferDesc *vb = (vertexBufferIndex < m_VertexBuffers.Size())
                                    ? static_cast<CKDX9VertexBufferDesc *>(m_VertexBuffers[vertexBufferIndex])
                                    : NULL;
    if (!vb || !vb->DxBuffer)
    {
        // Clean up and return failure
        m_Device->SetViewport(&oldViewport);
        SetRenderState(VXRENDERSTATE_ZENABLE, oldZEnable);
        SetRenderState(VXRENDERSTATE_ZWRITEENABLE, oldZWriteEnable);
        SetRenderState(VXRENDERSTATE_LIGHTING, oldLighting);
        SetRenderState(VXRENDERSTATE_CULLMODE, oldCullMode);
        SetRenderState(VXRENDERSTATE_CLIPPING, oldClipping);
        SetRenderState(VXRENDERSTATE_ALPHABLENDENABLE, oldAlphaBlend);
        SetRenderState(VXRENDERSTATE_SRCBLEND, oldSrcBlend);
        SetRenderState(VXRENDERSTATE_DESTBLEND, oldDestBlend);
        DX9RestoreSpriteState(this, savedSpriteState);
        return FALSE;
    }

    // Step 5: Lock vertex buffer and fill with quad data
    void *pVertices = NULL;
    CKDWORD startVertex = 0;
    HRESULT hr;

    // Try to append to existing buffer if there's room
    if (vb->m_CurrentVCount <= vb->m_MaxVertexCount &&
        totalVertices <= vb->m_MaxVertexCount - vb->m_CurrentVCount)
    {
        hr = vb->DxBuffer->Lock(
            sizeof(CKVertex) * vb->m_CurrentVCount,
            sizeof(CKVertex) * totalVertices,
            &pVertices,
            D3DLOCK_NOOVERWRITE
        );

        if (SUCCEEDED(hr) && pVertices)
        {
            startVertex = vb->m_CurrentVCount;
            vb->m_CurrentVCount += totalVertices;
        }
    }

    // If append failed, discard and start fresh
    if (!pVertices)
    {
        hr = vb->DxBuffer->Lock(
            0,
            sizeof(CKVertex) * totalVertices,
            &pVertices,
            D3DLOCK_DISCARD
        );
        if (SUCCEEDED(hr) && pVertices)
        {
            startVertex = 0;
            vb->m_CurrentVCount = totalVertices;
        }
    }

    // If all lock attempts failed, clean up and return
    if (!pVertices)
    {
        m_Device->SetViewport(&oldViewport);
        SetRenderState(VXRENDERSTATE_ZENABLE, oldZEnable);
        SetRenderState(VXRENDERSTATE_ZWRITEENABLE, oldZWriteEnable);
        SetRenderState(VXRENDERSTATE_LIGHTING, oldLighting);
        SetRenderState(VXRENDERSTATE_CULLMODE, oldCullMode);
        SetRenderState(VXRENDERSTATE_CLIPPING, oldClipping);
        SetRenderState(VXRENDERSTATE_ALPHABLENDENABLE, oldAlphaBlend);
        SetRenderState(VXRENDERSTATE_SRCBLEND, oldSrcBlend);
        SetRenderState(VXRENDERSTATE_DESTBLEND, oldDestBlend);
        DX9RestoreSpriteState(this, savedSpriteState);
        return FALSE;
    }

    // Fill vertex buffer with all quads
    CKVertex *vertices = static_cast<CKVertex *>(pVertices);
    const CKDWORD diffuseColor = (R_MASK | G_MASK | B_MASK | A_MASK);

    for (int i = 0; i < visibleFragments.Size(); ++i)
    {
        const SpriteFragment &fragment = visibleFragments[i];
        CKVertex *quad = &vertices[i * 4];

        // Set common vertex attributes
        for (int v = 0; v < 4; ++v)
        {
            quad[v].Diffuse = diffuseColor;
            quad[v].Specular = A_MASK;
        }

        // Top-left vertex
        quad[0].V = VxVector4(fragment.ScreenLeft, fragment.ScreenTop, 0.0f, 1.0f);
        quad[0].tu = fragment.TexU1;
        quad[0].tv = fragment.TexV1;

        // Bottom-left vertex
        quad[1].V = VxVector4(fragment.ScreenLeft, fragment.ScreenBottom, 0.0f, 1.0f);
        quad[1].tu = fragment.TexU1;
        quad[1].tv = fragment.TexV2;

        // Bottom-right vertex
        quad[2].V = VxVector4(fragment.ScreenRight, fragment.ScreenBottom, 0.0f, 1.0f);
        quad[2].tu = fragment.TexU2;
        quad[2].tv = fragment.TexV2;

        // Top-right vertex
        quad[3].V = VxVector4(fragment.ScreenRight, fragment.ScreenTop, 0.0f, 1.0f);
        quad[3].tu = fragment.TexU2;
        quad[3].tv = fragment.TexV1;
    }

    // Unlock the vertex buffer
    if (FAILED(vb->DxBuffer->Unlock()))
    {
        DX9RestoreSpriteState(this, savedSpriteState);
        return FALSE;
    }

    // Step 6: Setup vertex streams
    if (!SetupStreams(vb->DxBuffer, CKRST_VF_TLVERTEX, 0, sizeof(CKVertex)))
    {
        DX9RestoreSpriteState(this, savedSpriteState);
        return FALSE;
    }

    // Step a7: Render batches (groups of fragments with same texture)
    CKDWORD currentTextureId = 0xFFFFFFFF;
    int batchStart = 0;
    CKBOOL drawSucceeded = TRUE;

    // Process all fragments, detecting texture changes
    for (int i = 0; i <= visibleFragments.Size(); ++i)
    {
        // Process a batch when texture changes or at the end of all fragments
        if (i == visibleFragments.Size() || visibleFragments[i].TextureId != currentTextureId)
        {
            // Render current batch if it exists
            if (i > batchStart)
            {
                if (!SetTexture(currentTextureId, 0))
                {
                    drawSucceeded = FALSE;
                }
                else
                {
                    for (int j = batchStart; j < i; ++j)
                    {
                        if (FAILED(m_Device->DrawPrimitive(D3DPT_TRIANGLEFAN, startVertex + (j * 4), 2)))
                        {
                            drawSucceeded = FALSE;
                            break;
                        }
                    }
                }
            }

            if (!drawSucceeded)
                break;

            // Start new batch if not at the end
            if (i < visibleFragments.Size())
            {
                currentTextureId = visibleFragments[i].TextureId;
                batchStart = i;
            }
        }
    }

    // Step 8: Cleanup and restore state
    m_Device->SetTexture(0, NULL);
    m_Device->SetStreamSource(0, NULL, 0, 0);
    m_Device->SetViewport(&oldViewport);

    // Restore render states
    SetRenderState(VXRENDERSTATE_ZENABLE, oldZEnable);
    SetRenderState(VXRENDERSTATE_ZWRITEENABLE, oldZWriteEnable);
    SetRenderState(VXRENDERSTATE_LIGHTING, oldLighting);
    SetRenderState(VXRENDERSTATE_CULLMODE, oldCullMode);
    SetRenderState(VXRENDERSTATE_CLIPPING, oldClipping);
    SetRenderState(VXRENDERSTATE_ALPHABLENDENABLE, oldAlphaBlend);
    SetRenderState(VXRENDERSTATE_SRCBLEND, oldSrcBlend);
    SetRenderState(VXRENDERSTATE_DESTBLEND, oldDestBlend);

    const CKBOOL restoreSucceeded = DX9RestoreSpriteState(this, savedSpriteState);
    return drawSucceeded && restoreSucceeded;
}

void *CKDX9RasterizerContext::LockVertexBuffer(CKDWORD VB, CKDWORD StartVertex, CKDWORD VertexCount, CKRST_LOCKFLAGS Lock)
{
#ifdef TRACY_ENABLE
    ZoneScopedN(__FUNCTION__);
#endif

    if (VB >= m_VertexBuffers.Size())
        return NULL;

    CKDX9VertexBufferDesc *vb = static_cast<CKDX9VertexBufferDesc *>(m_VertexBuffers[VB]);
    if (!vb || !vb->DxBuffer)
        return NULL;

    if (StartVertex >= vb->m_MaxVertexCount || vb->m_VertexSize == 0)
        return NULL;

    const CKDWORD remainingVertices = vb->m_MaxVertexCount - StartVertex;
    if (VertexCount == 0 || VertexCount > remainingVertices)
        VertexCount = remainingVertices;

    if (StartVertex > 0xFFFFFFFFu / vb->m_VertexSize ||
        VertexCount > 0xFFFFFFFFu / vb->m_VertexSize)
    {
        return NULL;
    }

    void *pVertices = NULL;
    if (FAILED(vb->DxBuffer->Lock(StartVertex * vb->m_VertexSize, VertexCount * vb->m_VertexSize, &pVertices, Lock << 12)))
        return NULL;

    return pVertices;
}

CKBOOL CKDX9RasterizerContext::UnlockVertexBuffer(CKDWORD VB)
{
#ifdef TRACY_ENABLE
    ZoneScopedN(__FUNCTION__);
#endif
    if (VB >= m_VertexBuffers.Size())
        return FALSE;

    CKDX9VertexBufferDesc *vb = static_cast<CKDX9VertexBufferDesc *>(m_VertexBuffers[VB]);
    if (!vb || !vb->DxBuffer)
        return FALSE;

    return SUCCEEDED(vb->DxBuffer->Unlock());
}

int CKDX9RasterizerContext::CopyToMemoryBuffer(CKRECT *rect, VXBUFFER_TYPE buffer, VxImageDescEx &img_desc)
{
#ifdef TRACY_ENABLE
    ZoneScopedN(__FUNCTION__);
#endif
    CKDX9_LOG("CopyToMemoryBuffer begin frame=%u buffer=%s rect=%s",
              g_dx9Frame.frameIndex, DX9BufferTypeName(buffer), rect ? "set" : "full");
    if (!m_Device)
        return 0;

    D3DSURFACE_DESC desc = {};
    IDirect3DSurface9 *surface = NULL;
    int depthBytesPerPixel = 0;
    HRESULT hr;
    XBYTE *outputImage = img_desc.Image;

    // Step 1: Get the appropriate surface based on buffer type
    switch (buffer)
    {
        case VXBUFFER_BACKBUFFER:
            {
                hr = m_Device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &surface);
                if (FAILED(hr) || !surface)
                {
                    CKDX9_LOG("CopyToMemoryBuffer GetBackBuffer failed hr=0x%08lX surface=%p", (unsigned long)hr, surface);
                    return 0;
                }

                hr = surface->GetDesc(&desc);
                if (FAILED(hr))
                {
                    SAFERELEASE(surface);
                    return 0;
                }

                VX_PIXELFORMAT vxpf = D3DFormatToVxPixelFormat(desc.Format);
                VxPixelFormat2ImageDesc(vxpf, img_desc);
                // Format conversion describes the surface; the destination
                // storage remains owned and supplied by the caller.
                img_desc.Image = outputImage;
                break;
            }
        case VXBUFFER_ZBUFFER:
            {
                hr = m_Device->GetDepthStencilSurface(&surface);
                if (FAILED(hr) || !surface)
                {
                    CKDX9_LOG("CopyToMemoryBuffer GetDepthStencilSurface failed buffer=%s hr=0x%08lX surface=%p",
                              DX9BufferTypeName(buffer), (unsigned long)hr, surface);
                    return 0;
                }

                hr = surface->GetDesc(&desc);
                if (FAILED(hr))
                {
                    SAFERELEASE(surface);
                    return 0;
                }

                img_desc.BitsPerPixel = 32;
                img_desc.AlphaMask = 0;
                img_desc.BlueMask = B_MASK;
                img_desc.GreenMask = G_MASK;
                img_desc.RedMask = R_MASK;

                switch (desc.Format)
                {
                    case D3DFMT_D16_LOCKABLE:
                    case D3DFMT_D15S1:
                    case D3DFMT_D16:
                        depthBytesPerPixel = 2;
                        break;
                    case D3DFMT_D32:
                    case D3DFMT_D24S8:
                    case D3DFMT_D24X8:
                    case D3DFMT_D24X4S4:
                        depthBytesPerPixel = 4;
                        break;
                    default:
                        depthBytesPerPixel = 0;
                        break;
                }
                break; // Add missing break to prevent fallthrough
            }
        case VXBUFFER_STENCILBUFFER:
            {
                hr = m_Device->GetDepthStencilSurface(&surface);
                if (FAILED(hr) || !surface)
                {
                    CKDX9_LOG("CopyToMemoryBuffer GetDepthStencilSurface failed buffer=%s hr=0x%08lX surface=%p",
                              DX9BufferTypeName(buffer), (unsigned long)hr, surface);
                    return 0;
                }

                hr = surface->GetDesc(&desc);
                if (FAILED(hr))
                {
                    SAFERELEASE(surface);
                    return 0;
                }

                D3DFORMAT d3dFormat = desc.Format;
                img_desc.BitsPerPixel = 32;
                img_desc.AlphaMask = 0;
                img_desc.BlueMask = B_MASK;
                img_desc.GreenMask = G_MASK;
                img_desc.RedMask = R_MASK;

                if (d3dFormat == D3DFMT_D15S1)
                {
                    depthBytesPerPixel = 2;
                }
                else if (d3dFormat == D3DFMT_D24S8 || d3dFormat == D3DFMT_D24X4S4)
                {
                    depthBytesPerPixel = 4;
                }
                else
                {
                    // Unsupported stencil format
                    SAFERELEASE(surface);
                    return 0;
                }
                break;
            }
        default:
            return 0;
    }

    // Step 2: Calculate the boundaries of the region to copy
    int right, left, top, bottom;
    if (rect)
    {
        // Clamp rectangle to surface boundaries
        const int surfaceWidth = static_cast<int>(desc.Width);
        const int surfaceHeight = static_cast<int>(desc.Height);
        left = XMax(0, XMin(surfaceWidth, rect->left));
        right = XMax(0, XMin(surfaceWidth, rect->right));
        top = XMax(0, XMin(surfaceHeight, rect->top));
        bottom = XMax(0, XMin(surfaceHeight, rect->bottom));
    }
    else
    {
        // Use full surface if no rectangle specified
        top = 0;
        bottom = desc.Height;
        left = 0;
        right = desc.Width;
    }

    // Validate rectangle dimensions
    if (left >= right || top >= bottom)
    {
        SAFERELEASE(surface);
        return 0;
    }

    // Step 3: Set up the image description
    const int width = right - left;
    const int height = bottom - top;
    img_desc.Width = width;
    img_desc.Height = height;
    int bytesPerPixel = img_desc.BitsPerPixel / 8;
    if (bytesPerPixel <= 0 || width > INT_MAX / bytesPerPixel)
    {
        SAFERELEASE(surface);
        return 0;
    }
    img_desc.BytesPerLine = width * bytesPerPixel;

    // Validate the resulting format before reporting its required storage.
    if (img_desc.BytesPerLine == 0)
    {
        SAFERELEASE(surface);
        return 0;
    }

    if (height > INT_MAX / img_desc.BytesPerLine)
    {
        SAFERELEASE(surface);
        return 0;
    }

    const int requiredSize = img_desc.BytesPerLine * height;
    RECT srcRect = {(LONG)left, (LONG)top, (LONG)right, (LONG)bottom};

    // DumpToMemory is a two-call API: a null image requests the output
    // description and the number of bytes that the caller must allocate.
    if (!img_desc.Image)
    {
        // Standard D3D9 depth/stencil surfaces are usually not CPU-readable.
        // Only report a size when the exact surface/rectangle can actually be
        // locked; otherwise the advertised two-call operation could never finish.
        if (buffer != VXBUFFER_BACKBUFFER)
        {
            D3DLOCKED_RECT probe = {};
            hr = surface->LockRect(&probe, &srcRect, D3DLOCK_READONLY);
            if (FAILED(hr))
            {
                SAFERELEASE(surface);
                return 0;
            }
            surface->UnlockRect();
        }

        SAFERELEASE(surface);
        CKDX9_LOG("CopyToMemoryBuffer size query buffer=%s bytes=%d desc=%ux%u bpp=%d",
                  DX9BufferTypeName(buffer), requiredSize,
                  img_desc.Width, img_desc.Height, img_desc.BitsPerPixel);
        return requiredSize;
    }

    // Step 4: Copy the data
    int totalBytes = 0;
    IDirect3DSurface9 *tempSurface = NULL;
    IDirect3DSurface9 *resolveSurface = NULL;
    IDirect3DSurface9 *readbackSource = surface;

    // Try to create a temporary surface for the copy operation
    switch (buffer)
    {
        case VXBUFFER_BACKBUFFER:
            hr = m_Device->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format, D3DPOOL_SYSTEMMEM, &tempSurface, NULL);
            CKDX9_LOG("CopyToMemoryBuffer CreateOffscreenPlainSurface %ux%u fmt=0x%08X pool=SYSTEMMEM hr=0x%08lX temp=%p",
                      desc.Width, desc.Height, (unsigned)desc.Format, (unsigned long)hr, tempSurface);
            if (SUCCEEDED(hr) && desc.MultiSampleType != D3DMULTISAMPLE_NONE)
            {
                hr = m_Device->CreateRenderTarget(desc.Width, desc.Height, desc.Format,
                                                   D3DMULTISAMPLE_NONE, 0, FALSE,
                                                   &resolveSurface, NULL);
                if (SUCCEEDED(hr))
                    hr = m_Device->StretchRect(surface, NULL, resolveSurface, NULL, D3DTEXF_NONE);
                if (SUCCEEDED(hr))
                    readbackSource = resolveSurface;
                CKDX9_LOG("CopyToMemoryBuffer resolve msaa=%u surface=%p hr=0x%08lX",
                          (unsigned)desc.MultiSampleType, resolveSurface, (unsigned long)hr);
            }
            break;
        default:
            hr = E_FAIL;
            CKDX9_LOG("CopyToMemoryBuffer unsupported direct readback buffer=%s fmt=0x%08X",
                      DX9BufferTypeName(buffer), (unsigned)desc.Format);
            break;
    }

    if (SUCCEEDED(hr) && tempSurface)
    {
        hr = m_Device->GetRenderTargetData(readbackSource, tempSurface);
        CKDX9_LOG("CopyToMemoryBuffer GetRenderTargetData source=%p temp=%p hr=0x%08lX",
                  readbackSource, tempSurface, (unsigned long)hr);
        if (SUCCEEDED(hr))
        {
            D3DLOCKED_RECT lockedRect;
            hr = tempSurface->LockRect(&lockedRect, &srcRect, D3DLOCK_READONLY);
            CKDX9_LOG("CopyToMemoryBuffer temp LockRect hr=0x%08lX pitch=%ld bits=%p",
                      (unsigned long)hr, SUCCEEDED(hr) ? (long)lockedRect.Pitch : 0L,
                      SUCCEEDED(hr) ? lockedRect.pBits : NULL);
            if (SUCCEEDED(hr))
            {
                BYTE *srcData = (BYTE *)lockedRect.pBits;
                BYTE *destData = img_desc.Image;

                if (depthBytesPerPixel == 2 && bytesPerPixel == 4)
                {
                    // Convert 16-bit depth/stencil to 32-bit RGB
                    for (UINT row = 0; row < height; ++row)
                    {
                        BYTE *srcRow = srcData + (row * lockedRect.Pitch);
                        BYTE *destRow = destData + (row * img_desc.BytesPerLine);

                        for (UINT col = 0; col < width; ++col)
                        {
                            // Read 16-bit depth value
                            WORD depthValue = *((WORD *)(srcRow + col * 2));

                            // Write as 32-bit RGB value (scale to visible range)
                            DWORD *destPixel = (DWORD *)(destRow + col * 4);
                            BYTE intensity = (BYTE)((depthValue * 255) / 65535);
                            *destPixel = (intensity << 16) | (intensity << 8) | intensity;
                        }
                    }
                }
                else
                {
                    // Standard copy for normal color buffers or 32-bit depth/stencil
                    for (UINT row = 0; row < height; ++row)
                    {
                        memcpy(destData + (row * img_desc.BytesPerLine), srcData + (row * lockedRect.Pitch), width * bytesPerPixel);
                    }
                }

                tempSurface->UnlockRect();
                totalBytes = img_desc.BytesPerLine * height;
            }
        }
    }
    else
    {
        // Fallback: try to lock the original surface directly
        D3DLOCKED_RECT lockedRect;
        hr = surface->LockRect(&lockedRect, &srcRect, D3DLOCK_READONLY);
        CKDX9_LOG("CopyToMemoryBuffer fallback LockRect surface=%p rect=(%ld,%ld,%ld,%ld) hr=0x%08lX",
                  surface, srcRect.left, srcRect.top, srcRect.right, srcRect.bottom, (unsigned long)hr);
        if (SUCCEEDED(hr))
        {
            BYTE *srcData = (BYTE *)lockedRect.pBits;
            BYTE *destData = img_desc.Image;

            if (depthBytesPerPixel == 2 && bytesPerPixel == 4)
            {
                // Convert 16-bit depth/stencil to 32-bit RGB
                for (UINT row = 0; row < height; ++row)
                {
                    BYTE *srcRow = srcData + (row * lockedRect.Pitch);
                    BYTE *destRow = destData + (row * img_desc.BytesPerLine);

                    for (UINT col = 0; col < width; ++col)
                    {
                        // Read 16-bit depth value
                        WORD depthValue = *((WORD *)(srcRow + col * 2));

                        // Write as 32-bit RGB value (scale to visible range)
                        DWORD *destPixel = (DWORD *)(destRow + col * 4);
                        BYTE intensity = (BYTE)((depthValue * 255) / 65535);
                        *destPixel = (intensity << 16) | (intensity << 8) | intensity;
                    }
                }
            }
            else
            {
                // Standard copy for normal color buffers or 32-bit depth/stencil
                for (UINT row = 0; row < height; ++row)
                {
                    memcpy(destData + (row * img_desc.BytesPerLine), srcData + (row * lockedRect.Pitch), width * bytesPerPixel);
                }
            }

            surface->UnlockRect();
            totalBytes = img_desc.BytesPerLine * height;
        }
    }

    SAFERELEASE(resolveSurface);
    SAFERELEASE(tempSurface);

    // Cleanup
    SAFERELEASE(surface);
    CKDX9_LOG("CopyToMemoryBuffer end buffer=%s bytes=%d desc=%ux%u bpp=%d",
              DX9BufferTypeName(buffer), totalBytes, img_desc.Width, img_desc.Height, img_desc.BitsPerPixel);
    return totalBytes;
}

int CKDX9RasterizerContext::CopyFromMemoryBuffer(CKRECT *rect, VXBUFFER_TYPE buffer, const VxImageDescEx &img_desc)
{
#ifdef TRACY_ENABLE
    ZoneScopedN(__FUNCTION__);
#endif
    CKDX9_LOG("CopyFromMemoryBuffer begin frame=%u buffer=%s rect=%s src=%ux%u bpp=%d bpl=%d image=%p",
              g_dx9Frame.frameIndex, DX9BufferTypeName(buffer), rect ? "set" : "full",
              img_desc.Width, img_desc.Height, img_desc.BitsPerPixel, img_desc.BytesPerLine, img_desc.Image);
    // Input validation
    if (!m_Device || !img_desc.Image)
    {
        CKDX9_LOG("CopyFromMemoryBuffer rejected: null image");
        return 0;
    }

    // Standard D3D9 depth surfaces are not writable from system memory. The
    // lockable D16 format is the one portable exception; support it using the
    // same 32-bit grayscale descriptor exposed by DumpToMemory.
    if ((static_cast<CKDWORD>(buffer) & static_cast<CKDWORD>(VXBUFFER_ZBUFFER)) != 0)
    {
        IDirect3DSurface9 *depthSurface = NULL;
        HRESULT depthHr = m_Device->GetDepthStencilSurface(&depthSurface);
        if (FAILED(depthHr) || !depthSurface)
            return 0;

        D3DSURFACE_DESC depthDesc = {};
        depthHr = depthSurface->GetDesc(&depthDesc);
        if (FAILED(depthHr) || depthDesc.Format != D3DFMT_D16_LOCKABLE)
        {
            SAFERELEASE(depthSurface);
            return 0;
        }

        RECT depthRect = {0, 0, (LONG)depthDesc.Width, (LONG)depthDesc.Height};
        if (rect)
        {
            if (rect->left < 0 || rect->top < 0 ||
                rect->right > (int)depthDesc.Width || rect->bottom > (int)depthDesc.Height ||
                rect->left >= rect->right || rect->top >= rect->bottom)
            {
                SAFERELEASE(depthSurface);
                return 0;
            }
            SetRect(&depthRect, rect->left, rect->top, rect->right, rect->bottom);
        }

        const int width = depthRect.right - depthRect.left;
        const int height = depthRect.bottom - depthRect.top;
        if (width > INT_MAX / 4 ||
            img_desc.Width != (CKDWORD)width || img_desc.Height != (CKDWORD)height ||
            img_desc.BitsPerPixel != 32 || img_desc.BytesPerLine < width * 4 ||
            img_desc.AlphaMask != 0 || img_desc.RedMask != R_MASK ||
            img_desc.GreenMask != G_MASK || img_desc.BlueMask != B_MASK ||
            height > INT_MAX / img_desc.BytesPerLine)
        {
            SAFERELEASE(depthSurface);
            return 0;
        }

        D3DLOCKED_RECT lockedDepth = {};
        depthHr = depthSurface->LockRect(&lockedDepth, &depthRect, 0);
        if (FAILED(depthHr) || lockedDepth.Pitch < width * (int)sizeof(WORD))
        {
            SAFERELEASE(depthSurface);
            return 0;
        }

        for (int y = 0; y < height; ++y)
        {
            const CKBYTE *sourceRow = img_desc.Image + y * img_desc.BytesPerLine;
            WORD *destinationRow = reinterpret_cast<WORD *>(static_cast<CKBYTE *>(lockedDepth.pBits) + y * lockedDepth.Pitch);
            for (int x = 0; x < width; ++x)
            {
                CKDWORD pixel = 0;
                memcpy(&pixel, sourceRow + x * 4, sizeof(pixel));
                const CKDWORD intensity = (((pixel & R_MASK) >> 16) +
                                           ((pixel & G_MASK) >> 8) +
                                           (pixel & B_MASK)) / 3;
                destinationRow[x] = static_cast<WORD>(intensity * 257);
            }
        }

        depthHr = depthSurface->UnlockRect();
        SAFERELEASE(depthSurface);
        return SUCCEEDED(depthHr) ? img_desc.BytesPerLine * height : 0;
    }

    if (buffer != VXBUFFER_BACKBUFFER)
    {
        CKDX9_LOG("CopyFromMemoryBuffer rejected: unsupported buffer=%s", DX9BufferTypeName(buffer));
        return 0;
    }

    // Get back buffer
    IDirect3DSurface9 *backBuffer = NULL;
    HRESULT hr = m_Device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer);
    if (FAILED(hr) || !backBuffer)
    {
        CKDX9_LOG("CopyFromMemoryBuffer GetBackBuffer failed hr=0x%08lX back=%p", (unsigned long)hr, backBuffer);
        return 0;
    }

    // Get back buffer description
    D3DSURFACE_DESC desc = {};
    hr = backBuffer->GetDesc(&desc);
    if (FAILED(hr))
    {
        SAFERELEASE(backBuffer);
        return 0;
    }

    // Get back buffer format information
    VxImageDescEx backBufferDesc;
    VX_PIXELFORMAT vxpf = D3DFormatToVxPixelFormat(desc.Format);
    VxPixelFormat2ImageDesc(vxpf, backBufferDesc);

    // Calculate destination rectangle. CopyToVideo requires the requested
    // rectangle to be fully inside the target; silently clipping would change
    // the required source dimensions.
    int left = 0, top = 0, right = 0, bottom = 0;
    if (rect)
    {
        if (rect->left < 0 || rect->top < 0 ||
            rect->right > (int)desc.Width || rect->bottom > (int)desc.Height)
        {
            SAFERELEASE(backBuffer);
            return 0;
        }
        left = rect->left;
        top = rect->top;
        right = rect->right;
        bottom = rect->bottom;
    }
    else
    {
        // Use full backbuffer size
        right = desc.Width;
        bottom = desc.Height;
    }

    // Validate resulting rectangle
    int destWidth = right - left;
    int destHeight = bottom - top;
    if (destWidth <= 0 || destHeight <= 0)
    {
        SAFERELEASE(backBuffer);
        return 0;
    }

    // CKRenderContext::CopyToVideo requires both dimensions and the complete
    // pixel layout to match the destination buffer.
    int bytesPerPixel = img_desc.BitsPerPixel / 8;
    if (img_desc.Width != (CKDWORD)destWidth || img_desc.Height != (CKDWORD)destHeight ||
        bytesPerPixel <= 0 || img_desc.BytesPerLine < destWidth * bytesPerPixel ||
        img_desc.BitsPerPixel != backBufferDesc.BitsPerPixel ||
        img_desc.AlphaMask != backBufferDesc.AlphaMask ||
        img_desc.RedMask != backBufferDesc.RedMask ||
        img_desc.GreenMask != backBufferDesc.GreenMask ||
        img_desc.BlueMask != backBufferDesc.BlueMask)
    {
        SAFERELEASE(backBuffer);
        return 0;
    }

    // Create temporary surface matching source image size
    IDirect3DSurface9 *tempSurface = NULL;
    hr = m_Device->CreateOffscreenPlainSurface(
        img_desc.Width,
        img_desc.Height,
        desc.Format,
        D3DPOOL_SYSTEMMEM,
        &tempSurface,
        NULL
    );
    CKDX9_LOG("CopyFromMemoryBuffer CreateOffscreenPlainSurface %ux%u fmt=0x%08X pool=SYSTEMMEM hr=0x%08lX temp=%p",
              img_desc.Width, img_desc.Height, (unsigned)desc.Format, (unsigned long)hr, tempSurface);
    if (FAILED(hr) || !tempSurface)
    {
        SAFERELEASE(backBuffer);
        return 0;
    }

    // Lock the temporary surface for writing
    D3DLOCKED_RECT lockedRect;
    hr = tempSurface->LockRect(&lockedRect, NULL, 0);
    CKDX9_LOG("CopyFromMemoryBuffer temp LockRect hr=0x%08lX pitch=%ld bits=%p",
              (unsigned long)hr, SUCCEEDED(hr) ? (long)lockedRect.Pitch : 0L,
              SUCCEEDED(hr) ? lockedRect.pBits : NULL);
    if (FAILED(hr))
    {
        SAFERELEASE(tempSurface);
        SAFERELEASE(backBuffer);
        return 0;
    }

    // Copy the image data to the temporary surface
    BYTE *destData = static_cast<BYTE *>(lockedRect.pBits);
    BYTE *srcData = img_desc.Image;

    const UINT rowSize = static_cast<UINT>(destWidth * bytesPerPixel);
    if (lockedRect.Pitch < (int)rowSize)
    {
        tempSurface->UnlockRect();
        SAFERELEASE(tempSurface);
        SAFERELEASE(backBuffer);
        return 0;
    }

    // Copy each row
    for (UINT y = 0; y < img_desc.Height; ++y)
    {
        memcpy(destData, srcData, rowSize);
        destData += lockedRect.Pitch;
        srcData += img_desc.BytesPerLine;
    }

    // Unlock the surface
    hr = tempSurface->UnlockRect();
    if (FAILED(hr))
    {
        SAFERELEASE(tempSurface);
        SAFERELEASE(backBuffer);
        return 0;
    }

    // Define source and destination rectangles for copying to back buffer
    RECT srcRect = {
        0,
        0,
        static_cast<LONG>(img_desc.Width),
        static_cast<LONG>(img_desc.Height),
    };
    POINT destPoint = {left, top};

    // Copy from temporary surface to back buffer
    // Note: UpdateSurface requires: source surface, source rect, destination surface, destination point
    hr = m_Device->UpdateSurface(tempSurface, &srcRect, backBuffer, &destPoint);
    CKDX9_LOG("CopyFromMemoryBuffer UpdateSurface temp=%p back=%p src=(%ld,%ld,%ld,%ld) dest=(%ld,%ld) hr=0x%08lX",
              tempSurface, backBuffer, srcRect.left, srcRect.top, srcRect.right, srcRect.bottom,
              destPoint.x, destPoint.y, (unsigned long)hr);

    // Clean up
    SAFERELEASE(tempSurface);
    SAFERELEASE(backBuffer);

    // Return bytes processed if successful, otherwise 0
    if (SUCCEEDED(hr))
    {
        CKDX9_LOG("CopyFromMemoryBuffer success bytes=%u", img_desc.BytesPerLine * img_desc.Height);
        return img_desc.BytesPerLine * img_desc.Height;
    }
    else
    {
        CKDX9_LOG("CopyFromMemoryBuffer failed final hr=0x%08lX", (unsigned long)hr);
        return 0;
    }
}

void CKDX9RasterizerContext::SetTransparentMode(CKBOOL Trans)
{
    // If no change in state, return
    if (m_TransparentMode == Trans)
        return;

    SAFERELEASE(m_ScreenBackup);
    m_TransparentMode = Trans;

    if (Trans)
    {
        // The first restore must capture the complete window background.
        m_DirtyRects.Resize(0);
        m_CleanAllRects = TRUE;
    }
    else
    {
        ResetDirtyRects();
    }
}

void CKDX9RasterizerContext::AddDirtyRect(CKRECT *Rect)
{
    if (!Rect)
    {
        // Mark entire screen as dirty
        m_CleanAllRects = TRUE;
    }
    else
    {
        // Don't add if we're already cleaning everything
        if (m_CleanAllRects)
            return;

        // Validate rectangle
        if (Rect->right <= Rect->left || Rect->bottom <= Rect->top)
            return;

        // Add to dirty rectangles list
        m_DirtyRects.PushBack(*Rect);

        // Limit number of rectangles to prevent performance issues
        if (m_DirtyRects.Size() > 64)
            m_CleanAllRects = TRUE;
    }
}

void CKDX9RasterizerContext::RestoreScreenBackup()
{
#ifdef TRACY_ENABLE
    ZoneScopedN(__FUNCTION__);
#endif
    if (!m_TransparentMode || !m_Device || !m_Window)
        return;

    IDirect3DSurface9 *backBuffer = NULL;
    HRESULT hr = m_Device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer);
    if (FAILED(hr) || !backBuffer)
        return;

    D3DSURFACE_DESC backBufferDesc = {};
    hr = backBuffer->GetDesc(&backBufferDesc);
    if (FAILED(hr))
    {
        SAFERELEASE(backBuffer);
        return;
    }

    IDirect3DSurface9 *targetBackup = m_ScreenBackup;
    bool replaceBackup = targetBackup == NULL;
    if (targetBackup)
    {
        D3DSURFACE_DESC backupDesc = {};
        if (FAILED(targetBackup->GetDesc(&backupDesc)) ||
            backupDesc.Width != backBufferDesc.Width ||
            backupDesc.Height != backBufferDesc.Height ||
            backupDesc.Format != backBufferDesc.Format)
        {
            replaceBackup = true;
            targetBackup = NULL;
        }
    }

    if (replaceBackup)
    {
        hr = m_Device->CreateOffscreenPlainSurface(
            backBufferDesc.Width,
            backBufferDesc.Height,
            backBufferDesc.Format,
            D3DPOOL_SYSTEMMEM,
            &targetBackup,
            NULL);
        if (FAILED(hr) || !targetBackup)
        {
            SAFERELEASE(backBuffer);
            return;
        }
    }

    const bool refreshAll = replaceBackup || m_CleanAllRects;
    const bool refreshRequested = refreshAll || m_DirtyRects.Size() > 0;
    bool refreshSucceeded = !refreshRequested;

    if (refreshRequested)
    {
        D3DDISPLAYMODE displayMode = {};
        IDirect3DSurface9 *frontBuffer = NULL;
        D3DLOCKED_RECT frontLock = {};
        D3DLOCKED_RECT backupLock = {};
        bool frontLocked = false;
        bool backupLocked = false;

        hr = m_Device->GetDisplayMode(0, &displayMode);
        if (SUCCEEDED(hr))
        {
            hr = m_Device->CreateOffscreenPlainSurface(
                displayMode.Width,
                displayMode.Height,
                D3DFMT_A8R8G8B8,
                D3DPOOL_SYSTEMMEM,
                &frontBuffer,
                NULL);
        }
        if (SUCCEEDED(hr) && frontBuffer)
            hr = m_Device->GetFrontBufferData(0, frontBuffer);
        if (SUCCEEDED(hr) && frontBuffer)
        {
            hr = frontBuffer->LockRect(&frontLock, NULL, D3DLOCK_READONLY);
            frontLocked = SUCCEEDED(hr);
        }
        if (SUCCEEDED(hr) && frontLocked)
        {
            hr = targetBackup->LockRect(&backupLock, NULL, 0);
            backupLocked = SUCCEEDED(hr);
        }

        const VX_PIXELFORMAT targetFormat = D3DFormatToVxPixelFormat(backBufferDesc.Format);
        if (SUCCEEDED(hr) && frontLocked && backupLocked &&
            frontLock.Pitch > 0 && backupLock.Pitch > 0 && targetFormat != UNKNOWN_PF)
        {
            POINT windowOrigin = {0, 0};
            ClientToScreen(static_cast<HWND>(m_Window), &windowOrigin);

            CKDX9RasterizerDriver *driver = static_cast<CKDX9RasterizerDriver *>(m_Driver);
            if (driver && m_Owner && m_Owner->m_D3D9)
            {
                const HMONITOR monitor = m_Owner->m_D3D9->GetAdapterMonitor(driver->m_AdapterIndex);
                MONITORINFO monitorInfo = {};
                monitorInfo.cbSize = sizeof(monitorInfo);
                if (monitor && GetMonitorInfo(monitor, &monitorInfo))
                {
                    windowOrigin.x -= monitorInfo.rcMonitor.left;
                    windowOrigin.y -= monitorInfo.rcMonitor.top;
                }
            }

            VxImageDescEx sourceTemplate;
            VxImageDescEx targetTemplate;
            VxPixelFormat2ImageDesc(_32_ARGB8888, sourceTemplate);
            VxPixelFormat2ImageDesc(targetFormat, targetTemplate);
            sourceTemplate.BytesPerLine = frontLock.Pitch;
            targetTemplate.BytesPerLine = backupLock.Pitch;

            const int targetBytesPerPixel = targetTemplate.BitsPerPixel / 8;
            if (targetBytesPerPixel > 0)
            {
                if (refreshAll)
                {
                    for (UINT row = 0; row < backBufferDesc.Height; ++row)
                    {
                        memset(static_cast<CKBYTE *>(backupLock.pBits) +
                                   static_cast<size_t>(row) * backupLock.Pitch,
                               0, static_cast<size_t>(backupLock.Pitch));
                    }
                }

                if (refreshAll)
                {
                    const CKRECT fullRect = {
                        0, 0,
                        static_cast<int>(backBufferDesc.Width),
                        static_cast<int>(backBufferDesc.Height)};
                    DX9CaptureFrontBufferRect(
                        fullRect, windowOrigin, displayMode, backBufferDesc,
                        frontLock, backupLock, sourceTemplate, targetTemplate,
                        targetBytesPerPixel);
                }
                else
                {
                    for (XArray<CKRECT>::Iterator it = m_DirtyRects.Begin();
                         it != m_DirtyRects.End(); ++it)
                    {
                        DX9CaptureFrontBufferRect(
                            *it, windowOrigin, displayMode, backBufferDesc,
                            frontLock, backupLock, sourceTemplate, targetTemplate,
                            targetBytesPerPixel);
                    }
                }
                refreshSucceeded = true;
            }
        }

        if (backupLocked)
            targetBackup->UnlockRect();
        if (frontLocked)
            frontBuffer->UnlockRect();
        SAFERELEASE(frontBuffer);
    }

    if (replaceBackup)
    {
        if (!refreshSucceeded)
        {
            SAFERELEASE(targetBackup);
            SAFERELEASE(backBuffer);
            return;
        }
        SAFERELEASE(m_ScreenBackup);
        m_ScreenBackup = targetBackup;
    }

    // A discard swap chain does not preserve untouched back-buffer pixels, so
    // restore the complete cached background every frame. Dirty rectangles
    // select which regions are refreshed from the front buffer, not which
    // regions are copied to the back buffer.
    if (backBufferDesc.MultiSampleType == D3DMULTISAMPLE_NONE)
    {
        hr = m_Device->UpdateSurface(m_ScreenBackup, NULL, backBuffer, NULL);
    }
    else
    {
        hr = DX9DrawSurfaceToMultisampledBackBuffer(
            m_Device, m_ScreenBackup, backBuffer, backBufferDesc, m_SceneBegined);
    }
    SAFERELEASE(backBuffer);

    if (SUCCEEDED(hr) && refreshSucceeded)
        ResetDirtyRects();
}

CKBOOL CKDX9RasterizerContext::SetUserClipPlane(CKDWORD ClipPlaneIndex, const VxPlane &PlaneEquation)
{
    const CKDX9RasterizerDriver *driver = static_cast<const CKDX9RasterizerDriver *>(m_Driver);
    if (!m_Device || !driver || ClipPlaneIndex >= driver->m_D3DCaps.MaxUserClipPlanes ||
        ClipPlaneIndex >= 32)
    {
        return FALSE;
    }

    if (FAILED(m_Device->SetClipPlane(ClipPlaneIndex, (const float *)&PlaneEquation)))
        return FALSE;

    m_UserClipPlanes[ClipPlaneIndex] = PlaneEquation;
    m_UserClipPlaneValidMask |= 1u << ClipPlaneIndex;
    return TRUE;
}

CKBOOL CKDX9RasterizerContext::GetUserClipPlane(CKDWORD ClipPlaneIndex, VxPlane &PlaneEquation)
{
    const CKDX9RasterizerDriver *driver = static_cast<const CKDX9RasterizerDriver *>(m_Driver);
    if (!m_Device || !driver || ClipPlaneIndex >= driver->m_D3DCaps.MaxUserClipPlanes ||
        ClipPlaneIndex >= 32)
    {
        return FALSE;
    }

    const CKDWORD planeMask = 1u << ClipPlaneIndex;
    if ((m_UserClipPlaneValidMask & planeMask) != 0)
    {
        PlaneEquation = m_UserClipPlanes[ClipPlaneIndex];
        return TRUE;
    }

    if (FAILED(m_Device->GetClipPlane(ClipPlaneIndex, (float *)&PlaneEquation)))
        return FALSE;

    m_UserClipPlanes[ClipPlaneIndex] = PlaneEquation;
    m_UserClipPlaneValidMask |= planeMask;
    return TRUE;
}

CKBOOL CKDX9RasterizerContext::LoadCubeMapTexture(CKDWORD Texture, const VxImageDescEx &SurfDesc, CKRST_CUBEFACE Face, int miplevel)
{
#ifdef TRACY_ENABLE
    ZoneScopedN(__FUNCTION__);
#endif
    if (Texture >= m_Textures.Size() || static_cast<int>(Face) < 0 ||
        Face > CKRST_CUBEFACE_ZNEG || miplevel < -1)
        return FALSE;

    CKDX9TextureDesc *desc = static_cast<CKDX9TextureDesc *>(m_Textures[Texture]);
    if (!desc || !desc->DxCubeTexture)
        return FALSE;

    // Skip loading render targets since they're managed differently
    if ((desc->Flags & CKRST_TEXTURE_RENDERTARGET) != 0)
        return TRUE;

    // Ensure this is actually a cube map texture
    if ((desc->Flags & CKRST_TEXTURE_CUBEMAP) == 0)
        return FALSE;

    if (!DX9ValidateImageDesc(SurfDesc))
        return FALSE;

    // Determine the actual mipmap level to load
    int actualMipLevel = (miplevel < 0) ? 0 : miplevel;

    // Get surface description for the specified mipmap level
    D3DSURFACE_DESC surfaceDesc;
    HRESULT hr = desc->DxCubeTexture->GetLevelDesc(actualMipLevel, &surfaceDesc);
    if (FAILED(hr))
        return FALSE;


    // Path 1: DXTn compressed textures using D3DX helper functions
    if (DX9IsBlockCompressedFormat(surfaceDesc.Format))
    {
        if (!D3DXLoadSurfaceFromSurface || !D3DXLoadSurfaceFromMemory)
            return FALSE;

        // Get the cube map face surface
        IDirect3DSurface9 *pCubeMapSurface = NULL;
        hr = desc->DxCubeTexture->GetCubeMapSurface((D3DCUBEMAP_FACES)Face, actualMipLevel, &pCubeMapSurface);
        if (FAILED(hr) || !pCubeMapSurface)
            return FALSE;

        // Set up source rectangle with correct dimensions
        RECT srcRect = {0, 0, (LONG)SurfDesc.Width, (LONG)SurfDesc.Height};

        // Convert source format
        VX_PIXELFORMAT vxpf = VxImageDesc2PixelFormat(SurfDesc);
        D3DFORMAT format = VxPixelFormatToD3DFormat(vxpf);
        if (format == D3DFMT_UNKNOWN)
        {
            SAFERELEASE(pCubeMapSurface);
            return FALSE;
        }

        // Load data from memory to surface
        hr = D3DXLoadSurfaceFromMemory(pCubeMapSurface, NULL, NULL, SurfDesc.Image, format, SurfDesc.BytesPerLine, NULL, &srcRect, D3DX_FILTER_LINEAR, 0);
        if (FAILED(hr))
        {
            SAFERELEASE(pCubeMapSurface);
            return FALSE;
        }

        // Generate mipmaps if needed
        CKDWORD mipMapCount = m_Textures[Texture]->MipMapCount;
        if (miplevel == -1 && mipMapCount > 0)
        {
            // Create all mipmap levels from the loaded surface
            for (int i = 1; i < mipMapCount + 1; ++i)
            {
                IDirect3DSurface9 *pMipSurface = NULL;
                hr = desc->DxCubeTexture->GetCubeMapSurface((D3DCUBEMAP_FACES)Face, i, &pMipSurface);
                if (FAILED(hr) || !pMipSurface)
                {
                    SAFERELEASE(pCubeMapSurface);
                    return FALSE;
                }

                // Create mipmap using box filter
                hr = D3DXLoadSurfaceFromSurface(pMipSurface, NULL, NULL, pCubeMapSurface, NULL, NULL, D3DX_FILTER_BOX, 0);

                // Clean up mip surface regardless of result
                SAFERELEASE(pMipSurface);

                if (FAILED(hr))
                {
                    SAFERELEASE(pCubeMapSurface);
                    return FALSE;
                }
            }
        }

        // Clean up and return
        SAFERELEASE(pCubeMapSurface);
        return TRUE;
    }

    // Path 2: Manual texture loading and mipmap generation
    // Create a copy of the source image data
    VxImageDescEx src = SurfDesc;
    XArray<CKBYTE> mipCurrent;
    XArray<CKBYTE> mipNext;

    // Generate mipmaps if requested
    CKBOOL generateMipmaps = (miplevel == -1 && desc->MipMapCount > 0);

    // Allocate temporary buffer for mipmap generation if needed
    if (generateMipmaps)
    {
        if (surfaceDesc.Width == 0 || surfaceDesc.Height == 0 ||
            surfaceDesc.Height > DX9_SIZE_MAX / surfaceDesc.Width ||
            surfaceDesc.Width > static_cast<UINT>(INT_MAX / 4))
        {
            return FALSE;
        }
        const size_t pixelCount = static_cast<size_t>(surfaceDesc.Width) * surfaceDesc.Height;
        if (pixelCount > static_cast<size_t>(INT_MAX) / 4)
            return FALSE;

        const int bufferSize = static_cast<int>(pixelCount * 4);
        mipCurrent.Resize(bufferSize);
        mipNext.Resize(bufferSize);

        VxImageDescEx working;
        working.Width = surfaceDesc.Width;
        working.Height = surfaceDesc.Height;
        working.BitsPerPixel = 32;
        working.BytesPerLine = 4 * working.Width;
        working.AlphaMask = A_MASK;
        working.RedMask = R_MASK;
        working.GreenMask = G_MASK;
        working.BlueMask = B_MASK;
        working.Image = mipCurrent.Begin();
        VxDoBlit(src, working);
        src = working;
    }

    // Lock and load base level
    D3DLOCKED_RECT lockRect;
    hr = desc->DxCubeTexture->LockRect((D3DCUBEMAP_FACES)Face, actualMipLevel, &lockRect, NULL, 0);
    if (FAILED(hr))
    {
        // Clean up on failure
        return FALSE;
    }

    // Copy data to texture
    if (!LoadSurface(surfaceDesc, lockRect, src))
    {
        desc->DxCubeTexture->UnlockRect((D3DCUBEMAP_FACES)Face, actualMipLevel);
        return FALSE;
    }

    // Unlock base level
    hr = desc->DxCubeTexture->UnlockRect((D3DCUBEMAP_FACES)Face, actualMipLevel);
    if (FAILED(hr))
    {
        return FALSE;
    }

    // Generate and load mipmaps if needed
    if (generateMipmaps && !mipCurrent.IsEmpty() && !mipNext.IsEmpty())
    {
        VxImageDescEx current = src;

        // For each mipmap level
        for (int i = 1; i < desc->MipMapCount + 1; ++i)
        {
            VxGenerateMipMap(current, mipNext.Begin());

            VxImageDescEx next = current;
            if (next.Width > 1)
                next.Width >>= 1;
            if (next.Height > 1)
                next.Height >>= 1;
            next.BytesPerLine = 4 * next.Width;
            next.Image = mipNext.Begin();

            // Get destination surface description
            hr = desc->DxCubeTexture->GetLevelDesc(i, &surfaceDesc);
            if (FAILED(hr))
                return FALSE;

            // Lock the mipmap level
            hr = desc->DxCubeTexture->LockRect((D3DCUBEMAP_FACES)Face, i, &lockRect, NULL, 0);
            if (FAILED(hr))
                return FALSE;

            // Copy data to mipmap level
            if (!LoadSurface(surfaceDesc, lockRect, next))
            {
                desc->DxCubeTexture->UnlockRect((D3DCUBEMAP_FACES)Face, i);
                return FALSE;
            }

            // Unlock the mipmap level
            hr = desc->DxCubeTexture->UnlockRect((D3DCUBEMAP_FACES)Face, i);
            if (FAILED(hr))
                return FALSE;

            // Stop if we've reached 1x1 texture
            if (next.Width <= 1 && next.Height <= 1)
                break;

            current = next;
            mipCurrent.Swap(mipNext);
        }
    }

    return TRUE;
}

void *CKDX9RasterizerContext::LockIndexBuffer(CKDWORD IB, CKDWORD StartIndex, CKDWORD IndexCount, CKRST_LOCKFLAGS Lock)
{
#ifdef TRACY_ENABLE
    ZoneScopedN(__FUNCTION__);
#endif
    if (IB >= m_IndexBuffers.Size())
        return NULL;

    CKDX9IndexBufferDesc *ib = static_cast<CKDX9IndexBufferDesc *>(m_IndexBuffers[IB]);
    if (!ib || !ib->DxBuffer)
        return NULL;

    // Validate StartIndex and IndexCount
    if (StartIndex >= ib->m_MaxIndexCount)
        return NULL;

    const CKDWORD remainingIndices = ib->m_MaxIndexCount - StartIndex;
    if (IndexCount == 0 || IndexCount > remainingIndices)
        IndexCount = remainingIndices;

    // Convert CKRST_LOCKFLAGS to D3DLOCK flags
    DWORD d3dLockFlags = 0;
    switch (Lock)
    {
        case CKRST_LOCK_NOOVERWRITE:
            d3dLockFlags = D3DLOCK_NOOVERWRITE;
            break;
        case CKRST_LOCK_DISCARD:
            d3dLockFlags = D3DLOCK_DISCARD;
            break;
        default:
            d3dLockFlags = 0; // Default lock
            break;
    }

    // Calculate byte offset for 16-bit indices (2 bytes per index)
    UINT offsetInBytes = StartIndex * 2;
    UINT sizeInBytes = IndexCount * 2;

    // Lock the buffer
    void *pIndices = NULL;
    if (FAILED(ib->DxBuffer->Lock(offsetInBytes, sizeInBytes, &pIndices, d3dLockFlags)))
        return NULL;

    return pIndices;
}

CKBOOL CKDX9RasterizerContext::UnlockIndexBuffer(CKDWORD IB)
{
#ifdef TRACY_ENABLE
    ZoneScopedN(__FUNCTION__);
#endif
    if (IB >= m_IndexBuffers.Size())
        return FALSE;

    CKDX9IndexBufferDesc *ib = static_cast<CKDX9IndexBufferDesc *>(m_IndexBuffers[IB]);
    if (!ib || !ib->DxBuffer)
        return FALSE;

    return SUCCEEDED(ib->DxBuffer->Unlock());
}

CKBOOL CKDX9RasterizerContext::CreateTextureFromFile(CKDWORD Texture, CKSTRING Filename, TexFromFile *param)
{
#ifdef TRACY_ENABLE
    ZoneScopedN(__FUNCTION__);
#endif
    if (!m_Device || !m_Driver || !Filename || !param || param->MipLevels < 0)
        return FALSE;

    if (Texture >= m_Textures.Size())
        return FALSE;

    // Check if D3DX function is available
    if (!D3DXCreateTextureFromFileExA)
        return FALSE;

    // Determine the texture format to use
    D3DFORMAT format = VxPixelFormatToD3DFormat(param->Format);
    if (format == D3DFMT_UNKNOWN)
        format = D3DFMT_A8R8G8B8;

    // Determine mipmap settings
    UINT mipLevels = (param->MipLevels == 0) ? 1 : param->MipLevels;

    // Determine filter settings based on mipmaps
    DWORD filter = D3DX_FILTER_LINEAR;
    DWORD mipFilter = (mipLevels > 1) ? D3DX_FILTER_BOX : D3DX_FILTER_NONE;

    // Determine usage and pool based on dynamic flag
    DWORD usage = 0;
    D3DPOOL pool = D3DPOOL_MANAGED;

    if (param->IsDynamic)
    {
        usage = D3DUSAGE_DYNAMIC;
        pool = D3DPOOL_DEFAULT;
    }

    // Create the texture
    IDirect3DTexture9 *pTexture = NULL;
    HRESULT hr = D3DXCreateTextureFromFileExA(
        m_Device,               // Device
        Filename,               // Source file
        D3DX_DEFAULT,           // Width (use file's width)
        D3DX_DEFAULT,           // Height (use file's height)
        mipLevels,              // Number of mipmap levels
        usage,                  // Usage flags
        format,                 // Pixel format
        pool,                   // Memory pool
        filter,                 // Filter for resizing if needed
        mipFilter,              // Mipmap filter
        param->ColorKey,        // Color key for transparency
        NULL,                   // Source info (don't need)
        NULL,                   // Palette (don't need)
        &pTexture               // Result
    );
    if (FAILED(hr) || !pTexture)
        return FALSE;

    // Get texture information
    D3DSURFACE_DESC surfaceDesc = {};
    hr = pTexture->GetLevelDesc(0, &surfaceDesc);
    if (FAILED(hr))
    {
        SAFERELEASE(pTexture);
        return FALSE;
    }

    // Keep the old descriptor and resource alive until the replacement is
    // completely described and every device stage has been rebound.
    CKDX9TextureDesc *oldDesc = static_cast<CKDX9TextureDesc *>(m_Textures[Texture]);
    CKDX9TextureDesc *desc = new CKDX9TextureDesc();
    if (!desc)
    {
        SAFERELEASE(pTexture);
        return FALSE;
    }

    // Set up the texture descriptor
    desc->DxTexture = pTexture;
    D3DFormatToTextureDesc(surfaceDesc.Format, desc);

    // Set appropriate flags
    CKDWORD flags = CKRST_TEXTURE_VALID | CKRST_TEXTURE_RGB;

    // Check if the texture has alpha
    D3DFORMAT actualFormat = surfaceDesc.Format;
    if (actualFormat == D3DFMT_A8R8G8B8 || 
        actualFormat == D3DFMT_A1R5G5B5 || 
        actualFormat == D3DFMT_A4R4G4B4 ||
        actualFormat == D3DFMT_A8B8G8R8 ||
        actualFormat == D3DFMT_A16B16G16R16 ||
        actualFormat == D3DFMT_A8P8 ||
        actualFormat == D3DFMT_A8L8 ||
        actualFormat == D3DFMT_A4L4 ||
        actualFormat == D3DFMT_DXT2 ||
        actualFormat == D3DFMT_DXT3 ||
        actualFormat == D3DFMT_DXT4 ||
        actualFormat == D3DFMT_DXT5)
    {
        flags |= CKRST_TEXTURE_ALPHA;
    }

    // Add dynamic flag if applicable
    if (param->IsDynamic)
    {
        flags |= CKRST_TEXTURE_HINTPROCEDURAL;
    }
    else
    {
        flags |= CKRST_TEXTURE_HINTSTATIC;
    }

    // Add managed flag if applicable
    if (pool == D3DPOOL_MANAGED)
    {
        flags |= CKRST_TEXTURE_MANAGED;
    }

    desc->Flags = flags;
    desc->Format.Width = surfaceDesc.Width;
    desc->Format.Height = surfaceDesc.Height;

    D3DLOCKED_RECT lockedRect;
    if (SUCCEEDED(pTexture->LockRect(0, &lockedRect, NULL, D3DLOCK_READONLY)))
    {
        desc->Format.BytesPerLine = lockedRect.Pitch;
        pTexture->UnlockRect(0);
    }
    else
    {
        // Fallback to an estimate if locking fails
        int bytesPerPixel = desc->Format.BitsPerPixel / 8;
        desc->Format.BytesPerLine = ((surfaceDesc.Width * bytesPerPixel) + 3) & ~3;
    }

    desc->MipMapCount = pTexture->GetLevelCount() - 1;

    IDirect3DBaseTexture9 *oldTexture = NULL;
    if (oldDesc)
    {
        if (oldDesc->Flags & CKRST_TEXTURE_CUBEMAP)
            oldTexture = oldDesc->DxCubeTexture;
        else if (oldDesc->Flags & CKRST_TEXTURE_VOLUMEMAP)
            oldTexture = oldDesc->DxVolumeTexture;
        else
            oldTexture = oldDesc->DxTexture;
    }

    const int stageCount = m_Driver->m_3DCaps.MaxNumberTextureStage;
    if (oldTexture && !DX9ReplaceBoundTexture(m_Device, stageCount, oldTexture, pTexture))
    {
        delete desc;
        return FALSE;
    }

    m_Textures[Texture] = desc;
    delete oldDesc;

    return TRUE;
}

void CKDX9RasterizerContext::RestoreWindowStyle(HWND Window, LONG originalStyle, CKBOOL wasFullscreen)
{
    if (Window && wasFullscreen)
    {
        SetWindowLongA(Window, GWL_STYLE, originalStyle);
    }
}

CKBOOL CKDX9RasterizerContext::UpdateDeviceProperties()
{
    if (!m_Device)
        return FALSE;

    // Get back buffer information
    IDirect3DSurface9 *pBackBuffer = NULL;
    if (SUCCEEDED(m_Device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &pBackBuffer)))
    {
        VxImageDescEx desc;
        D3DSURFACE_DESC surfaceDesc;
        if (SUCCEEDED(pBackBuffer->GetDesc(&surfaceDesc)))
        {
            SAFERELEASE(pBackBuffer);
            m_PixelFormat = D3DFormatToVxPixelFormat(surfaceDesc.Format);
            VxPixelFormat2ImageDesc(m_PixelFormat, desc);
            m_Bpp = desc.BitsPerPixel;
            m_Width = surfaceDesc.Width;
            m_Height = surfaceDesc.Height;
        }
        else
        {
            SAFERELEASE(pBackBuffer);
            return FALSE;
        }
    }
    else
    {
        return FALSE;
    }

    // Get depth/stencil information
    IDirect3DSurface9 *pStencilSurface = NULL;
    if (SUCCEEDED(m_Device->GetDepthStencilSurface(&pStencilSurface)))
    {
        D3DSURFACE_DESC desc;
        if (SUCCEEDED(pStencilSurface->GetDesc(&desc)))
        {
            SAFERELEASE(pStencilSurface);
            m_ZBpp = DepthBitPerPixelFromFormat(desc.Format, &m_StencilBpp);
        }
        else
        {
            SAFERELEASE(pStencilSurface);
            return FALSE;
        }
    }
    else
    {
        // Not fatal - some renderers might work without depth buffer
        m_ZBpp = 0;
        m_StencilBpp = 0;
    }

    return TRUE;
}

CKBOOL CKDX9RasterizerContext::InitializeDeviceStates()
{
    if (!m_Device)
        return FALSE;

    m_UserClipPlaneValidMask = 0;

    if (m_Width == 0 || m_Height == 0 ||
        m_Width > static_cast<CKDWORD>(INT_MAX) ||
        m_Height > static_cast<CKDWORD>(INT_MAX))
    {
        return FALSE;
    }

    CKViewportData viewport = {
        0, 0, static_cast<int>(m_Width), static_cast<int>(m_Height), 0.0f, 1.0f};
    if (!SetViewport(&viewport) ||
        !SetRenderState(VXRENDERSTATE_NORMALIZENORMALS, TRUE) ||
        !SetRenderState(VXRENDERSTATE_LOCALVIEWER, TRUE) ||
        !SetRenderState(VXRENDERSTATE_COLORVERTEX, FALSE))
    {
        return FALSE;
    }

    return TRUE;
}

void CKDX9RasterizerContext::DestroyDevice()
{
    if (m_Owner->m_FullscreenContext == this)
        m_Owner->m_FullscreenContext = NULL;

    SAFERELEASE(m_Device);
}

void CKDX9RasterizerContext::PrepareDeviceReset()
{
#ifdef TRACY_ENABLE
    ZoneScopedN(__FUNCTION__);
#endif
    if (!m_Device || m_InCreateDestroy)
    {
        CKDX9_LOG("PrepareDeviceReset skipped device=%p inCreateDestroy=%d", m_Device, m_InCreateDestroy);
        return;
    }

    CKDX9_LOG("PrepareDeviceReset begin back=%p depth=%p screen=%p currentTexture=%u",
              m_DefaultBackBuffer, m_DefaultDepthBuffer, m_ScreenBackup, m_CurrentTextureIndex);

    ReleaseVertexDeclarations();
    ReleaseStateBlocks();
    FlushNonManagedObjects();
    ClearStreamCache();

    CKDX9_LOG("PrepareDeviceReset end back=%p depth=%p screen=%p currentTexture=%u",
              m_DefaultBackBuffer, m_DefaultDepthBuffer, m_ScreenBackup, m_CurrentTextureIndex);
}

HRESULT CKDX9RasterizerContext::ResetDevice()
{
    CKDX9_LOG("ResetDevice begin this=%p device=%p size=%ux%u fmt=0x%08X depth=0x%08X windowed=%d msaa=%u/%lu",
              this, m_Device, m_PresentParams.BackBufferWidth, m_PresentParams.BackBufferHeight,
              (unsigned)m_PresentParams.BackBufferFormat,
              (unsigned)m_PresentParams.AutoDepthStencilFormat,
              m_PresentParams.Windowed,
              (unsigned)m_PresentParams.MultiSampleType,
              (unsigned long)m_PresentParams.MultiSampleQuality);

    HRESULT coop = m_Device ? m_Device->TestCooperativeLevel() : E_POINTER;
    CKDX9_LOG("ResetDevice TestCooperativeLevel hr=0x%08lX", (unsigned long)coop);

    // Prepare for reset by releasing resources
    FlushNonManagedObjects();

    HRESULT hr = m_Device->Reset(&m_PresentParams);
    CKDX9_LOG("ResetDevice Reset hr=0x%08lX", (unsigned long)hr);
    if (FAILED(hr))
        return hr;

    // Reset restores D3D's default state, so values cached for the old device
    // state must not suppress the initialization calls below.
    FlushRenderStateCache();
    ResetTextureStageStateCache();
    m_InverseWinding = FALSE;

    // Update device properties
    if (!UpdateDeviceProperties())
    {
        CKDX9_LOG("ResetDevice UpdateDeviceProperties failed");
        return E_FAIL;
    }

    // Restore device states
    if (!InitializeDeviceStates())
    {
        CKDX9_LOG("ResetDevice InitializeDeviceStates failed");
        return E_FAIL;
    }

    // Shader bytecode is retained across reset and can be restored immediately.
    // Textures and buffers keep their descriptors but remain invalid until the
    // owner recreates and reloads their contents.
    RestoreDeviceResources();

    CKDX9_LOG("ResetDevice success");
    return S_OK;
}

void CKDX9RasterizerContext::ConfigureMultisampling()
{
    // Disable multisampling if not requested
    if (m_Antialias == 0)
    {
        m_PresentParams.MultiSampleType = D3DMULTISAMPLE_NONE;
        m_PresentParams.MultiSampleQuality = 0;
        return;
    }

    // Determine requested sampling level
    D3DMULTISAMPLE_TYPE requestedType = (m_Antialias < D3DMULTISAMPLE_2_SAMPLES || 
                                         m_Antialias > D3DMULTISAMPLE_16_SAMPLES) ? 
        D3DMULTISAMPLE_2_SAMPLES : (D3DMULTISAMPLE_TYPE)m_Antialias;

    // Try each level from requested down to 2x
    for (int type = requestedType; type >= D3DMULTISAMPLE_2_SAMPLES; --type)
    {
        DWORD qualityLevels = 0;
        if (SUCCEEDED(m_Owner->m_D3D9->CheckDeviceMultiSampleType(
            static_cast<CKDX9RasterizerDriver *>(m_Driver)->m_AdapterIndex, 
            D3DDEVTYPE_HAL,
            m_PresentParams.BackBufferFormat, 
            m_PresentParams.Windowed, 
            (D3DMULTISAMPLE_TYPE)type,
            &qualityLevels)))
        {
            m_PresentParams.MultiSampleType = (D3DMULTISAMPLE_TYPE)type;
            m_PresentParams.MultiSampleQuality = (qualityLevels > 0) ? (qualityLevels - 1) : 0;
            return;
        }
    }

    // No supported multisampling level found
    m_PresentParams.MultiSampleType = D3DMULTISAMPLE_NONE;
    m_PresentParams.MultiSampleQuality = 0;
}

void CKDX9RasterizerContext::UpdateDirectXData()
{
    // Reset the structure to defaults
    m_DirectXData.DDBackBuffer = NULL;
    m_DirectXData.DDPrimaryBuffer = NULL;
    m_DirectXData.DDZBuffer = NULL;
    m_DirectXData.D3DViewport = NULL;
    m_DirectXData.DirectDraw = NULL;
    m_DirectXData.DDClipper = NULL;

    // Set device pointer
    m_DirectXData.D3DDevice = m_Device;
    m_DirectXData.DxVersion = DIRECT3D_VERSION;

    // Get back buffer
    IDirect3DSurface9 *pBackBuffer = NULL;
    HRESULT hr = m_Device ? m_Device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &pBackBuffer) : E_FAIL;
    if (SUCCEEDED(hr) && pBackBuffer)
    {
        SAFERELEASE(pBackBuffer);
    }

    // Get depth/stencil buffer
    IDirect3DSurface9 *pZStencilSurface = NULL;
    hr = m_Device ? m_Device->GetDepthStencilSurface(&pZStencilSurface) : E_FAIL;
    if (SUCCEEDED(hr) && pZStencilSurface)
    {
        SAFERELEASE(pZStencilSurface);
    }
}

CKBOOL CKDX9RasterizerContext::InternalDrawPrimitiveVB(VXPRIMITIVETYPE pType, CKDX9VertexBufferDesc *VB, CKDWORD StartIndex, CKDWORD VertexCount, WORD *indices, int indexcount, CKBOOL Clip)
{
#ifdef TRACY_ENABLE
    ZoneScopedN(__FUNCTION__);
#endif

    if (!VB || !VB->DxBuffer || !m_Device)
        return FALSE;

    if (VertexCount == 0 || (indices && indexcount == 0))
        return TRUE; // Nothing to draw, but not an error
    if (indices && indexcount < 0)
        return FALSE;

    HRESULT hr;
    CKDWORD ibstart = 0;

    // Handle indexed drawing
    if (indices)
    {
        // Ensure we have a valid index buffer of sufficient size
        CKDX9IndexBufferDesc *desc = m_IndexBuffer[Clip ? 1 : 0];
        CKBOOL needNewBuffer = FALSE;

        // Check if we need to create or recreate the index buffer
        if (!desc)
        {
            needNewBuffer = TRUE;
        }
        else if (indexcount > desc->m_MaxIndexCount)
        {
            delete desc;
            desc = NULL;
            needNewBuffer = TRUE;
        }
        else if (!desc->DxBuffer)
        {
            delete desc;
            desc = NULL;
            needNewBuffer = TRUE;
        }

        // Create a new index buffer if needed
        if (needNewBuffer)
        {
            desc = new CKDX9IndexBufferDesc;
            if (!desc)
                return FALSE;

            // Calculate appropriate buffer size (not too small, not too large)
            const CKDWORD requestedIndexCount = static_cast<CKDWORD>(indexcount);
            CKDWORD maxIndexCount = requestedIndexCount;
            if (maxIndexCount < 10000)
                maxIndexCount = 10000;
            else if (maxIndexCount <= 0xFFFFFFFFu - 100)
                maxIndexCount += 100;
            if (maxIndexCount > 0xFFFFFFFFu / sizeof(WORD))
            {
                delete desc;
                return FALSE;
            }

            // Set up usage flags
            DWORD usage = D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY;
            if (m_SoftwareVertexProcessing)
                usage |= D3DUSAGE_SOFTWAREPROCESSING;

            // Create the DirectX index buffer
            hr = m_Device->CreateIndexBuffer(
                sizeof(WORD) * maxIndexCount, // Size in bytes
                usage,                // Usage flags
                D3DFMT_INDEX16,       // Format (16-bit indices)
                D3DPOOL_DEFAULT,      // Memory pool
                &desc->DxBuffer,      // Resulting buffer
                NULL                  // Shared handle (not used)
            );
            if (FAILED(hr) || !desc->DxBuffer)
            {
                delete desc;
                return FALSE;
            }

            desc->m_MaxIndexCount = maxIndexCount;
            desc->m_CurrentICount = 0;
            m_IndexBuffer[Clip ? 1 : 0] = desc;
        }

        // Lock the index buffer for writing
        void *pbData = NULL;
        CKBOOL lockSuccess = FALSE;

        // Try to append data if there's room
        const CKDWORD requestedIndexCount = static_cast<CKDWORD>(indexcount);
        if (desc->m_CurrentICount <= desc->m_MaxIndexCount &&
            requestedIndexCount <= desc->m_MaxIndexCount - desc->m_CurrentICount)
        {
            hr = desc->DxBuffer->Lock(
                sizeof(WORD) * desc->m_CurrentICount,
                sizeof(WORD) * requestedIndexCount,
                &pbData,                     // Pointer to receive data
                D3DLOCK_NOOVERWRITE          // Don't overwrite existing data
            );
            if (SUCCEEDED(hr) && pbData)
            {
                ibstart = desc->m_CurrentICount;
                desc->m_CurrentICount += requestedIndexCount;
                lockSuccess = TRUE;
            }
        }

        // If appending failed, try discarding and starting fresh
        if (!lockSuccess)
        {
            hr = desc->DxBuffer->Lock(
                0,                  // Start from beginning
                sizeof(WORD) * requestedIndexCount,
                &pbData,            // Pointer to receive data
                D3DLOCK_DISCARD     // Discard previous contents
            );
            if (SUCCEEDED(hr) && pbData)
            {
                ibstart = 0;
                desc->m_CurrentICount = requestedIndexCount;
                lockSuccess = TRUE;
            }
        }

        // Copy index data and unlock
        if (lockSuccess && pbData)
        {
            memcpy(pbData, indices, sizeof(WORD) * requestedIndexCount);
            hr = desc->DxBuffer->Unlock();
            if (FAILED(hr))
                return FALSE;
        }
        else
        {
            // If we couldn't lock the buffer, we can't draw
            return FALSE;
        }
    }

    // Set up vertex streams (positions, normals, colors, etc.)
    if (!SetupStreams(VB->DxBuffer, VB->m_VertexFormat, 0, VB->m_VertexSize))
        return FALSE;

    // Calculate primitive count based on primitive type
    CKDWORD primitiveCount = indices ? static_cast<CKDWORD>(indexcount) : VertexCount;

    // Adjust primitive count based on primitive type
    switch (pType)
    {
        case VX_LINELIST:
            primitiveCount /= 2;
            break;
        case VX_LINESTRIP:
            primitiveCount = primitiveCount > 0 ? primitiveCount - 1 : 0;
            break;
        case VX_TRIANGLELIST:
            primitiveCount /= 3;
            break;
        case VX_TRIANGLESTRIP:
        case VX_TRIANGLEFAN:
            primitiveCount = primitiveCount > 2 ? primitiveCount - 2 : 0;
            break;
        // Point lists don't need adjustment
        case VX_POINTLIST:
        default:
            break;
    }

    // Ensure we have at least one primitive to draw
    if (primitiveCount == 0)
        return TRUE; // Nothing to draw, but not an error

    // Draw non-indexed primitives
    if (!indices || pType == VX_POINTLIST)
    {
        hr = m_Device->DrawPrimitive(
            (D3DPRIMITIVETYPE)pType,  // Primitive type
            StartIndex,               // Starting vertex
            primitiveCount            // Number of primitives
        );
        return SUCCEEDED(hr);
    }

    // Draw indexed primitives
    hr = SetD3DIndices(m_IndexBuffer[Clip ? 1 : 0]->DxBuffer);
    if (FAILED(hr))
        return FALSE;

    hr = m_Device->DrawIndexedPrimitive(
        (D3DPRIMITIVETYPE)pType,  // Primitive type
        StartIndex,               // Base vertex index
        0,                        // Minimum vertex index
        VertexCount,              // Number of vertices
        ibstart,                  // Start index
        primitiveCount            // Number of primitives
    );
    return SUCCEEDED(hr);
}

CKBOOL CKDX9RasterizerContext::SetupStreams(LPDIRECT3DVERTEXBUFFER9 Buffer, CKDWORD VFormat, CKDWORD VOffset, CKDWORD VSize)
{
#ifdef TRACY_ENABLE
    ZoneScopedN(__FUNCTION__);
#endif

    if (!m_Device || !Buffer || VSize == 0)
        return FALSE;

    HRESULT hr = S_OK;
    CKBOOL useFixedPipeline = TRUE;

    // Check if we're using programmable vertex pipeline
    if (m_CurrentVertexShaderCache != 0 && m_CurrentVertexShaderCache < m_VertexShaders.Size())
    {
        CKDX9VertexShaderDesc *desc = static_cast<CKDX9VertexShaderDesc *>(m_VertexShaders[m_CurrentVertexShaderCache]);

        // Verify shader exists and is valid
        if (desc && desc->DxShader)
        {
            // Get or create appropriate vertex declaration for this shader and format
            IDirect3DVertexDeclaration9 *pDecl = NULL;

            // Look for existing declaration in cache
            XNHashTable<LPDIRECT3DVERTEXDECLARATION9, DWORD>::Iterator it = m_VertexDeclarations.Find(VFormat);
            if (it != m_VertexDeclarations.End())
            {
                // Use cached declaration if found
                pDecl = *it;
            }
            else
            {
                // Create new declaration if not found
                if (CreateVertexDeclaration(VFormat, &pDecl) && pDecl)
                {
                    // Store for future use
                    m_VertexDeclarations.Insert(VFormat, pDecl);
                }
            }

            // Set up programmable pipeline if we have a valid declaration
            if (pDecl)
            {
                // First, set vertex declaration (vertex format for shader)
                hr = m_Device->SetVertexDeclaration(pDecl);
                if (SUCCEEDED(hr))
                {
                    // Then set vertex shader
                    hr = m_Device->SetVertexShader(desc->DxShader);
                    if (SUCCEEDED(hr))
                    {
                        // Successfully set up programmable pipeline
                        useFixedPipeline = FALSE;
                    }
                }
            }
        }
    }

    // Fall back to fixed function pipeline if needed
    if (useFixedPipeline)
    {
        // Reset to fixed function pipeline if previously using programmable pipeline
        if (m_CurrentVertexShaderCache != 0)
        {
            hr = m_Device->SetVertexShader(NULL);
            if (FAILED(hr))
                return FALSE;
            m_CurrentVertexShaderCache = 0;
        }

        // Update FVF if needed
        if (VFormat != m_CurrentVertexFormatCache)
        {
            hr = m_Device->SetFVF(VFormat);
            if (FAILED(hr))
                return FALSE;
            m_CurrentVertexFormatCache = VFormat;
        }
    }

    // Set vertex buffer if it's changed
    if (Buffer != m_CurrentVertexBufferCache || m_CurrentVertexSizeCache != VSize)
    {
        // Set the vertex buffer as the data source for stream 0
        hr = m_Device->SetStreamSource(0, Buffer, VOffset, VSize);
        if (SUCCEEDED(hr))
        {
            // Update cache values
            m_CurrentVertexBufferCache = Buffer;
            m_CurrentVertexSizeCache = VSize;
        }
        else
        {
            // Clear cache on failure
            m_CurrentVertexBufferCache = NULL;
            m_CurrentVertexSizeCache = 0;
            return FALSE;
        }
    }

    return TRUE;
}

CKBOOL CKDX9RasterizerContext::CreateTexture(CKDWORD Texture, CKTextureDesc *DesiredFormat)
{
#ifdef TRACY_ENABLE
    ZoneScopedN(__FUNCTION__);
#endif
    if (!DesiredFormat || !m_Device)
        return FALSE;

    if (Texture >= m_Textures.Size())
        return FALSE;

    // CreateObject may be used to recreate the descriptor already stored at
    // this index after a device reset. Snapshot the public description before
    // replacing the backend object to avoid reading freed memory.
    CKTextureDesc requestedFormat;
    requestedFormat.Flags = DesiredFormat->Flags;
    requestedFormat.Format = DesiredFormat->Format;
    requestedFormat.MipMapCount = DesiredFormat->MipMapCount;
    DesiredFormat = &requestedFormat;

    if ((DesiredFormat->Flags & CKRST_TEXTURE_VOLUMEMAP) != 0)
    {
        // CKTextureDesc does not provide a volume depth, so creating a 2D
        // texture while advertising a volume map would corrupt the contract.
        return FALSE;
    }

    if (DesiredFormat->Format.Width == 0 || DesiredFormat->Format.Height == 0 ||
        DesiredFormat->Format.Width > static_cast<CKDWORD>(INT_MAX) ||
        DesiredFormat->Format.Height > static_cast<CKDWORD>(INT_MAX) ||
        DesiredFormat->MipMapCount == 0xFFFFFFFFu)
    {
        return FALSE;
    }

    // Adjust mipmap count (1 means no mipmaps)
    if (DesiredFormat->MipMapCount == 1)
        DesiredFormat->MipMapCount = 0;

    // Get driver and determine appropriate texture format
    CKDX9RasterizerDriver *driver = static_cast<CKDX9RasterizerDriver *>(m_Driver);
    if (!driver)
        return FALSE;

    CKDWORD flags = DesiredFormat->Flags;
    CKBOOL isRenderTarget = (flags & CKRST_TEXTURE_RENDERTARGET) != 0;
    CKBOOL isCubeMap = (flags & CKRST_TEXTURE_CUBEMAP) != 0;

    // Determine usage flags. Dynamic and render-target resources must use the
    // default pool; combining either usage with D3DPOOL_MANAGED is invalid.
    DWORD usage = 0;
    if (isRenderTarget)
        usage |= D3DUSAGE_RENDERTARGET;
    else if (flags & CKRST_TEXTURE_HINTPROCEDURAL)
        usage |= D3DUSAGE_DYNAMIC;

    // Find appropriate format for the texture
    D3DFORMAT format = driver->FindNearestTextureFormat(DesiredFormat, m_PresentParams.BackBufferFormat, usage);
    if (format == D3DFMT_UNKNOWN)
    {
        if (flags & CKRST_TEXTURE_RENDERTARGET)
            format = m_PresentParams.BackBufferFormat;
        else if (flags & CKRST_TEXTURE_ALPHA)
            format = D3DFMT_A8R8G8B8;
        else
            format = D3DFMT_X8R8G8B8;
    }

    // Calculate dimensions based on hardware capabilities
    int width = DesiredFormat->Format.Width;
    int height = DesiredFormat->Format.Height;
    CKDWORD textureCaps = driver->m_3DCaps.TextureCaps;

    // Power of 2 adjustment if required
    if ((textureCaps & CKRST_TEXTURECAPS_POW2) != 0)
    {
        CKDWORD widthMsb = GetMsb(width, 15);
        if ((1u << widthMsb) < static_cast<CKDWORD>(width) && widthMsb < 15)
            ++widthMsb;
        width = 1u << widthMsb;

        CKDWORD heightMsb = GetMsb(height, 15);
        if ((1u << heightMsb) < static_cast<CKDWORD>(height) && heightMsb < 15)
            ++heightMsb;
        height = 1u << heightMsb;
    }

    // Square texture adjustment if required
    if ((isCubeMap || (textureCaps & CKRST_TEXTURECAPS_SQUAREONLY) != 0) && width != height)
    {
        // Make dimensions square by taking the larger value
        width = height = XMax(width, height);
    }

    // Clamp dimensions to hardware limits
    width = XMax(width, static_cast<int>(driver->m_3DCaps.MinTextureWidth));
    width = XMin(width, static_cast<int>(driver->m_3DCaps.MaxTextureWidth));
    height = XMax(height, static_cast<int>(driver->m_3DCaps.MinTextureHeight));
    height = XMin(height, static_cast<int>(driver->m_3DCaps.MaxTextureHeight));

    // Determine mipmap levels and memory pool
    UINT levels = DesiredFormat->MipMapCount != 0 ? DesiredFormat->MipMapCount + 1 : 1;

    // Render targets and dynamic textures must be in the default pool.
    if (isRenderTarget || (usage & D3DUSAGE_DYNAMIC) != 0)
        flags &= ~CKRST_TEXTURE_MANAGED;

    // Choose appropriate memory pool
    D3DPOOL pool;
    if (flags & CKRST_TEXTURE_MANAGED)
        pool = D3DPOOL_MANAGED; // Managed textures
    else
        pool = D3DPOOL_DEFAULT; // Default for other textures

    // Keep the old descriptor alive until replacement succeeds. Besides making
    // failure non-destructive, this lets us update only stages that actually
    // reference the old D3D resource.
    CKDX9TextureDesc *oldDesc = static_cast<CKDX9TextureDesc *>(m_Textures[Texture]);
    CKDX9TextureDesc *desc = new CKDX9TextureDesc();
    if (!desc)
        return FALSE;

    LPDIRECT3DSURFACE9 originalRenderTarget = NULL;
    LPDIRECT3DSURFACE9 originalDepthStencil = NULL;

    if (isRenderTarget)
    {
        m_Device->GetRenderTarget(0, &originalRenderTarget);
        m_Device->GetDepthStencilSurface(&originalDepthStencil);
    }

    HRESULT hr = S_OK;
    CKBOOL success = FALSE;

    // Create regular texture or cube texture based on flags
    if (!isCubeMap)
    {
        // Create regular 2D texture
        LPDIRECT3DTEXTURE9 pTexture = NULL;
        hr = m_Device->CreateTexture(width, height, levels, usage, format, pool, &pTexture, NULL);
        if (SUCCEEDED(hr) && pTexture)
        {
            // Get texture properties
            D3DSURFACE_DESC surfaceDesc = {};
            hr = pTexture->GetLevelDesc(0, &surfaceDesc);
            if (SUCCEEDED(hr))
            {
                // Set texture descriptor properties
                desc->DxTexture = pTexture;
                D3DFormatToTextureDesc(surfaceDesc.Format, desc);
                desc->Flags = flags | CKRST_TEXTURE_VALID;
                desc->Format.Width = surfaceDesc.Width;
                desc->Format.Height = surfaceDesc.Height;

                D3DLOCKED_RECT lockedRect;
                if (SUCCEEDED(desc->DxTexture->LockRect(0, &lockedRect, NULL, D3DLOCK_READONLY)))
                {
                    // Set BytesPerLine to the actual pitch from the driver
                    desc->Format.BytesPerLine = lockedRect.Pitch;
                    desc->DxTexture->UnlockRect(0);
                }
                else
                {
                    // Fallback to an aligned calculation if locking fails
                    int bytesPerPixel = desc->Format.BitsPerPixel / 8;
                    desc->Format.BytesPerLine = ((width * bytesPerPixel) + 3) & ~3; // 4-byte alignment
                }

                desc->MipMapCount = pTexture->GetLevelCount() - 1;
                success = TRUE;
            }
            else
            {
                // Failed to get level description
                SAFERELEASE(pTexture);
            }
        }
    }
    else
    {
        // Create cube texture
        LPDIRECT3DCUBETEXTURE9 pCubeTexture = NULL;
        hr = m_Device->CreateCubeTexture(width, levels, usage, format, pool, &pCubeTexture, NULL);
        if (SUCCEEDED(hr) && pCubeTexture)
        {
            // Get texture properties
            D3DSURFACE_DESC surfaceDesc = {};
            hr = pCubeTexture->GetLevelDesc(0, &surfaceDesc);
            if (SUCCEEDED(hr))
            {
                // Set texture descriptor properties
                desc->DxCubeTexture = pCubeTexture;
                D3DFormatToTextureDesc(surfaceDesc.Format, desc);
                desc->Flags = flags | CKRST_TEXTURE_VALID | CKRST_TEXTURE_CUBEMAP;
                desc->Format.Width = surfaceDesc.Width;
                desc->Format.Height = surfaceDesc.Height;

                D3DLOCKED_RECT lockedRect;
                if (SUCCEEDED(desc->DxCubeTexture->LockRect(D3DCUBEMAP_FACE_POSITIVE_X, 0, &lockedRect, NULL, D3DLOCK_READONLY)))
                {
                    // Set BytesPerLine to the actual pitch from the driver
                    desc->Format.BytesPerLine = lockedRect.Pitch;
                    desc->DxCubeTexture->UnlockRect(D3DCUBEMAP_FACE_POSITIVE_X, 0);
                }
                else
                {
                    // Fallback to an aligned calculation if locking fails
                    int bytesPerPixel = desc->Format.BitsPerPixel / 8;
                    desc->Format.BytesPerLine = ((width * bytesPerPixel) + 3) & ~3; // 4-byte alignment
                }

                desc->MipMapCount = pCubeTexture->GetLevelCount() - 1;
                success = TRUE;
            }
            else
            {
                // Failed to get level description
                SAFERELEASE(pCubeTexture);
            }
        }
    }

    // Restore original render target if we're creating a render target
    if (isRenderTarget && success)
    {
        desc->DxRenderTexture = NULL;

        // Restore the original render target
        if (originalRenderTarget)
        {
            m_Device->SetRenderTarget(0, originalRenderTarget);
            SAFERELEASE(originalRenderTarget);
        }

        if (originalDepthStencil)
        {
            m_Device->SetDepthStencilSurface(originalDepthStencil);
            SAFERELEASE(originalDepthStencil);
        }
    }
    else
    {
        // Clean up render target resources even if we're not creating a render target
        SAFERELEASE(originalRenderTarget);
        SAFERELEASE(originalDepthStencil);
    }

    if (success)
    {
        IDirect3DBaseTexture9 *oldTexture = NULL;
        IDirect3DBaseTexture9 *newTexture = NULL;
        if (oldDesc)
        {
            oldTexture = (oldDesc->Flags & CKRST_TEXTURE_CUBEMAP)
                             ? static_cast<IDirect3DBaseTexture9 *>(oldDesc->DxCubeTexture)
                             : static_cast<IDirect3DBaseTexture9 *>(oldDesc->DxTexture);
        }
        newTexture = (desc->Flags & CKRST_TEXTURE_CUBEMAP)
                         ? static_cast<IDirect3DBaseTexture9 *>(desc->DxCubeTexture)
                         : static_cast<IDirect3DBaseTexture9 *>(desc->DxTexture);

        if (oldTexture &&
            !DX9ReplaceBoundTexture(m_Device, m_Driver->m_3DCaps.MaxNumberTextureStage,
                                    oldTexture, newTexture))
        {
            delete desc;
            return FALSE;
        }

        m_Textures[Texture] = desc;
        delete oldDesc;
    }
    else
    {
        delete desc;
    }
    return success;
}

CKBOOL CKDX9RasterizerContext::CreateVertexShader(CKDWORD VShader, CKVertexShaderDesc *DesiredFormat)
{
#ifdef TRACY_ENABLE
    ZoneScopedN(__FUNCTION__);
#endif
#if LOGGING && LOG_CREATEVERTEXSHADER
    fprintf(stderr, "create vertex shader %d\n", VShader);
#endif
    if (VShader >= m_VertexShaders.Size() || DesiredFormat == 0)
        return FALSE;

    if (!m_Device)
        return FALSE;

    // Direct3D receives only a token pointer, not a byte count, so reject
    // malformed streams before copying or handing them to the runtime.
    if (!DX9ValidateShaderFunction(DesiredFormat->m_Function, DesiredFormat->m_FunctionSize))
        return FALSE;

    // Case 1: DesiredFormat is already the shader in our array (update existing)
    if (DesiredFormat == m_VertexShaders[VShader])
    {
        CKDX9VertexShaderDesc *desc = static_cast<CKDX9VertexShaderDesc *>(DesiredFormat);
        return desc->Create(this, DesiredFormat);
    }

    // Case 2: Need to create a new shader
    CKDX9VertexShaderDesc *newDesc = new CKDX9VertexShaderDesc;
    if (!newDesc)
        return FALSE;

    // Set initial data
    newDesc->Owner = this;
    newDesc->m_Function = NULL;
    newDesc->m_FunctionSize = 0;

    // Copy by validated DWORD count; rounding byte sizes with 32-bit addition
    // allowed m_FunctionSize + 3 to wrap and under-allocate the destination.
    if (!DX9CopyShaderFunction(DesiredFormat->m_Function,
                               DesiredFormat->m_FunctionSize,
                               &newDesc->m_Function))
    {
        delete newDesc;
        return FALSE;
    }
    newDesc->m_FunctionSize = DesiredFormat->m_FunctionSize;

    // Try to create the actual shader
    HRESULT hr = m_Device->CreateVertexShader(reinterpret_cast<const DWORD *>(newDesc->m_Function), &newDesc->DxShader);
    if (FAILED(hr))
    {
        // Handle device lost case with a retry
        if (hr == D3DERR_DEVICELOST)
        {
            // Keep the descriptor but mark it for recreation when device is recovered
            newDesc->DxShader = NULL;
        }
        else
        {
            // Clean up on other errors
            delete newDesc;
            return FALSE;
        }
    }

    // Clean up any existing shader
    CKVertexShaderDesc *oldShader = m_VertexShaders[VShader];
    if (oldShader)
    {
        delete oldShader;
    }

    // Store the new shader
    m_VertexShaders[VShader] = newDesc;
    return (newDesc->DxShader != NULL);
}

CKBOOL CKDX9RasterizerContext::CreatePixelShader(CKDWORD PShader, CKPixelShaderDesc *DesiredFormat)
{
#ifdef TRACY_ENABLE
    ZoneScopedN(__FUNCTION__);
#endif
#if LOGGING && LOG_CREATEPIXELSHADER
    fprintf(stderr, "create pixel shader %d\n", PShader);
#endif
    if (PShader >= m_PixelShaders.Size() || !DesiredFormat)
        return FALSE;

    // Validate required resources
    if (!m_Device)
        return FALSE;

    // Direct3D receives only a token pointer, not a byte count, so reject
    // malformed streams before copying or handing them to the runtime.
    if (!DX9ValidateShaderFunction(DesiredFormat->m_Function, DesiredFormat->m_FunctionSize))
        return FALSE;

    // Case 1: DesiredFormat is already the shader in our array (update existing)
    if (DesiredFormat == m_PixelShaders[PShader])
    {
        CKDX9PixelShaderDesc *desc = static_cast<CKDX9PixelShaderDesc *>(DesiredFormat);
        return desc->Create(this, DesiredFormat);
    }

    // Case 2: Need to create a new shader
    CKDX9PixelShaderDesc *newDesc = new CKDX9PixelShaderDesc;
    if (!newDesc)
        return FALSE;

    // Set initial data
    newDesc->Owner = this;
    newDesc->m_Function = NULL;
    newDesc->m_FunctionSize = 0;

    // Copy by validated DWORD count; rounding byte sizes with 32-bit addition
    // allowed m_FunctionSize + 3 to wrap and under-allocate the destination.
    if (!DX9CopyShaderFunction(DesiredFormat->m_Function,
                               DesiredFormat->m_FunctionSize,
                               &newDesc->m_Function))
    {
        delete newDesc;
        return FALSE;
    }
    newDesc->m_FunctionSize = DesiredFormat->m_FunctionSize;

    // Try to create the actual shader
    HRESULT hr = m_Device->CreatePixelShader(reinterpret_cast<const DWORD *>(newDesc->m_Function), &newDesc->DxShader);
    if (FAILED(hr))
    {
        // Handle device lost case with a retry
        if (hr == D3DERR_DEVICELOST)
        {
            // Keep the descriptor but mark it for recreation when device is recovered
            newDesc->DxShader = NULL;
        }
        else
        {
            // Clean up on other errors
            delete newDesc;
            return FALSE;
        }
    }

    // Clean up any existing shader
    CKPixelShaderDesc *oldShader = m_PixelShaders[PShader];
    if (oldShader)
    {
        delete oldShader;
    }

    // Store the new shader
    m_PixelShaders[PShader] = newDesc;
    return (newDesc->DxShader != NULL);
}

CKBOOL CKDX9RasterizerContext::CreateVertexBuffer(CKDWORD VB, CKVertexBufferDesc *DesiredFormat)
{
#ifdef TRACY_ENABLE
    ZoneScopedN(__FUNCTION__);
#endif
    if (VB >= m_VertexBuffers.Size() || !DesiredFormat || !m_Device)
        return FALSE;

    DWORD fvf = DesiredFormat->m_VertexFormat;
    DWORD size = DesiredFormat->m_VertexSize;
    if (size == 0 || DesiredFormat->m_MaxVertexCount == 0 ||
        DesiredFormat->m_MaxVertexCount > 0xFFFFFFFFu / size)
    {
        return FALSE;
    }

    DWORD usage = 0;
    if (DesiredFormat->m_Flags & CKRST_VB_DYNAMIC)
        usage |= D3DUSAGE_DYNAMIC;
    if (DesiredFormat->m_Flags & CKRST_VB_WRITEONLY)
        usage |= D3DUSAGE_WRITEONLY;
    if (m_SoftwareVertexProcessing)
        usage |= D3DUSAGE_SOFTWAREPROCESSING;

    IDirect3DVertexBuffer9 *buffer = NULL;
    if (FAILED(m_Device->CreateVertexBuffer(DesiredFormat->m_MaxVertexCount * size, usage, fvf, D3DPOOL_DEFAULT, &buffer, NULL)))
        return FALSE;

    if (m_VertexBuffers[VB] == DesiredFormat)
    {
        CKDX9VertexBufferDesc *desc = static_cast<CKDX9VertexBufferDesc *>(DesiredFormat);
        IDirect3DVertexBuffer9 *oldBuffer = desc->DxBuffer;
        desc->DxBuffer = buffer;
        desc->m_Flags |= CKRST_VB_VALID;
        SAFERELEASE(oldBuffer);
        return TRUE;
    }

    if (m_VertexBuffers[VB])
        delete m_VertexBuffers[VB];

    CKDX9VertexBufferDesc *desc = new CKDX9VertexBufferDesc;
    if (!desc)
    {
        SAFERELEASE(buffer);
        return FALSE;
    }
    desc->m_CurrentVCount = DesiredFormat->m_CurrentVCount;
    desc->m_VertexSize = size;
    desc->m_MaxVertexCount = DesiredFormat->m_MaxVertexCount;
    desc->m_VertexFormat = fvf;
    desc->m_Flags = DesiredFormat->m_Flags;
    desc->DxBuffer = buffer;
    desc->m_Flags |= CKRST_VB_VALID;
    m_VertexBuffers[VB] = desc;
    return TRUE;
}

CKBOOL CKDX9RasterizerContext::CreateIndexBuffer(CKDWORD IB, CKIndexBufferDesc *DesiredFormat)
{
#ifdef TRACY_ENABLE
    ZoneScopedN(__FUNCTION__);
#endif
    if (IB >= m_IndexBuffers.Size() || !DesiredFormat || !m_Device)
        return FALSE;

    if (DesiredFormat->m_MaxIndexCount == 0 ||
        DesiredFormat->m_MaxIndexCount > 0xFFFFFFFFu / sizeof(WORD))
    {
        return FALSE;
    }

    DWORD usage = D3DUSAGE_WRITEONLY;
    if ((DesiredFormat->m_Flags & CKRST_VB_DYNAMIC) != 0)
        usage |= D3DUSAGE_DYNAMIC;

    // Add software processing flag if necessary (for consistency with vertex buffers)
    if (m_SoftwareVertexProcessing)
        usage |= D3DUSAGE_SOFTWAREPROCESSING;

    // Check if we're updating an existing buffer
    CKDX9IndexBufferDesc *existingDesc = static_cast<CKDX9IndexBufferDesc *>(m_IndexBuffers[IB]);
    if (existingDesc == DesiredFormat)
    {
        // We're updating the same buffer descriptor
        LPDIRECT3DINDEXBUFFER9 oldBuffer = existingDesc->DxBuffer;

        LPDIRECT3DINDEXBUFFER9 newBuffer = NULL;
        if (FAILED(m_Device->CreateIndexBuffer(sizeof(WORD) * DesiredFormat->m_MaxIndexCount, usage, D3DFMT_INDEX16, D3DPOOL_DEFAULT, &newBuffer, NULL)))
        {
            return FALSE;
        }

        // Update descriptor
        existingDesc->DxBuffer = newBuffer;
        existingDesc->m_Flags |= CKRST_VB_VALID;

        SAFERELEASE(oldBuffer);
        return TRUE;
    }

    LPDIRECT3DINDEXBUFFER9 buffer = NULL;
    if (FAILED(m_Device->CreateIndexBuffer(sizeof(WORD) * DesiredFormat->m_MaxIndexCount, usage, D3DFMT_INDEX16, D3DPOOL_DEFAULT, &buffer, NULL)))
    {
        return FALSE;
    }

    // Clean up existing buffer if present
    if (m_IndexBuffers[IB])
        delete m_IndexBuffers[IB];

    CKDX9IndexBufferDesc *desc = new CKDX9IndexBufferDesc;
    if (!desc)
    {
        SAFERELEASE(buffer);
        return FALSE;
    }

    desc->m_CurrentICount = DesiredFormat->m_CurrentICount;
    desc->m_MaxIndexCount = DesiredFormat->m_MaxIndexCount;
    desc->m_Flags = DesiredFormat->m_Flags | CKRST_VB_VALID;
    desc->DxBuffer = buffer;
    m_IndexBuffers[IB] = desc;
    return TRUE;
}

CKBOOL CKDX9RasterizerContext::CreateVertexDeclaration(CKDWORD VFormat, LPDIRECT3DVERTEXDECLARATION9 *ppDecl)
{
#ifdef TRACY_ENABLE
    ZoneScopedN(__FUNCTION__);
#endif
    if (!ppDecl || !m_Device)
        return FALSE;

    *ppDecl = NULL;

    // Try to use D3DX utility function first (preferred method)
    if (D3DXDeclaratorFromFVF)
    {
        D3DVERTEXELEMENT9 declarator[MAX_FVF_DECL_SIZE];
        memset(declarator, 0, sizeof(declarator));

        HRESULT hr = D3DXDeclaratorFromFVF(VFormat, declarator);
        if (SUCCEEDED(hr))
        {
            hr = m_Device->CreateVertexDeclaration(declarator, ppDecl);
            if (SUCCEEDED(hr))
                return TRUE;
        }
    }

    // Manual declaration creation as fallback

    // Limit declaration element count (D3D9 supports up to 16 elements plus END)
    const int MAX_DECLARATION_ELEMENTS = 17;
    D3DVERTEXELEMENT9 declaration[MAX_FVF_DECL_SIZE];
    memset(declaration, 0, sizeof(declaration));

    // Track current element index and byte offset
    int elementIndex = 0;
    WORD offset = 0;
    BYTE stream = 0; // Default stream index

    // -------------------- POSITION ELEMENTS --------------------
    // Handle various position formats
    if (VFormat & CKRST_VF_POSITION)
    {
        declaration[elementIndex].Stream = stream;
        declaration[elementIndex].Offset = offset;
        declaration[elementIndex].Type = D3DDECLTYPE_FLOAT3;
        declaration[elementIndex].Method = D3DDECLMETHOD_DEFAULT;
        declaration[elementIndex].Usage = D3DDECLUSAGE_POSITION;
        declaration[elementIndex].UsageIndex = 0;

        offset += 12; // 3 floats
        elementIndex++;
    }
    else if (VFormat & CKRST_VF_RASTERPOS)
    {
        declaration[elementIndex].Stream = stream;
        declaration[elementIndex].Offset = offset;
        declaration[elementIndex].Type = D3DDECLTYPE_FLOAT4;
        declaration[elementIndex].Method = D3DDECLMETHOD_DEFAULT;
        declaration[elementIndex].Usage = D3DDECLUSAGE_POSITION;
        declaration[elementIndex].UsageIndex = 0;

        offset += 16; // 4 floats
        elementIndex++;
    }
    else if (VFormat & CKRST_VF_POSITIONMASK)
    {
        // Handle vertex blending with multiple weights
        int weightCount = CKRST_VF_GETWCOUNT(VFormat);
        if (weightCount > 0)
        {
            // Position (always 3 floats)
            declaration[elementIndex].Stream = stream;
            declaration[elementIndex].Offset = offset;
            declaration[elementIndex].Type = D3DDECLTYPE_FLOAT3;
            declaration[elementIndex].Method = D3DDECLMETHOD_DEFAULT;
            declaration[elementIndex].Usage = D3DDECLUSAGE_POSITION;
            declaration[elementIndex].UsageIndex = 0;
            offset += 12;
            elementIndex++;

            // Add blend weights (1-4 weights supported)
            declaration[elementIndex].Stream = stream;
            declaration[elementIndex].Offset = offset;

            // Map number of weights to appropriate declaration type
            switch (weightCount)
            {
                case 1: declaration[elementIndex].Type = D3DDECLTYPE_FLOAT1; offset += 4; break;
                case 2: declaration[elementIndex].Type = D3DDECLTYPE_FLOAT2; offset += 8; break;
                case 3: declaration[elementIndex].Type = D3DDECLTYPE_FLOAT3; offset += 12; break;
                case 4: default: declaration[elementIndex].Type = D3DDECLTYPE_FLOAT4; offset += 16; break;
            }

            declaration[elementIndex].Method = D3DDECLMETHOD_DEFAULT;
            declaration[elementIndex].Usage = D3DDECLUSAGE_BLENDWEIGHT;
            declaration[elementIndex].UsageIndex = 0;
            elementIndex++;

            // Add blend indices if matrix palette is used
            if (VFormat & CKRST_VF_MATRIXPAL)
            {
                declaration[elementIndex].Stream = stream;
                declaration[elementIndex].Offset = offset;
                declaration[elementIndex].Type = D3DDECLTYPE_UBYTE4;
                declaration[elementIndex].Method = D3DDECLMETHOD_DEFAULT;
                declaration[elementIndex].Usage = D3DDECLUSAGE_BLENDINDICES;
                declaration[elementIndex].UsageIndex = 0;
                offset += 4;
                elementIndex++;
            }
        }
    }

    // -------------------- NORMAL ELEMENTS --------------------
    // Handle normal data (tangent/binormal support for bump mapping)
    if (VFormat & CKRST_VF_NORMAL)
    {
        // Primary normal vector
        declaration[elementIndex].Stream = stream;
        declaration[elementIndex].Offset = offset;
        declaration[elementIndex].Type = D3DDECLTYPE_FLOAT3;
        declaration[elementIndex].Method = D3DDECLMETHOD_DEFAULT;
        declaration[elementIndex].Usage = D3DDECLUSAGE_NORMAL;
        declaration[elementIndex].UsageIndex = 0;
        offset += 12;
        elementIndex++;

        // Check for tangent/binormal vectors (could be indicated by special combination)
        // Tangent and binormal data handling could be added here if necessary
        // This would require defining custom flags or encoding in upper bits of VFormat
    }

    // -------------------- POINT SIZE --------------------
    if (VFormat & CKRST_VF_PSIZE)
    {
        declaration[elementIndex].Stream = stream;
        declaration[elementIndex].Offset = offset;
        declaration[elementIndex].Type = D3DDECLTYPE_FLOAT1;
        declaration[elementIndex].Method = D3DDECLMETHOD_DEFAULT;
        declaration[elementIndex].Usage = D3DDECLUSAGE_PSIZE;
        declaration[elementIndex].UsageIndex = 0;
        offset += 4;
        elementIndex++;
    }

    // -------------------- COLOR ELEMENTS --------------------
    if (VFormat & CKRST_VF_DIFFUSE)
    {
        declaration[elementIndex].Stream = stream;
        declaration[elementIndex].Offset = offset;
        declaration[elementIndex].Type = D3DDECLTYPE_D3DCOLOR;
        declaration[elementIndex].Method = D3DDECLMETHOD_DEFAULT;
        declaration[elementIndex].Usage = D3DDECLUSAGE_COLOR;
        declaration[elementIndex].UsageIndex = 0;
        offset += 4;
        elementIndex++;
    }

    if (VFormat & CKRST_VF_SPECULAR)
    {
        declaration[elementIndex].Stream = stream;
        declaration[elementIndex].Offset = offset;
        declaration[elementIndex].Type = D3DDECLTYPE_D3DCOLOR;
        declaration[elementIndex].Method = D3DDECLMETHOD_DEFAULT;
        declaration[elementIndex].Usage = D3DDECLUSAGE_COLOR;
        declaration[elementIndex].UsageIndex = 1;
        offset += 4;
        elementIndex++;
    }

    // -------------------- TEXTURE COORDINATE ELEMENTS --------------------
    // Get number of texture coordinate sets in the format
    DWORD texCount = CKRST_VF_GETTEXCOUNT(VFormat);

    // Process each texture coordinate set
    for (DWORD i = 0; i < texCount && i < 8 && elementIndex < MAX_DECLARATION_ELEMENTS - 1; i++)
    {
        // Get number of components for this texture coordinate set
        DWORD texDimension = 2; // Default is 2D (UV)

        // Check for texture coordinate dimension override (stored in bits 16-31)
        DWORD texFormat = (VFormat >> (16 + i * 2)) & 0x3;
        switch (texFormat)
        {
            case CKRST_VF_TEXFORMAT1: texDimension = 1; break; // 1D (U)
            case CKRST_VF_TEXFORMAT3: texDimension = 3; break; // 3D (UVW)
            case CKRST_VF_TEXFORMAT4: texDimension = 4; break; // 4D (UVWQ)
            default: texDimension = 2; break;                  // 2D (UV)
        }

        // Map dimension to D3D declaration type
        D3DDECLTYPE declType;
        switch (texDimension)
        {
            case 1: declType = D3DDECLTYPE_FLOAT1; break;
            case 3: declType = D3DDECLTYPE_FLOAT3; break;
            case 4: declType = D3DDECLTYPE_FLOAT4; break;
            default: declType = D3DDECLTYPE_FLOAT2; break;
        }
        

        // Determine appropriate texture coordinate usage type
        // For special bump mapping, use different semantics
        D3DDECLUSAGE usage = D3DDECLUSAGE_TEXCOORD;

        // Set up the declaration element
        declaration[elementIndex].Stream = stream;
        declaration[elementIndex].Offset = offset;
        declaration[elementIndex].Type = declType;
        declaration[elementIndex].Method = D3DDECLMETHOD_DEFAULT;
        declaration[elementIndex].Usage = usage;
        declaration[elementIndex].UsageIndex = (BYTE)i;

        // Update offset for next element
        offset += (WORD)(texDimension * sizeof(float));
        elementIndex++;
    }

    // Add terminator element (required by Direct3D 9)
    declaration[elementIndex].Stream = 0xFF;
    declaration[elementIndex].Offset = 0;
    declaration[elementIndex].Type = D3DDECLTYPE_UNUSED;
    declaration[elementIndex].Method = 0;
    declaration[elementIndex].Usage = 0;
    declaration[elementIndex].UsageIndex = 0;

    // Create the vertex declaration
    HRESULT hr = m_Device->CreateVertexDeclaration(declaration, ppDecl);
    return SUCCEEDED(hr);
}

void CKDX9RasterizerContext::FlushCaches()
{
#ifdef TRACY_ENABLE
    ZoneScopedN(__FUNCTION__);
#endif
    // Reset render state cache
    FlushRenderStateCache();
    ResetTextureStageStateCache();
    m_TextureStageStateCacheEnabled = FALSE;
    m_InverseWinding = FALSE;

    // First release any existing state blocks to prevent memory leaks
    ReleaseStateBlocks();

#if USE_D3DSTATEBLOCKS
    // Only proceed if device is valid and in a usable state
    if (!m_Device || FAILED(m_Device->TestCooperativeLevel()))
        return;

    HRESULT hr;

    // Create new state blocks for texture filtering and blending operations
    for (int i = 0; i < 8; i++) // For each texture stage
    {
        for (int j = 0; j < 8; j++) // For each filter type
        {
            // Create min filter state blocks
            hr = m_Device->BeginStateBlock();
            if (SUCCEEDED(hr))
            {
#if LOGGING && LOG_FLUSHCACHES
                fprintf(stderr, "begin TextureMinFilterStateBlocks %d %d -> 0x%x\n", i, j, hr);
#endif
                SetTextureStageState(i, CKRST_TSS_MINFILTER, j);
                hr = m_Device->EndStateBlock(&m_TextureMinFilterStateBlocks[j][i]);

#if LOGGING && LOG_FLUSHCACHES
                fprintf(stderr, "end TextureMinFilterStateBlocks %d %d -> 0x%x\n", i, j, hr);
#endif

                // If EndStateBlock failed, ensure pointer is NULL
                if (FAILED(hr))
                    m_TextureMinFilterStateBlocks[j][i] = NULL;
            }

            // Create mag filter state blocks
            hr = m_Device->BeginStateBlock();
            if (SUCCEEDED(hr))
            {
#if LOGGING && LOG_FLUSHCACHES
                fprintf(stderr, "begin TextureMagFilterStateBlocks %d %d -> 0x%x\n", i, j, hr);
#endif
                SetTextureStageState(i, CKRST_TSS_MAGFILTER, j);
                hr = m_Device->EndStateBlock(&m_TextureMagFilterStateBlocks[j][i]);

#if LOGGING && LOG_FLUSHCACHES
                fprintf(stderr, "end TextureMagFilterStateBlocks %d %d -> 0x%x\n", i, j, hr);
#endif

                // If EndStateBlock failed, ensure pointer is NULL
                if (FAILED(hr))
                    m_TextureMagFilterStateBlocks[j][i] = NULL;
            }
        }

        // Create texture map blend state blocks
        for (int k = 0; k < 10; k++)
        {
            hr = m_Device->BeginStateBlock();
            if (SUCCEEDED(hr))
            {
#if LOGGING && LOG_FLUSHCACHES
                fprintf(stderr, "begin TextureMapBlendStateBlocks %d %d -> 0x%x\n", i, k, hr);
#endif
                SetTextureStageState(i, CKRST_TSS_TEXTUREMAPBLEND, k);
                hr = m_Device->EndStateBlock(&m_TextureMapBlendStateBlocks[k][i]);

#if LOGGING && LOG_FLUSHCACHES
                fprintf(stderr, "end TextureMapBlendStateBlocks %d %d -> 0x%x\n", i, k, hr);
#endif

                // If EndStateBlock failed, ensure pointer is NULL
                if (FAILED(hr))
                    m_TextureMapBlendStateBlocks[k][i] = NULL;
            }
        }
    }
#endif
    m_TextureStageStateCacheEnabled = TRUE;
}

void CKDX9RasterizerContext::FlushNonManagedObjects()
{
#ifdef TRACY_ENABLE
    ZoneScopedN(__FUNCTION__);
#endif
    // Skip if device is null or we're already in create/destroy process
    if (!m_Device || m_InCreateDestroy)
        return;

    HRESULT hr;

    // Clear all texture stages first
    for (int i = 0; i < m_Driver->m_3DCaps.MaxNumberTextureStage; ++i)
    {
        hr = m_Device->SetTexture(i, NULL);
        if (FAILED(hr))
        {
#if LOGGING
            fprintf(stderr, "Failed to clear texture stage %d\n", i);
#endif
        }
    }

    // Reset indices and vertex streams
    hr = SetD3DIndices(NULL);
    if (FAILED(hr))
    {
#if LOGGING
        fprintf(stderr, "Failed to clear index buffer\n");
#endif
    }

    // Clear vertex streams (correctly using all parameters)
    for (UINT i = 0; i < 8; ++i) // DirectX 9 typically supports 8 streams
    {
        hr = m_Device->SetStreamSource(i, NULL, 0, 0);
        if (FAILED(hr))
        {
#if LOGGING
            fprintf(stderr, "Failed to clear vertex stream %d\n", i);
#endif
        }
    }

    // Clear vertex and pixel shaders
    hr = m_Device->SetVertexShader(NULL);
    if (FAILED(hr))
    {
#if LOGGING
        fprintf(stderr, "Failed to clear vertex shader\n");
#endif
    }

    hr = m_Device->SetPixelShader(NULL);
    if (FAILED(hr))
    {
#if LOGGING
        fprintf(stderr, "Failed to clear pixel shader\n");
#endif
    }

    // Reset render target and depth buffer
    if (m_DefaultBackBuffer)
    {
        m_Device->SetRenderTarget(0, m_DefaultBackBuffer);
        if (m_DefaultDepthBuffer)
            m_Device->SetDepthStencilSurface(m_DefaultDepthBuffer);
    }

    SAFERELEASE(m_DefaultBackBuffer);
    SAFERELEASE(m_DefaultDepthBuffer);

    // Reset current texture index
    m_CurrentTextureIndex = 0;
    m_RenderTargetTextureStageMask = 0;

    // Release default-pool texture resources while preserving their public
    // descriptors. The owner uses those descriptors and object indices to
    // recreate/reload content after reset.
    for (XArray<CKTextureDesc *>::Iterator it = m_Textures.Begin(); it != m_Textures.End(); ++it)
    {
        CKTextureDesc *desc = *it;
        if (desc && !(desc->Flags & CKRST_TEXTURE_MANAGED))
        {
            CKDX9TextureDesc *dx9Desc = static_cast<CKDX9TextureDesc *>(desc);
            if (dx9Desc->DxRenderTexture == dx9Desc->DxTexture)
                dx9Desc->DxRenderTexture = NULL;
            SAFERELEASE(dx9Desc->DxLockedSurface);
            SAFERELEASE(dx9Desc->DxTexture);
            SAFERELEASE(dx9Desc->DxRenderTexture);
            dx9Desc->LockedFlags = 0;
            dx9Desc->Flags &= ~CKRST_TEXTURE_VALID;
        }
    }

    // Release temporary Z-buffers
    ReleaseTempZBuffers();

    // CKRasterizer clients recreate vertex buffers after a device reset only
    // when their object slots are empty. Keeping invalid descriptors here makes
    // clients retain stale UI geometry and skip the required vertex upload.
    FlushObjects(CKRST_OBJ_VERTEXBUFFER);

    for (XArray<CKIndexBufferDesc *>::Iterator it = m_IndexBuffers.Begin(); it != m_IndexBuffers.End(); ++it)
    {
        CKDX9IndexBufferDesc *desc = static_cast<CKDX9IndexBufferDesc *>(*it);
        if (desc)
        {
            SAFERELEASE(desc->DxBuffer);
            desc->m_Flags &= ~CKRST_VB_VALID;
        }
    }

    // Shader descriptors own a bytecode copy, so retain it for immediate
    // recreation after Reset.
    for (XArray<CKVertexShaderDesc *>::Iterator it = m_VertexShaders.Begin(); it != m_VertexShaders.End(); ++it)
    {
        CKDX9VertexShaderDesc *desc = static_cast<CKDX9VertexShaderDesc *>(*it);
        if (desc)
            SAFERELEASE(desc->DxShader);
    }

    for (XArray<CKPixelShaderDesc *>::Iterator it = m_PixelShaders.Begin(); it != m_PixelShaders.End(); ++it)
    {
        CKDX9PixelShaderDesc *desc = static_cast<CKDX9PixelShaderDesc *>(*it);
        if (desc)
            SAFERELEASE(desc->DxShader);
    }

    // Release static index buffers
    ReleaseIndexBuffers();

    // Reset vertex format cache
    ClearStreamCache();
    ResetTextureStageStateCache();
}

void CKDX9RasterizerContext::RestoreDeviceResources()
{
    for (XArray<CKVertexShaderDesc *>::Iterator it = m_VertexShaders.Begin(); it != m_VertexShaders.End(); ++it)
    {
        CKDX9VertexShaderDesc *desc = static_cast<CKDX9VertexShaderDesc *>(*it);
        if (desc && !desc->Create(this, desc))
            CKDX9_LOG("RestoreDeviceResources failed to recreate vertex shader desc=%p", desc);
    }

    for (XArray<CKPixelShaderDesc *>::Iterator it = m_PixelShaders.Begin(); it != m_PixelShaders.End(); ++it)
    {
        CKDX9PixelShaderDesc *desc = static_cast<CKDX9PixelShaderDesc *>(*it);
        if (desc && !desc->Create(this, desc))
            CKDX9_LOG("RestoreDeviceResources failed to recreate pixel shader desc=%p", desc);
    }
}

void CKDX9RasterizerContext::ReleaseStateBlocks()
{
    SAFERELEASE(m_SpriteStateBlock);
    if (m_Device)
    {
        for (int i = 0; i < 8; i++)
        {
            for (int j = 0; j < 8; j++)
            {
                SAFERELEASE(m_TextureMinFilterStateBlocks[j][i]);
                SAFERELEASE(m_TextureMagFilterStateBlocks[j][i]);
            }

            for (int k = 0; k < 10; k++)
            {
                SAFERELEASE(m_TextureMapBlendStateBlocks[k][i]);
            }
        }

        memset(m_TextureMinFilterStateBlocks, 0, sizeof(m_TextureMinFilterStateBlocks));
        memset(m_TextureMagFilterStateBlocks, 0, sizeof(m_TextureMagFilterStateBlocks));
        memset(m_TextureMapBlendStateBlocks, 0, sizeof(m_TextureMapBlendStateBlocks));
    }
}

void CKDX9RasterizerContext::ReleaseIndexBuffers()
{
    // Make sure device isn't using the index buffers
    if (m_Device)
    {
        SetD3DIndices(NULL);
    }

    // Properly release index buffers with proper cleanup
    for (int i = 0; i < 2; ++i)
    {
        if (m_IndexBuffer[i])
        {
            SAFERELEASE(m_IndexBuffer[i]->DxBuffer);
            delete m_IndexBuffer[i];
            m_IndexBuffer[i] = NULL;
        }
    }
}

void CKDX9RasterizerContext::ClearStreamCache()
{
    // Clear vertex stream cache
    m_CurrentVertexBufferCache = NULL;
    m_CurrentVertexSizeCache = 0;
    m_CurrentVertexFormatCache = 0;
    m_CurrentVertexShaderCache = 0;
    m_CurrentPixelShaderCache = 0;

    // Reset device stream sources if device is available
    if (m_Device)
    {
        // Clear all possible stream sources (typically up to 8 for DX9)
        for (UINT i = 0; i < 8; ++i)
        {
            m_Device->SetStreamSource(i, NULL, 0, 0);
        }

        // Reset FVF and shader states
        m_Device->SetFVF(0);
        m_Device->SetVertexShader(NULL);
    }
}

void CKDX9RasterizerContext::ReleaseScreenBackup()
{
    if (m_InCreateDestroy)
        return;

    // If in transparent mode, notify that screen backup is no longer available
    if (m_TransparentMode)
    {
        // Add dirty rect to indicate the whole screen needs redrawing
        CKRECT rect = {0, 0, (int)m_Width, (int)m_Height};
        AddDirtyRect(&rect);
    }

    SAFERELEASE(m_ScreenBackup);
}

void CKDX9RasterizerContext::ReleaseVertexDeclarations()
{
    // Make sure we're not in the middle of creating/destroying resources
    if (m_InCreateDestroy)
        return;

    // Clear any active vertex declaration from the device
    if (m_Device)
    {
        m_Device->SetVertexDeclaration(NULL);
    }

    // Release all declarations in the hash table
    for (XNHashTable<LPDIRECT3DVERTEXDECLARATION9, DWORD>::Iterator it = m_VertexDeclarations.Begin(); it != m_VertexDeclarations.End(); ++it)
    {
        LPDIRECT3DVERTEXDECLARATION9 decl = *it;
        SAFERELEASE(decl);
    }

    // Clear the hash table
    m_VertexDeclarations.Clear();

    // Reset vertex format cache to force recreation of declarations
    m_CurrentVertexFormatCache = 0;
}

CKDWORD CKDX9RasterizerContext::DX9PresentInterval(DWORD PresentInterval)
{
    // Map the input interval to D3D9 presentation intervals
    switch (PresentInterval)
    {
        case 0: return D3DPRESENT_INTERVAL_IMMEDIATE;
        case 1: return D3DPRESENT_INTERVAL_ONE;
        case 2: return D3DPRESENT_INTERVAL_TWO;
        case 3: return D3DPRESENT_INTERVAL_THREE;
        case 4: return D3DPRESENT_INTERVAL_FOUR;
        default: return D3DPRESENT_INTERVAL_DEFAULT;
    }
}

CKBOOL CKDX9RasterizerContext::LoadSurface(const D3DSURFACE_DESC &ddsd, const D3DLOCKED_RECT &LockRect, const VxImageDescEx &SurfDesc)
{
#ifdef TRACY_ENABLE
    ZoneScopedN(__FUNCTION__);
#endif
    if (!LockRect.pBits || LockRect.Pitch <= 0 || ddsd.Width == 0 || ddsd.Height == 0 ||
        ddsd.Width > static_cast<UINT>(INT_MAX) ||
        ddsd.Height > static_cast<UINT>(INT_MAX) ||
        !DX9ValidateImageDesc(SurfDesc))
        return FALSE;

    // Set up target image descriptor
    VxImageDescEx desc;
    desc.Size = sizeof(VxImageDescEx);

    // Get pixel format from the DirectX surface format
    VX_PIXELFORMAT vxpf = D3DFormatToVxPixelFormat(ddsd.Format);
    if (vxpf == UNKNOWN_PF)
    {
        // Fallback to a compatible format if we couldn't determine the format
        if (ddsd.Format == D3DFMT_A8R8G8B8 || ddsd.Format == D3DFMT_X8R8G8B8)
            vxpf = _32_ARGB8888;
        else if (ddsd.Format == D3DFMT_R8G8B8)
            vxpf = _24_RGB888;
        else if (ddsd.Format == D3DFMT_R5G6B5)
            vxpf = _16_RGB565;
        else if (ddsd.Format == D3DFMT_A1R5G5B5 || ddsd.Format == D3DFMT_X1R5G5B5)
            vxpf = _16_ARGB1555;
        else
            return FALSE; // Couldn't determine a compatible format
    }

    // Block-compressed surfaces are organized as rows of 4x4 blocks, not
    // pixel rows. They must be uploaded through the D3DX path above.
    if (DX9IsBlockCompressedFormat(ddsd.Format))
        return FALSE;

    // Fill in target descriptor
    VxPixelFormat2ImageDesc(vxpf, desc);
    desc.Width = ddsd.Width;
    desc.Height = ddsd.Height;
    desc.BytesPerLine = LockRect.Pitch;
    desc.Image = static_cast<XBYTE *>(LockRect.pBits);

    const size_t targetWidth = static_cast<size_t>(ddsd.Width);
    const size_t targetBitsPerPixel = static_cast<size_t>(desc.BitsPerPixel);
    if (desc.BitsPerPixel <= 0 ||
        targetWidth > (DX9_SIZE_MAX - 7u) / targetBitsPerPixel)
    {
        return FALSE;
    }
    const size_t targetRowBytes = (targetWidth * targetBitsPerPixel + 7u) / 8u;
    if (static_cast<size_t>(LockRect.Pitch) < targetRowBytes ||
        static_cast<size_t>(ddsd.Height) >
            DX9_SIZE_MAX / static_cast<size_t>(LockRect.Pitch))
    {
        return FALSE;
    }

    // Check if we need format conversion
    if (vxpf != VxImageDesc2PixelFormat(SurfDesc) || desc.Width != SurfDesc.Width || desc.Height != SurfDesc.Height)
    {
        // Locked block-compressed surfaces can only be handled safely by the higher-level D3DX upload path.
        if (ddsd.Format >= D3DFMT_DXT1 && ddsd.Format <= D3DFMT_DXT5)
        {
            return FALSE;
        }

        // Perform the blitting operation with format conversion
        VxDoBlit(SurfDesc, desc);
        return TRUE;
    }
    else
    {
        // Formats match - perform straight memory copy
        const int height = SurfDesc.Height;
        const int srcStride = SurfDesc.BytesPerLine;
        const int dstStride = LockRect.Pitch;
        const size_t bytesPerRow =
            (static_cast<size_t>(SurfDesc.Width) *
             static_cast<size_t>(SurfDesc.BitsPerPixel) + 7u) / 8u;

        // If stride matches, do a single copy
        if (srcStride == dstStride && static_cast<size_t>(srcStride) == bytesPerRow)
        {
            memcpy(LockRect.pBits, SurfDesc.Image,
                   static_cast<size_t>(srcStride) * static_cast<size_t>(height));
        }
        else
        {
            // Copy the pixels only; source and destination padding need not match.
            XBYTE *src = SurfDesc.Image;
            XBYTE *dst = static_cast<XBYTE *>(LockRect.pBits);

            for (int y = 0; y < height; ++y)
            {
                memcpy(dst, src, bytesPerRow);
                src += srcStride;
                dst += dstStride;
            }
        }

        return TRUE;
    }
}

LPDIRECT3DSURFACE9 CKDX9RasterizerContext::GetTempZBuffer(int Width, int Height)
{
#ifdef TRACY_ENABLE
    ZoneScopedN(__FUNCTION__);
#endif
    // Validate inputs and device
    if (Width <= 0 || Height <= 0 || !m_Device)
        return NULL;

    // Find appropriate power-of-two dimensions to use as index
    int widthPow2 = 1;
    while (widthPow2 < Width)
        widthPow2 <<= 1;

    int heightPow2 = 1;
    while (heightPow2 < Height)
        heightPow2 <<= 1;

    // Calculate index in z-buffer array
    // Use most significant bits for a more evenly distributed index
    CKDWORD widthLog2 = 0;
    CKDWORD heightLog2 = 0;

    // Calculate log2 of width and height
    if (widthPow2 > 1)
        widthLog2 = GetMsb(widthPow2, 15); // Limit to 4 bits (0-15)

    if (heightPow2 > 1)
        heightLog2 = GetMsb(heightPow2, 15); // Limit to 4 bits (0-15)

    // Combine into a single index (4 bits width, 4 bits height)
    CKDWORD index = (heightLog2 << 4) | widthLog2;

    // Ensure index is within bounds
    if (index >= NBTEMPZBUFFER)
    {
        // If index too big, use the largest buffer (at index NBTEMPZBUFFER-1)
        index = NBTEMPZBUFFER - 1;
    }

    // Check if we already have a buffer at this index
    LPDIRECT3DSURFACE9 surface = m_TempZBuffers[index];
    if (surface)
    {
        // Verify surface is compatible
        D3DSURFACE_DESC desc;
        if (SUCCEEDED(surface->GetDesc(&desc)) &&
            desc.Width >= (UINT)Width && desc.Height >= (UINT)Height &&
            desc.MultiSampleType == D3DMULTISAMPLE_NONE)
        {
            // Buffer is large enough, reuse it
            return surface;
        }

        // Not compatible - release existing buffer
        SAFERELEASE(m_TempZBuffers[index]);
    }

    // Create a new Z-buffer - use at least the requested dimensions
    // Render-target textures created by D3D9 are not multisampled, so their
    // depth surface must also be non-multisampled regardless of the swap chain.
    HRESULT hr = m_Device->CreateDepthStencilSurface(
        widthPow2, heightPow2,
        m_PresentParams.AutoDepthStencilFormat,
        D3DMULTISAMPLE_NONE,
        0,
        TRUE, // Discard lock hint for better performance
        &m_TempZBuffers[index],
        NULL);
    if (FAILED(hr))
    {
        // Final attempt with a common format. It still has to be compatible
        // with the non-MSAA render target.
        hr = m_Device->CreateDepthStencilSurface(
            widthPow2, heightPow2,
            D3DFMT_D24S8,
            D3DMULTISAMPLE_NONE,
            0,
            TRUE,
            &m_TempZBuffers[index],
            NULL);
    }

    return (SUCCEEDED(hr)) ? m_TempZBuffers[index] : NULL;
}
