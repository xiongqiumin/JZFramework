#include <stdio.h>
#include <windows.h>
#include "lvgl.h"
#include "JZLvgl.h"
#include "JZApplication.h"

#define MONITOR_HOR_RES 800
#define MONITOR_VER_RES 600

// --- GDI 全局 ---
static HWND g_hWnd = NULL;
static HDC g_hMemDC = NULL;
static HBITMAP g_hBmp = NULL;
static VOID * g_pixels = NULL;
static int g_width = MONITOR_HOR_RES;
static int g_height = MONITOR_VER_RES;
static lv_display_t * g_display = NULL;
static lv_indev_t * g_pointer = NULL;
static int16_t g_mouse_x = 0;
static int16_t g_mouse_y = 0;
static bool g_mouse_pressed = false;
static int16_t g_wheel = 0;

// --- Tick ---
static uint32_t my_tick_cb(void)
{
    LARGE_INTEGER freq, pc;
    if(QueryPerformanceFrequency(&freq) && QueryPerformanceCounter(&pc))
        return (uint32_t)(pc.QuadPart * 1000 / freq.QuadPart);
    return (uint32_t)GetTickCount();
}

// --- Display flush ---
static void my_flush_cb(lv_display_t * disp, const lv_area_t * area, uint8_t * px_map)
{
    HDC hdc = GetDC(g_hWnd);
    if(hdc) {
        BitBlt(hdc, area->x1, area->y1, lv_area_get_width(area), lv_area_get_height(area),
               g_hMemDC, area->x1, area->y1, SRCCOPY);
        ReleaseDC(g_hWnd, hdc);
    }
    lv_display_flush_ready(disp);
}

// --- Pointer ---
static void my_pointer_read_cb(lv_indev_t * indev, lv_indev_data_t * data)
{
    data->point.x = g_mouse_x;
    data->point.y = g_mouse_y;
    data->state = g_mouse_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    if(g_wheel) { data->enc_diff = g_wheel; g_wheel = 0; }
}

// --- Framebuffer ---
static bool create_frame_buffer(int w, int h)
{
    HDC hdc = GetDC(NULL);
    g_hMemDC = CreateCompatibleDC(hdc);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    g_hBmp = CreateDIBSection(g_hMemDC, &bi, DIB_RGB_COLORS, &g_pixels, NULL, 0);
    ReleaseDC(NULL, hdc);
    if(!g_hBmp) { DeleteDC(g_hMemDC); return false; }
    SelectObject(g_hMemDC, g_hBmp);
    return true;
}

// --- WndProc ---
static LRESULT CALLBACK wndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch(msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC hdc = BeginPaint(hWnd, &ps);
        if(g_hMemDC) { RECT r; GetClientRect(hWnd, &r); BitBlt(hdc, 0, 0, r.right, r.bottom, g_hMemDC, 0, 0, SRCCOPY); }
        EndPaint(hWnd, &ps); return 0;
    }
    case WM_MOUSEMOVE:    g_mouse_x = LOWORD(lParam); g_mouse_y = HIWORD(lParam); break;
    case WM_LBUTTONDOWN:  g_mouse_pressed = true; g_mouse_x = LOWORD(lParam); g_mouse_y = HIWORD(lParam); break;
    case WM_LBUTTONUP:    g_mouse_pressed = false; break;
    case WM_MOUSEWHEEL:   g_wheel += GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA; break;
    case WM_CLOSE:        DestroyWindow(hWnd); return 0;
    case WM_DESTROY:      PostQuitMessage(0); return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

static bool initWindow()
{
    WNDCLASSEXW wc = { sizeof(wc), CS_HREDRAW | CS_VREDRAW, wndProc, 0, 0, GetModuleHandleW(NULL),
                       NULL, LoadCursorW(NULL, (LPCWSTR)IDC_ARROW), (HBRUSH)(COLOR_WINDOW + 1), NULL, L"LVGL_GDI", NULL };
    if(!RegisterClassExW(&wc)) return false;
    RECT r = { 0, 0, g_width, g_height };
    AdjustWindowRect(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
    g_hWnd = CreateWindowExW(0, L"LVGL_GDI", L"JZFramework LVGL Demo", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                              CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
                              NULL, NULL, GetModuleHandleW(NULL), NULL);
    return g_hWnd != NULL;
}

// ===================== UI =====================

static void createUI()
{
    auto screen = JZScreen::active();

    auto* title = new JZLvglLabel(screen);
    title->setText("JZFramework LVGL Demo");
    title->center();

    auto* btn = new JZLvglButton(screen);
    btn->setSize(140, 40);
    btn->setSizePos(140, 40, g_width/2 - 70, 80);
    btn->setText("Click Me!");
    connect(&btn->sigClicked, [=]() {
        static int count = 0;
        count++;
        char buf[64];
        snprintf(buf, sizeof(buf), "Clicked: %d", count);
        btn->setText(buf);
    });

    auto* info = new JZLvglLabel(screen);
    info->setText("Welcome!");
    info->setSizePos(300, 30, g_width/2 - 150, 150);
    info->setAlign(LV_TEXT_ALIGN_CENTER);

    // 使用 JZLvglLayout
    auto* row = new JZLvglWidget(screen);
    row->setSize(400, 50);
    row->setLayout(JZLvglLayout::hbox(8, 8));
    row->setPos(g_width/2 - 200, 220);

    auto* btn2 = new JZLvglButton(row);
    btn2->setSize(100, 40);
    btn2->setText("Btn A");
    connect(&btn2->sigClicked, [=]() { info->setText("Button A clicked"); });

    auto* btn3 = new JZLvglButton(row);
    btn3->setSize(100, 40);
    btn3->setText("Btn B");
    connect(&btn3->sigClicked, [=]() { info->setText("Button B clicked"); });

    auto* btn4 = new JZLvglButton(row);
    btn4->setSize(100, 40);
    btn4->setText("Btn C");
    connect(&btn4->sigClicked, [=]() { info->setText("Button C clicked"); });
}

// ===================== Main =====================

int main()
{
    JZApplication app;

    printf("JZFramework LVGL Demo starting...\n");

    if(!initWindow()) { printf("Failed to create window!\n"); return 1; }
    if(!create_frame_buffer(g_width, g_height)) { printf("Failed to create framebuffer!\n"); return 1; }

    lv_init();
    lv_tick_set_cb(my_tick_cb);

    size_t bufSize = (size_t)g_width * g_height * 4;
    g_display = lv_display_create(g_width, g_height);
    lv_display_set_flush_cb(g_display, my_flush_cb);
    lv_display_set_buffers(g_display, g_pixels, NULL, (uint32_t)bufSize, LV_DISPLAY_RENDER_MODE_DIRECT);

    g_pointer = lv_indev_create();
    lv_indev_set_type(g_pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(g_pointer, my_pointer_read_cb);

    lv_group_t * g = lv_group_create();
    lv_group_set_default(g);

    createUI();

    ShowWindow(g_hWnd, SW_SHOW);
    UpdateWindow(g_hWnd);

    MSG msg;
    for(;;) {
        while(PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            if(msg.message == WM_QUIT) {
                lv_deinit();
                if(g_hBmp) DeleteObject(g_hBmp);
                if(g_hMemDC) DeleteDC(g_hMemDC);
                return (int)msg.wParam;
            }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        
        app.processEvents();
        uint32_t wait = lv_timer_handler();
        if(wait == UINT32_MAX) wait = 5;
        if(wait > 30) wait = 30;
        if(wait > 0) MsgWaitForMultipleObjects(0, NULL, FALSE, wait, QS_ALLINPUT);
    }
    return app.exec();
}
