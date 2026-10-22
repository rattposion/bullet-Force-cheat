// ============================================================================
//  hooks.cpp  (v30)  — render (wglSwapBuffers), hooks de thread, wndproc, cursor
//
//  >>> FIX v30  (PATCH R1 — VIGIA DO RENDER)
//   O overlay some durante a partida (ESP, profiler, watermark). Agora tem:
//    [R1a] motivo nomeado em CADA ponto onde o frame deixa de ser desenhado
//          (sem-hwnd / imgui-off / nao-live / viewport-0 / newframe-excecao /
//          sem-drawdata / desenho-falhou). Log 1x/s com o streak.
//    [R1b] contador frameLeftOpen: quantas vezes um frame do ImGui ficou
//          ABERTO (NewFrame sem Render). Se ESP::Draw der excecao no meio,
//          o contexto corrompe e o overlay some pra sempre — agora da' pra ver.
//    [R1c] thread de vigilancia: se o hk_wglSwapBuffers parar de ser chamado
//          (outro overlay hookou depois de nos, ou MinHook caiu), loga
//          "SWAP MUDO" + estado da janela/wndproc/stage.
//    [R1d] idade do snapshot do ESP e do ultimo tick -> mostra se o overlay
//          some porque a fonte de dados parou.
//
//  >>> FIX v29 (A11): assinatura correta do FireBullet (method e' o ULTIMO).
//  >>> FIX v28 (A9): sonda de ABI/espaco/times.
//  >>> FIX v27 (A2,A6,A7,A8,D1,B2).   >>> FIX v26: slot da direcao detectado.
//  >>> FIX v21: SetStage nao escreve g_sdkReady (dono: Combat::Tick).
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

// ---------------------------------------------------------------------------
//  [A11c] MODO DE TESTE DO HOOK DE TIRO
//    0 = normal | 1 = pass-through cru | 2 = CONTINUE_SEARCH | 3 = runtime_invoke
// ---------------------------------------------------------------------------
#define FIRE_TEST_MODE    0

#define WATCH_MAX_LINES   12
#define PROBE_DIFF_MAX    8
#define TICK_WATCHDOG_MS  400
#define WND_REAPPLY_MS    500
#define MOUSE_FALLBACK_MS 120
#define INPUT_TEST_LEVEL  7

extern const char* g_phase;
#ifndef PHASE
#define PHASE(x) (g_phase = (x))
#endif

namespace Hooks {

    // -----------------------------------------------------------------------
    //  Globais do hooks.h
    // -----------------------------------------------------------------------
    std::atomic<bool>     g_hooksLive{ false };
    std::atomic<bool>     g_imguiReady{ false };
    std::atomic<bool>     g_sdkReady{ false };
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
    static std::atomic<uint32_t> g_fireAv{ 0 };

    static std::atomic<uint32_t> g_abortPre{ 0 };
    static std::atomic<uint32_t> g_abortMenu{ 0 };
    static std::atomic<uint32_t> g_abortEsp{ 0 };
    static std::atomic<uint32_t> g_abortMisc{ 0 };
    static std::atomic<uint32_t> g_abortRender{ 0 };
    static std::atomic<uint32_t> g_abortDraw{ 0 };

    // ---- [R1] vigia do render ----
    static std::atomic<ULONGLONG> g_lastSwapMs{ 0 };
    static std::atomic<ULONGLONG> g_lastDrawOkMs{ 0 };
    static std::atomic<ULONGLONG> g_lastTickMs{ 0 };
    static std::atomic<uint32_t>  g_swapCalls{ 0 };
    static std::atomic<uint32_t>  g_noDrawStreak{ 0 };
    static std::atomic<uint32_t>  g_noDrawReason{ 0 };
    static std::atomic<uint32_t>  g_ndCount[10]{};
    static std::atomic<uint32_t>  g_frameLeftOpen{ 0 };
    static std::atomic<uint32_t>  g_espStale{ 0 };

    static std::atomic<ULONGLONG> g_lastWndMsg{ 0 };
    static std::atomic<ULONGLONG> g_lastMouseMsg{ 0 };

