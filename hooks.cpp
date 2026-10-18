// ============================================================================
//  hooks.cpp  (v4 — alinhado com o hooks.h novo)
//
//  >>> MUDANCAS DESTA VERSAO (interface):
//  A) g_frameCounter: uint32_t -> uint64_t. Todos os pontos que comparavam
//     com ele passaram a usar uint64_t e sentinela ~0ull.
//  B) g_viewW/g_viewH: int -> std::atomic<int>, escritos SO' por
//     SetViewSize(). Para o par coerente, GetViewSize().
//  C) SafeFont: ImFont* -> ImFont& (sempre referencia valida) + FontCount().
//  D) Shutdown: void -> bool. Idempotente, termina sempre em Down.
//  E) Ciclo de vida por Stage. SetStage() e' o UNICO escritor dos espelhos
//     g_sdkReady/g_hooksLive/g_imguiReady. InitImGui/Init usam Advance(),
//     que so' avanca — nunca regride um espelho ja' publicado.
//
//  >>> CONTEUDO FUNCIONAL DA v3 (preservado):
//  23) hk_lateUpdate SEM early-return por instancia (era o que matava o ESP).
//  24) WATCHDOG: swap assume o tick se o LateUpdate sumir por >250 ms.
//  25) ClassOf valida com Plausible/PlausibleObj.
//  26) Shutdown reseta g_lastTickMs.
//  16-22, 1-15: mantidos (throttle de log, __try nos blocos certos, CS manual,
//     rebind sem re-init, FindClassNamed exato, etc.)
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

#pragma comment(lib, "opengl32.lib")

#define WATCH_MAX_LINES   12
#define PROBE_DIFF_MAX    8
#define TICK_WATCHDOG_MS  250

namespace Hooks {

    // -----------------------------------------------------------------------
    //  Estado global
    // -----------------------------------------------------------------------
    std::atomic<bool>     g_imguiReady{ false };
    std::atomic<bool>     g_hooksLive{ false };
    std::atomic<bool>     g_sdkReady{ false };
    std::atomic<uint64_t> g_frameCounter{ 0 };

    // Espelhos do viewport. Escritos SO' por SetViewSize().
    std::atomic<int> g_viewW{ 1920 };
    std::atomic<int> g_viewH{ 1080 };

    uint32_t FrameId() { return (uint32_t)g_frameCounter.load(std::memory_order_relaxed); }

    // -----------------------------------------------------------------------
    //  Ciclo de vida — fonte unica
    // -----------------------------------------------------------------------
    static std::atomic<int> g_stage{ (int)Stage::Down };

    Stage StageNow() { return (Stage)g_stage.load(std::memory_order_acquire); }
    bool  IsLive() { return StageNow() == Stage::Live; }

