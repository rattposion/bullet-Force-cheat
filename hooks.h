#pragma once
// ============================================================================
//  hooks.h — v4  (bfi / Bullet Force)
//  DONO das declaracoes de tudo o que hooks.cpp define.
//
//  >>> DIAGNOSTICO
//    Se o build disser que 'Stage' ou 'Hooks' nao existem, procure no Output
//    a linha:   >>> bfi: hooks.h v4 compilado
//    Se ela NAO aparecer em cada .cpp que inclui este header, o compilador
//    esta lendo OUTRO arquivo (caminho/projeto errado) — nao e' problema de
//    codigo.
//
//  DONO DOS FLAGS:
//    g_sdkReady  -> escrito APENAS por Combat::Tick.
//    g_imguiReady / g_hooksLive -> escritos APENAS pelo SetStage (hooks.cpp).
//    g_viewW / g_viewH -> DEFINIDOS em hooks.cpp (nao static: sdk.cpp le').
//    g_phase -> DEFINIDO em UM .cpp (dllmain.cpp).
// ============================================================================
#ifndef BFI_HOOKS_H
#define BFI_HOOKS_H

#pragma message(">>> bfi: hooks.h v4 compilado")

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdarg>

#include "imgui.h"

namespace Hooks {

    // ------------------------------------------------------------------ Stage
    enum class Stage : int {
        Down = 0,
        Init,
        Sdk,
        Hooks,
        ImGui,
        Live,
        Tearing
    };

    struct ViewSize { int w, h; };

    // ------------------------------------------------------------------ flags
    extern std::atomic<bool>     g_hooksLive;
    extern std::atomic<bool>     g_imguiReady;
    extern std::atomic<bool>     g_sdkReady;          // dono: Combat::Tick
    extern std::atomic<uint64_t> g_frameCounter;
    extern std::atomic<uint32_t> g_imguiGeneration;   // ++ a cada CreateContext

    // lidos pelo sdk.cpp — DEFINIDOS em hooks.cpp (nao static)
    extern std::atomic<int>      g_viewW;
    extern std::atomic<int>      g_viewH;

    // ------------------------------------------------------- ciclo de vida
    Stage       StageNow();
    const char* StageName(Stage s);
    bool        AtLeast(Stage s);      // false durante Tearing
    bool        IsLive();
    void        SetStage(Stage s);
    void        Advance(Stage s);      // avanca sem regredir

    // ------------------------------------------------------------ viewport
    void     SetViewSize(int w, int h);
    ViewSize GetViewSize();
    int      ViewW();
    int      ViewH();

    // --------------------------------------------------------- frame/fonte
    uint32_t FrameId();
    int      FontCount();
    ImFont& SafeFont(int idx);

    // ---------------------------------------------------------- teclas/dbg
    void HandleKeys();
    void LogWndProcInfo();

    // ------------------------------------------------------------- install
    bool InstallSwap();
    bool InstallMainThread();
    bool InstallCombat();
    bool InstallCwp();
    bool InstallCursor();

    bool Init();
    bool Reinstall();
    bool Shutdown();

    // ==================== helpers inline (nao precisam de .cpp) =============

    static constexpr int kStageCount = 7;
    static constexpr const char* kStageNames[kStageCount] = {
        "Down", "Init", "Sdk", "Hooks", "ImGui", "Live", "Tearing"
    };

    inline const char* StageTag(Stage s) {
        const int i = (int)s;
        return (i >= 0 && i < kStageCount) ? kStageNames[i] : "?";
    }

    inline bool Tearing() {
        return StageNow() == Stage::Tearing;
    }

    inline bool ImGuiLive() {
        return g_imguiReady.load(std::memory_order_acquire)
            && g_hooksLive.load(std::memory_order_acquire)
            && StageNow() != Stage::Tearing;
    }

    inline ImFont* SafeFontPtr(int idx) {
        if (!ImGui::GetCurrentContext()) return nullptr;
        ImGuiIO& io = ImGui::GetIO();
        if (!io.Fonts) return nullptr;
        const int n = io.Fonts->Fonts.Size;
        if (n <= 0) return nullptr;
        if (idx < 0 || idx >= n) idx = 0;
        return io.Fonts->Fonts[idx];
    }

    inline int SafeFontCount() {
        if (!ImGui::GetCurrentContext()) return 0;
        ImGuiIO& io = ImGui::GetIO();
        return io.Fonts ? io.Fonts->Fonts.Size : 0;
    }

    // buffer de formatacao do PHASE_F (um por thread)
    inline const char* PhaseFmt(const char* fmt, ...) {
        static thread_local char b[192];
        if (!fmt) { b[0] = 0; return b; }
        va_list ap; va_start(ap, fmt);
        vsnprintf(b, sizeof(b), fmt, ap);
        va_end(ap);
        b[sizeof(b) - 1] = 0;
        return b;
    }

} // namespace Hooks

// ---------------------------------------------------------------------------
//  Stage / ViewSize TAMBEM no escopo global: o hooks.cpp usa Stage::Down sem
//  qualificar e outros arquivos usam Hooks::Stage::X. As duas formas sao o
//  MESMO tipo (alias), entao nao existe ambiguidade.
// ---------------------------------------------------------------------------
using Stage = Hooks::Stage;
using ViewSize = Hooks::ViewSize;

// ---------------------------------------------------------------------------
//  fase atual (VEH / log [crash]). DEFINIDA em UM .cpp (dllmain.cpp).
// ---------------------------------------------------------------------------
extern const char* g_phase;

// atribuicao pura: sem chamada de funcao, sem depender de nada do header
#ifndef PHASE
#define PHASE(x) (g_phase = (x))
#endif

// versao formatada (opcional):  PHASE_F("UpdateTargets alvo=%p", (void*)p);
#ifndef PHASE_F
#define PHASE_F(...) (g_phase = Hooks::PhaseFmt(__VA_ARGS__))
#endif

#endif // BFI_HOOKS_H