    static std::atomic<int> s_dirSlot{ -1 };

    // forward: o vigia do render (RenderWatchThread) usa o endereco do wndproc
    static LRESULT CALLBACK hk_wndProc(HWND, UINT, WPARAM, LPARAM);

    uint32_t FrameId() { return (uint32_t)g_frameCounter.load(std::memory_order_relaxed); }

    // -----------------------------------------------------------------------
    //  Escada INPUT_TEST_LEVEL
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
    //  Ciclo de vida
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
    typedef bool(__fastcall* tFireBullet)(void*, int, float, Vec3, Vec3, bool, double, int, void*);

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
    static inline float Dot3(const Vec3& a, const Vec3& b) {
        return a.x * b.x + a.y * b.y + a.z * b.z;
    }

    static Vec3 RotVec(const Quat& q, const Vec3& v) {
        const float x = q.x, y = q.y, z = q.z, w = q.w;
        const float tx = 2.f * (y * v.z - z * v.y);
        const float ty = 2.f * (z * v.x - x * v.z);
        const float tz = 2.f * (x * v.y - y * v.x);
        return Vec3(v.x + w * tx + (y * tz - z * ty),
            v.y + w * ty + (z * tx - x * tz),
            v.z + w * tz + (x * ty - y * tx));
    }
    static Vec3 RotInvVec(const Quat& q, const Vec3& v) {
        const Quat c{ -q.x, -q.y, -q.z, q.w };
        return RotVec(c, v);
    }

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

    // -----------------------------------------------------------------------
    //  [R1] VIGIA DO RENDER
    // -----------------------------------------------------------------------
    enum NdReason : uint32_t {
        ND_OK = 0, ND_NO_SWAP, ND_NO_HWND, ND_IMGUI_OFF, ND_NOT_LIVE,
        ND_VP_ZERO, ND_NEWFRAME_EXC, ND_NO_DRAWDATA, ND_DRAW_EXC, ND_ESP_STALE
    };

    static const char* NdName(uint32_t r) {
        switch (r) {
        case ND_OK:            return "ok";
        case ND_NO_SWAP:       return "sem-o_swap";
        case ND_NO_HWND:       return "sem-hwnd";
        case ND_IMGUI_OFF:     return "imgui-off";
        case ND_NOT_LIVE:      return "stage<Live";
        case ND_VP_ZERO:       return "viewport-0";
        case ND_NEWFRAME_EXC:  return "newframe-excecao";
        case ND_NO_DRAWDATA:   return "sem-drawdata";
        case ND_DRAW_EXC:      return "desenho-falhou";
        case ND_ESP_STALE:     return "esp-snapshot-velho";
        default:               return "?";
        }
    }

    static inline void NoteNoDraw(uint32_t r) {
        g_noDrawReason.store(r, std::memory_order_relaxed);
        g_noDrawStreak.fetch_add(1, std::memory_order_relaxed);
        if (r < 10) g_ndCount[r].fetch_add(1, std::memory_order_relaxed);
    }
    static inline void ClearNoDraw() {
        g_noDrawStreak.store(0, std::memory_order_relaxed);
        g_lastDrawOkMs.store(GetTickCount64(), std::memory_order_relaxed);
    }

