#pragma once
// ============================================================================
//  aimbot.h  —  v16
//
//  FIXES:
//   [H1] Declaracoes que matam os LNK2019 (Install/Tick/Best/AimPoint/Silent::Override).
//   [H2] Silent::Override(Vec3 muzzle, Vec3& outDir)  <-- assinatura batendo com a
//        chamada no hook do FireBullet (hooks.cpp). 2 argumentos, NAO 3.
//        muzzle = posicao do cano (por valor)
//        outDir = direcao do tiro (por REFERENCIA -> e' reescrita no lugar)
//   [H3] <cstdint> incluido.
//   [H4] v16: declaradas as helpers de math (definidas em aimbot.cpp).
//        A versao de 1 argumento Dist3D(Vec3) e' inline e vive no features.h.
// ============================================================================

#include <cstdint>
#include <cmath>

#include "common.h"
#include "il2cpp.h"

namespace Aimbot {

    // -----------------------------------------------------------------------
    //  ciclo de vida
    // -----------------------------------------------------------------------
    bool  Install();            // chamado por Hooks::Init
    void  Tick();               // chamado por Combat::S_Aimbot
    void  Update();             // alias interno (mesmo que Tick)
    void  Shutdown();
    void  OnDomainReload();

    // -----------------------------------------------------------------------
    //  helpers usados por outros modulos
    // -----------------------------------------------------------------------
    Il2CppObject* Best();                 // combat.cpp   -> melhor alvo
    Vec3          AimPoint(void* target); // triggerbot.cpp -> ponto de mira

    // -----------------------------------------------------------------------
    //  silencioso — chamado no hook do FireBullet (hooks.cpp)
    //  Override(p2, p3): p2 = origem/muzzle, p3 = direcao (reescrita por ref)
    // -----------------------------------------------------------------------
    namespace Silent {
        bool Override(Vec3 muzzle, Vec3& outDir);
    }

    // -----------------------------------------------------------------------
    //  math (definidas em aimbot.cpp)
    // -----------------------------------------------------------------------
    Vec3  Sub(Vec3 a, Vec3 b);
    float Dot(Vec3 a, Vec3 b);
    Vec3  Norm(Vec3 v);
    ::Quat LookRot(Vec3 fwd);
    ::Quat Slerp(::Quat a, ::Quat b, float t);
    Vec3  LocalPos();
    Vec3  CamForward();
    float Dist3D(Vec3 a, Vec3 b);

    // -----------------------------------------------------------------------
    //  extras / debug
    // -----------------------------------------------------------------------
    void  DumpWeapon();
    bool  HasTarget();
    Il2CppObject* CurrentTarget();

} // namespace Aimbot