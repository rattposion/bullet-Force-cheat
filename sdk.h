#pragma once

#include "common.h"
#include "il2cpp.h"
#include "il2cpp_api.h"
#include "memory.h"
#include "config.h"

#include <vector>
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

namespace Hooks { extern int g_viewW; extern int g_viewH; }

extern const char* g_phase;
#ifndef PHASE
#define PHASE(x) (g_phase = (x))
#endif

extern Config g_cfg;

#define MAX_TARGETS 128

struct Target {
    Il2CppObject* obj = nullptr;
    Il2CppObject* player = nullptr;

    Il2CppObject* headBone = nullptr;
    Il2CppObject* footBone = nullptr;

    Vec3   pos{ 0.f, 0.f, 0.f };
    Vec3   feet{ 0.f, 0.f, 0.f };
    Vec3   head{ 0.f, 0.f, 0.f };
    Vec2   scrHead{ 0.f, 0.f };
    Vec2   scrFeet{ 0.f, 0.f };

    bool   onScreen = false;
    bool   alive = false;
    bool   enemy = true;
    bool   npc = false;
    bool   visible = false;

    int    team = 0;
    int    weapon = 0;
    float  health = 100.f;
    float  hp = 100.f;
    float  dist = 0.f;

    uint32_t slot = 0;      // slot estavel (nao muda enquanto o player existir)
    uint64_t lastSeen = 0;

    char   name[40]{};
};

namespace SDK {

    extern std::unordered_map<std::string, Il2CppClass*> g_classes;
    extern std::mutex g_classesMx;

    extern uint32_t O_health, O_alive, O_team, O_isLocal, O_name, O_weapon;
    extern uint32_t O_vel, O_speed, O_rot, O_rotation, O_cam, O_camTf;
    extern uint32_t O_mCamera, O_humanMeshAnimator, O_humanMeshReferences;
    extern uint32_t O_stats, O_gunView, O_gunStats, O_gunStatsOnView;
    extern uint32_t O_curAmmo, O_maxAmmo, O_reloading, O_spread, O_fireRate, O_dmg;
    extern bool     O_rotationIsQuat;

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

    extern Il2CppObject* g_camera;
    extern Il2CppObject* g_local;
    extern Il2CppObject* g_aimTarget;
    extern Il2CppObject* g_localPlayer;
    extern int   g_localTeam;
    extern Vec3  g_localPos;
    extern Target g_targets[MAX_TARGETS];
    extern int   g_targetCount;
    extern CRITICAL_SECTION g_targetCs;
    extern int   dbg_drawn;
    extern uint64_t g_frameId;

    extern bool  g_viewProjOk;
    extern float g_viewProj[16];

    // slot do parametro que e a DIRECAO no FireBullet (0 = a3, 1 = a4)
    extern std::atomic<int>  g_fireDirSlot;
    // hook de recoil ativo (se true, noRecoil nao usa fallback de rotacao)
    extern std::atomic<bool> g_recoilHookActive;
    // o campo de cadencia resolvido e RPM (e nao um delay em segundos)
    extern bool g_rateIsRpm;

    int           ScreenW();
    int           ScreenH();
    Il2CppObject* CameraObj();

    // raio do FOV em pixels derivado da camera REAL.
    float FovRadiusPx(float deg);

    bool Init();
    void Shutdown();
    void BuildCache();

    void UpdateView();
    void RefreshTargets();
    void UpdateTargets();
    std::vector<Target> Snapshot();

    Il2CppObject* GetLocalPlayer();
    Il2CppObject* GetCamera();
    bool  IsA(Il2CppObject* o, Il2CppClass* c);
    bool  IsEnemy(Il2CppObject* o);
    float GetHealth(Il2CppObject* o);
    Vec3  GetPos(Il2CppObject* o);
    void  SetPos(Il2CppObject* o, Vec3 v);
    Vec3  GetVelocity(Il2CppObject* o);
    Vec3  MuzzlePos();

    Il2CppObject* FindBoneContains(Il2CppObject* root, const char* sub, int budget = 8192);
    Vec3  GetBonePos(void* bone);
    void  GetNameBuf(Il2CppObject* o, char* out, int n);
    std::string GetName(Il2CppObject* o);

    bool  WorldToScreen(Vec3 w, Vec2& s);
    bool  VisibleLine(Vec3 a, Vec3 b);
    Vec3  TargetAimPos(Il2CppObject* p, int boneMode);

    // >>> NOVO: mapeamento de osso compartilhado (aimbot.cpp e TargetAimPos usam
    //     o MESMO). 0=auto 1=cabeca 2=peito 3=estomago 4=perna 5=aleatorio.
    Vec3  BonePoint(Vec3 head, Vec3 feet, int boneMode);