    static void ReportRenderHealth() {
        static ULONGLONG last = 0;
        const ULONGLONG now = GetTickCount64();
        if (now - last < 1000) return;
        last = now;

        const uint32_t streak = g_noDrawStreak.load(std::memory_order_relaxed);
        if (streak == 0 && !C.probeLog) return;

        const ESP::Snap* sn = ESP::Latest();
        const uint32_t espAge = sn
            ? (uint32_t)(g_frameCounter.load(std::memory_order_relaxed) - sn->frame)
            : 0xFFFFFFFFu;
        const ULONGLONG tickAge = g_lastTickMs.load() ? (now - g_lastTickMs.load()) : 0;
        const ULONGLONG drawAge = g_lastDrawOkMs.load() ? (now - g_lastDrawOkMs.load()) : 0;

        Log("[rdiag] sem desenho: motivo=%s streak=%u | frameLeftOpen=%u espStale=%u "
            "abort[pre=%u menu=%u esp=%u misc=%u render=%u draw=%u] | hwnd=%p valido=%d imgui=%d "
            "live=%d menu=%d espSnapAge=%u espCount=%d tickAge=%llums drawAge=%llums",
            NdName(g_noDrawReason.load(std::memory_order_relaxed)), streak,
            g_frameLeftOpen.load(), g_espStale.load(),
            g_abortPre.load(), g_abortMenu.load(), g_abortEsp.load(),
            g_abortMisc.load(), g_abortRender.load(), g_abortDraw.load(),
            (void*)g_hwnd, (int)(g_hwnd && IsWindow(g_hwnd)), (int)g_imguiInit,
            (int)AtLeast(Stage::Live), (int)C.menuOpen,
            espAge, sn ? sn->count : -1,
            (unsigned long long)tickAge, (unsigned long long)drawAge);
    }

    static HANDLE g_rwThread = nullptr;
    static std::atomic<bool> g_rwStop{ false };

    static DWORD WINAPI RenderWatchThread(LPVOID) {
        ULONGLONG lastLogged = 0;
        for (;;) {
            if (g_rwStop.load()) break;
            Sleep(500);

            const ULONGLONG now = GetTickCount64();
            const ULONGLONG last = g_lastSwapMs.load(std::memory_order_relaxed);
            if (!last) continue;

            const ULONGLONG gap = now - last;
            if (gap < 1200) continue;
            if (now - lastLogged < 2000) continue;
            lastLogged = now;

            LONG_PTR cur = g_hwnd ? GetWindowLongPtrA(g_hwnd, GWLP_WNDPROC) : 0;
            Log("[rdiag] SWAP MUDO ha %llu ms — o hk_wglSwapBuffers NAO esta sendo chamado | "
                "hwnd=%p valido=%d wndproc=%p nosso=%p stage=%s imgui=%d swapCalls=%u",
                (unsigned long long)gap, (void*)g_hwnd,
                (int)(g_hwnd && IsWindow(g_hwnd)),
                (void*)cur, (void*)hk_wndProc, StageName(StageNow()),
                (int)g_imguiInit, g_swapCalls.load());
        }
        return 0;
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

    static inline bool MenuWantsMouse() {
        return C.menuOpen && g_imguiReady.load(std::memory_order_relaxed);
    }

    static bool s_silentLatched = false;
    static inline bool SilentGate() {
        if (!C.silentAim) return false;
        if (C.aimbot)     return Aimbot::IsActive();
        if (C.aimToggle)  return s_silentLatched;
        return C.aimKeyDown;
    }

    static Vec3 SafeLocalEye() {
        Vec3 lp = SDK::g_localPos;
        if (fabsf(lp.x) < 0.01f && fabsf(lp.z) < 0.01f && SDK::g_localPlayer)
            lp = SDK::GetPos(SDK::g_localPlayer);

        if (Il2CppObject* cam = SDK::CamTfObj()) {
            Vec3 c = SDK::GetPos(cam);
            if (c.isSane() && !c.zero()) return c;
        }
        if (lp.isSane() && !lp.zero()) return Vec3(lp.x, lp.y + 1.5f, lp.z);
        return SDK::MuzzlePos();
    }

    static bool RealMuzzle(Vec3& out) {
        Il2CppObject* gun = SDK::GetGunStatsLive();
        if (!gun || !SDK::ValidInstance(gun)) return false;

        static uint32_t offMuzzle = 0;
        if (!offMuzzle) {
            uint32_t o = 0;
            if (Il2CppClass* k = SDK::SafeClass(gun))
                o = SDK::PickFieldOff(k,
                    { "muzzlePoint", "muzzle", "muzzleTransform", "barrel", "firePoint" },
                    "Transform");
            if (!o) o = 0x268;
            offMuzzle = o;
            Log("[fire] muzzlePoint off=0x%X", offMuzzle);
        }

        Il2CppObject* tf = SDK::SafeReadObjPub(gun, offMuzzle);
        if (!tf) return false;

        Vec3 p = SDK::GetPos(tf);
        if (!p.isSane() || p.zero()) return false;
        out = p;
        return true;
    }

    static bool SaneOrigin(const Vec3& p) {
        if (!p.isSane() || p.zero()) return false;
        Vec3 lp = SDK::g_localPos;
        if (fabsf(lp.x) < 0.01f && fabsf(lp.z) < 0.01f) return true;
        return (p - lp).len() < 12.f;
    }

    static LONG CALLBACK VehHandler(EXCEPTION_POINTERS* ep) {
        if (!ep || !ep->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;
        if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
            g_vehCount.fetch_add(1, std::memory_order_relaxed);
            const auto* r = ep->ExceptionRecord;
            Log("[veh] AV %s em %p (alvo=%p) fase=%s",
                (r->ExceptionInformation[0] == 0) ? "read" :
                (r->ExceptionInformation[0] == 1 ? "write" : "exec"),
                r->ExceptionAddress, (void*)r->ExceptionInformation[1],
                g_phase ? g_phase : "?");
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }

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

    static bool g_cursorFreed = false;
    static bool g_cursorWasShowing = true;

    static bool IsCursorShowing() {
        CURSORINFO ci{};
        ci.cbSize = sizeof(ci);
        if (!GetCursorInfo(&ci)) return true;
        return (ci.flags & CURSOR_SHOWING) != 0;
    }

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
            ForceCursor(true);
            ClipCursor(nullptr);
        }
        else if (g_cursorFreed) {
            g_cursorFreed = false;
            ForceCursor(g_cursorWasShowing);
            Log("[cursor] menu fechado: cursor devolvido (showing=%d)", (int)g_cursorWasShowing);
        }
    }

