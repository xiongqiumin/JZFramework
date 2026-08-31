#include "JZImGuiBackend.h"

#ifdef _WIN32

#include "JZLayer.h"
#include "JZDrawTask.h"
#include "JZImage.h"
#include "JZDisplayDevice.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <cmath>

#include "imgui.h"
#include "imgui_impl_dx11.h"

static ImU32 toImColor(const JZColor& c, uint8_t opacity)
{
    uint8_t a = (uint8_t)((c.alpha() * opacity) / 255);
    return IM_COL32(c.red(), c.green(), c.blue(), a);
}

// JZRect (right = left + w - 1) -> ImGui 物理像素 [min, max)
static void toImRect(const JZRect& rc, float scale, ImVec2& outMin, ImVec2& outMax)
{
    outMin = ImVec2(rc.left() * scale, rc.top() * scale);
    outMax = ImVec2((rc.right() + 1) * scale, (rc.bottom() + 1) * scale);
}

JZImGuiBackend::JZImGuiBackend()
{
}

JZImGuiBackend::~JZImGuiBackend()
{
    destroy();
}

bool JZImGuiBackend::init(JZDisplayDevice* device, void* hwnd, unsigned physicalW, unsigned physicalH)
{
    m_widgetDevice = device;
    if (!hwnd || !createD3DDevice(hwnd, physicalW, physicalH))
        return false;

    createRenderTarget();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;   // 不落盘 imgui.ini
    ImGui::StyleColorsDark();               // 供日后穿插原生 ImGui 调试面板使用

    ImGui_ImplDX11_Init(m_d3dDevice, m_d3dContext);
    rebuildFonts();
    return true;
}

void JZImGuiBackend::destroy()
{
    if (ImGui::GetCurrentContext()) {
        ImGui_ImplDX11_Shutdown();
        ImGui::DestroyContext();
    }

    for (auto& kv : m_textures) {
        if (kv.second)
            ((ID3D11ShaderResourceView*)kv.second)->Release();
    }
    m_textures.clear();

    releaseRenderTarget();

    if (m_swapChain)   { m_swapChain->Release();     m_swapChain = nullptr; }
    if (m_d3dContext)  { m_d3dContext->Release();    m_d3dContext = nullptr; }
    if (m_d3dDevice)   { m_d3dDevice->Release();     m_d3dDevice = nullptr; }
}

bool JZImGuiBackend::createD3DDevice(void* hwnd, unsigned physicalW, unsigned physicalH)
{
    DXGI_SWAP_CHAIN_DESC scd;
    ZeroMemory(&scd, sizeof(scd));
    scd.BufferDesc.Width = physicalW;
    scd.BufferDesc.Height = physicalH;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 2;
    scd.OutputWindow = (HWND)hwnd;
    scd.Windowed = TRUE;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                               nullptr, 0, D3D11_SDK_VERSION, &scd,
                                               &m_swapChain, &m_d3dDevice, nullptr, &m_d3dContext);
    if (FAILED(hr)) {
        // WARP 软渲染兜底
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags,
                                           nullptr, 0, D3D11_SDK_VERSION, &scd,
                                           &m_swapChain, &m_d3dDevice, nullptr, &m_d3dContext);
    }
    return SUCCEEDED(hr);
}

void JZImGuiBackend::releaseRenderTarget()
{
    if (m_rtv) {
        m_rtv->Release();
        m_rtv = nullptr;
    }
}

void JZImGuiBackend::createRenderTarget()
{
    releaseRenderTarget();
    if (!m_swapChain || !m_d3dDevice)
        return;

    ID3D11Texture2D* backBuffer = nullptr;
    if (SUCCEEDED(m_swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backBuffer)) && backBuffer) {
        m_d3dDevice->CreateRenderTargetView(backBuffer, nullptr, &m_rtv);
        backBuffer->Release();
    }
}

