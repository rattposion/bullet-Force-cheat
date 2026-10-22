#pragma once
// ============================================================================
//  sdk_state.h — TODO o estado mutavel do SDK.
//
//  REGRA DE CONCORRENCIA (leia antes de adicionar qualquer global aqui)
//  ---------------------------------------------------------------------
//  Tres categorias, com regras diferentes. Enquadre o que voce adicionar:
//
//  [A] IMUTAVEL APOS Init()  — offsets (O_*), classes (k*), MethodInfo (M_*).
//      Escritos UMA vez no Init(), antes de os hooks ficarem vivos.
//      Leitura: qualquer thread, SEM lock.
//
//  [B] ESTADO VIVO           — g_localPlayer, g_camera, g_aimTarget,
//      g_localTeam, g_localPos, g_viewProj, g_viewProjOk, g_frameId.
//      Escritos pelo thread do jogo no tick. NAO leia de fora da thread do
//      jogo: use [E] ReadFrame().
//      g_frameId tem DONO UNICO: o hk_wglSwapBuffers (1 por apresentacao).
//
//  [C] PROTEGIDO POR LOCK    — g_targets[]/g_targetCount (g_targetCs) e
//      g_classes (g_classesMx).
//      Nunca acesse direto: use LockTargets/UnlockTargets, Snapshot(),
//      FindClass(). O lock e' do CONJUNTO: ler g_targetCount e depois
//      g_targets[i] fora do lock pode ver dois frames diferentes.
//
//  [D] ATOMICOS              — g_fireDirSlot, g_recoilHookActive,
//      g_cursorState, g_w2s*, g_glErrTotal.
//      Sem lock. Use .load()/.store().
//
//  [E] SNAPSHOT DE FRAME     — g_fd / g_fdSeq. Publicado 1x por tick pela
//      thread do jogo (PublishFrame), lido por qualquer outra thread
//      (ReadFrame). Seqlock: seq impar = escrevendo. Quem le' NAO toca nos
//      globais [B] — le' este struct.
//
//  [F] FONTE UNICA DE TAMANHO — g_vpW/g_vpH. Escrito SO' pelo
//      hk_wglSwapBuffers, a partir do VIEWPORT do GL. W2S, DisplaySize e
//      glViewport usam este mesmo par; nao existe segundo valor.
//
//  Se um dia um global nao couber em nenhuma: e' sinal de que ele precisa de
//  um dono unico e de uma regra escrita aqui.
// ============================================================================

#include "common.h"
#include "il2cpp.h"
#include "sdk_types.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <windows.h>

// ---------------------------------------------------------------------------
//  O ponteiro il2cpp_method_get_return_type e' DECLARADO pelo il2cpp_api.h.
//  Aqui so' damos um NOME ao tipo (typedef nao tem linkage, nao conflita).
//
//  TIPO REAL (copiado do il2cpp_api.h):  Il2CppType* (__cdecl*)(const MethodInfo*)
//  NAO use 'const Il2CppType*' no retorno — da' C2373 contra o il2cpp_api.h.
// ---------------------------------------------------------------------------
typedef Il2CppType* (__cdecl* t_il2cpp_get_return_type)(const MethodInfo*);
// extern t_il2cpp_get_return_type il2cpp_method_get_return_type;

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

    // ---- [B] estado vivo (thread do jogo; fora dela use ReadFrame) ----
    extern Il2CppObject* g_camera;
    extern Il2CppObject* g_local;
    extern Il2CppObject* g_aimTarget;
    extern Il2CppObject* g_localPlayer;
    extern int   g_localTeam;
    extern Vec3  g_localPos;
    extern bool  g_viewProjOk;
    extern float g_viewProj[16];
    extern uint64_t g_frameId;          // dono unico: hk_wglSwapBuffers
    extern int   dbg_drawn;

    // ---- [C] lista de alvos (g_targetCs) ----
    extern Target g_targets[MAX_TARGETS];
    extern int    g_targetCount;
    extern CRITICAL_SECTION g_targetCs;

    // ---- [D] atomicos ----
    // slot do parametro que e' a DIRECAO no FireBullet (0 = a3, 1 = a4)
    extern std::atomic<int>  g_fireDirSlot;
    // hook de recoil ativo (se true, noRecoil nao usa fallback de rotacao)
    extern std::atomic<bool> g_recoilHookActive;
    // -1 = API de cursor ausente | bit0 = visible | (>>1)&3 = lockState
    extern std::atomic<int>  g_cursorState;

    // ---- [D] contadores de rejeicao do WorldToScreen (diagnostico) ----
    extern std::atomic<uint32_t> g_w2sOk, g_w2sSemMatriz, g_w2sAtras, g_w2sFora;

    // ---- [D] falhas do renderer, drenadas por frame (glGetError) ----
    extern std::atomic<uint32_t> g_glErrTotal;

    // ---- [E] SNAPSHOT DE FRAME -------------------------------------------
    //  Publicado UMA vez por tick pela thread do jogo; lido por qualquer outra
    //  thread. Seqlock: seq impar = escrita em andamento, par = pronto.
    struct FrameData {
        float    viewProj[16]{};
        bool     viewProjOk = false;
        Vec3     localPos{};
        Il2CppObject* localPlayer = nullptr;
        Il2CppObject* aimTarget = nullptr;
        int      viewW = 0, viewH = 0;
        uint64_t frame = 0;
    };
    extern FrameData g_fd[2];
    extern std::atomic<uint32_t> g_fdSeq;      // impar = escrevendo
    extern std::atomic<uint32_t> g_fdPublish;  // frames publicados
    extern std::atomic<uint32_t> g_fdTorn;     // leituras que pegaram escrita

    // ---- [F] FONTE UNICA DE TAMANHO --------------------------------------
    extern std::atomic<int> g_vpW, g_vpH;

    // ---- [A] flag de diagnostico ----
    // o campo de cadencia resolvido e RPM (e nao um delay em segundos)
    extern bool g_rateIsRpm;

    // ---- setters do estado (unica excecao ao "nao declara funcoes") ----
    void SetViewport(int w, int h);          // [F] dono: hk_wglSwapBuffers
    void PublishFrame();                     // [E] dono: thread do jogo
    bool ReadFrame(FrameData& out);          // [E] qualquer thread
    bool WorldToScreenF(const FrameData& f, Vec3 w, Vec2& out);   // [E] W2S seguro

    // NOTA: int TeamRelation(Il2CppObject* o) esta' no sdk.h (decl) e no
    // sdk.cpp (def). 0 = mesmo time, 1 = inimigo, -1 = indefinido.
    // No call site: -1 e' TRUE em contexto booleano — use '== 1' se quiser
    // "so' inimigo confirmado".

} // namespace SDK