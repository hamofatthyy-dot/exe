#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>
#include <thread>
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dwmapi.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

// ---------------- offsets (لسه فاضية، هتتلي في المرحلة الجاية) ----------------
namespace off {
    constexpr uintptr_t gameWorld   = 0x0;
    constexpr uintptr_t localPlayer = 0x0;
    constexpr uintptr_t entityList  = 0x0;
    constexpr uintptr_t entArray    = 0x10;
    constexpr uintptr_t entCount    = 0x18;
    constexpr uintptr_t position    = 0x0;
    constexpr uintptr_t health      = 0x0;
    constexpr uintptr_t team        = 0x0;
    constexpr uintptr_t camera      = 0x0;
    constexpr uintptr_t viewMatrix  = 0x0;
    constexpr float     maxDist     = 400.f;
}

struct Vec2 { float x, y; };
struct Vec3 { float x, y, z; };
struct Ent  { uintptr_t ptr; Vec3 pos; float hp; int team; std::string name; };

static Vec3  vsub(const Vec3& a, const Vec3& b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
static float vlen(const Vec3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }

static bool w2s(const Vec3& w, Vec2& out, const float* m, int W, int H) {
    float cw = m[3] * w.x + m[7] * w.y + m[11] * w.z + m[15];
    if (cw < 0.01f) return false;
    float cx = m[0] * w.x + m[4] * w.y + m[8]  * w.z + m[12];
    float cy = m[1] * w.x + m[5] * w.y + m[9]  * w.z + m[13];
    out.x = (W * 0.5f) * (1.f + cx / cw);
    out.y = (H * 0.5f) * (1.f - cy / cw);
    return true;
}

// ---------------- memory ----------------
class Mem {
public:
    HANDLE proc = nullptr;
    DWORD  pid  = 0;
    uintptr_t base = 0;

    bool attach(const wchar_t* name) {
        HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        PROCESSENTRY32W pe{ sizeof(pe) };
        for (BOOL ok = Process32FirstW(s, &pe); ok; ok = Process32NextW(s, &pe))
            if (!_wcsicmp(pe.szExeFile, name)) { pid = pe.th32ProcessID; break; }
        CloseHandle(s);
        if (!pid) return false;

        proc = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, pid);
        if (!proc) return false;

        HANDLE ms = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
        MODULEENTRY32W me{ sizeof(me) };
        for (BOOL ok = Module32FirstW(ms, &me); ok; ok = Module32NextW(ms, &me))
            if (!_wcsicmp(me.szModule, name)) { base = (uintptr_t)me.modBaseAddr; break; }
        CloseHandle(ms);
        return base != 0;
    }

    template <class T> T read(uintptr_t a) {
        T o{};
        if (!a || !proc) return o;
        ReadProcessMemory(proc, (LPCVOID)a, &o, sizeof(T), nullptr);
        return o;
    }

    std::string readStr(uintptr_t p) {
        if (!p) return {};
        int len = read<int>(p + 0x10);
        if (len <= 0 || len > 48) return {};
        std::string s((size_t)len, 0);
        for (int i = 0; i < len; i++) s[i] = read<char>(p + 0x14 + i);
        return s;
    }
};

// ---------------- globals ----------------
static Mem g_mem;
static HWND g_hwnd = nullptr;
static ID3D11Device* g_dev = nullptr;
static ID3D11DeviceContext* g_ctx = nullptr;
static IDXGISwapChain* g_swap = nullptr;
static ID3D11RenderTargetView* g_rtv = nullptr;
static int g_w = 1920, g_h = 1080;
static bool g_menu = true;
static bool g_live = false;
static const char* g_status = "waiting for RobRun.exe";

struct Config {
    bool esp = true, boxes = true, names = true, health = true, skeleton = true, dist = true, teamFilter = true;
    bool aim = false, aimHead = true, aimPred = false, aimVisibleOnly = true;
    bool loot = false, containers = false, vehicles = false;
    float fov = 130.f, smooth = 4.f, sens = 1.f, maxDist = 400.f;
    int aimKey = VK_XBUTTON2;
} g_cfg;

