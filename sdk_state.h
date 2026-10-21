#pragma once
// ============================================================================
//  sdk_state.h — TODO o estado mutavel do SDK. Nao declara funcoes.
//
//  REGRA DE CONCORRENCIA (leia antes de adicionar qualquer global aqui)
//  ---------------------------------------------------------------------
//  Tres categorias, com regras diferentes. Enquadre o que voce adicionar:
//
//  [A] IMUTAVEL APOS Init()  — offsets (O_*), classes (k*), MethodInfo (M_*).
//      Escritos UMA vez no Init(), antes de os hooks ficarem vivos.
//      Leitura: qualquer thread, SEM lock.
//
//  [B] ESTADO VIVO           — g_localPlayer, g_camera, g_local, g_aimTarget,
//      g_localTeam, g_localPos, g_viewProj, g_viewProjOk, g_frameId.
//      Escritos pelo thread do jogo no tick. Leia UMA vez por frame para um
//      local e use o local — nao releia campo a campo no meio de um calculo,
//      senao voce mistura dois frames.
//
//      >>> STICKY (importante): g_localPlayer e g_camera NAO sao zerados
//          numa falha transitoria (objeto destruido, SafeClass reprovando
//          por um frame, respawn, troca de cena). O tick so' os SUBSTITUI
//          quando a re-resolucao devolve algo valido.
//
//          Consequencia direta: "g_localPlayer != nullptr" passou a
//          significar "ULTIMO local conhecido", NAO "local valido agora".
//          Quem precisa da segunda pergunta le g_localValid ([D]).
//
//          Motivo: o pipeline de alvos (posicao, tela, onScreen) NAO depende
//          do local player para funcionar. Ele so' alimenta dist/team/filtro.
//          Zerar o ponteiro por uma falha de um frame derrubava o frame
//          inteiro — era a causa do ESP sumir.
//
//  [C] PROTEGIDO POR LOCK    — g_targets[]/g_targetCount (g_targetCs) e
//      g_classes (g_classesMx).
//      Nunca acesse direto: use LockTargets/UnlockTargets, Snapshot(),
//      FindClass(). O lock e' do CONJUNTO: ler g_targetCount e depois
//      g_targets[i] fora do lock pode ver dois frames diferentes.
//
//  [D] ATOMICOS              — g_fireDirSlot, g_recoilHookActive,
//      g_localValid, g_viewValid.
//      Sem lock. Use .load()/.store().
//
//      g_localValid / g_viewValid sao TELEMETRIA de prontidao, escritas pelo
//      tick e lidas livremente (log, gate de render). NUNCA use g_localValid
//      como pre-requisito para desenhar: local ausente nao invalida alvos.
//
//  Se um dia um global nao couber em nenhuma: e' sinal de que ele precisa de
//  um dono unico e de uma regra escrita aqui.
// ============================================================================

#include "common.h"
#include "il2cpp.h"
#include "sdk_types.h"

#include <string>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <windows.h>

// il2cpp_method_get_return_type — declarado aqui (extern "C" identico ao padrao
// do il2cpp_api.h) para o hooks.cpp poder escolher a ABI do hook de recoil.
// Se tambem adicionar em il2cpp_api.h, mantenha o MESMO tipo.
extern "C" {
    extern Il2CppType* (*il2cpp_method_get_return_type)(const MethodInfo*);
}

namespace SDK {

    // ---- [C] cache de classes (g_classesMx) ----
    extern std::unordered_map<std::string, Il2CppClass*> g_classes;
    extern std::mutex g_classesMx;

    // ---- [A] offsets ----
    extern uint32_t O_health, O_alive, O_team, O_isLocal, O_name, O_weapon;
    extern uint32_t O_vel, O_speed, O_rot, O_rotation, O_cam, O_camTf;
    extern uint32_t O_mCamera, O_humanMeshAnimator, O_humanMeshReferences;
    extern uint32_t O_stats, O_gunView, O_gunStats, O_gunStatsOnView;
    extern uint32_t O_curAmmo, O_maxAmmo, O_reloading, O_spread, O_fireRate, O_dmg;
    extern bool     O_rotationIsQuat;

    // ---- [A] classes resolvidas ----
    extern Il2CppClass* kEntity;
    extern Il2CppClass* kPlayerScript;
    extern Il2CppClass* kPlayer;
    extern Il2CppClass* kNPC;
    extern Il2CppClass* kTransform;
    extern Il2CppClass* kCamera;
    extern Il2CppClass* kGunStats;
    extern Il2CppClass* kVector3;
    extern Il2CppClass* kQuaternion;
    extern Il2CppClass* kAntiHack;

    // ---- [A] metodos resolvidos ----
    extern MethodInfo* M_shoot;
    extern MethodInfo* M_recoil;
    extern MethodInfo* M_fireBullet;
    extern MethodInfo* M_FireBullet;
    extern MethodInfo* M_getTransform;
    extern MethodInfo* M_w2s;
    extern MethodInfo* M_lookRot;
    extern MethodInfo* M_set_position;
    extern MethodInfo* M_set_forward;
    extern MethodInfo* M_get_fov;
    extern MethodInfo* M_set_fov;

    // ---- [B] estado vivo (escrito no tick, lido 1x por frame) ----
    //  g_localPlayer e g_camera sao STICKY: ver doc no topo do arquivo.
    extern Il2CppObject* g_camera;
    extern Il2CppObject* g_local;
    extern Il2CppObject* g_aimTarget;
    extern Il2CppObject* g_localPlayer;
    extern int   g_localTeam;
    extern Vec3  g_localPos;
    extern bool  g_viewProjOk;
    extern float g_viewProj[16];
    extern uint64_t g_frameId;
    extern int   dbg_drawn;

    // ---- [C] lista de alvos (g_targetCs) ----
    extern Target g_targets[MAX_TARGETS];
    extern int    g_targetCount;
    extern CRITICAL_SECTION g_targetCs;

    // ---- [D] atomicos ----
    // slot do parametro que e a DIRECAO no FireBullet (0 = a3, 1 = a4)
    extern std::atomic<int>  g_fireDirSlot;
    // hook de recoil ativo (se true, noRecoil nao usa fallback de rotacao)
    extern std::atomic<bool> g_recoilHookActive;

    // ---- [D] TELEMETRIA de prontidao ----
    //  g_localValid : o local player foi RESOLVIDO E VALIDADO no frame atual.
    //                 (false enquanto morto/respawnando NAO significa que o
    //                 pipeline parou — ver [B].)
    //  g_viewValid  : existe matriz view*proj publicada e dentro do TTL.
    extern std::atomic<bool> g_localValid;
    extern std::atomic<bool> g_viewValid;

    // ---- [A] flag de diagnostico ----
    // o campo de cadencia resolvido e RPM (e nao um delay em segundos)
    extern bool g_rateIsRpm;
}