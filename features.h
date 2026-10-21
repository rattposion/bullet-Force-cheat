#pragma once
// ============================================================================
//  features.h — declaracoes publicas compartilhadas.
//
//  ESTE ARQUIVO NAO DEFINE NADA. Cada simbolo tem um dono:
//      memoria (mem.h / scan.h) -> #include "memory.h"
//      Hooks                    -> #include "hooks.h"
//      Unlock                   -> #include "unlock.h"
//      SDK (estado + tipos)     -> #include "sdk.h" / "sdk_state.h"
//      g_phase / PHASE          -> sdk.h
//
//  Se qualquer um desses blocos aparecer definido aqui, apague: e' duplicata
//  e gera C2084 / C2995 / C2556.
// ============================================================================

#include <atomic>
#include <cstdint>

#include "il2cpp.h"
#include "il2cpp_api.h"
#include "common.h"
#include "memory.h"
#include "hooks.h"
#include "sdk.h"
#include "config.h"
#include "unlock.h"

// ---------------------------------------------------------------------------
//  Estado global que NAO pertence a nenhum dos donos acima.
// ---------------------------------------------------------------------------
//  g_phase fica no sdk.h, junto do macro PHASE que o escreve.
//  Aqui so' o flag de saida do processo.
extern std::atomic<bool> g_unload;

// Menu_WantsMouse: definida em features.cpp, consultada pelo hook de cursor
// (hk_SetCursorPos) E pelo WndProc. E' o unico lugar que sabe se o mouse e' do
// ImGui — usar C.menuOpen no lugar dela engolia o SetCursorPos do jogo.
bool Menu_WantsMouse();

namespace Menu {
    // ALIAS de C.menuOpen — mesma celula de memoria, nao um bool proprio.
    // Requer, no menu.cpp:   bool& Menu::g_open = g_cfg.menuOpen;
    extern bool& g_open;

    void Draw();            // definida em menu.cpp
    void Render();          // definida em features.cpp (dono unico)
}

namespace ESP { void Draw(); }
namespace Misc { void Panic(); void Draw(); void Tick(); }

// ============================================================================
//  COMBAT + PROFILER POR PASSO
//  Cada passo do Tick e' cronometrado (QPC) e exposto pro overlay, pra achar
//  o gargalo real em vez de chutar.
//
//  >>> As estatisticas sao escritas sob SRWLOCK em Combat::Stat(). O overlay
//      roda em OUTRA thread: ler g_stats/g_statsN direto e' leitura rasgada.
//      Use CopyStats() (compartilhado) ou ResetStats() (exclusivo).
// ============================================================================
namespace Combat {
    const int kMaxSteps = 16;
    struct StepStat { char name[24]; float ms; float last; };
    extern StepStat g_stats[kMaxSteps];
    extern int       g_statsN;
    extern float     g_tickMs;      // media exponencial do tick inteiro
    void  Tick();
    void  Init();
    void  Shutdown();
    // Copia o snapshot das estatisticas para 'out'. Devolve quantas linhas
    // escreveu (<= maxN). Nunca segura o lock durante o desenho.
    int   CopyStats(StepStat* out, int maxN);
    void  ResetStats();
}

namespace Weapons { void Init(); void Tick(); }
namespace Movement { void Tick(); }

// ============================================================================
//  AIMBOT
//  NAO declare 'struct Quat' aqui dentro: cria Aimbot::Quat, um tipo distinto
//  do ::Quat, e toda chamada de SDK que recebe Quat& quebra (E0312/E0349/E0434)
//  alem de gerar LookRot sobrecarregado (E0308/C2668).
//  Regra: '::Quat' explicito aqui e no aimbot.cpp.
// ============================================================================
namespace Aimbot {
    Il2CppObject* Best();
    bool  Install();
    void  Tick();
    void  Shutdown();
    void  Bone(Il2CppObject* p, Il2CppObject* target);   // definida em features.cpp
    Vec3  Sub(Vec3, Vec3);
    float Dot(Vec3, Vec3);
    Vec3  Norm(Vec3);
    ::Quat LookRot(Vec3);
    ::Quat Slerp(::Quat, ::Quat, float);
    Vec3  AimPoint(void*);
    Vec3  LocalPos();
    Vec3  CamForward();
    namespace Silent { bool Override(Vec3 origin, Vec3& dir); }
}

namespace Recoil { void Install(); void Tick(); }
namespace Triggerbot { bool Install(); void Tick(); }
namespace AntiCheat { bool Install(); void Tick(); }