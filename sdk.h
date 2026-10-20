#pragma once
// ============================================================================
//  sdk.h — UMBRELLA.
//  v6.8 — TeamRelation exposto (1=inimigo, 0=aliado, -1=indeterminado).
// ============================================================================

#include "common.h"
#include "il2cpp.h"
#include "il2cpp_api.h"
#include "memory.h"
#include "config.h"
#include "hooks.h"
#include "sdk_types.h"
#include "sdk_state.h"

#include <vector>
#include <string>
#include <initializer_list>

extern const char* g_phase;
#ifndef PHASE
#define PHASE(x) (g_phase = (x))
#endif

// globais (escopo de arquivo, definidas em sdk.cpp)
bool         ValidInstance_(const void* o);
Il2CppClass* SafeClass_(Il2CppObject* o);

// >>> [FIX] helper de validacao de ponteiro (inline, publico)
static inline bool PtrOk_(const void* o) {
    uintptr_t p = (uintptr_t)o;
    if (p == 0) return false;
    if (p == (uintptr_t)-1) return false;
    if (p < 0x10000) return false;
    if (p > 0x00007FFFFFFFFFFF) return false;
    if ((p & 0x7) != 0) return false;
    return true;
}

namespace SDK {

    int           ScreenW();
    int           ScreenH();
    Il2CppObject* CameraObj();

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

    // >>> [FIX LNK2019] team helpers
    int   GetTeamId(Il2CppObject* o);
    int   GetLocalTeam();

    // >>> [FIX C3861] relacao de time estrita p/ filtro do aimbot
    //  1 = inimigo, 0 = aliado, -1 = indeterminado
    int   TeamRelation(Il2CppObject* o);

    Il2CppObject* FindBoneContains(Il2CppObject* root, const char* sub, int budget = 8192);
    Vec3  GetBonePos(void* bone);
    void  GetNameBuf(Il2CppObject* o, char* out, int n);
    std::string GetName(Il2CppObject* o);

    bool  WorldToScreen(Vec3 w, Vec2& s);
    bool  VisibleLine(Vec3 a, Vec3 b);
    Vec3  TargetAimPos(Il2CppObject* p, int boneMode);
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

    // >>> [FIX] Lock/Unlock com guard interno
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
    void  ClearTfCache();
    void  ClearBoneCache();
    void  PruneTfCache();

    bool         ValidInstance(Il2CppObject* o);
    Il2CppClass* SafeClass(Il2CppObject* o);

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
    Il2CppObject* GetGunStatsLive();

    int  ResolveWeaponFields(Il2CppObject* stats, uint32_t& cur, uint32_t& max,
        uint32_t& reserve, uint32_t& reload, uint32_t& spread,
        uint32_t& rate, uint32_t& dmg);
    void DumpFieldsWithValues(Il2CppObject* o);
}

// ---------------------------------------------------------------------------
//  BLOCO 'using'
// ---------------------------------------------------------------------------
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

// >>> [FIX LNK2019]
using SDK::GetTeamId;
using SDK::GetLocalTeam;

// >>> [FIX C3861/E0135] exposto pro aimbot.cpp
using SDK::TeamRelation;

using SDK::ClearTfCache;
using SDK::ClearBoneCache;
using SDK::PruneTfCache;
using SDK::ValidInstance;
using SDK::SafeClass;
