#pragma once
// ============================================================================
//  hooks.h — ciclo de vida por Stage + viewport + fontes.
//
//  g_viewW / g_viewH: DECLARADOS aqui, DEFINIDOS em hooks.cpp.
//  O sdk.cpp le' direto (WorldToScreen / ScreenW), por isso nao podem ser
//  static.
//
//  >>> v2
//    * g_phase + PHASE centralizados aqui (o log de crash usa).
//      g_phase e' DEFINIDO em UM .cpp (dllmain.cpp).
//    * Declarados os helpers de instalacao (definidos em hooks.cpp).
//
//  ATENCAO — dono dos flags:
//    g_sdkReady  -> escrito APENAS por Combat::Tick (nao pelo SetStage).
//    g_imguiReady/g_hooksLive -> escritos pelo SetStage (hooks.cpp).
// ============================================================================
#include <atomic>
#include <cstdint>

#include "imgui.h"

enum class Stage : int {
    Down = 0, Init, Sdk, Hooks, ImGui, Live, Tearing
};

struct ViewSize { int w, h; };

// ---------------------------------------------------------------------------
//  fase atual — usada pelo VEH / log [crash]. Definir em UM .cpp.
// ---------------------------------------------------------------------------
extern const char* g_phase;
#ifndef PHASE
#define PHASE(x) (g_phase = (x))
#endif

namespace Hooks {

    extern std::atomic<bool>     g_hooksLive;
    extern std::atomic<bool>     g_imguiReady;
    extern std::atomic<bool>     g_sdkReady;          // dono: Combat::Tick
    extern std::atomic<uint64_t> g_frameCounter;
    extern std::atomic<uint32_t> g_imguiGeneration;   // ++ a cada CreateContext

    // Usados pelo sdk.cpp. DEFINIDOS em hooks.cpp (nao static).
    extern std::atomic<int>      g_viewW;
    extern std::atomic<int>      g_viewH;

    Stage       StageNow();
    const char* StageName(Stage s);
    bool        AtLeast(Stage s);      // false durante Tearing
    bool        IsLive();
    void        SetStage(Stage s);
    void        Advance(Stage s);      // avanca sem regredir

    void     SetViewSize(int w, int h);
    ViewSize GetViewSize();
    int      ViewW();
    int      ViewH();

    uint32_t FrameId();
    int      FontCount();
    ImFont& SafeFont(int idx);

    void HandleKeys();
    void LogWndProcInfo();

    // instalacao (definidas em hooks.cpp)
    bool InstallSwap();
    bool InstallMainThread();
    bool InstallCombat();
    bool InstallCwp();
    bool InstallCursor();

    bool Init();
    bool Reinstall();
    bool Shutdown();
}