    Il2CppObject* CamTfObj();
    Quat  LookRot(Vec3 dir);
    bool  GetRot(Il2CppObject* tf, Quat& out);
    bool  SetRot(Il2CppObject* tf, Quat q);
    void  SetPlayerRot(Il2CppObject* p, const Quat& q);
    Quat  GetPlayerRot(Il2CppObject* p);

    Il2CppObject* GetGunStats(Il2CppObject* player);
    Il2CppObject* GetWeapon(Il2CppObject* player);

    bool  AimLock(Il2CppObject* p, Quat& savedOriginal);
    void  RestoreAim(Il2CppObject* p, const Quat& q);
    bool  FireBulletReady();
    void  FireBulletAt(Vec3 dir);

    void  DumpFields(const char* className);
    void  DumpFields(Il2CppClass* k, const char* tag);

    Il2CppClass* FindClass(const char* name, const char* ns = nullptr);
    Il2CppObject* GetTransform(void* obj);
    Il2CppObject* SafeReadObjPub(void* obj, uint32_t off);

    uint32_t PickFieldOff(Il2CppClass* k, std::initializer_list<const char*> names,
        const char* typeSub = nullptr);

    void  LockTargets();
    void  UnlockTargets();
    void* MethodPtr(MethodInfo* m);
    const char* MethodReturnName(MethodInfo* m);

    bool  HasLineOfSight(Vec3 eye, Vec3 aim);
    bool  HasLineOfSightEx(Vec3 eye, Vec3 aim,
        Il2CppObject* localTf, Il2CppObject* targetTf);
    bool  HasLineOfSightExRaw(Vec3 eye, Vec3 aim,
        Il2CppObject* localTf, Il2CppObject* targetTf);
    void  LosInvalidate();

    bool  LosReady();
    const char* LosBackend();
    bool  TargetVisible(int idx, Vec3 eye, Vec3 aim);
    bool  RaycastTest(Vec3 from, Vec3 to, float& outHitDist);
    float CameraCotHalfFovY();

    Il2CppClass* AntiHackClass();
    Il2CppClass* FindAntiHackClass();
    Il2CppObject* AntiHackInstance();
    void  ListClassesContaining(const char* sub);

    Il2CppObject* FindGunStatsObj();
    Il2CppObject* GetGunStatsLive();     // nunca devolve o clone em branco

    int  ResolveWeaponFields(Il2CppObject* stats, uint32_t& cur, uint32_t& max,
        uint32_t& reserve, uint32_t& reload, uint32_t& spread,
        uint32_t& rate, uint32_t& dmg);
    void DumpFieldsWithValues(Il2CppObject* o);
}

using SDK::g_targets;
using SDK::g_targetCount;
using SDK::g_targetCs;
using SDK::g_localPlayer;
using SDK::g_localTeam;
using SDK::g_localPos;
using SDK::dbg_drawn;
using SDK::ScreenW;
using SDK::ScreenH;
using SDK::CameraObj;
using SDK::FindClass;
using SDK::GetTransform;
using SDK::SafeReadObjPub;
using SDK::VisibleLine;
using SDK::HasLineOfSight;
using SDK::HasLineOfSightEx;
using SDK::HasLineOfSightExRaw;
using SDK::LosInvalidate;
using SDK::LosReady;
using SDK::LosBackend;
using SDK::TargetVisible;
using SDK::RaycastTest;
using SDK::CameraCotHalfFovY;
using SDK::FovRadiusPx;
using SDK::BonePoint;
using SDK::LockTargets;
using SDK::UnlockTargets;
using SDK::FindBoneContains;
using SDK::GetBonePos;
using SDK::GetPos;
using SDK::SetPos;
using SDK::GetHealth;
using SDK::IsA;
using SDK::IsEnemy;
using SDK::GetName;
using SDK::GetNameBuf;
using SDK::GetVelocity;
using SDK::GetGunStats;
using SDK::GetGunStatsLive;
using SDK::GetWeapon;
using SDK::FindGunStatsObj;
using SDK::ResolveWeaponFields;
using SDK::DumpFieldsWithValues;
using SDK::GetLocalPlayer;
using SDK::DumpFields;
using SDK::TargetAimPos;
using SDK::CamTfObj;
using SDK::LookRot;
using SDK::GetRot;
using SDK::SetRot;
using SDK::SetPlayerRot;
using SDK::GetPlayerRot;
using SDK::FireBulletReady;
using SDK::FireBulletAt;
using SDK::MuzzlePos;
using SDK::MethodReturnName;
using SDK::AntiHackClass;
using SDK::FindAntiHackClass;
using SDK::AntiHackInstance;
using SDK::ListClassesContaining;