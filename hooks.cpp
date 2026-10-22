// ============================================================================
//  hooks.cpp  (v26)  — render (wglSwapBuffers), hooks de thread, wndproc, cursor
//
//  >>> FIX v26  (principio 5: tamanho e escala; principio 6: instrumentacao)
//   1) O VIEWPORT do GL passa a ser a UNICA fonte de tamanho. GetClientRect e'
//      so' fallback, e a divergencia e' logada uma vez. W2S, DisplaySize e
//      glViewport usam o mesmo par por construcao.
//   2) DrainGlErrors(): falha do renderer vira contador, nao silencio.
//
//  >>> FIX v25  (bala nao acerta nada + AV write em fase=hk_fire)
//   1) slot da direcao DETECTADO (|v| ~ 1); muzzle real; PHASE por etapa.
//   2) Writable(): checagem de gravabilidade em ApplyNoRecoil.
//   3) Direcao final renormalizada.
//
//  >>> FIX v24: silent aim SEGURO (restaura o original se falhar) + sonda.
//  >>> FIX v23: fwd decl de hk_wndProc (C2065); (LPCSTR)IDC_ARROW (C2664).
//  >>> FIX v22: HandleKeys incondicional; INPUT_TEST_LEVEL 7; dono unico do
//      toggle; cursor auto-curavel; injecao de mouse so' como fallback.
//  >>> FIX v21: SetStage nao escreve g_sdkReady (dono: Combat::Tick).
//  >>> FIX v20: anti-blink via WithinFrameScope; hk_fire7 leve.
//  >>> FIX v19: hk_fire7 blindado por etapa; Override(p2,p3).
//  >>> FIX v18: bool Init(); VEH proprio.
//  >>> FIX v17: sem enum Stage local; g_frameOpen atomico; SDK::SetRot.
//  >>> FIX v16: cada desenho com seu __try e ImGui::Render() incondicional.
// ============================================================================
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <atomic>
#include <cmath>
#include <GL/gl.h>

#include "MinHook.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_opengl3.h"

#include "il2cpp.h"
#include "il2cpp_api.h"
#include "common.h"
#include "memory.h"
#include "config.h"
#include "sdk.h"
#include "hooks.h"
#include "features.h"
#include "aimbot.h"

#pragma comment(lib, "opengl32.lib")

#define WATCH_MAX_LINES   12
#define PROBE_DIFF_MAX    8
#define TICK_WATCHDOG_MS  250
#define WND_REAPPLY_MS    500     // reaplica o wndproc se o jogo restaurar o dele
#define MOUSE_FALLBACK_MS 120     // injeta mouse so' se o wndproc ficou mudo

// escada de isolamento: 0 = so' menu do cheat, 7 = tudo
#define INPUT_TEST_LEVEL  7

// g_phase e' DEFINIDO em UM .cpp (sdk.cpp). Aqui so' usamos via PHASE.
extern const char* g_phase;
#ifndef PHASE
#define PHASE(x) (g_phase = (x))
#endif

namespace Hooks {

    // -----------------------------------------------------------------------
    //  Globais DECLARADAS no hooks.h (por isso nao sao static)
    // -----------------------------------------------------------------------
    std::atomic<bool>     g_hooksLive{ false };
    std::atomic<bool>     g_imguiReady{ false };
    std::atomic<bool>     g_sdkReady{ false };       // dono: Combat::Tick
    std::atomic<uint64_t> g_frameCounter{ 0 };
    std::atomic<uint32_t> g_imguiGeneration{ 0 };
    std::atomic<int>      g_viewW{ 1920 };
    std::atomic<int>      g_viewH{ 1080 };

    // -----------------------------------------------------------------------
    //  Internos
    // -----------------------------------------------------------------------
    static std::atomic<int> g_stage{ (int)Stage::Down };

    static std::atomic<uint32_t> g_frameDrops{ 0 };
    static std::atomic<uint32_t> g_wndProcCalls{ 0 };
    static std::atomic<uint32_t> g_cwpCalls{ 0 };
    static std::atomic<uint32_t> g_avCount{ 0 };
    static std::atomic<uint32_t> g_vehCount{ 0 };

    static std::atomic<uint32_t> g_abortPre{ 0 };
    static std::atomic<uint32_t> g_abortMenu{ 0 };
    static std::atomic<uint32_t> g_abortEsp{ 0 };
    static std::atomic<uint32_t> g_abortMisc{ 0 };
    static std::atomic<uint32_t> g_abortRender{ 0 };
    static std::atomic<uint32_t> g_abortDraw{ 0 };

    // ultimo instante em que o hk_wndProc viu algo (fallbacks)
    static std::atomic<ULONGLONG> g_lastWndMsg{ 0 };
    static std::atomic<ULONGLONG> g_lastMouseMsg{ 0 };

    // slot do parametro que e' a DIRECAO no FireBullet (-1 = nao detectado)
    static std::atomic<int> s_dirSlot{ -1 };

    uint32_t FrameId() { return (uint32_t)g_frameCounter.load(std::memory_order_relaxed); }

    // -----------------------------------------------------------------------
    //  Escada INPUT_TEST_LEVEL  (NAO confundir com AtLeast(Stage))
    // -----------------------------------------------------------------------
    namespace Lvl {
        constexpr int Menu = 0;
        constexpr int Esp = 1;
        constexpr int Tick = 2;
        constexpr int Scp = 3;
        constexpr int CursorFn = 4;
        constexpr int Subclass = 5;
        constexpr int BlockKeys = 6;
        constexpr int Watchdog = 7;
    }
    inline bool AtLvl(int l) { return INPUT_TEST_LEVEL >= l; }

