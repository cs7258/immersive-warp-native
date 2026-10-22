#pragma once
#include "Common.h"
#include <memory>
#include <mutex>

// Engine: shared D3D11 device, per-output desktop duplication + warp/blend renderer.
class Engine {
public:
    static Engine& inst();

    bool init();                       // create device + shaders
    void shutdown();
    bool loadImage(const std::wstring& path);   // WIC -> texture (image source)
    bool hasImage() const { return m_imgSRV != nullptr; }
    int imageWidth() const { return m_imgW; }
    int imageHeight() const { return m_imgH; }

    // windows are created by Windows.cpp; engine attaches a render target per output
    bool attachOutput(int displayIdx, HWND hwnd);
    void detachAll();

    void renderAll();                  // one frame over all outputs (called from render thread)

private:
    Engine() = default;

    struct OutputCtx {
        HWND hwnd = nullptr;
        int displayIdx = -1;
        UINT w = 0, h = 0;
        IDXGIOutput1* output1 = nullptr;
        IDXGIOutputDuplication* dupl = nullptr;
        ID3D11Texture2D* frameTex = nullptr;      // cached copy of last duplicated frame
        ID3D11ShaderResourceView* srv = nullptr;
        bool frameValid = false;
        IDXGISwapChain* swap = nullptr;
        ID3D11Texture2D* backBuf = nullptr;
        ID3D11RenderTargetView* rtv = nullptr;
        ID3D11Buffer* meshVB = nullptr;
        ID3D11Buffer* meshIB = nullptr;
        UINT indexCount = 0;
        int meshRev = -1;
        ID3D11Buffer* frameCB = nullptr;
        ID3D11Buffer* overlayVB = nullptr;
        UINT overlayLineVtx = 0;   // drawn as LINELIST
        UINT overlayTriVtx = 0;    // drawn as TRIANGLELIST, starts after line verts
    };

    bool compileShaders();
    bool createDuplication(OutputCtx& oc);   // picks a source display that is NOT an output (no recursion)
    void resizeSwapChain(OutputCtx& oc, UINT w, UINT h);
    void captureFrame(OutputCtx& oc);
    void buildMesh(OutputCtx& oc, const OutputCfg& cfg);
    void buildOverlay(OutputCtx& oc, const OutputCfg& cfg);
    void updateFrameCB(OutputCtx& oc, const OutputCfg& cfg);
    void drawOutput(OutputCtx& oc, const OutputCfg& cfg);

    ID3D11Device* m_dev = nullptr;
    ID3D11DeviceContext* m_ctx = nullptr;
    IDXGIFactory1* m_factory = nullptr;

    ID3D11VertexShader* m_vs = nullptr;
    ID3D11PixelShader* m_ps = nullptr;
    ID3D11VertexShader* m_vsOv = nullptr;
    ID3D11PixelShader* m_psOv = nullptr;
    ID3D11InputLayout* m_layout = nullptr;
    ID3D11InputLayout* m_layoutOv = nullptr;
    ID3D11SamplerState* m_sampler = nullptr;
    // image source. Old textures are retired (not released) on reload so the
    // render thread can never touch a freed object mid-frame.
    ID3D11Texture2D* m_imgTex = nullptr;
    ID3D11ShaderResourceView* m_imgSRV = nullptr;
    int m_imgW = 0, m_imgH = 0;
    std::vector<ID3D11ShaderResourceView*> m_imgGarbageSRV;
    std::vector<ID3D11Texture2D*> m_imgGarbageTex;
    ID3D11BlendState* m_blendOpaque = nullptr;
    ID3D11RasterizerState* m_raster = nullptr;

    // shared_ptr + mutex: the render thread snapshots this list each frame,
    // while the UI thread may destroy/rebuild outputs at any moment (preview
    // checkbox, Toggle, Start/Stop). Refcounting guarantees a ctx outlives the
    // frame that is currently drawing it.
    std::vector<std::shared_ptr<OutputCtx>> m_outs;
    std::mutex m_outsMutex;
};