void JZImGuiBackend::resizeSwapChain(unsigned physicalW, unsigned physicalH)
{
    if (!m_d3dDevice || !m_swapChain || physicalW == 0 || physicalH == 0)
        return;

    releaseRenderTarget();
    m_swapChain->ResizeBuffers(0, physicalW, physicalH, DXGI_FORMAT_UNKNOWN, 0);
    createRenderTarget();
}

void JZImGuiBackend::rebuildFonts()
{
    if (!ImGui::GetCurrentContext())
        return;

    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();

    // 优先微软雅黑 (含简体中文), 找不到再依次回退
    static const char* candidates[] = {
        "C:/Windows/Fonts/msyh.ttc",
        "C:/Windows/Fonts/msyh.ttf",
        "C:/Windows/Fonts/simhei.ttf",
        "C:/Windows/Fonts/simsun.ttc",
    };
    const ImWchar* ranges = io.Fonts->GetGlyphRangesChineseSimplifiedCommon();
    m_font = nullptr;
    for (const char* path : candidates) {
        float scale = m_widgetDevice ? m_widgetDevice->scale() : 1.0f;
        m_font = io.Fonts->AddFontFromFileTTF(path, 16.0f * scale, nullptr, ranges);
        if (m_font)
            break;
    }
    if (!m_font)
        m_font = io.Fonts->AddFontDefault();
}

void JZImGuiBackend::render(JZLayer* layer)
{
    if (!m_swapChain || !m_rtv || !layer || !ImGui::GetCurrentContext())
        return;

    // 1. 启动 ImGui 帧 (不走任何即时模式控件)
    ImGui_ImplDX11_NewFrame();
    ImGui::NewFrame();

    // 2. 把 JZWidget 树产生的绘制任务回放到主窗口画布 (BackgroundDrawList)
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    replayTasks(dl, layer);

    // 3. 结束并提交
    ImGui::Render();

    ID3D11RenderTargetView* rtv = m_rtv;
    m_d3dContext->OMSetRenderTargets(1, &rtv, nullptr);
    const float clearColor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    m_d3dContext->ClearRenderTargetView(m_rtv, clearColor);

    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    m_swapChain->Present(1, 0);   // VSync

    layer->clearTasks();
}

void* JZImGuiBackend::imageTexture(const JZImageData* img)
{
    auto it = m_textures.find(img);
    if (it != m_textures.end())
        return it->second;

    if (!m_d3dDevice || !img->pixels() || img->width() <= 0 || img->height() <= 0)
        return nullptr;

    D3D11_TEXTURE2D_DESC desc;
    desc.Width = (UINT)img->width();
    desc.Height = (UINT)img->height();
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = 0;
    desc.MiscFlags = 0;

    D3D11_SUBRESOURCE_DATA init;
    init.pSysMem = img->pixels();
    init.SysMemPitch = (UINT)(img->width() * 4);
    init.SysMemSlicePitch = 0;

    ID3D11Texture2D* tex = nullptr;
    if (FAILED(m_d3dDevice->CreateTexture2D(&desc, &init, &tex)))
        return nullptr;

    D3D11_SHADER_RESOURCE_VIEW_DESC srvd;
    srvd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvd.Texture2D.MostDetailedMip = 0;
    srvd.Texture2D.MipLevels = 1;

    ID3D11ShaderResourceView* srv = nullptr;
    HRESULT hr = m_d3dDevice->CreateShaderResourceView(tex, &srvd, &srv);
    tex->Release();
    if (FAILED(hr))
        return nullptr;

    m_textures[img] = srv;
    return srv;
}