static void setClickThrough(bool on) {
    LONG_PTR ex = GetWindowLongPtrW(g_hwnd, GWL_EXSTYLE);
    if (on) ex |= WS_EX_TRANSPARENT; else ex &= ~WS_EX_TRANSPARENT;
    SetWindowLongPtrW(g_hwnd, GWL_EXSTYLE, ex);
}

static void applyTheme() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 4.f; s.FrameRounding = 3.f; s.GrabRounding = 3.f;
    s.WindowBorderSize = 1.f; s.FrameBorderSize = 1.f;
    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg]      = ImVec4(0.05f, 0.05f, 0.06f, 0.96f);
    c[ImGuiCol_Border]        = ImVec4(0.65f, 0.08f, 0.10f, 0.85f);
    c[ImGuiCol_Tab]           = ImVec4(0.10f, 0.10f, 0.11f, 1.f);
    c[ImGuiCol_TabSelected]   = ImVec4(0.62f, 0.07f, 0.09f, 1.f);
    c[ImGuiCol_TabHovered]    = ImVec4(0.75f, 0.10f, 0.12f, 1.f);
    c[ImGuiCol_CheckMark]     = ImVec4(0.90f, 0.15f, 0.17f, 1.f);
    c[ImGuiCol_SliderGrab]    = ImVec4(0.80f, 0.12f, 0.14f, 1.f);
    c[ImGuiCol_SliderGrabActive] = ImVec4(1.f, 0.25f, 0.27f, 1.f);
    c[ImGuiCol_Button]        = ImVec4(0.18f, 0.18f, 0.20f, 1.f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.62f, 0.07f, 0.09f, 1.f);
    c[ImGuiCol_FrameBg]       = ImVec4(0.11f, 0.11f, 0.12f, 1.f);
    c[ImGuiCol_Header]        = ImVec4(0.62f, 0.07f, 0.09f, 0.85f);
}

static void attachLoop() {
    while (true) {
        if (!g_mem.pid) {
            if (g_mem.attach(L"RobRun.exe")) { g_live = true; g_status = "attached"; }
            else g_status = "waiting for RobRun.exe";
        } else if (!g_mem.read<uintptr_t>(g_mem.base + off::gameWorld) && off::gameWorld) {
            g_status = "attached, bad offset";
        }
        Sleep(2000);
    }
}