    static BOOL WINAPI hk_SetCursorPos(int x, int y) {
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
                return TRUE;
            }
        }

        LRESULT r = o_wndProc ? CallWindowProcA(o_wndProc, h, msg, wp, lp)
            : DefWindowProcA(h, msg, wp, lp);
        --t_wndDepth;
        return r;
    }

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
        g_lastTickMs.store(GetTickCount64(), std::memory_order_relaxed);   // [R1d]
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
    //  FIRE  (assinatura real: method e' o ULTIMO)
    // -----------------------------------------------------------------------
    static bool __fastcall hk_fire7(void* self, int p0, float p1, Vec3 p2, Vec3 p3,
        bool p4, double p5, int p6, void* method) {
        PHASE("hk_fire");
        if (!o_fire7) return false;

        if (StageNow() < Stage::Live || !self || !g_sdkReady.load(std::memory_order_relaxed))
            return o_fire7(self, p0, p1, p2, p3, p4, p5, p6, method);

        static int probe = 0;
        if (probe < 8) {
            ++probe;
            __try {
                const Vec3 lp = SDK::g_localPos;
                Log("[fire-probe] p0=%d p1=%.3f p4=%d p5=%.3f p6=%d method=%p",
                    p0, p1, (int)p4, p5, p6, method);
                Log("[fire-probe] p2=(%.2f,%.2f,%.2f)|%.3f| dLocal=%.2f  p3=(%.2f,%.2f,%.2f)|%.3f| dLocal=%.2f",
                    p2.x, p2.y, p2.z, Len(p2), (p2 - lp).len(),
                    p3.x, p3.y, p3.z, Len(p3), (p3 - lp).len());

                if (Il2CppObject* lpo = SDK::g_localPlayer) {
                    const Quat pr = SDK::GetPlayerRot(lpo);
                    const Vec3 rotFwd = RotVec(pr, Vec3(0.f, 0.f, 1.f));
                    Log("[fire-probe] espaco: dot(p2,rotFwd)=%.3f dot(p3,rotFwd)=%.3f",
                        Dot3(p2, rotFwd), Dot3(p3, rotFwd));
                }

                int t0 = 0, t1 = 0, t2 = 0, tOut = 0;
                SDK::LockTargets();
                for (int i = 0; i < SDK::g_targetCount; ++i) {
                    const int tt = SDK::g_targets[i].team;
                    if (tt == 0) ++t0; else if (tt == 1) ++t1;
                    else if (tt == 2) ++t2; else ++tOut;
                }
                SDK::UnlockTargets();
                Log("[fire-probe] times: local=%d t0=%d t1=%d t2=%d outros=%d",
                    SDK::g_localTeam, t0, t1, t2, tOut);

                if (s_dirSlot.load() < 0) {
                    const float d2 = fabsf(Len(p2) - 1.f), d3 = fabsf(Len(p3) - 1.f);
                    int slot = (d2 <= d3) ? 0 : 1;
                    if (d2 > 0.25f && d3 > 0.25f) slot = 1;
                    s_dirSlot.store(slot);
                    SDK::g_fireDirSlot.store(slot);
                    Log("[fire] dirSlot auto = %d (|p2|=%.3f |p3|=%.3f)", slot, Len(p2), Len(p3));
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER) { Log("[fire-probe] AV na sonda"); }
        }

#if FIRE_TEST_MODE == 1
        PHASE("fire:orig");
        return o_fire7(self, p0, p1, p2, p3, p4, p5, p6, method);
#endif

        const int slot = s_dirSlot.load();
        const Vec3 keep2 = p2;
        const Vec3 keep3 = p3;

        if (SilentGate()) {
            __try {
                Vec3& dir = (slot == 0) ? p2 : p3;
                const Vec3 keepDir = dir;

                Vec3 origin = (slot == 0) ? p3 : p2;
                if (!SaneOrigin(origin)) {
                    Vec3 m;
                    if (RealMuzzle(m) && SaneOrigin(m)) origin = m;
                    else origin = SafeLocalEye();
                }

                PHASE("fire:aim");
                const bool ok = Aimbot::Silent::Override(origin, dir);

                if (!ok) dir = keepDir;
                else {
                    const float L = Len(dir);
                    if (L > 1e-4f) { dir.x /= L; dir.y /= L; dir.z /= L; }
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                p2 = keep2; p3 = keep3;
                Log("[fire] AV no silent aim");
            }
        }

        if (C.noRecoil) {
            __try { PHASE("fire:recoil"); ApplyNoRecoil(); }
            __except (EXCEPTION_EXECUTE_HANDLER) { Log("[fire] AV no no-recoil"); }
        }

#if FIRE_TEST_MODE == 3
        __try {
            int32_t ib0 = p0, ib4 = p4 ? 1 : 0, ib6 = p6;
            float   fb1 = p1;
            Vec3    vb2 = p2, vb3 = p3;
            double  db5 = p5;
            void* args[7] = { &ib0, &fb1, &vb2, &vb3, &ib4, &db5, &ib6 };
            PHASE("fire:invoke");
            il2cpp_runtime_invoke(SDK::M_FireBullet, self, args, nullptr);
            PHASE("fire:post");
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { Log("[fire] AV no runtime_invoke"); }
        return true;
#endif

#if FIRE_TEST_MODE == 2
        PHASE("fire:orig");
        __try {
            o_fire7(self, p0, p1, p2, p3, p4, p5, p6, method);
        }
        __except (EXCEPTION_CONTINUE_SEARCH) {}
        PHASE("fire:post");
        return true;
#else
        __try {
            PHASE("fire:orig");
            const bool r = o_fire7(self, p0, p1, p2, p3, p4, p5, p6, method);
            PHASE("fire:post");
            return r;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            const uint32_t n = g_fireAv.fetch_add(1, std::memory_order_relaxed) + 1;
            Log("[fire] AV no original (n=%u) slot=%d silent=%d", n, slot, (int)C.silentAim);
            return true;
        }
#endif
    }

    // -----------------------------------------------------------------------
    //  SWAP
    // -----------------------------------------------------------------------
    static bool ImGuiInFrame() {
        ImGuiContext* g = ImGui::GetCurrentContext();
        return g && g->WithinFrameScope;
    }

    static BOOL WINAPI hk_wglSwapBuffers(HDC hdc) {
        PHASE("hk_swap");
        g_frameCounter.fetch_add(1, std::memory_order_relaxed);
        SDK::g_frameId = (uint32_t)g_frameCounter.load(std::memory_order_relaxed);
        g_lastSwapMs.store(GetTickCount64(), std::memory_order_relaxed);     // [R1c]
        g_swapCalls.fetch_add(1, std::memory_order_relaxed);

        if (!o_swap) { NoteNoDraw(ND_NO_SWAP); return TRUE; }

        __try {
            static int winPoll = 0;
            if (!g_hwnd || ++winPoll >= 8) {
                winPoll = 0;
                HWND hwnd = ResolveWindow(hdc);
                if (hwnd && hwnd != g_hwnd) RebindWindow(hwnd);
            }

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

            HandleKeys();

            Watchdog();

            static ULONGLONG lastBeat = 0;
            const ULONGLONG nowb = GetTickCount64();
            if (C.beatLog && nowb - lastBeat >= 250) {
                lastBeat = nowb;
                Log("[beat] alvos=%d invis=%d wndCall=%u cwpCall=%u av=%u veh=%u fireAv=%u abort=%u menu=%d",
                    SDK::g_targetCount, ESP::Invisiveis(),
                    g_wndProcCalls.load(), g_cwpCalls.load(),
                    g_avCount.load(), g_vehCount.load(), g_fireAv.load(),
                    g_abortPre.load() + g_abortMenu.load() + g_abortEsp.load() +
                    g_abortMisc.load() + g_abortRender.load() + g_abortDraw.load(),
                    (int)C.menuOpen);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { Log("[crash] bloco1 swap (AV)"); }

        if (!g_imguiInit || !g_hwnd) { NoteNoDraw(ND_IMGUI_OFF); ReportRenderHealth(); return o_swap(hdc); }

        GLint vp[4] = { 0, 0, 0, 0 };
        glGetIntegerv(GL_VIEWPORT, vp);

        int w = 0, h = 0;
        {
            if (vp[2] > 0 && vp[3] > 0) { w = vp[2]; h = vp[3]; }
            else {
                RECT rc{};
                if (GetClientRect(g_hwnd, &rc)) { w = rc.right - rc.left; h = rc.bottom - rc.top; }
            }
            if (w <= 0 || h <= 0) { NoteNoDraw(ND_VP_ZERO); ReportRenderHealth(); return o_swap(hdc); }

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

        bool frameOk = false;
        __try {
            // [R1b] se um frame ficou ABERTO, o contexto esta' em risco
            if (ImGuiInFrame()) {
                g_frameLeftOpen.fetch_add(1, std::memory_order_relaxed);
                ImGui::EndFrame();
            }

            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplWin32_NewFrame();

            ImGuiIO& io = ImGui::GetIO();
            io.DisplaySize = ImVec2((float)SDK::ScreenW(), (float)SDK::ScreenH());

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
        __except (EXCEPTION_EXECUTE_HANDLER) {
            g_abortPre.fetch_add(1, std::memory_order_relaxed);
            NoteNoDraw(ND_NEWFRAME_EXC);
        }

        if (!frameOk) { ReportRenderHealth(); if (g_imCsInit) LeaveCriticalSection(&g_imCs); return o_swap(hdc); }

        bool espDrew = false;
        __try { Menu::Render(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { g_abortMenu.fetch_add(1, std::memory_order_relaxed); }

        if (AtLeast(Stage::Live) && AtLvl(Lvl::Esp)) {
            // [R1d] o overlay some se a fonte parar: loga quando o snap envelhece
            const ESP::Snap* sn = ESP::Latest();
            if (!sn || (uint32_t)(g_frameCounter.load(std::memory_order_relaxed) - sn->frame) > 90)
                g_espStale.fetch_add(1, std::memory_order_relaxed);

            __try { ESP::Draw(); espDrew = true; }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                g_abortEsp.fetch_add(1, std::memory_order_relaxed);
                Log("[rdiag] EXCECAO em ESP::Draw (frame do ImGui pode ter ficado aberto)");
            }

            __try { Misc::Draw(); }
            __except (EXCEPTION_EXECUTE_HANDLER) { g_abortMisc.fetch_add(1, std::memory_order_relaxed); }
        }
        else {
            NoteNoDraw(ND_NOT_LIVE);
        }

        __try { ImGui::Render(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { g_abortRender.fetch_add(1, std::memory_order_relaxed); }

        if (C.probeLog) DrainGlErrors("antes");

        ImDrawData* dd = ImGui::GetDrawData();
        if (dd && dd->Valid && dd->CmdListsCount > 0) {
            glViewport(0, 0, SDK::ScreenW(), SDK::ScreenH());
            __try { ImGui_ImplOpenGL3_RenderDrawData(dd); }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                g_abortDraw.fetch_add(1, std::memory_order_relaxed);
                NoteNoDraw(ND_DRAW_EXC);
            }
            if (g_noDrawReason.load() != ND_DRAW_EXC) ClearNoDraw();
        }
        else {
            NoteNoDraw(ND_NO_DRAWDATA);
        }

        if (C.probeLog) DrainGlErrors("depois");

        static uint32_t lastSum = 0;
        static ULONGLONG lastAbLog = 0;
        const uint32_t sum = g_abortPre + g_abortMenu + g_abortEsp +
            g_abortMisc + g_abortRender + g_abortDraw;
        if (sum != lastSum && GetTickCount64() - lastAbLog >= 1000) {
            lastSum = sum;
            lastAbLog = GetTickCount64();
            Log("[crash] aborts: pre=%u menu=%u esp=%u misc=%u render=%u draw=%u frameLeftOpen=%u",
                g_abortPre.load(), g_abortMenu.load(), g_abortEsp.load(),
                g_abortMisc.load(), g_abortRender.load(), g_abortDraw.load(),
                g_frameLeftOpen.load());
        }

        ReportRenderHealth();

        glViewport(vp[0], vp[1], vp[2], vp[3]);
        if (g_imCsInit) LeaveCriticalSection(&g_imCs);

        return o_swap(hdc);
    }

    void HandleKeys() {
        if (Edge(C.menuKey))   C.menuOpen = !C.menuOpen;
        if (Edge(C.panicKey))  Misc::Panic();
        if (Edge(C.saveKey))   ConfigIO::Save();
        if (Edge(C.unloadKey)) { ConfigIO::Save(); g_unload = true; }
        if (C.aimKey)          C.aimKeyDown = KeyDown(C.aimKey);
        if (C.aimToggle && C.silentAim && Edge(C.aimKey)) s_silentLatched = !s_silentLatched;
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
        MethodInfo* m = SDK::M_FireBullet;
        if (!m) {
            Il2CppClass* k = FindClassNamed("PlayerScript");
            m = k ? FindMethodByArity(k, "FireBullet", 7) : nullptr;
        }
        if (!m) { Log("[hooks] FireBullet(7) NAO encontrado"); return false; }

        const uint32_t np = il2cpp_method_get_param_count ? il2cpp_method_get_param_count(m) : 0;
        if (np != 7)
            Log("[hooks] AVISO: FireBullet resolvido com %u params (esperado 7) — fallback de nome?", np);
        Log("[fire] FireBullet params=%u ret=%s", np, SDK::MethodReturnName(m));

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

        // [R1c] vigia do render
        if (!g_rwThread) {
            g_rwStop.store(false);
            g_rwThread = CreateThread(nullptr, 0, RenderWatchThread, nullptr, 0, nullptr);
            Log("[rdiag] vigia do render: thread=%p", g_rwThread);
        }

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

        g_rwStop.store(true);
        if (g_rwThread) { WaitForSingleObject(g_rwThread, 1500); CloseHandle(g_rwThread); g_rwThread = nullptr; }

        g_lastLuMs.store(0, std::memory_order_relaxed);
        SetStage(Stage::Down);
        Log("[hooks] Shutdown concluido");
        return true;
    }

} // namespace Hooks