    bool AtLeast(Stage s) {
        const Stage cur = StageNow();
        if (cur == Stage::Tearing) return false;   // durante teardown nada e' garantido
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

    // UNICO lugar que escreve os espelhos. Por isso nao podem divergir.
    void SetStage(Stage s) {
        const int v = (int)s;
        g_stage.store(v, std::memory_order_release);

        g_sdkReady.store(v >= (int)Stage::Sdk, std::memory_order_release);
        g_hooksLive.store(v >= (int)Stage::Hooks, std::memory_order_release);
        g_imguiReady.store(v >= (int)Stage::ImGui, std::memory_order_release);
    }

    // Avanca sem regredir. Usado pelo fluxo normal (Init/InitImGui/Rebind).
    // Nao mexe durante Tearing: quem manda no teardown e' o Shutdown().
    static void Advance(Stage s) {
        const int cur = (int)g_stage.load(std::memory_order_relaxed);
        if (cur == (int)Stage::Tearing) return;
        if ((int)s > cur) SetStage(s);
    }

    // -----------------------------------------------------------------------
    //  Viewport — par publicado por um unico writer
    // -----------------------------------------------------------------------
    void SetViewSize(int w, int h) {
        g_viewW.store(w, std::memory_order_release);
        g_viewH.store(h, std::memory_order_release);
    }

    ViewSize GetViewSize() {
        // Re-tenta enquanto a largura estiver mudando. O writer publica
        // altura antes de largura, entao largura estavel => par coerente.
        for (;;) {
            const int w1 = g_viewW.load(std::memory_order_acquire);
            const int h = g_viewH.load(std::memory_order_acquire);
            if (w1 == g_viewW.load(std::memory_order_acquire))
                return ViewSize{ w1, h };
        }
    }

    static bool g_mhInit = false;
    static std::atomic<uint64_t> g_lastTickFrame{ ~0ull };
    static std::atomic<uint64_t> g_lastTickMs{ 0 };

    static std::atomic<bool> g_mainIsPlayerClass{ false };

    // ---- anti-recoil ----
    static ::Quat   g_pendingCam{ 0.f, 0.f, 0.f, 1.f };
    static std::atomic<uint64_t> g_lastShotMs{ 0 };
    static int      g_watchLines = 0;

    static bool KeyDown(int vk) { return vk ? (GetAsyncKeyState(vk) & 0x8000) != 0 : false; }

    static bool Edge(int vk) {
        static bool prev[256] = {};
        if (!vk) return false;
        bool cur = KeyDown(vk);
        bool r = cur && !prev[vk & 0xFF];
        prev[vk & 0xFF] = cur;
        return r;
    }

    static int WatchMax() {
        int v = C.watchMaxLines;
        return (v > 0 && v < 4096) ? v : WATCH_MAX_LINES;
    }

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

    static MethodInfo* FindMethodAny(Il2CppClass* k, const char* name) {
        if (!k || !name) return nullptr;
        void* iter = nullptr;
        for (;;) {
            const MethodInfo* m = il2cpp_class_get_methods(k, &iter);
            if (!m) break;
            if (strcmp(il2cpp_method_get_name(m), name) == 0) return (MethodInfo*)m;
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

    // -----------------------------------------------------------------------
    //  NO-RECOIL
    // -----------------------------------------------------------------------
    static void ApplyNoRecoil() {
        if (!C.noRecoil || !SDK::g_localPlayer) return;
        __try {
            Il2CppObject* ct = SDK::CamTfObj();
            if (!ct) return;
            ::Quat pr = SDK::GetPlayerRot(SDK::g_localPlayer);
            SDK::SetRot(ct, pr);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    static void ApplyNoRecoilFrame(uint64_t f) {
        static std::atomic<uint64_t> last{ ~0ull };
        uint64_t prev = last.exchange(f);
        if (prev == f) return;
        ApplyNoRecoil();
    }

    // -----------------------------------------------------------------------
    //  UM unico ponto de entrada do tick. 1x por frame, sem filtro de
    //  instancia, com watchdog. Locais POD -> pode rodar dentro de __try.
    // -----------------------------------------------------------------------
    static void RunTickOnce() {
        const uint64_t f = g_frameCounter.load(std::memory_order_relaxed);
        uint64_t previous = g_lastTickFrame.load(std::memory_order_relaxed);
        for (;;) {
            if (previous == f) return;
            if (g_lastTickFrame.compare_exchange_weak(
                previous, f,
                std::memory_order_acq_rel,
                std::memory_order_relaxed)) {
                break;
            }
        }

        g_lastTickMs.store(GetTickCount64(), std::memory_order_relaxed);
        SDK::g_frameId = (uint32_t)f;
        Combat::Tick();
    }

    // -----------------------------------------------------------------------
    //  teclas
    // -----------------------------------------------------------------------
    void HandleKeys() {
        if (Edge(C.menuKey)) C.menuOpen = !C.menuOpen;
        if (Edge(C.panicKey)) Misc::Panic();
        if (Edge(C.saveKey)) ConfigIO::Save();
        if (Edge(C.unloadKey)) { ConfigIO::Save(); g_unload = true; }
        if (C.aimKey) C.aimKeyDown = KeyDown(C.aimKey);
    }

    // -----------------------------------------------------------------------
    //  cursor
    // -----------------------------------------------------------------------
    typedef BOOL(WINAPI* tSetCursorPos)(int, int);
    static tSetCursorPos o_setCursorPos = nullptr;

    static BOOL WINAPI hk_SetCursorPos(int x, int y) {
        if (C.menuOpen) return TRUE;
        return o_setCursorPos ? o_setCursorPos(x, y) : TRUE;
    }

    bool InstallCursor() {
        HMODULE u32 = GetModuleHandleA("user32.dll");
        if (!u32) return false;
        void* p = (void*)GetProcAddress(u32, "SetCursorPos");
        if (!p) return false;
        if (MH_CreateHook(p, &hk_SetCursorPos, (void**)&o_setCursorPos) != MH_OK) return false;
        MH_EnableHook(p);
        Log("[hooks] cursor hook OK");
        return true;
    }

    // -----------------------------------------------------------------------
    //  WNDPROC + render
    // -----------------------------------------------------------------------
    static HWND    g_hwnd = nullptr;
    static WNDPROC o_wndProc = nullptr;
    static bool    g_imguiInit = false;
    static bool    g_imguiCtx = false;
    static bool    g_win32Backend = false;
    static bool    g_glBackend = false;

    static CRITICAL_SECTION  g_imCs;
    static bool              g_imCsInit = false;
    static std::atomic<bool> g_frameOpen{ false };

    struct ImLock {
        ImLock() { if (g_imCsInit) EnterCriticalSection(&g_imCs); }
        ~ImLock() { if (g_imCsInit) LeaveCriticalSection(&g_imCs); }
    };

    // -----------------------------------------------------------------------
    //  Fontes
    //  SafeFont devolve SEMPRE referencia valida. FontCount() devolve 0 antes
    //  do atlas existir.
    // -----------------------------------------------------------------------
    int FontCount() {
        if (!g_imguiReady.load()) return 0;
        if (!ImGui::GetCurrentContext()) return 0;
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

    static thread_local int t_wndDepth = 0;

    static LRESULT CALLBACK hk_wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        if (t_wndDepth > 0) {
            return o_wndProc ? CallWindowProcA(o_wndProc, h, msg, wp, lp)
                : DefWindowProcA(h, msg, wp, lp);
        }

        ++t_wndDepth;
        LRESULT r;

        if (g_imguiInit && g_imCsInit) {
            EnterCriticalSection(&g_imCs);
            ImGui_ImplWin32_WndProcHandler(h, msg, wp, lp);
            if (C.menuOpen) {
                ImGuiIO& io = ImGui::GetIO();
                if (io.WantCaptureMouse || io.WantCaptureKeyboard) {
                    LeaveCriticalSection(&g_imCs);
                    --t_wndDepth;
                    return TRUE;
                }
            }
            LeaveCriticalSection(&g_imCs);
        }

        r = o_wndProc ? CallWindowProcA(o_wndProc, h, msg, wp, lp)
            : DefWindowProcA(h, msg, wp, lp);
        --t_wndDepth;
        return r;
    }

    typedef BOOL(WINAPI* tSwapBuffers)(HDC);
    static tSwapBuffers o_swap = nullptr;

    static bool InitImGui(HWND hwnd) {
        if (g_imguiInit) return true;
        if (!hwnd) return false;
        if (!g_imCsInit) { InitializeCriticalSection(&g_imCs); g_imCsInit = true; }

        if (!g_imguiCtx) {
            ImGui::CreateContext();
            ImGuiIO& io = ImGui::GetIO();
            io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
            io.IniFilename = nullptr;
            ImGui::StyleColorsDark();
            g_imguiCtx = true;
        }

        if (!g_win32Backend) {
            if (!ImGui_ImplWin32_Init(hwnd)) {
                Log("[hooks] ImGui_ImplWin32_Init FALHOU");
                return false;
            }
            g_win32Backend = true;
        }

        if (!g_glBackend) {
            if (!ImGui_ImplOpenGL3_Init("#version 130")) {
                Log("[hooks] ImGui_ImplOpenGL3_Init FALHOU");
                ImGui_ImplWin32_Shutdown();
                g_win32Backend = false;
                return false;
            }
            g_glBackend = true;
        }

        g_imguiInit = true;
        Advance(Stage::ImGui);          // publica o espelho pelo caminho unico
        Log("[hooks] ImGui inicializado (hwnd=%p)", hwnd);
        return true;
    }

    static HWND ResolveWindow(HDC hdc) {
        HWND h = WindowFromDC(hdc);
        if (h) h = GetAncestor(h, GA_ROOT);
        if (!h) {
            HWND fg = GetForegroundWindow();
            RECT r;
            if (fg && GetClientRect(fg, &r) && (r.right - r.left) > 0 && (r.bottom - r.top) > 0)
                h = fg;
        }
        if (!h) return nullptr;

        RECT r;
        if (!GetClientRect(h, &r)) return nullptr;
        if ((r.right - r.left) <= 0 || (r.bottom - r.top) <= 0) return nullptr;
        return h;
    }

    static void RebindWindow(HWND hwnd) {
        if (!hwnd) return;
        HWND oldWnd = g_hwnd;
        if (oldWnd == hwnd) return;

        if (g_imCsInit) EnterCriticalSection(&g_imCs);

        if (oldWnd && o_wndProc)
            SetWindowLongPtrA(oldWnd, GWLP_WNDPROC, (LONG_PTR)o_wndProc);

        WNDPROC prev = (WNDPROC)SetWindowLongPtrA(hwnd, GWLP_WNDPROC, (LONG_PTR)hk_wndProc);
        if (prev && prev != hk_wndProc) o_wndProc = prev;

        g_hwnd = hwnd;
        Log("[hooks] janela = %p (anterior=%p)", hwnd, oldWnd);

        if (!g_imguiInit) {
            InitImGui(hwnd);
        }
        else if (g_win32Backend) {
            ImGui_ImplWin32_Shutdown();
            g_win32Backend = false;
            if (!ImGui_ImplWin32_Init(hwnd)) {
                Log("[hooks] ImGui_ImplWin32_Init FALHOU (rebind)");
                // Falha PARCIAL, nao e' mudanca de estagio: ajusta o espelho
                // direto. E' a unica excecao a regra do SetStage.
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

    static BOOL WINAPI hk_wglSwapBuffers(HDC hdc) {
        PHASE("hk_swap");
        g_frameCounter.fetch_add(1, std::memory_order_relaxed);

        // o cache de LOS nao pode congelar com o jogo pausado
        SDK::g_frameId = (uint32_t)g_frameCounter.load(std::memory_order_relaxed);

        if (!o_swap) return TRUE;

        // ---------------------------------------------------------------
        // bloco 1: janela / wndproc / teclas / beat / WATCHDOG  (so POD)
        // ---------------------------------------------------------------
        __try {
            static int winPoll = 0;
            if (!g_hwnd || ++winPoll >= 8) {
                winPoll = 0;
                HWND hwnd = ResolveWindow(hdc);
                if (hwnd && hwnd != g_hwnd) RebindWindow(hwnd);
            }

            if (g_hwnd && o_wndProc) {
                static ULONGLONG lastWndCheck = 0;
                ULONGLONG nowc = GetTickCount64();
                if (nowc - lastWndCheck > 500) {
                    lastWndCheck = nowc;
                    WNDPROC cur = (WNDPROC)GetWindowLongPtrA(g_hwnd, GWLP_WNDPROC);
                    if (cur && cur != hk_wndProc) {
                        WNDPROC prev = (WNDPROC)SetWindowLongPtrA(g_hwnd, GWLP_WNDPROC, (LONG_PTR)hk_wndProc);
                        if (prev && prev != hk_wndProc) o_wndProc = prev;
                        Log("[hooks] wndproc reinstalado (estava %p)", cur);
                    }
                }
            }

            ApplyNoRecoilFrame(g_frameCounter.load(std::memory_order_relaxed));

            if (C.beatLog) {
                static ULONGLONG lastBeat = 0;
                ULONGLONG nb = GetTickCount64();
                if (nb - lastBeat > 2000) {
                    lastBeat = nb;
                    Log("[hooks] beat: menu=%d stage=%s alvos=%d local=%p fase=%s",
                        (int)C.menuOpen, StageName(StageNow()), SDK::g_targetCount,
                        SDK::g_localPlayer, g_phase ? g_phase : "?");
                }
            }

            HandleKeys();

            // WATCHDOG: se o LateUpdate nao rodou o tick nos ultimos 250 ms
            // (troca de instancia / cena / hook morto), o swap assume.
            if (GetTickCount64() - g_lastTickMs.load(std::memory_order_relaxed) > TICK_WATCHDOG_MS) {
                PHASE("swap-watchdog");
                RunTickOnce();
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { Log("[crash] swap pre-ImGui"); }

        if (!g_imguiInit || !g_hwnd) return o_swap(hdc);

        RECT rc;
        if (!GetClientRect(g_hwnd, &rc)) return o_swap(hdc);
        int w = rc.right - rc.left, h = rc.bottom - rc.top;
        if (w <= 0 || h <= 0) return o_swap(hdc);
        SetViewSize(w, h);                       // writer unico do viewport

        if (g_imCsInit) EnterCriticalSection(&g_imCs);
        __try {
            if (g_frameOpen.exchange(true)) ImGui::EndFrame();

            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplWin32_NewFrame();

            ImGuiIO& io = ImGui::GetIO();
            if (io.DisplaySize.x <= 0.f || io.DisplaySize.y <= 0.f)
                io.DisplaySize = ImVec2((float)w, (float)h);

            ImGui::NewFrame();

            Menu::Render();
            if (g_sdkReady.load()) { ESP::Draw(); Misc::Draw(); }

            ImGui::Render();
            g_frameOpen = false;

            ImDrawData* dd = ImGui::GetDrawData();
            if (dd && dd->Valid && dd->CmdListsCount > 0) {
                glViewport(0, 0, ViewW(), ViewH());
                ImGui_ImplOpenGL3_RenderDrawData(dd);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            g_frameOpen = false;
            Log("[crash] frame ImGui (AV)");
        }
        if (g_imCsInit) LeaveCriticalSection(&g_imCs);

        return o_swap(hdc);
    }

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

    // -----------------------------------------------------------------------
    //  PROBE  (tudo gateado por C.probeLog)
    // -----------------------------------------------------------------------
    struct FloatProbe { uint32_t off; char name[40]; };
    static FloatProbe g_probe[512];
    static int        g_probeN = -1;
    static float      g_probeBefore[512];
    static int        g_probeShots = 0;
    static float      g_frameBefore[512];
    static int        g_frameProbeFrames = 0;

    static void CollectProbeFields(Il2CppClass* k) {
        g_probeN = 0;
        if (!k) return;
        __try {
            for (Il2CppClass* c = k; c && g_probeN < 512; c = il2cpp_class_get_parent(c)) {
                void* it = nullptr;
                for (;;) {
                    FieldInfo* f = il2cpp_class_get_fields(c, &it);
                    if (!f || g_probeN >= 512) break;
                    const Il2CppType* t = il2cpp_field_get_type(f);
                    const char* tn = (t && il2cpp_type_get_name) ? il2cpp_type_get_name(t) : nullptr;
                    if (!tn || !strstr(tn, "Single")) continue;
                    uint32_t off = (uint32_t)il2cpp_field_get_offset(f);
                    if (!off) continue;
                    g_probe[g_probeN].off = off;
                    const char* fn = il2cpp_field_get_name(f);
                    strncpy_s(g_probe[g_probeN].name, fn ? fn : "?", _TRUNCATE);
                    ++g_probeN;
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        Log("[probe] campos float em PlayerScript: %d", g_probeN);
    }

    static void ProbeSnapshot(float* dst) {
        for (int i = 0; i < g_probeN; ++i)
            dst[i] = SafeRead<float>((uintptr_t)SDK::g_localPlayer + g_probe[i].off, 0.f);
    }

    static void ProbeDiff(const float* ref, const char* tag) {
        if (!SDK::g_localPlayer || g_probeN <= 0) return;
        int lines = 0;
        for (int i = 0; i < g_probeN && lines < PROBE_DIFF_MAX; ++i) {
            float now = SafeRead<float>((uintptr_t)SDK::g_localPlayer + g_probe[i].off, 0.f);
            if (now != ref[i]) {
                Log("[%s] float %s @0x%X %.4f -> %.4f",
                    tag, g_probe[i].name, g_probe[i].off, ref[i], now);
                ++lines;
            }
        }
        if (lines == 0) Log("[%s] nenhum float mudou", tag);
    }

    // -----------------------------------------------------------------------
    //  main thread
    // -----------------------------------------------------------------------
    typedef void(__fastcall* tLateUpdate)(void* self, void* method);
    static tLateUpdate o_lateUpdate = nullptr;

    static void __fastcall hk_lateUpdate(void* self, void* method) {
        PHASE("hk_lateUpdate");

        const bool isLocalInst = (!g_mainIsPlayerClass.load() ||
            !SDK::g_localPlayer || self == SDK::g_localPlayer);

        const bool wantWatch = isLocalInst && C.probeLog && g_lastShotMs.load() &&
            (GetTickCount64() - g_lastShotMs.load() < 150) &&
            g_frameProbeFrames < 6 && g_watchLines < WatchMax();

        if (o_lateUpdate) {
            __try { o_lateUpdate(self, method); }
            __except (EXCEPTION_EXECUTE_HANDLER) { Log("[crash] LateUpdate original AV"); }
        }

        if (isLocalInst) ApplyNoRecoilFrame(g_frameCounter.load(std::memory_order_relaxed));

        ::Quat r1{ 0.f, 0.f, 0.f, 1.f }, p1{ 0.f, 0.f, 0.f, 1.f };
        Il2CppObject* camTf = nullptr;

        __try {
            if (wantWatch) {
                if (g_probeN < 0) CollectProbeFields(ClassOf(SDK::g_localPlayer));
                camTf = SDK::CamTfObj();
                if (camTf) SDK::GetRot(camTf, r1);
                if (SDK::g_localPlayer) p1 = SDK::GetPlayerRot(SDK::g_localPlayer);

                ++g_frameProbeFrames;
                ++g_watchLines;

                Log("[watch] f%d camTf (%.4f,%.4f,%.4f,%.4f) playerRot (%.4f,%.4f,%.4f,%.4f) preTiroCam (%.4f,%.4f,%.4f,%.4f)",
                    g_frameProbeFrames,
                    r1.x, r1.y, r1.z, r1.w,
                    p1.x, p1.y, p1.z, p1.w,
                    g_pendingCam.x, g_pendingCam.y, g_pendingCam.z, g_pendingCam.w);

                ProbeSnapshot(g_frameBefore);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { Log("[crash] watch/probe"); }

        // SEM early-return por instancia: o throttle por frame dentro do
        // RunTickOnce ja evita trabalho repetido.
        RunTickOnce();
    }

    bool InstallMainThread() {
        static const char* classes[] = { "PlayerScript", "GameManager", "Game", "HUD", "UIManager",
                                         "GameController", "Main", "ClientInstance" };
        static const char* methods[] = { "LateUpdate", "Update", "FixedUpdate" };

        for (size_t c = 0; c < sizeof(classes) / sizeof(classes[0]); ++c) {
            Il2CppClass* k = FindClassNamed(classes[c]);
            if (!k) continue;
            for (size_t m = 0; m < sizeof(methods) / sizeof(methods[0]); ++m) {
                void* ptr = FindMethodPtr(classes[c], methods[m], 0);
                if (!ptr) continue;
                if (MH_CreateHook(ptr, &hk_lateUpdate, (void**)&o_lateUpdate) == MH_OK) {
                    MH_EnableHook(ptr);

                    bool isPlayer = (strcmp(classes[c], "PlayerScript") == 0 ||
                        strcmp(classes[c], "Entity") == 0 ||
                        strcmp(classes[c], "Player") == 0);
                    g_mainIsPlayerClass = isPlayer;

                    Log("[hooks] MAIN THREAD = %s::%s (%p) filtroPorInstancia=%d",
                        classes[c], methods[m], ptr, (int)isPlayer);
                    return true;
                }
            }
        }
        Log("[hooks] main thread: NENHUM alvo encontrado");
        return false;
    }

    // =======================================================================
    //  FIRE BULLET
    // =======================================================================
    typedef bool(__fastcall* tFire7)(void* self, int32_t a1, float a2, Vec3 a3, Vec3 a4,
        bool a5, double a6, int32_t a7);
    static tFire7 o_fire7 = nullptr;

    static std::atomic<uint32_t> g_fireCalls{ 0 };
    static int g_fireLogged = 0;

    static bool __fastcall hk_fire7(void* self, int32_t a1, float a2, Vec3 a3, Vec3 a4,
        bool a5, double a6, int32_t a7) {
        PHASE("hk_fire7");
        g_fireCalls.fetch_add(1, std::memory_order_relaxed);

        const bool isLocal = (SDK::g_localPlayer && self == SDK::g_localPlayer);
        Il2CppObject* camTf = isLocal ? SDK::CamTfObj() : nullptr;

        ::Quat camBefore{ 0.f, 0.f, 0.f, 1.f };
        bool haveRot = false;
        const bool aiming = (C.aimbot && C.aimKeyDown && SDK::g_aimTarget);

        __try {
            const int fireLogMax = C.probeLog ? 3 : 1;
            if (g_fireLogged < fireLogMax) {
                ++g_fireLogged;
                Log("[fire7] self=%p a1=%d(%p) a2=%.3f a3=(%.1f,%.1f,%.1f)|a3|=%.2f a4=(%.1f,%.1f,%.1f)|a4|=%.2f a5=%d a6=%.3f a7=%d",
                    self, a1, (void*)(intptr_t)a1, (double)a2,
                    a3.x, a3.y, a3.z, (double)a3.len(),
                    a4.x, a4.y, a4.z, (double)a4.len(),
                    (int)a5, a6, a7);
            }

            if (isLocal && camTf) {
                haveRot = SDK::GetRot(camTf, camBefore);

                if (aiming && haveRot) {
                    Vec3 me = SDK::GetPos(camTf);
                    Vec3 tp = SDK::TargetAimPos(SDK::g_aimTarget, C.aimBone);
                    if (!tp.isSane() || tp.zero()) tp = SDK::GetPos(SDK::g_aimTarget);
                    Vec3 d = tp - me;
                    float L = d.len();
                    if (L > 0.0001f) {
                        ::Quat q = SDK::LookRot(d * (1.f / L));
                        SDK::SetRot(camTf, q);
                        SDK::SetPlayerRot(SDK::g_localPlayer, q);
                    }
                }

                if (C.noRecoil) g_pendingCam = camBefore;
            }

            if (isLocal && (C.silentAim || C.magicBullets)) {
                float d3 = fabsf(a3.len() - 1.f);
                float d4 = fabsf(a4.len() - 1.f);
                bool dirIsA4 = (d4 <= d3);
                if ((dirIsA4 ? d4 : d3) < 0.30f) {
                    int slot = dirIsA4 ? 1 : 0;
                    if (SDK::g_fireDirSlot.load() != slot) {
                        SDK::g_fireDirSlot.store(slot);
                        Log("[fire7] direcao = param%d", slot + 3);
                    }
                    if (dirIsA4) Aimbot::Silent::Override(a3, a4);
                    else         Aimbot::Silent::Override(a4, a3);
                }
            }

            if (isLocal && C.probeLog && g_probeShots < 2) {
                if (g_probeN < 0) CollectProbeFields(ClassOf(self));
                ProbeSnapshot(g_probeBefore);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { Log("[crash] hk_fire7 pre"); }

        bool ret = false;
        __try { ret = o_fire7 ? o_fire7(self, a1, a2, a3, a4, a5, a6, a7) : false; }
        __except (EXCEPTION_EXECUTE_HANDLER) { Log("[crash] hk_fire7 original AV"); }

        if (isLocal) {
            ApplyNoRecoil();

            g_lastShotMs.store(GetTickCount64());
            g_frameProbeFrames = 0;
            g_watchLines = 0;

            if (C.probeLog && g_probeShots < 2 && camTf) {
                ::Quat r1{ 0.f, 0.f, 0.f, 1.f };
                __try { SDK::GetRot(camTf, r1); }
                __except (EXCEPTION_EXECUTE_HANDLER) {}
                Log("[probe] camTf rot (%.4f,%.4f,%.4f,%.4f) -> (%.4f,%.4f,%.4f,%.4f)",
                    camBefore.x, camBefore.y, camBefore.z, camBefore.w, r1.x, r1.y, r1.z, r1.w);
                ProbeDiff(g_probeBefore, "probe");
                Log("[probe] ---- fim diff tiro %d ----", ++g_probeShots);
            }
        }

        return ret;
    }

    // =======================================================================
    //  FIRE ONE SHOT
    // =======================================================================
    typedef bool(__fastcall* tFireOne)(void* self, int32_t a1, float a2, Vec3 a3, Vec3 a4,
        uint8_t a5, uint8_t a6, double a7, int32_t a8, int64_t a9);
    static tFireOne o_fireOne = nullptr;
    static int      g_fireOneLogged = 0;

    static bool __fastcall hk_fireOne(void* self, int32_t a1, float a2, Vec3 a3, Vec3 a4,
        uint8_t a5, uint8_t a6, double a7, int32_t a8, int64_t a9) {
        PHASE("hk_fireOne");

        const int logMax = C.probeLog ? 3 : 1;
        if (g_fireOneLogged < logMax) {
            ++g_fireOneLogged;
            Log("[fireOne] self=%p a1=%d(%p) a2=%.3f |a3|=%.2f |a4|=%.2f a5=%d a6=%d a7=%.2f a8=%d a9=%lld",
                self, a1, (void*)(intptr_t)a1, (double)a2,
                (double)a3.len(), (double)a4.len(), (int)a5, (int)a6, a7, a8, (long long)a9);
        }

        bool ret = false;
        __try { ret = o_fireOne ? o_fireOne(self, a1, a2, a3, a4, a5, a6, a7, a8, a9) : false; }
        __except (EXCEPTION_EXECUTE_HANDLER) {}

        if (SDK::g_localPlayer && self == SDK::g_localPlayer)
            ApplyNoRecoil();

        return ret;
    }

    // -----------------------------------------------------------------------
    //  shoot (passthroughs)
    // -----------------------------------------------------------------------
    typedef void(__fastcall* tShoot)(void* self, void* method);
    static tShoot o_shoot1 = nullptr, o_shoot2 = nullptr, o_shoot3 = nullptr, o_shoot4 = nullptr;

    static void __fastcall hk_shoot1(void* s, void* m) {
        if (s == SDK::g_localPlayer) ApplyNoRecoil();
        if (o_shoot1) o_shoot1(s, m);
    }
    static void __fastcall hk_shoot2(void* s, void* m) {
        if (s == SDK::g_localPlayer) ApplyNoRecoil();
        if (o_shoot2) o_shoot2(s, m);
    }
    static void __fastcall hk_shoot3(void* s, void* m) {
        if (s == SDK::g_localPlayer) ApplyNoRecoil();
        if (o_shoot3) o_shoot3(s, m);
    }
    static void __fastcall hk_shoot4(void* s, void* m) {
        if (s == SDK::g_localPlayer) ApplyNoRecoil();
        if (o_shoot4) o_shoot4(s, m);
    }

    bool InstallCombat() {
        PHASE("InstallCombat");
        bool ok = false;

        Il2CppClass* ps = FindClassNamed("PlayerScript");
        if (ps) {
            static const char* names[] = { "SemiAutoShoot", "Shoot", "AutoShoot", "FullAutoShoot" };
            static void* detours[] = { (void*)&hk_shoot1, (void*)&hk_shoot2,
                                       (void*)&hk_shoot3, (void*)&hk_shoot4 };
            static tShoot* tramps[] = { &o_shoot1, &o_shoot2, &o_shoot3, &o_shoot4 };

            int n = 0;
            for (size_t i = 0; i < 4; ++i) {
                MethodInfo* m = FindMethodByArity(ps, names[i], 0);
                if (!m) { Log("[hooks] %s nao encontrado", names[i]); continue; }
                void* p = SDK::MethodPtr(m);
                if (!p) { Log("[hooks] %s sem ptr", names[i]); continue; }
                if (MH_CreateHook(p, detours[n], (void**)tramps[n]) == MH_OK) {
                    MH_EnableHook(p);
                    Log("[hooks] hook shoot OK: %s (%p)", names[i], p);
                    if (!SDK::M_shoot) SDK::M_shoot = m;
                    ok = true;
                    if (++n >= 4) break;
                }
            }

            MethodInfo* fos = FindMethodByArity(ps, "FireOneShot", 9);
            if (fos) {
                void* p = SDK::MethodPtr(fos);
                Log("[hooks] FireOneShot: ret=%s ptr=%p", SDK::MethodReturnName(fos), p);
                if (p && MH_CreateHook(p, &hk_fireOne, (void**)&o_fireOne) == MH_OK) {
                    MH_EnableHook(p);
                    ok = true;
                    Log("[hooks] hook FireOneShot OK (anti-recoil)");
                }
            }
            else Log("[hooks] FireOneShot/9 nao encontrado");
        }
        else Log("[hooks] PlayerScript nulo");

        if (SDK::M_FireBullet) {
            uint32_t pc = il2cpp_method_get_param_count(SDK::M_FireBullet);
            void* p = SDK::MethodPtr(SDK::M_FireBullet);
            Log("[hooks] FireBullet: params=%u ret=%s ptr=%p", pc,
                SDK::MethodReturnName(SDK::M_FireBullet), p);
            if (p && pc == 7 && MH_CreateHook(p, &hk_fire7, (void**)&o_fire7) == MH_OK) {
                MH_EnableHook(p);
                ok = true;
                Log("[hooks] hook FireBullet OK (7 params, ret bool)");
            }
        }
        else Log("[hooks] M_FireBullet nulo");

        return ok;
    }

    // -----------------------------------------------------------------------
    void ApplyFeatures() {
        Log("[hooks] ApplyFeatures: noRecoil=%d noSpread=%d infAmmo=%d rapidFire=%d noReload=%d silent=%d probeLog=%d",
            (int)C.noRecoil, (int)C.noSpread, (int)C.infAmmo, (int)C.rapidFire,
            (int)C.noReload, (int)C.silentAim, (int)C.probeLog);
    }

    // -----------------------------------------------------------------------
    //  CICLO DE VIDA
    // -----------------------------------------------------------------------
    bool Init() {
        if (IsLive()) return true;                 // idempotente

        Advance(Stage::Init);

        if (!g_mhInit) {
            if (MH_Initialize() != MH_OK) {
                Log("[hooks] MH_Initialize FALHOU");
                return false;                      // fica em Init; Shutdown limpa
            }
            g_mhInit = true;
        }

        if (!g_imCsInit) { InitializeCriticalSection(&g_imCs); g_imCsInit = true; }

        g_lastTickMs.store(GetTickCount64(), std::memory_order_relaxed);

        bool ok = false;
        ok |= InstallSwap();
        ok |= InstallMainThread();
        ok |= InstallCursor();
        ok |= InstallCombat();

        Advance(Stage::Hooks);

        Aimbot::Install();
        Triggerbot::Install();
        AntiCheat::Install();

        ApplyFeatures();

        Advance(Stage::Live);
        Log("[hooks] instalados (sucesso=%d) stage=%s", (int)ok, StageName(StageNow()));
        return ok;
    }

    bool Shutdown() {
        if (!g_mhInit && StageNow() == Stage::Down) {
            Log("[hooks] Shutdown: ja' estava em Down");
            return true;
        }

        SetStage(Stage::Tearing);

        // Durante o teardown nada e' garantido: derruba os espelhos na mao
        // (SetStage(Tearing) os deixaria em true, porque Tearing > Live).
        g_sdkReady.store(false, std::memory_order_release);
        g_hooksLive.store(false, std::memory_order_release);
        g_imguiReady.store(false, std::memory_order_release);

        SDK::g_recoilHookActive = false;
        g_frameOpen = false;

        if (o_wndProc && g_hwnd)
            SetWindowLongPtrA(g_hwnd, GWLP_WNDPROC, (LONG_PTR)o_wndProc);
        o_wndProc = nullptr;

        MH_DisableHook(MH_ALL_HOOKS);
        Sleep(120);

        {
            ImLock lk;
            if (g_glBackend) { ImGui_ImplOpenGL3_Shutdown(); g_glBackend = false; }
            if (g_win32Backend) { ImGui_ImplWin32_Shutdown(); g_win32Backend = false; }
            if (g_imguiCtx) { ImGui::DestroyContext(); g_imguiCtx = false; }
            g_imguiInit = false;
        }

        MH_Uninitialize();
        g_mhInit = false;

        g_hwnd = nullptr;
        o_swap = nullptr;
        o_setCursorPos = nullptr;
        o_lateUpdate = nullptr;
        o_fire7 = nullptr;
        o_fireOne = nullptr;
        o_shoot1 = o_shoot2 = o_shoot3 = o_shoot4 = nullptr;

        g_lastTickFrame.store(~0ull, std::memory_order_relaxed);
        g_lastTickMs.store(0);
        g_lastShotMs.store(0);
        g_frameProbeFrames = 0;
        g_watchLines = 0;
        g_probeShots = 0;
        g_fireLogged = 0;
        g_fireOneLogged = 0;
        g_probeN = -1;

        SetStage(Stage::Down);
        Log("[hooks] Shutdown");
        return true;
    }

} // namespace Hooks