static void drawUI() {
    ImGui::SetNextWindowSize({ 460, 520 }, ImGuiCond_Once);
    ImGui::SetNextWindowPos({ 60, 60 }, ImGuiCond_Once);
    ImGui::Begin("ROBRUN EXTERNAL", &g_menu);

    ImGui::TextColored({ 0.85f, 0.15f, 0.17f, 1.f }, "status: %s", g_status);
    ImGui::Separator();

    if (ImGui::BeginTabBar("tabs")) {
        if (ImGui::BeginTabItem("Visuals")) {
            ImGui::Checkbox("Player ESP", &g_cfg.esp);
            ImGui::Checkbox("Boxes", &g_cfg.boxes);
            ImGui::Checkbox("Names", &g_cfg.names);
            ImGui::Checkbox("Health", &g_cfg.health);
            ImGui::Checkbox("Skeleton", &g_cfg.skeleton);
            ImGui::Checkbox("Distance", &g_cfg.dist);
            ImGui::Checkbox("Team filter", &g_cfg.teamFilter);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Aimbot")) {
            ImGui::Checkbox("Enable", &g_cfg.aim);
            ImGui::Checkbox("Head only", &g_cfg.aimHead);
            ImGui::Checkbox("Visible only", &g_cfg.aimVisibleOnly);
            ImGui::Checkbox("Prediction", &g_cfg.aimPred);
            ImGui::SliderFloat("FOV", &g_cfg.fov, 20.f, 400.f);
            ImGui::SliderFloat("Smooth", &g_cfg.smooth, 1.f, 15.f);
            ImGui::SliderFloat("Sensitivity", &g_cfg.sens, 0.2f, 3.f);
            ImGui::Text("Key: Mouse 4 (hold)");
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("World")) {
            ImGui::Checkbox("Loot ESP", &g_cfg.loot);
            ImGui::Checkbox("Containers", &g_cfg.containers);
            ImGui::Checkbox("Vehicles", &g_cfg.vehicles);
            ImGui::SliderFloat("Max distance", &g_cfg.maxDist, 50.f, 800.f);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Misc")) {
            ImGui::Text("FPS: %.0f", ImGui::GetIO().Framerate);
            ImGui::Text("PID: %lu", g_mem.pid);
            ImGui::Text("Base: 0x%llX", (unsigned long long)g_mem.base);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Profile")) {
            if (ImGui::Button("Save", { 120, 0 })) { /* لاحقًا */ }
            ImGui::SameLine();
            if (ImGui::Button("Load", { 120, 0 })) { /* لاحقًا */ }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

static void collect(std::vector<Ent>& out) {
    if (!g_live || !off::gameWorld) return;
    uintptr_t world = g_mem.read<uintptr_t>(g_mem.base + off::gameWorld);
    if (!world) return;
    uintptr_t list = g_mem.read<uintptr_t>(world + off::entityList);
    if (!list) return;
    uintptr_t arr = g_mem.read<uintptr_t>(list + off::entArray);
    int count = g_mem.read<int>(list + off::entCount);
    if (!arr || count <= 0 || count > 512) return;

    for (int i = 0; i < count; i++) {
        uintptr_t e = g_mem.read<uintptr_t>(arr + 0x20 + i * 8);
        if (!e) continue;
        Ent en{};
        en.ptr = e;
        en.pos = g_mem.read<Vec3>(e + off::position);
        en.hp  = g_mem.read<float>(e + off::health);
        en.team = g_mem.read<int>(e + off::team);
        if (g_cfg.names) en.name = g_mem.readStr(g_mem.read<uintptr_t>(e + off::position + 0x8));
        if (en.hp > 0.f) out.push_back(en);
    }
}

static void render() {
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    if (g_menu) drawUI();

    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    std::vector<Ent> ents;
    collect(ents);

    if (g_cfg.aim) {
        ImVec2 c{ g_w * 0.5f, g_h * 0.5f };
        dl->AddCircle(c, g_cfg.fov * 0.5f, IM_COL32(220, 40, 45, 130), 64, 1.f);
    }

    ImGui::Render();
    const float clear[4]{ 0, 0, 0, 0 };
    g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
    g_ctx->ClearRenderTargetView(g_rtv, clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    g_swap->Present(1, 0);
}

static LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (ImGui_ImplWin32_WndProcHandler(h, m, w, l)) return true;
    if (m == WM_KEYDOWN && w == VK_INSERT) {
        g_menu = !g_menu;
        setClickThrough(!g_menu);
    }
    return DefWindowProcW(h, m, w, l);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int) {
    SetProcessDPIAware();
    g_w = GetSystemMetrics(SM_CXSCREEN);
    g_h = GetSystemMetrics(SM_CYSCREEN);

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = wndProc;
    wc.hInstance = inst;
    wc.lpszClassName = L"rr";
    RegisterClassExW(&wc);

    g_hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_TOOLWINDOW,
        L"rr", L"rr", WS_POPUP, 0, 0, g_w, g_h, nullptr, nullptr, inst, nullptr);

    MARGINS m{ -1 };
    DwmExtendFrameIntoClientArea(g_hwnd, &m);

    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = g_w; sd.BufferDesc.Height = g_h;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 144; sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.OutputWindow = g_hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;

    D3D_FEATURE_LEVEL fl;
    if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &sd, &g_swap, &g_dev, &fl, &g_ctx))) return 1;

    ID3D11Texture2D* back = nullptr;
    g_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back);
    g_dev->CreateRenderTargetView(back, nullptr, &g_rtv);
    back->Release();

    ShowWindow(g_hwnd, SW_SHOW);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    applyTheme();
    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_dev, g_ctx);

    std::thread(attachLoop).detach();

    MSG msg{};
    while (msg.message != WM_QUIT) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        render();
    }
    return 0;
}
