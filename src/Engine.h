#pragma once
#include "Common.h"
#include <d3d11_1.h>   // ID3D11DeviceContext1 (ClearView, recursion selftest)
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
    void setSampling(int q);                    // 0 linear, 1 aniso 4x, 2 aniso 8x, 3 bicubic
    int sampling() const { return m_sampIdx.load(); }
    int imageWidth() const { return m_imgW; }
    int imageHeight() const { return m_imgH; }
    // built-in test image library (like the original's Images page):
    // 0 = color bars, 1 = checkerboard, 2 = gray ramp, 3 = identity grid, -1 = clear
    bool setTestImage(int id);
    int testImage() const { return m_testImg; }

    // windows are created by Windows.cpp; engine attaches a render target per output
    bool attachOutput(int displayIdx, HWND hwnd);
    void detachAll();

    void renderAll();                  // one frame over all outputs (called from render thread)

    // diagnostics for CMDNET "GET perf" (remote health monitoring)
    unsigned long long frames() const { return m_frameSeq.load(); }
    double fps() const { return m_fpsX10.load() / 10.0; }   // rolling 1s window
    // per-stage frame timings, EMA smoothed, fixed point x100 ms
    // (render thread is the only writer; GET perf reads atomically)
    double capMs() const { return m_capMs100.load() / 100.0; }    // desktop duplication
    double drawMs() const { return m_drawMs100.load() / 100.0; }  // mesh+CB+draw+present
    double frameMs() const { return m_frameMs100.load() / 100.0; }// whole renderAll
    // end-to-end latency: QPC of the newest duplicated desktop frame
    // (DXGI_OUTDUPL_FRAME_INFO.LastPresentTime) -> Present. 0 until the first
    // real frame; not measured on the MAG engine (no duplication timestamps).
    double latMs() const { return m_latMs100.load() / 100.0; }

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
        // DD cursor (pointer): captured with the desktop frame, drawn warped on
        // top of the warp surface so the mouse tracks the geometry.
        UINT srcW = 0, srcH = 0;             // source desktop size (cursor UV)
        ID3D11Texture2D* cursorTex = nullptr;
        ID3D11ShaderResourceView* cursorSRV = nullptr;
        ID3D11Buffer* cursorVB = nullptr;
        UINT cursorW = 0, cursorH = 0;
        int cursorHotX = 0, cursorHotY = 0;
        int cursorX = 0, cursorY = 0;       // source-desktop pixel position
        bool cursorVisible = false;
        bool cursorValid = false;           // a cursor texture exists
        DXGI_OUTDUPL_POINTER_SHAPE_INFO cursorShape{};
        bool exclusive = false;             // took exclusive fullscreen
        bool frameValid = false;
        LONGLONG lastPresentQpc = 0;  // QPC of newest acquired desktop frame
                                      // (frame info LastPresentTime; 0 = none)
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
        // Releases every COM object. Runs when the last shared_ptr goes away,
        // i.e. the render thread is no longer drawing this ctx. Without it a
        // rebuild (preview toggle, source change) leaked the duplication and
        // the next DuplicateOutput on the same output failed with E_INVALIDARG.
        ~OutputCtx();
    };

    bool compileShaders();
    bool createDuplication(OutputCtx& oc);   // picks a source display that is NOT an output (no recursion)
    void resizeSwapChain(OutputCtx& oc, UINT w, UINT h);
    void captureFrame(OutputCtx& oc);
    void buildMesh(OutputCtx& oc, const OutputCfg& cfg);
    void buildOverlay(OutputCtx& oc, const OutputCfg& cfg);
    void updateFrameCB(OutputCtx& oc, const OutputCfg& cfg);
    void drawOutput(OutputCtx& oc, const OutputCfg& cfg);
    static void updateCursorTexture(ID3D11Device* dev, OutputCtx& oc, const BYTE* buf, const DXGI_OUTDUPL_POINTER_SHAPE_INFO& info);
    void captureSnapshot(OutputCtx& oc);   // swapchain back buffer -> PNG (g_snapReq)

    ID3D11Device* m_dev = nullptr;
    ID3D11DeviceContext* m_ctx = nullptr;
    IDXGIFactory1* m_factory = nullptr;

    ID3D11VertexShader* m_vs = nullptr;
    ID3D11PixelShader* m_ps = nullptr;
    ID3D11VertexShader* m_vsOv = nullptr;
    ID3D11PixelShader* m_psOv = nullptr;
    ID3D11InputLayout* m_layout = nullptr;
    ID3D11InputLayout* m_layoutOv = nullptr;
    // three prebuilt samplers (linear / aniso4x / aniso8x); the render thread
    // indexes by an atomic so quality can be switched without touching D3D
    // objects from the UI thread
    ID3D11SamplerState* m_samplers[4] = { nullptr, nullptr, nullptr, nullptr };
    std::atomic<int> m_sampIdx{ 0 };
    // dynamic test pattern phase (render-thread-only state, no locking)
    double m_animAcc = 0.0;
    ULONGLONG m_animTick = 0;
    // image source. Old textures are retired (not released) on reload so the
    // render thread can never touch a freed object mid-frame.
    ID3D11Texture2D* m_imgTex = nullptr;
    ID3D11ShaderResourceView* m_imgSRV = nullptr;
    int m_imgW = 0, m_imgH = 0;
    int m_testImg = -1;
    std::vector<ID3D11ShaderResourceView*> m_imgGarbageSRV;
    std::vector<ID3D11Texture2D*> m_imgGarbageTex;

    void clearImage();
    bool uploadImage(UINT w, UINT h, const std::vector<BYTE>& bgra, const char* tag);
    ID3D11BlendState* m_blendOpaque = nullptr;
    ID3D11BlendState* m_blendAlpha = nullptr;   // cursor: SRC_ALPHA / INV_SRC_ALPHA
    ID3D11VertexShader* m_vsCur = nullptr;
    ID3D11PixelShader* m_psCur = nullptr;
    ID3D11InputLayout* m_layoutCur = nullptr;
    std::atomic<bool> m_deviceLost{ false };
    ID3D11RasterizerState* m_raster = nullptr;

    // recursion selftest: paint a magenta marker into our own scanout, then
    // read back the Desktop Duplication capture - if the marker shows up in
    // the captured desktop frame, our own output leaked into the capture
    // (warp feedback loop). 0=off 1=marker frames 2=readback attempts 3=done
    ID3D11DeviceContext1* m_ctx1 = nullptr;   // ClearView (marker rect)
    int m_recSelftest = 0;
    int m_recFrames = 0;
    int m_recTries = 0;
    bool m_recDetected = false;
    void recursionCheck(OutputCtx& oc);

    // shared_ptr + mutex: the render thread snapshots this list each frame,
    // while the UI thread may destroy/rebuild outputs at any moment (preview
    // checkbox, Toggle, Start/Stop). Refcounting guarantees a ctx outlives the
    // frame that is currently drawing it.
    std::vector<std::shared_ptr<OutputCtx>> m_outs;
    std::mutex m_outsMutex;
    UINT m_frameCount = 0;
    // bumped once per rendered frame; detachAll waits for one bump so the
    // render thread's in-flight snapshot (and its reference to a released ctx)
    // is gone before a new duplication is created on the same output
    std::atomic<unsigned long long> m_frameSeq{ 0 };
    // rolling fps: render thread is the only writer, GET perf reads atomically
    std::atomic<unsigned> m_fpsX10{ 0 };   // frames per second, fixed point x10
    unsigned m_fpsAccum = 0;               // frames since the last fps window
    ULONGLONG m_fpsTick = 0;               // window start (GetTickCount64 ms)
    // per-stage frame timings: EMA over the last frames, x100 ms fixed point
    std::atomic<unsigned> m_capMs100{ 0 };     // capture stage (desktop duplication)
    std::atomic<unsigned> m_drawMs100{ 0 };    // mesh + CB + draw + present
    std::atomic<unsigned> m_frameMs100{ 0 };   // whole renderAll pass
    std::atomic<unsigned> m_latMs100{ 0 };     // capture->present latency, EMA
};
