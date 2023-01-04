#pragma once
#include "CKRasterizer.h"
#include "XBitArray.h"
#include <Windows.h>

#include "GL/glew.h"

#include "GLFW/glfw3.h"
#include <gl/GL.h>
//#include "GL/glext.h"
#include "gl/wglext.h"

#define GLFW_EXPOSE_NATIVE_WIN32
#include "GLFW/glfw3native.h"


class CKGLRasterizerContext;

class CKGLRasterizer : public CKRasterizer
{
public:
	CKGLRasterizer(void);
	virtual ~CKGLRasterizer(void);

	virtual XBOOL Start(WIN_HANDLE AppWnd);
	virtual void Close(void);

public:
	XBOOL m_Init;
	WNDCLASSEXA m_WndClass;
};

class CKGLRasterizerDriver: public CKRasterizerDriver
{
public:
	CKGLRasterizerDriver(CKGLRasterizer *rst);
    virtual ~CKGLRasterizerDriver();
	virtual CKRasterizerContext *CreateContext();
	
    CKBOOL InitializeCaps();
	
public:
    CKBOOL m_Inited;
    UINT m_AdapterIndex;
	WNDCLASSEXA m_WndClass;
};

typedef struct CKGLVertexBufferDesc : public CKVertexBufferDesc
{
public:
    GLuint GLBuffer;
public:
    CKGLVertexBufferDesc() { GLBuffer = 0; }
    ~CKGLVertexBufferDesc() { glDeleteBuffers(1, &GLBuffer); }
} CKGLVertexBufferDesc;

typedef struct CKGLIndexBufferDesc : public CKIndexBufferDesc
{
public:
    GLuint GLBuffer;
public:
    CKGLIndexBufferDesc() { GLBuffer = 0; }
    ~CKGLIndexBufferDesc() { glDeleteBuffers(1, &GLBuffer); }
} CKGLIndexBufferDesc;

typedef struct CKGLVertexShaderDesc : public CKVertexShaderDesc
{
public:
    GLuint GLShader;
    CKGLRasterizerContext *Owner;
    XArray<BYTE> m_FunctionData;

public:
    BOOL Create(CKGLRasterizerContext *Ctx, CKVertexShaderDesc *Format);
    virtual ~CKGLVertexShaderDesc();
    CKGLVertexShaderDesc()
    {
        GLShader = 0;
        Owner = NULL;
    }
} CKGLVertexShaderDesc;

typedef struct CKGLPixelShaderDesc : public CKPixelShaderDesc
{
public:
    GLuint GLShader;
    CKGLRasterizerContext *Owner;
    XArray<BYTE> m_FunctionData;
public:
    BOOL Create(CKGLRasterizerContext *Ctx, CKGLPixelShaderDesc *Format);
    virtual ~CKGLPixelShaderDesc();
    CKGLPixelShaderDesc()
    {
        GLShader = 0;
        Owner = NULL;
    }
} CKGLPixelShaderDesc;

class CKGLRasterizerContext : public CKRasterizerContext
{
public:
    //--- Construction/destruction
    CKGLRasterizerContext();
    virtual ~CKGLRasterizerContext();

    //--- Creation
    virtual CKBOOL Create(WIN_HANDLE Window, int PosX = 0, int PosY = 0, int Width = 0, int Height = 0, int Bpp = -1,
                          CKBOOL Fullscreen = 0, int RefreshRate = 0, int Zbpp = -1, int StencilBpp = -1);
    //---
    virtual CKBOOL Resize(int PosX = 0, int PosY = 0, int Width = 0, int Height = 0, CKDWORD Flags = 0);
    virtual CKBOOL Clear(CKDWORD Flags = CKRST_CTXCLEAR_ALL, CKDWORD Ccol = 0, float Z = 1.0f, CKDWORD Stencil = 0,
                         int RectCount = 0, CKRECT *rects = NULL);
    virtual CKBOOL BackToFront(CKBOOL vsync);

    //--- Scene
    virtual CKBOOL BeginScene();
    virtual CKBOOL EndScene();

    //--- Lighting & Material States
    virtual CKBOOL SetLight(CKDWORD Light, CKLightData *data);
    virtual CKBOOL EnableLight(CKDWORD Light, CKBOOL Enable);
    virtual CKBOOL SetMaterial(CKMaterialData *mat);

    //--- Viewport State
    virtual CKBOOL SetViewport(CKViewportData *data);

    //--- Transform Matrix
    virtual CKBOOL SetTransformMatrix(VXMATRIX_TYPE Type, const VxMatrix &Mat);

    //--- Render states
    virtual CKBOOL SetRenderState(VXRENDERSTATETYPE State, CKDWORD Value);
    virtual CKBOOL GetRenderState(VXRENDERSTATETYPE State, CKDWORD *Value);