void JZImGuiBackend::replayTasks(ImDrawList* dl, JZLayer* layer)
{
    const float s = m_widgetDevice ? m_widgetDevice->scale() : 1.0f;

    for (auto& task : layer->tasks()) {
        // 每个任务按自身脏区裁剪 (逻辑像素 -> 物理像素)
        bool clipped = !task->clip.isEmpty();
        if (clipped) {
            ImVec2 cmin, cmax;
            toImRect(task->clip, s, cmin, cmax);
            dl->PushClipRect(cmin, cmax, true);
        }

        switch (task->type) {
        case JZDrawTask::Line: {
            auto* t = static_cast<JZLineTask*>(task.get());
            dl->AddLine(ImVec2(t->x1 * s, t->y1 * s), ImVec2(t->x2 * s, t->y2 * s),
                        toImColor(t->pen.color(), t->opacity),
                        s * (t->pen.width() > 0 ? t->pen.width() : 1));
            break;
        }
        case JZDrawTask::Rect: {
            auto* t = static_cast<JZFillRectTask*>(task.get());
            ImVec2 pmin, pmax;
            toImRect(t->area, s, pmin, pmax);
            dl->AddRectFilled(pmin, pmax, toImColor(t->color, t->opacity), t->radius * s);
            break;
        }
        case JZDrawTask::Border: {
            auto* t = static_cast<JZBorderTask*>(task.get());
            ImVec2 pmin, pmax;
            toImRect(t->area, s, pmin, pmax);
            dl->AddRect(pmin, pmax, toImColor(t->pen.color(), t->opacity), t->radius * s,
                        s * (t->pen.width() > 0 ? t->pen.width() : 1));
            break;
        }
        case JZDrawTask::Arc: {
            auto* t = static_cast<JZArcTask*>(task.get());
            // 角度: 1/10 度, 0 = 3 点方向, 顺时针; 屏幕坐标系 y 向下, 方向天然一致。
            int start = t->arcStartAngle;
            int end = t->arcEndAngle;
            if (end - start >= 3600)
                end = start + 3599;   // 整圆留 0.1 度缺口, 避免整圆弧路径不闭合
            float a0 = start * 3.14159265358979f / 1800.0f;
            float a1 = end   * 3.14159265358979f / 1800.0f;
            float radius = s * (t->arcRadius > 0 ? t->arcRadius : 1);
            dl->PathArcTo(ImVec2(t->arcCx * s, t->arcCy * s), radius, a0, a1);
            dl->PathStroke(toImColor(t->pen.color(), t->opacity),
                           s * (t->pen.width() > 0 ? t->pen.width() : 1));
            break;
        }
        case JZDrawTask::Text: {
            auto* t = static_cast<JZTextTask*>(task.get());
            if (!t->text.empty())
                dl->AddText(nullptr, t->font.size() * s, ImVec2(t->area.x() * s, t->area.y() * s),
                            toImColor(t->color, t->opacity), t->text.c_str());
            break;
        }
        case JZDrawTask::Image: {
            auto* t = static_cast<JZImageTask*>(task.get());
            if (t->image && t->image->pixels()) {
                ImVec2 pmin, pmax;
                toImRect(t->area, s, pmin, pmax);
                void* srv = imageTexture(t->image.get());
                if (srv) {
                    ImTextureID texId = (ImTextureID)(uintptr_t)srv;
                    dl->AddImage(texId, pmin, pmax, ImVec2(0, 0), ImVec2(1, 1),
                                 IM_COL32(255, 255, 255, t->opacity));
                }
            }
            break;
        }
        case JZDrawTask::Changed:
        case JZDrawTask::None:
        default:
            break;
        }

        if (clipped)
            dl->PopClipRect();
    }
}

#else // !_WIN32

// 非 Windows 平台: 暂无渲染后端, 仅保留逻辑层可编译运行
JZImGuiBackend::JZImGuiBackend() {}
JZImGuiBackend::~JZImGuiBackend() {}
bool JZImGuiBackend::init(JZDisplayDevice*, void*, unsigned, unsigned) { return false; }
void JZImGuiBackend::destroy() {}
void JZImGuiBackend::render(JZLayer*) {}
void JZImGuiBackend::resizeSwapChain(unsigned, unsigned) {}
void JZImGuiBackend::rebuildFonts() {}

#endif // _WIN32


