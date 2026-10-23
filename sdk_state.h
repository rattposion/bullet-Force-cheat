#pragma once
// ============================================================================
//  sdk_state.h — TODO o estado mutavel do SDK.
//
//  [A] IMUTAVEL APOS Init()  — offsets (O_*), classes (k*), MethodInfo (M_*).
//  [B] ESTADO VIVO           — g_localPlayer, g_camera, g_aimTarget, ...
//  [C] PROTEGIDO POR LOCK    — g_targets[]/g_targetCount (g_targetCs).
//  [D] ATOMICOS              — g_fireDirSlot, g_recoilHookActive, g_cursorState...
//  [E] SNAPSHOT DE FRAME     — g_fd / g_fdSeq (seqlock).
//  [F] FONTE UNICA DE TAMANHO— g_vpW/g_vpH.
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

typedef Il2CppType* (__cdecl* t_il2cpp_get_return_type)(const MethodInfo*);

namespace SDK {

    // ---- [C] cache de classes (g_classesMx) ----
    extern std::unordered_map<std::string, Il2CppClass*> g_classes;
    extern std::mutex g_classesMx;

    // ---- [A] offsets ----
    extern uint32_t O_health, O_alive, O_team, O_isLocal, O_name, O_weapon;
    extern uint32_t O_vel, O_speed, O_rot, O_rotation, O_cam, O_camTf;
    extern uint32_t O_grounded;                 // NOVO (bhop)
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
    extern uint64_t g_frameId;
    extern int   dbg_drawn;

    // ---- [C] lista de alvos (g_targetCs) ----
    extern Target g_targets[MAX_TARGETS];
    extern int    g_targetCount;
    extern CRITICAL_SECTION g_targetCs;

    // ---- [D] atomicos ----
    extern std::atomic<int>  g_fireDirSlot;
    extern std::atomic<bool> g_recoilHookActive;
    extern std::atomic<int>  g_cursorState;

    // ---- [D] contadores de rejeicao do WorldToScreen ----
    extern std::atomic<uint32_t> g_w2sOk, g_w2sSemMatriz, g_w2sAtras, g_w2sFora;

    // ---- [D] falhas do renderer ----
    extern std::atomic<uint32_t> g_glErrTotal;

    // ---- [E] SNAPSHOT DE FRAME ----
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
    extern std::atomic<uint32_t> g_fdSeq;
    extern std::atomic<uint32_t> g_fdPublish;
    extern std::atomic<uint32_t> g_fdTorn;

    // ---- [F] FONTE UNICA DE TAMANHO ----
    extern std::atomic<int> g_vpW, g_vpH;

    // ---- [A] flag de diagnostico ----
    extern bool g_rateIsRpm;

    // ---- setters do estado ----
    void SetViewport(int w, int h);
    void PublishFrame();
    bool ReadFrame(FrameData& out);
    bool WorldToScreenF(const FrameData& f, Vec3 w, Vec2& out);

} // namespace SDK