    //--- Texture States
    virtual CKBOOL SetTexture(CKDWORD Texture, int Stage = 0);
    virtual CKBOOL SetTextureStageState(int Stage, CKRST_TEXTURESTAGESTATETYPE Tss, CKDWORD Value);

    //--- Vertex & Pixel shaders
    virtual CKBOOL SetVertexShader(CKDWORD VShaderIndex);
    virtual CKBOOL SetPixelShader(CKDWORD PShaderIndex);
    virtual CKBOOL SetVertexShaderConstant(CKDWORD Register, const void *Data, CKDWORD CstCount);
    virtual CKBOOL SetPixelShaderConstant(CKDWORD Register, const void *Data, CKDWORD CstCount);

    //--- Drawing
    virtual CKBOOL DrawPrimitive(VXPRIMITIVETYPE pType, CKWORD *indices, int indexcount, VxDrawPrimitiveData *data);
    virtual CKBOOL DrawPrimitiveVB(VXPRIMITIVETYPE pType, CKDWORD VertexBuffer, CKDWORD StartIndex, CKDWORD VertexCount,
                                   CKWORD *indices = NULL, int indexcount = NULL);
    virtual CKBOOL DrawPrimitiveVBIB(VXPRIMITIVETYPE pType, CKDWORD VB, CKDWORD IB, CKDWORD MinVIndex,
                                     CKDWORD VertexCount, CKDWORD StartIndex, int Indexcount);

    //--- Creation of Textures, Sprites and Vertex Buffer
    CKBOOL CreateObject(CKDWORD ObjIndex, CKRST_OBJECTTYPE Type, void *DesiredFormat) override;

    //--- Vertex Buffers
    virtual void *LockVertexBuffer(CKDWORD VB, CKDWORD StartVertex, CKDWORD VertexCount,
                                   CKRST_LOCKFLAGS Lock = CKRST_LOCK_DEFAULT);
    virtual CKBOOL UnlockVertexBuffer(CKDWORD VB);

    //--- Textures
    virtual CKBOOL LoadTexture(CKDWORD Texture, const VxImageDescEx &SurfDesc, int miplevel = -1);
    virtual CKBOOL CopyToTexture(CKDWORD Texture, VxRect *Src, VxRect *Dest, CKRST_CUBEFACE Face = CKRST_CUBEFACE_XPOS);
    //-- Sets the rendering to occur on a texture (reset the texture format to match )
    virtual CKBOOL SetTargetTexture(CKDWORD TextureObject, int Width = 0, int Height = 0,
                                    CKRST_CUBEFACE Face = CKRST_CUBEFACE_XPOS, CKBOOL GenerateMipMap = FALSE);

    //--- Sprites
    virtual CKBOOL DrawSprite(CKDWORD Sprite, VxRect *src, VxRect *dst);

    //--- Utils
    virtual int CopyToMemoryBuffer(CKRECT *rect, VXBUFFER_TYPE buffer, VxImageDescEx &img_desc);
    virtual int CopyFromMemoryBuffer(CKRECT *rect, VXBUFFER_TYPE buffer, const VxImageDescEx &img_desc);
    
    virtual CKBOOL SetUserClipPlane(CKDWORD ClipPlaneIndex, const VxPlane &PlaneEquation);
    virtual CKBOOL GetUserClipPlane(CKDWORD ClipPlaneIndex, VxPlane &PlaneEquation);

    virtual void *LockIndexBuffer(CKDWORD IB, CKDWORD StartIndex, CKDWORD IndexCount,
                                  CKRST_LOCKFLAGS Lock = CKRST_LOCK_DEFAULT);
    virtual CKBOOL UnlockIndexBuffer(CKDWORD IB);
protected:
    //--- Objects creation
    CKBOOL CreateTexture(CKDWORD Texture, CKTextureDesc *DesiredFormat);
    CKBOOL CreateVertexShader(CKDWORD VShader, CKVertexShaderDesc *DesiredFormat);
    CKBOOL CreatePixelShader(CKDWORD PShader, CKPixelShaderDesc *DesiredFormat);
    CKBOOL CreateVertexBuffer(CKDWORD VB, CKVertexBufferDesc *DesiredFormat);
    CKBOOL CreateIndexBuffer(CKDWORD IB, CKIndexBufferDesc *DesiredFormat);

    //---- Cleanup
    void FlushCaches();
    void FlushNonManagedObjects();
    void ReleaseStateBlocks();
    void ReleaseBuffers();
    void ClearStreamCache();
    void ReleaseScreenBackup();
public:
    CKGLRasterizer *m_Owner;
    GLFWwindow* m_GLFWwindow;
    HDC m_DC;
};
