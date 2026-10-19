#pragma once
// ============================================================================
//  aimbot.h  —  v14 (corrigido)
//
//  FIXES:
//   [H1] Adiciona Tick()/Install()/Best()/AimPoint()/Silent::Override()
//        -> mata os 5 LNK2019 vindos de combat.obj / triggerbot.obj / hooks.obj
//   [H2] Declara Update() como alias de Tick() (usado internamente).
//   [H3] Inclui <cstdint> p/ ULONGLONG-safe e evita redefinicao de tipos.
// ============================================================================

#include "common.h"
#include "il2cpp.h"

namespace Aimbot {

    // -----------------------------------------------------------------------
    //  ciclo de vida
    // -----------------------------------------------------------------------
    bool  Install();            // chamado por Hooks::Init            [LNK2019]
    void  Tick();               // chamado por Combat::S_Aimbot       [LNK2019]
    void  Update();             // alias interno (mesmo que Tick)
    void  Shutdown();
    void  OnDomainReload();

    // -----------------------------------------------------------------------
    //  helpers usados por outros modulos
    // -----------------------------------------------------------------------
    Il2CppObject* Best();                 // combat.cpp   -> melhor alvo    [LNK2019]
    Vec3          AimPoint(void* target); // triggerbot.cpp -> ponto de mira [LNK2019]

    // silencioso — chamado no hook do FireBullet (hooks.cpp)             [LNK2019]
    namespace Silent {
        bool Override(Vec3 muzzle, Vec3& outDir);
    }

    // -----------------------------------------------------------------------
    //  extras / debug
    // -----------------------------------------------------------------------
    void  DumpWeapon();
    bool  HasTarget();
    Il2CppObject* CurrentTarget();

} // namespace Aimbot