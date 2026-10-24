#pragma once
// ============================================================================
//  aimbot.h  —  v19
//
//  >>> v19 (MIRA FORTE)
//   [G1] FOV e selecao por ANGULO em GRAUS (espaco de mundo), independente da
//        projecao de tela.
//   [G2] Histerese que NAO larga a mira durante o cooldown de troca.
//   [G3] Fallback de bone (cabeca -> peito -> quadril) com vis por bone.
//   [G4] Predicao de movimento (historico por frame, sem heap).
//   [G5] Deadzone angular: nao reescreve a rotacao se ja esta em cima.
//   [G6] Perf: muzzle/LOS fora do loop, cap de raycast por TopN.
//
//  >>> v17/v18 (mantidos)
//   [H1] Declaracoes que matam os LNK2019.
//   [H2] Silent::Override(Vec3 muzzle, Vec3& outDir) — 2 args, NAO 3.
//   [H3] <cstdint> incluido.
//   [H4] helpers de math declaradas.
//   [H5]/[B1b] IsActive() para o gate do silent aim (hooks.cpp).
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
    void  Shutdown();
    void  OnDomainReload();

    bool  IsActive();           // [H5] tecla segurada OU toggle ligado

    // -----------------------------------------------------------------------
    //  helpers usados por outros modulos
    // -----------------------------------------------------------------------
    Il2CppObject* Best();                 // combat.cpp -> melhor alvo (cache/frame, por angulo)
    Vec3          AimPoint(void* target); // triggerbot.cpp -> ponto de mira (bone + predicao)

    // -----------------------------------------------------------------------
    //  silencioso — hook do FireBullet (hooks.cpp). 2 args.
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
    //  NOVO v19 — util pra HUD/debug e pro triggerbot
    // -----------------------------------------------------------------------
    float AngleTo(Vec3 world);              // graus entre o crosshair e um ponto
    float AngleToTarget(void* target);      // idem, pelo ponto de mira do alvo
    Vec3  Predict(Il2CppObject* e, Vec3 p); // lead pelo vetor de velocidade
    bool  BonePos(Il2CppObject* e, int bone, Vec3& out);
    int   CurrentBone();                    // bone em uso (-1 = nenhum)
    float CurrentAngle();                   // erro angular atual (graus)
    void  ClearTarget();
    void  ForceReacquire();

    // -----------------------------------------------------------------------
    //  extras / debug
    // -----------------------------------------------------------------------
    void  DumpWeapon();
    bool  HasTarget();
    Il2CppObject* CurrentTarget();

} // namespace Aimbot