    // -----------------------------------------------------------------------
    //  Ciclo de vida — Stage vem do hooks.h
    // -----------------------------------------------------------------------
    Stage StageNow() { return (Stage)g_stage.load(std::memory_order_acquire); }
    bool  IsLive() { return StageNow() == Stage::Live; }

    bool AtLeast(Stage s) {
        const Stage cur = StageNow();
        if (cur == Stage::Tearing) return false;
        return (int)cur >= (int)s;
    }

    const char* StageName(Stage s) {
        switch (s) {
        case Stage::Down:    return "Down";
        case Stage::Init:    return "Init";
        case Stage::Sdk:     return "Sdk";
        case Stage::Hooks:   return "Hooks";
        case Stage::ImGui:   return "ImGui";
        case Stage::Live:    return "Live";
        case Stage::Tearing: return "Tearing";
        }
        return "?";
    }

    // >>> [FIX v21] g_sdkReady NAO aqui: o dono e' Combat::Tick (latch).
    void SetStage(Stage s) {
        const int v = (int)s;
        g_stage.store(v, std::memory_order_release);
        g_hooksLive.store(v >= (int)Stage::Hooks, std::memory_order_release);
        g_imguiReady.store(v >= (int)Stage::ImGui, std::memory_order_release);
    }

    void Advance(Stage s) {
        const int cur = (int)g_stage.load(std::memory_order_acquire);
        if (cur == (int)Stage::Tearing) return;
        if ((int)s > cur) SetStage(s);
    }

    // -----------------------------------------------------------------------
    //  Viewport
    // -----------------------------------------------------------------------
    void SetViewSize(int w, int h) {
        if (w > 0) g_viewW.store(w, std::memory_order_relaxed);
        if (h > 0) g_viewH.store(h, std::memory_order_relaxed);
    }
    ViewSize GetViewSize() {
        return ViewSize{ g_viewW.load(std::memory_order_relaxed),
                         g_viewH.load(std::memory_order_relaxed) };
    }
    int ViewW() { return g_viewW.load(std::memory_order_relaxed); }
    int ViewH() { return g_viewH.load(std::memory_order_relaxed); }

    // -----------------------------------------------------------------------
    //  Handles / estado
    // -----------------------------------------------------------------------
    static HWND   g_hwnd = nullptr;
    static bool   g_imguiInit = false;
    static bool   g_win32Backend = false;
    static CRITICAL_SECTION g_imCs;
    static bool   g_imCsInit = false;
    static HHOOK  g_cwp = nullptr;
    static PVOID  g_veh = nullptr;

    typedef BOOL(WINAPI* tSwapBuffers)(HDC);
    typedef BOOL(WINAPI* tSetCursorPos)(int, int);
    typedef void(__fastcall* tLateUpdate)(void*, void*);
    typedef bool(__fastcall* tFireBullet)(void*, void*, int, float, Vec3, Vec3, bool, double, int);

    static tSwapBuffers  o_swap = nullptr;
    static tSetCursorPos o_setCursorPos = nullptr;
    static tLateUpdate   o_lateUpdate = nullptr;
    static tFireBullet   o_fire7 = nullptr;
    static WNDPROC       o_wndProc = nullptr;

    static std::atomic<uint64_t>  g_lastTickFrame{ ~0ull };
    static std::atomic<ULONGLONG> g_lastLuMs{ 0 };

