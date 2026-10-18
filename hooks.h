#pragma once
// ============================================================================
//  hooks.h — estado de ciclo de vida + hooks de plataforma.
//  DONO UNICO do namespace Hooks.
//
//  >>> Se este arquivo ainda tiver as linhas ANTIGAS junto das novas, apague:
//        extern int g_viewW; extern int g_viewH;   (agora sao std::atomic<int>)
//        void Shutdown();                          (agora e' bool)
//
//  ===========================================================================
//  UM UNICO ESTADO. OS FLAGS SAO DERIVADOS.
//  ===========================================================================
//  Tres atomics escritos em tres lugares permitem combinacao invalida
//  (hooksLive = true com sdkReady = false). Aqui a verdade e' UMA: g_stage.
//  Os tres flags sao espelhos escritos SO' por SetStage().
//
//  Progressao: Down -> Init -> Sdk -> Hooks -> ImGui -> Live
//  Shutdown() passa por Tearing e volta para Down.
//
//  Espelhos:  g_sdkReady = (stage >= Sdk)
//             g_hooksLive = (stage >= Hooks)
//             g_imguiReady = (stage >= ImGui)
//
//  ===========================================================================
//  THREADS
//  ===========================================================================
//  SetStage/Init/Shutdown : thread de init/unload, uma por vez.
//  StageNow/AtLeast/FrameNow/GetViewSize : qualquer thread, lock-free.
// ============================================================================

#include <atomic>
#include <cstdint>

#include "common.h"
#include "mem.h"

namespace Hooks {

    enum class Stage : int {
        Down = 0,   // nada instalado. Estado inicial e final.
        Init,       // Init() comecou.
        Sdk,        // SDK resolvido.
        Hooks,      // hooks instalados e ativos.
        ImGui,      // contexto ImGui criado, render pronto.
        Live,       // tudo pronto.
        Tearing,    // Shutdown() em andamento.
    };

    Stage       StageNow();
    bool        IsLive();
    bool        AtLeast(Stage s);
    const char* StageName(Stage s);

    void SetStage(Stage s);              // UNICO escritor dos espelhos

    // Espelhos (compat). Derivados de Stage(). NAO escreva por fora.
    extern std::atomic<bool> g_sdkReady;
    extern std::atomic<bool> g_hooksLive;
    extern std::atomic<bool> g_imguiReady;

    // -----------------------------------------------------------------------
    //  Contador de frames
    //  Incrementado UMA vez por frame APRESENTADO, no thread de render
    //  (hk_wglSwapBuffers), ANTES de chamar Render().
    //  Monotonico; so' zera em Init(). Para saber se um frame passou, guarde
    //  um snapshot e compare (a != b) — nao use o valor absoluto.
    // -----------------------------------------------------------------------
    extern std::atomic<uint64_t> g_frameCounter;
    inline uint64_t FrameNow() { return g_frameCounter.load(std::memory_order_acquire); }

    // -----------------------------------------------------------------------
    //  Viewport
    //  std::atomic<int>: leitura/escrita sem corrida. SetViewSize e' o UNICO
    //  writer (hook de resize). Ler os dois separadamente pode misturar um
    //  frame; se precisar do par coerente, use GetViewSize(), que re-tenta
    //  enquanto a largura estiver mudando.
    // -----------------------------------------------------------------------
    struct ViewSize { int w, h; };

    void     SetViewSize(int w, int h);
    ViewSize GetViewSize();
    inline int ViewW() { return GetViewSize().w; }
    inline int ViewH() { return GetViewSize().h; }

    // Espelhos por nome antigo. Mesmo writer unico.
    extern std::atomic<int> g_viewW;
    extern std::atomic<int> g_viewH;

    // -----------------------------------------------------------------------
    //  Fontes
    //  SafeFont devolve SEMPRE referencia valida: idx fora de
    //  [0, FontCount()) e' clampado; se o atlas nao existe, devolve o font
    //  default do contexto. Validade: ate' o atlas ser reconstruido — pegue
    //  por frame, nao guarde.
    // -----------------------------------------------------------------------
    int     FontCount();
    ImFont& SafeFont(int idx);

    // -----------------------------------------------------------------------
    //  Ciclo de vida
    //
    //  Init(): idempotente. Se ja' esta' em Live, devolve true sem refazer.
    //          Falha parcial NAO volta para Down sozinho — fica no stage
    //          alcancado, para voce inspecionar com StageNow(). Shutdown()
    //          limpa o parcial. Retorna true somente se terminou em Live.
    //
    //  Shutdown(): pode ser chamada repetidamente e de QUALQUER stage,
    //          inclusive Down (no-op, true). Desfaz na ordem inversa. Termina
    //          sempre em Down. Depois dela, Init() pode rodar de novo.
    //          Retorna true se terminou em Down.
    // -----------------------------------------------------------------------
    bool Init();
    bool Shutdown();

    void ApplyFeatures();
    void HandleKeys();

    bool InstallSwap();
    bool InstallMainThread();
    bool InstallCursor();
    bool InstallCombat();

} // namespace Hooks