    // -----------------------------------------------------------------------
    //  Helpers
    // -----------------------------------------------------------------------
    static bool Plausible(const void* p) {
        uintptr_t a = (uintptr_t)p;
        if (a < 0x10000 || a > 0x00007FFFFFFFFFFFULL) return false;
        if (a & 7) return false;
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(p, &mbi, sizeof(mbi))) return false;
        if (mbi.State != MEM_COMMIT) return false;
        if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
        return true;
    }
    static bool PlausibleObj(const void* p) {
        if (!Plausible(p)) return false;
        return Plausible(*(const void**)p);
    }

    static inline float Len(const Vec3& v) {
        return sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
    }

    // a pagina aceita ESCRITA? (Plausible so' checa leitura)
    static bool Writable(const void* p) {
        uintptr_t a = (uintptr_t)p;
        if (a < 0x10000 || a > 0x00007FFFFFFFFFFFULL) return false;
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(p, &mbi, sizeof(mbi))) return false;
        if (mbi.State != MEM_COMMIT) return false;
        const DWORD ruim = PAGE_NOACCESS | PAGE_GUARD | PAGE_READONLY |
            PAGE_EXECUTE | PAGE_EXECUTE_READ;
        return (mbi.Protect & ruim) == 0;
    }

    // >>> [FIX v26] falha do renderer vira contador (principio 6)
    static uint32_t DrainGlErrors(const char* tag) {
        uint32_t n = 0;
        GLenum e;
        while ((e = glGetError()) != GL_NO_ERROR && n < 8) {
            Log("[gl] %s erro=0x%X", tag, (unsigned)e);
            ++n;
        }
        if (n) SDK::g_glErrTotal.fetch_add(n, std::memory_order_relaxed);
        return n;
    }

    static MethodInfo* FindMethodByArity(Il2CppClass* k, const char* name, uint32_t argc) {
        if (!k || !name) return nullptr;
        void* iter = nullptr;
        for (;;) {
            const MethodInfo* m = il2cpp_class_get_methods(k, &iter);
            if (!m) break;
            if (il2cpp_method_get_param_count(m) == argc &&
                strcmp(il2cpp_method_get_name(m), name) == 0)
                return (MethodInfo*)m;
        }
        return nullptr;
    }
    static Il2CppClass* FindClassNamed(const char* name) {
        if (!name) return nullptr;
        Il2CppClass* k = SDK::FindClass(name);
        if (!k) return nullptr;
        const char* n = il2cpp_class_get_name(k);
        if (!n || strcmp(n, name) != 0) return nullptr;
        return k;
    }
    static void* FindMethodPtr(const char* klass, const char* method, uint32_t argc) {
        Il2CppClass* k = FindClassNamed(klass);
        if (!k) return nullptr;
        MethodInfo* m = FindMethodByArity(k, method, argc);
        return m ? SDK::MethodPtr(m) : nullptr;
    }
    static Il2CppClass* ClassOf(void* o) {
        if (!il2cpp_object_get_class || !PlausibleObj(o)) return nullptr;
        return il2cpp_object_get_class((const Il2CppObject*)o);
    }

    static bool KeyDown(int vk) {
        if (vk <= 0 || vk > 255) return false;
        return (GetAsyncKeyState(vk) & 0x8000) != 0;
    }
    static bool Edge(int vk) {
        static bool prev[256] = {};
        if (vk <= 0 || vk > 255) return false;
        const bool now = KeyDown(vk);
        const bool was = prev[vk];
        prev[vk] = now;
        return now && !was;
    }

    // o menu quer o mouse? (fonte unica de verdade)
    static inline bool MenuWantsMouse() {
        return C.menuOpen && g_imguiReady.load(std::memory_order_relaxed);
    }

    // -----------------------------------------------------------------------
    //  VEH — conta e reporta
    // -----------------------------------------------------------------------
    static LONG CALLBACK VehHandler(EXCEPTION_POINTERS* ep) {
        if (!ep || !ep->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;
        if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
            g_vehCount.fetch_add(1, std::memory_order_relaxed);
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // -----------------------------------------------------------------------
    //  Fontes
    // -----------------------------------------------------------------------
    int FontCount() {
        if (!g_imguiReady.load() || !ImGui::GetCurrentContext()) return 0;
        ImGuiIO& io = ImGui::GetIO();
        return io.Fonts ? io.Fonts->Fonts.Size : 0;
    }
    ImFont& SafeFont(int idx) {
        if (g_imguiReady.load() && ImGui::GetCurrentContext()) {
            ImGuiIO& io = ImGui::GetIO();
            const int n = io.Fonts ? io.Fonts->Fonts.Size : 0;
            if (n > 0) {
                if (idx < 0) idx = 0;
                if (idx >= n) idx = n - 1;
                if (ImFont* f = io.Fonts->Fonts[idx]) return *f;
            }
            if (ImFont* f = ImGui::GetFont()) return *f;
        }
        static ImFont s_dummy;
        return s_dummy;
    }

    // -----------------------------------------------------------------------
    //  NO-RECOIL — valida objeto E destino antes de escrever
    // -----------------------------------------------------------------------
    static void ApplyNoRecoil() {
        if (!C.noRecoil || !SDK::g_localPlayer) return;
        __try {
            Il2CppObject* ct = SDK::CamTfObj();
            if (!ct || !Writable(ct)) return;

            void* dst = (uint8_t*)ct + SDK::O_rotation;
            if (!Writable(dst)) {
                Log("[fire] no-recoil: destino NAO gravavel (%p) ct=%p", dst, ct);
                return;
            }
            ::Quat pr = SDK::GetPlayerRot(SDK::g_localPlayer);
            SDK::SetRot(ct, pr);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { g_avCount.fetch_add(1, std::memory_order_relaxed); }
    }

    // -----------------------------------------------------------------------
    //  CURSOR — auto-curavel, sem contador
    // -----------------------------------------------------------------------
    static bool g_cursorFreed = false;
    static bool g_cursorWasShowing = true;

    static bool IsCursorShowing() {
        CURSORINFO ci{};
        ci.cbSize = sizeof(ci);
        if (!GetCursorInfo(&ci)) return true;
        return (ci.flags & CURSOR_SHOWING) != 0;
    }

    // idempotente: so' chama ShowCursor se o estado real divergir
    static void ForceCursor(bool show) {
        if (IsCursorShowing() == show) return;
        int guard = 0;
        if (show) { while (!IsCursorShowing() && ++guard < 64) ShowCursor(TRUE); }
        else { while (IsCursorShowing() && ++guard < 64) ShowCursor(FALSE); }
    }

    static void ApplyCursorOwnership() {
        const bool want = MenuWantsMouse();

        if (want) {
            if (!g_cursorFreed) {
                g_cursorWasShowing = IsCursorShowing();
                g_cursorFreed = true;
                Log("[cursor] menu aberto: cursor LIVRE (wasShowing=%d)", (int)g_cursorWasShowing);
            }
            ForceCursor(true);        // reafirma todo frame: o jogo re-esconde
            ClipCursor(nullptr);      // reafirma: o jogo re-clipa
        }
        else if (g_cursorFreed) {
            g_cursorFreed = false;
            ForceCursor(g_cursorWasShowing);
            Log("[cursor] menu fechado: cursor devolvido (showing=%d)", (int)g_cursorWasShowing);
        }
    }

    static BOOL WINAPI hk_SetCursorPos(int x, int y) {
        // menu aberto: o jogo recentraliza o cursor a cada frame -> engole o warp
        if (MenuWantsMouse()) return TRUE;
        return o_setCursorPos ? o_setCursorPos(x, y) : TRUE;
    }

    bool InstallCursor() {
        HMODULE u32 = GetModuleHandleA("user32.dll");
        if (!u32) return false;
        void* p = (void*)GetProcAddress(u32, "SetCursorPos");
        if (!p) return false;
        if (MH_CreateHook(p, &hk_SetCursorPos, (void**)&o_setCursorPos) != MH_OK) return false;
        MH_EnableHook(p);
        Log("[cursor] hook SetCursorPos OK");
        return true;
    }

    // -----------------------------------------------------------------------
    //  WNDPROC
    // -----------------------------------------------------------------------
    static LRESULT CALLBACK hk_wndProc(HWND, UINT, WPARAM, LPARAM);

    void LogWndProcInfo() {
        if (!g_hwnd) { Log("[wndproc] hwnd=null"); return; }
        LONG_PTR cur = GetWindowLongPtrA(g_hwnd, GWLP_WNDPROC);
        MEMORY_BASIC_INFORMATION mbi{};
        VirtualQuery((void*)cur, &mbi, sizeof(mbi));
        Log("[wndproc] hwnd=%p cur=%p state=0x%X protect=0x%X orig=%p ours=%p",
            (void*)g_hwnd, (void*)cur, (unsigned)mbi.State, (unsigned)mbi.Protect,
            (void*)o_wndProc, (void*)hk_wndProc);
    }

    static thread_local int t_wndDepth = 0;

    static bool IsMouseMsg(UINT m) {
        switch (m) {
        case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK: case WM_RBUTTONDOWN: case WM_RBUTTONUP:
        case WM_RBUTTONDBLCLK: case WM_MBUTTONDOWN: case WM_MBUTTONUP:
        case WM_MBUTTONDBLCLK: case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
            return true;
        }
        return false;
    }

    static LRESULT CALLBACK hk_wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        g_wndProcCalls.fetch_add(1, std::memory_order_relaxed);
        const ULONGLONG now = GetTickCount64();
        g_lastWndMsg.store(now, std::memory_order_relaxed);
        if (IsMouseMsg(msg)) g_lastMouseMsg.store(now, std::memory_order_relaxed);

        if (t_wndDepth > 0)
            return o_wndProc ? CallWindowProcA(o_wndProc, h, msg, wp, lp)
            : DefWindowProcA(h, msg, wp, lp);

        ++t_wndDepth;

        // menu aberto: nao deixa o jogo esconder o cursor
        if (C.menuOpen && msg == WM_SETCURSOR) {
            SetCursor(LoadCursorA(nullptr, (LPCSTR)IDC_ARROW));
            --t_wndDepth;
            return TRUE;
        }

        bool ate = false;
        if (g_imguiInit && g_imCsInit && C.menuOpen) {
            EnterCriticalSection(&g_imCs);
            ImGui_ImplWin32_WndProcHandler(h, msg, wp, lp);
            ImGuiIO& io = ImGui::GetIO();
            ate = io.WantCaptureMouse || io.WantCaptureKeyboard;
            LeaveCriticalSection(&g_imCs);

            if (ate && IsMouseMsg(msg)) {
                --t_wndDepth;
                return TRUE;            // clique nao chega no jogo
            }
        }

        LRESULT r = o_wndProc ? CallWindowProcA(o_wndProc, h, msg, wp, lp)
            : DefWindowProcA(h, msg, wp, lp);
        --t_wndDepth;
        return r;
    }

    // fallback: so' alimenta se o hk_wndProc estiver mudo
    static LRESULT CALLBACK CwpProc(int code, WPARAM wp, LPARAM lp) {
        if (code == HC_ACTION && lp) {
            const CWPSTRUCT* c = (const CWPSTRUCT*)lp;
            g_cwpCalls.fetch_add(1, std::memory_order_relaxed);
            const ULONGLONG now = GetTickCount64();
            const bool wndAlive = (now - g_lastWndMsg.load(std::memory_order_relaxed)) < 50;
            if (!wndAlive && g_imguiInit && g_imCsInit && c->hwnd == g_hwnd) {
                EnterCriticalSection(&g_imCs);
                ImGui_ImplWin32_WndProcHandler(c->hwnd, c->message, c->wParam, c->lParam);
                LeaveCriticalSection(&g_imCs);
            }
        }
        return CallNextHookEx(nullptr, code, wp, lp);
    }

    // -----------------------------------------------------------------------
    //  ImGui — ciclo de vida
    // -----------------------------------------------------------------------
    static void ShutdownImGui() {
        if (!g_imguiInit) return;
        ImGui_ImplOpenGL3_Shutdown();
        if (g_win32Backend) { ImGui_ImplWin32_Shutdown(); g_win32Backend = false; }
        ImGui::DestroyContext();
        g_imguiGeneration.fetch_add(1, std::memory_order_acq_rel);
        g_imguiInit = false;
        Advance(Stage::Hooks);
    }

    static bool InitImGui(HWND hwnd) {
        if (g_imguiInit) return true;
        if (!hwnd) return false;

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        g_imguiGeneration.fetch_add(1, std::memory_order_acq_rel);

        ImGui::GetIO().IniFilename = nullptr;
        ImGui::StyleColorsDark();
        Menu::ApplyStyle();

        if (!ImGui_ImplWin32_Init(hwnd)) { Log("[imgui] Win32 init FALHOU"); return false; }
        g_win32Backend = true;

        if (!ImGui_ImplOpenGL3_Init()) {
            Log("[imgui] OpenGL3 init FALHOU");
            ImGui_ImplWin32_Shutdown();
            g_win32Backend = false;
            return false;
        }

        g_imguiInit = true;
        Log("[imgui] contexto criado");
        Advance(Stage::ImGui);
        return true;
    }

    // -----------------------------------------------------------------------
    //  Janela
    // -----------------------------------------------------------------------
    static HWND ResolveWindow(HDC hdc) {
        HWND h = hdc ? WindowFromDC(hdc) : nullptr;
        if (h && h != GetDesktopWindow()) return h;
        if (g_hwnd && IsWindow(g_hwnd)) return g_hwnd;
        HWND fg = GetForegroundWindow();
        if (fg && fg != GetDesktopWindow()) return fg;
        return FindWindowA("UnityWndClass", nullptr);
    }

    static void RebindWindow(HWND hwnd) {
        if (!hwnd || hwnd == GetDesktopWindow()) return;
        if (g_imCsInit) EnterCriticalSection(&g_imCs);

        HWND oldWnd = g_hwnd;
        LONG_PTR prev = GetWindowLongPtrA(hwnd, GWLP_WNDPROC);

        if (prev == (LONG_PTR)hk_wndProc && hwnd == g_hwnd) {
            if (g_imCsInit) LeaveCriticalSection(&g_imCs);
            return;
        }

        SetWindowLongPtrA(hwnd, GWLP_WNDPROC, (LONG_PTR)hk_wndProc);
        if (prev && prev != (LONG_PTR)hk_wndProc) o_wndProc = (WNDPROC)prev;

        g_hwnd = hwnd;
        Log("[hooks] janela = %p (anterior=%p) orig=%p", (void*)hwnd, (void*)oldWnd, (void*)o_wndProc);

        if (!g_imguiInit) {
            InitImGui(hwnd);
        }
        else if (g_win32Backend) {
            ImGui_ImplWin32_Shutdown();
            g_win32Backend = false;
            if (!ImGui_ImplWin32_Init(hwnd)) {
                Log("[hooks] ImGui_ImplWin32_Init FALHOU (rebind)");
                g_imguiReady.store(false, std::memory_order_release);
            }
            else {
                g_win32Backend = true;
                ImGui::GetIO().IniFilename = nullptr;
                g_imguiReady.store(true, std::memory_order_release);
            }
        }

        if (g_imCsInit) LeaveCriticalSection(&g_imCs);
    }

    // -----------------------------------------------------------------------
    //  TICK — 1x por frame, dedupe por frame id, com watchdog
    // -----------------------------------------------------------------------
    static void RunTickOnce() {
        const uint64_t f = g_frameCounter.load(std::memory_order_relaxed);
        uint64_t previous = g_lastTickFrame.load(std::memory_order_relaxed);
        for (;;) {
            if (previous == f) return;
            if (g_lastTickFrame.compare_exchange_weak(
                previous, f, std::memory_order_acq_rel, std::memory_order_relaxed))
                break;
        }
        SDK::g_frameId = (uint32_t)f;
        __try { Combat::Tick(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { Log("[crash] Combat::Tick"); }
    }

    static void __fastcall hk_lateUpdate(void* self, void* method) {
        PHASE("hk_lateUpdate");
        if (AtLvl(Lvl::Tick)) {
            g_lastLuMs.store(GetTickCount64(), std::memory_order_relaxed);
            RunTickOnce();
        }
        if (o_lateUpdate) o_lateUpdate(self, method);
    }

    static void Watchdog() {
        if (!AtLvl(Lvl::Watchdog)) return;
        if (!g_sdkReady.load(std::memory_order_acquire)) return;
        const ULONGLONG now = GetTickCount64();
        if (now - g_lastLuMs.load(std::memory_order_relaxed) < TICK_WATCHDOG_MS) return;
        g_lastLuMs.store(now, std::memory_order_relaxed);
        RunTickOnce();
    }

    // -----------------------------------------------------------------------
    //  FIRE — blindado + silent aim com slot DETECTADO
    // -----------------------------------------------------------------------
    static bool __fastcall hk_fire7(void* self, void* method, int p0, float p1,
        Vec3 p2, Vec3 p3, bool p4, double p5, int p6) {
        PHASE("hk_fire");
        if (!o_fire7) return false;

        if (StageNow() < Stage::Live || !self || !g_sdkReady.load(std::memory_order_relaxed))
            return o_fire7(self, method, p0, p1, p2, p3, p4, p5, p6);

        // ---- sonda + deteccao automatica do slot da direcao (|v| ~ 1) ----
        static int probe = 0;
        if (probe < 8) {
            ++probe;
            const float l2 = Len(p2), l3 = Len(p3);
            Log("[fire-probe] p2=(%.2f,%.2f,%.2f)|%.3f| p3=(%.2f,%.2f,%.2f)|%.3f| local=(%.1f,%.1f,%.1f)",
                p2.x, p2.y, p2.z, l2, p3.x, p3.y, p3.z, l3,
                SDK::g_localPos.x, SDK::g_localPos.y, SDK::g_localPos.z);

            if (s_dirSlot.load() < 0) {
                const float d2 = fabsf(l2 - 1.f), d3 = fabsf(l3 - 1.f);
                int slot = (d2 <= d3) ? 0 : 1;
                if (d2 > 0.25f && d3 > 0.25f) {
                    Log("[fire-probe] nenhum param e' unitario — assumindo slot 0");
                    slot = 0;
                }
                s_dirSlot.store(slot);
                SDK::g_fireDirSlot.store(slot);
                Log("[fire] dirSlot auto = %d (|p2|=%.3f |p3|=%.3f)", slot, l2, l3);
            }
        }
        const int slot = s_dirSlot.load();

        const Vec3 keep2 = p2;
        const Vec3 keep3 = p3;

        // ---- silent aim ----
        if (C.aimKey) {
            __try {
                Vec3& dir = (slot == 0) ? p2 : p3;
                const Vec3 keepDir = dir;

                // muzzle: usa o parametro irmao SO' se ele parecer uma posicao
                Vec3 muz = (slot == 0) ? p3 : p2;
                const float lm = Len(muz);
                const float lp = Len(SDK::g_localPos);
                if (!(lm > 1.f && lm < 5000.f) || fabsf(lm - lp) > lp)
                    muz = SDK::g_localPos;

                PHASE("fire:aim");
                const bool ok = Aimbot::Silent::Override(muz, dir);

                if (!ok) dir = keepDir;
                else {
                    const float L = Len(dir);           // mantem unitario
                    if (L > 1e-4f) { dir.x /= L; dir.y /= L; dir.z /= L; }
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                p2 = keep2; p3 = keep3;
                Log("[fire] AV no silent aim");
            }
        }

        // ---- no-recoil ----
        if (C.noRecoil) {
            __try { PHASE("fire:recoil"); ApplyNoRecoil(); }
            __except (EXCEPTION_EXECUTE_HANDLER) { Log("[fire] AV no no-recoil"); }
        }

        __try {
            PHASE("fire:orig");
            const bool r = o_fire7(self, method, p0, p1, p2, p3, p4, p5, p6);
            PHASE("fire:post");                 // sobreviveu ao original
            return r;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { Log("[fire] AV no original"); return false; }
    }

    // -----------------------------------------------------------------------
    //  SWAP — orquestracao do frame
    // -----------------------------------------------------------------------
    static bool ImGuiInFrame() {
        ImGuiContext* g = ImGui::GetCurrentContext();
        return g && g->WithinFrameScope;
    }

    static BOOL WINAPI hk_wglSwapBuffers(HDC hdc) {
        PHASE("hk_swap");
        g_frameCounter.fetch_add(1, std::memory_order_relaxed);
        SDK::g_frameId = (uint32_t)g_frameCounter.load(std::memory_order_relaxed);

        if (!o_swap) return TRUE;

        // ---------------- bloco 1: janela / teclas / cursor / beat ----------------
        __try {
            static int winPoll = 0;
            if (!g_hwnd || ++winPoll >= 8) {
                winPoll = 0;
                HWND hwnd = ResolveWindow(hdc);
                if (hwnd && hwnd != g_hwnd) RebindWindow(hwnd);
            }

            // reaplicador SEMPRE ligado: o Unity restaura o GWLP_WNDPROC dele
            if (g_hwnd && o_wndProc) {
                static ULONGLONG lastWndCheck = 0;
                const ULONGLONG nowc = GetTickCount64();
                if (nowc - lastWndCheck > WND_REAPPLY_MS) {
                    lastWndCheck = nowc;
                    LONG_PTR cur = GetWindowLongPtrA(g_hwnd, GWLP_WNDPROC);
                    if (cur != (LONG_PTR)hk_wndProc) {
                        Log("[hooks] wndproc restaurado (cur=%p) — reaplicando", (void*)cur);
                        SetWindowLongPtrA(g_hwnd, GWLP_WNDPROC, (LONG_PTR)hk_wndProc);
                    }
                }
            }

            if (AtLvl(Lvl::CursorFn)) ApplyCursorOwnership();

            // HandleKeys INCONDICIONAL: e' ele que faz o toggle do menu.
            HandleKeys();

            Watchdog();

            static ULONGLONG lastBeat = 0;
            const ULONGLONG nowb = GetTickCount64();
            if (nowb - lastBeat >= 250) {
                lastBeat = nowb;
                Log("[beat] alvos=%d invis=%d wndCall=%u cwpCall=%u av=%u veh=%u abort=%u menu=%d",
                    SDK::g_targetCount, ESP::Invisiveis(),
                    g_wndProcCalls.load(), g_cwpCalls.load(),
                    g_avCount.load(), g_vehCount.load(),
                    g_abortPre.load() + g_abortMenu.load() + g_abortEsp.load() +
                    g_abortMisc.load() + g_abortRender.load() + g_abortDraw.load(),
                    (int)C.menuOpen);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { Log("[crash] bloco1 swap (AV)"); }

        if (!g_imguiInit || !g_hwnd) return o_swap(hdc);

        // >>> [FIX v26] FONTE UNICA: o VIEWPORT do GL e' a verdade; GetClientRect
        // e' so' fallback. W2S, DisplaySize e glViewport usam o MESMO par.
        GLint vp[4] = { 0, 0, 0, 0 };
        glGetIntegerv(GL_VIEWPORT, vp);          // estado a restaurar

        int w = 0, h = 0;
        {
            if (vp[2] > 0 && vp[3] > 0) { w = vp[2]; h = vp[3]; }
            else {
                RECT rc{};
                if (GetClientRect(g_hwnd, &rc)) { w = rc.right - rc.left; h = rc.bottom - rc.top; }
            }
            if (w <= 0 || h <= 0) return o_swap(hdc);

            RECT rc{};
            if (GetClientRect(g_hwnd, &rc) && (rc.right != w || rc.bottom != h)) {
                static bool warned = false;
                if (!warned) {
                    warned = true;
                    Log("[vp] DIVERGENCIA cliente=%dx%d viewport=%dx%d -> usando o viewport",
                        (int)rc.right, (int)rc.bottom, w, h);
                }
            }
            SDK::SetViewport(w, h);
            SetViewSize(w, h);
        }

        if (g_imCsInit) EnterCriticalSection(&g_imCs);

        // ---------------- bloco 2: frame ----------------
        // ANTI-BLINK: usa o estado do ImGui. Se um frame ficou aberto, fecha.
        bool frameOk = false;
        __try {
            if (ImGuiInFrame()) ImGui::EndFrame();

            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplWin32_NewFrame();

            ImGuiIO& io = ImGui::GetIO();
            io.DisplaySize = ImVec2((float)SDK::ScreenW(), (float)SDK::ScreenH());

            // injeccao manual so' como FALLBACK (wndproc mudo).
            if (C.menuOpen) {
                const bool wndMute =
                    (GetTickCount64() - g_lastMouseMsg.load(std::memory_order_relaxed))
                    > MOUSE_FALLBACK_MS;
                    if (wndMute) {
                        POINT pt{};
                        if (GetCursorPos(&pt) && ScreenToClient(g_hwnd, &pt))
                            io.AddMousePosEvent((float)pt.x, (float)pt.y);
                        io.AddMouseButtonEvent(0, (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0);
                        io.AddMouseButtonEvent(1, (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0);
                        io.AddMouseButtonEvent(2, (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0);
                    }
            }

            ImGui::NewFrame();
            frameOk = true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { g_abortPre.fetch_add(1, std::memory_order_relaxed); }

        if (frameOk) {
            __try { Menu::Render(); }
            __except (EXCEPTION_EXECUTE_HANDLER) { g_abortMenu.fetch_add(1, std::memory_order_relaxed); }

            if (AtLeast(Stage::Live) && AtLvl(Lvl::Esp)) {
                __try { ESP::Draw(); }
                __except (EXCEPTION_EXECUTE_HANDLER) { g_abortEsp.fetch_add(1, std::memory_order_relaxed); }

                __try { Misc::Draw(); }
                __except (EXCEPTION_EXECUTE_HANDLER) { g_abortMisc.fetch_add(1, std::memory_order_relaxed); }
            }

            // SEMPRE fecha o frame (Render = EndFrame + build draw data)
            __try { ImGui::Render(); }
            __except (EXCEPTION_EXECUTE_HANDLER) { g_abortRender.fetch_add(1, std::memory_order_relaxed); }

            DrainGlErrors("antes");

            ImDrawData* dd = ImGui::GetDrawData();
            if (dd && dd->Valid && dd->CmdListsCount > 0) {
                glViewport(0, 0, SDK::ScreenW(), SDK::ScreenH());
                __try { ImGui_ImplOpenGL3_RenderDrawData(dd); }
                __except (EXCEPTION_EXECUTE_HANDLER) { g_abortDraw.fetch_add(1, std::memory_order_relaxed); }
            }

            DrainGlErrors("depois");

            static uint32_t lastSum = 0;
            static ULONGLONG lastAbLog = 0;
            const uint32_t sum = g_abortPre + g_abortMenu + g_abortEsp +
                g_abortMisc + g_abortRender + g_abortDraw;
            if (sum != lastSum && GetTickCount64() - lastAbLog >= 1000) {
                lastSum = sum;
                lastAbLog = GetTickCount64();
                Log("[crash] aborts: pre=%u menu=%u esp=%u misc=%u render=%u draw=%u",
                    g_abortPre.load(), g_abortMenu.load(), g_abortEsp.load(),
                    g_abortMisc.load(), g_abortRender.load(), g_abortDraw.load());
            }
        }

        glViewport(vp[0], vp[1], vp[2], vp[3]);
        if (g_imCsInit) LeaveCriticalSection(&g_imCs);

        return o_swap(hdc);
    }

    // -----------------------------------------------------------------------
    //  Teclas — DONO UNICO do toggle do menu
    // -----------------------------------------------------------------------
    void HandleKeys() {
        if (Edge(C.menuKey))   C.menuOpen = !C.menuOpen;   // unico lugar que flipa
        if (Edge(C.panicKey))  Misc::Panic();
        if (Edge(C.saveKey))   ConfigIO::Save();
        if (Edge(C.unloadKey)) { ConfigIO::Save(); g_unload = true; }
        if (C.aimKey)          C.aimKeyDown = KeyDown(C.aimKey);
    }

    // -----------------------------------------------------------------------
    //  Instalacao
    // -----------------------------------------------------------------------
    bool InstallSwap() {
        HMODULE gl = GetModuleHandleA("opengl32.dll");
        if (!gl) { Log("[hooks] opengl32.dll nao carregada"); return false; }
        void* p = (void*)GetProcAddress(gl, "wglSwapBuffers");
        if (!p) { Log("[hooks] wglSwapBuffers NAO encontrado"); return false; }
        if (MH_CreateHook(p, &hk_wglSwapBuffers, (void**)&o_swap) != MH_OK) {
            Log("[hooks] MH_CreateHook(wglSwapBuffers) FALHOU");
            return false;
        }
        MH_EnableHook(p);
        Log("[hooks] wglSwapBuffers hook OK");
        return true;
    }

    bool InstallMainThread() {
        void* p = FindMethodPtr("PlayerScript", "LateUpdate", 0);
        if (!p) { Log("[hooks] PlayerScript::LateUpdate NAO encontrado"); return false; }
        if (MH_CreateHook(p, &hk_lateUpdate, (void**)&o_lateUpdate) != MH_OK) {
            Log("[hooks] MH_CreateHook(LateUpdate) FALHOU");
            return false;
        }
        MH_EnableHook(p);
        Log("[hooks] main thread: PlayerScript::LateUpdate");
        return true;
    }

    bool InstallCombat() {
        Il2CppClass* k = FindClassNamed("PlayerScript");
        MethodInfo* m = k ? FindMethodByArity(k, "FireBullet", 7) : nullptr;
        if (m) {
            Log("[fire] FireBullet(7) return=%s", SDK::MethodReturnName(m));
        }
        if (!m) { Log("[hooks] FireBullet(7) NAO encontrado"); return false; }

        void* p = SDK::MethodPtr(m);
        if (!p) { Log("[hooks] FireBullet(7) sem ponteiro"); return false; }
        if (MH_CreateHook(p, &hk_fire7, (void**)&o_fire7) != MH_OK) {
            Log("[hooks] MH_CreateHook(FireBullet) FALHOU");
            return false;
        }
        MH_EnableHook(p);
        Log("[hooks] FireBullet(7) hook OK (%p)", p);
        return true;
    }

    bool InstallCwp() {
        g_cwp = SetWindowsHookExA(WH_CALLWNDPROC, CwpProc, nullptr, GetCurrentThreadId());
        if (!g_cwp) { Log("[input] SetWindowsHookEx(CALLWNDPROC) FALHOU"); return false; }
        Log("[input] cwp=%p tid=%u lvl=%d (fallback)", (void*)g_cwp, GetCurrentThreadId(), INPUT_TEST_LEVEL);
        return true;
    }

    // -----------------------------------------------------------------------
    //  Init / Reinstall / Shutdown
    // -----------------------------------------------------------------------
    bool Init() {
        if (!g_imCsInit) { InitializeCriticalSection(&g_imCs); g_imCsInit = true; }
        if (MH_Initialize() != MH_OK) { Log("[hooks] MH_Initialize FALHOU"); return false; }

        SetStage(Stage::Init);
        Log("[input] INPUT_TEST_LEVEL=%d", INPUT_TEST_LEVEL);

        if (!g_veh) g_veh = AddVectoredExceptionHandler(1, VehHandler);

        if (AtLvl(Lvl::CursorFn)) InstallCursor();
        InstallCwp();

        if (!InstallSwap()) return false;
        Advance(Stage::Hooks);

        if (!InstallMainThread()) Log("[hooks] LateUpdate falhou — watchdog assume");
        if (!InstallCombat())     Log("[hooks] FireBullet falhou (aimbot ok)");

        g_lastLuMs.store(GetTickCount64(), std::memory_order_relaxed);
        SetStage(Stage::Live);
        Log("[hooks] init ok stage=%s veh=%p menuKey=0x%X", StageName(StageNow()), g_veh, C.menuKey);
        return true;
    }

    bool Reinstall() {
        if (g_hwnd) {
            LONG_PTR prev = GetWindowLongPtrA(g_hwnd, GWLP_WNDPROC);
            if (prev != (LONG_PTR)hk_wndProc) {
                o_wndProc = (WNDPROC)prev;
                SetWindowLongPtrA(g_hwnd, GWLP_WNDPROC, (LONG_PTR)hk_wndProc);
            }
        }
        MH_DisableHook(MH_ALL_HOOKS);
        MH_EnableHook(MH_ALL_HOOKS);
        Log("[hooks] Reinstall ok");
        return true;
    }

    bool Shutdown() {
        if (StageNow() == Stage::Down) { Log("[hooks] Shutdown: ja' em Down"); return true; }
        SetStage(Stage::Tearing);

        g_hooksLive.store(false, std::memory_order_release);
        g_imguiReady.store(false, std::memory_order_release);

        SDK::g_recoilHookActive = false;

        if (g_cursorFreed) { g_cursorFreed = false; ForceCursor(g_cursorWasShowing); }

        if (o_wndProc && g_hwnd)
            SetWindowLongPtrA(g_hwnd, GWLP_WNDPROC, (LONG_PTR)o_wndProc);
        o_wndProc = nullptr;

        if (g_cwp) { UnhookWindowsHookEx(g_cwp); g_cwp = nullptr; }
        if (g_veh) { RemoveVectoredExceptionHandler(g_veh); g_veh = nullptr; }

        MH_DisableHook(MH_ALL_HOOKS);
        Sleep(120);

        if (g_imCsInit) EnterCriticalSection(&g_imCs);
        ShutdownImGui();
        if (g_imCsInit) LeaveCriticalSection(&g_imCs);

        MH_Uninitialize();
        g_lastLuMs.store(0, std::memory_order_relaxed);
        SetStage(Stage::Down);
        Log("[hooks] Shutdown concluido");
        return true;
    }

} // namespace Hooks