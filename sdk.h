#pragma once
// ============================================================================
//  sdk.h — UMBRELLA.
//    [J] = so' no thread do jogo   [R] = seguro no render   [L] = exige lock
// ============================================================================

#include <atomic>

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

bool         ValidInstance_(const void* o);
Il2CppClass* SafeClass_(Il2CppObject* o);

inline bool PtrOk_(const void* o) {
    const uintptr_t p = (uintptr_t)o;
    if (p == 0) return false;
    if (p == (uintptr_t)-1) return false;
    if (p < 0x10000) return false;
    if (p > 0x00007FFFFFFFFFFF) return false;
    if ((p & 0x7) != 0) return false;
    return true;
}

namespace SDK {

    // ------------------------------------------------------------ tela
    int           ScreenW();
    int           ScreenH();
    float         FovRadiusPx(float deg);
    float         CameraCotHalfFovY();
    Il2CppObject* CameraObj();

    // ------------------------------------------------------------ cursor do JOGO
    extern std::atomic<int> g_cursorState;
    void RefreshCursorState();

    // ------------------------------------------------------------ ciclo
    bool Init();
    void Shutdown();
    void BuildCache();
    bool OnDomainReload();
    void InvalidateRuntime();

    // ------------------------------------------------------------ por frame
    void UpdateView();
    void UpdateTargets();
    void RefreshTargets();

    // ------------------------------------------------------------ alvos
    void  LockTargets();
    void  UnlockTargets();
    void  SnapshotInto(std::vector<Target>& out);
    std::vector<Target> Snapshot();

    Il2CppObject* GetLocalPlayer();
    Il2CppObject* GetCamera();
    bool  IsA(Il2CppObject* o, Il2CppClass* c);
    bool  IsEnemy(Il2CppObject* o);
    int   TeamRelation(Il2CppObject* o);
    int   GetTeamId(Il2CppObject* o);
    int   GetLocalTeam();
    float GetHealth(Il2CppObject* o);
    float GetHealthRaw(Il2CppObject* o);            // NOVO — sem normalizar
    bool  SetHealth(Il2CppObject* o, float value);  // NOVO — respeita SyncVar

    Vec3  GetPos(Il2CppObject* o);
    void  SetPos(Il2CppObject* o, Vec3 v);
    Vec3  GetVelocity(Il2CppObject* o);
    Vec3  MuzzlePos();

    Il2CppObject* FindBoneContains(Il2CppObject* root, const char* sub,
        int budget = 8192);
    Vec3  GetBonePos(void* bone);
    void  GetNameBuf(Il2CppObject* o, char* out, int n);
    std::string GetName(Il2CppObject* o);

    bool  WorldToScreen(Vec3 w, Vec2& s);
    bool  VisibleLine(Vec3 a, Vec3 b);
    Vec3  TargetAimPos(Il2CppObject* p, int boneMode);
    Vec3  BonePoint(Vec3 head, Vec3 feet, int boneMode);

    // ------------------------------------------------------------ rotacao
    Il2CppObject* CamTfObj();
    Quat  LookRot(Vec3 dir);
    bool  GetRot(Il2CppObject* tf, Quat& out);
    bool  SetRot(Il2CppObject* tf, Quat q);
    void  SetPlayerRot(Il2CppObject* p, const Quat& q);
    Quat  GetPlayerRot(Il2CppObject* p);

    // ------------------------------------------------------------ arma
    Il2CppObject* GetGunStats(Il2CppObject* player);
    Il2CppObject* GetGunStatsLive();
    Il2CppObject* FindGunStatsObj();
    Il2CppObject* GetWeapon(Il2CppObject* player);
    int  ResolveWeaponFields(Il2CppObject* stats, uint32_t& cur, uint32_t& max,
        uint32_t& reserve, uint32_t& reload, uint32_t& spread,
        uint32_t& rate, uint32_t& dmg);

    bool  AimLock(Il2CppObject* p, Quat& savedOriginal);
    void  RestoreAim(Il2CppObject* p, const Quat& q);
    bool  FireBulletReady();
    void  FireBulletAt(Vec3 dir);

    // ------------------------------------------------------------ LOS
    bool  HasLineOfSight(Vec3 eye, Vec3 aim);
    bool  HasLineOfSightEx(Vec3 eye, Vec3 aim,
        Il2CppObject* localTf, Il2CppObject* targetTf);
    bool  HasLineOfSightExRaw(Vec3 eye, Vec3 aim,
        Il2CppObject* localTf, Il2CppObject* targetTf);
    bool  TargetVisible(int idx, Vec3 eye, Vec3 aim);
    bool  RaycastTest(Vec3 from, Vec3 to, float& outHitDist);

    void  LosInvalidate();
    bool  LosReady();
    const char* LosBackend();

    void  ClearTfCache();
    void  ClearBoneCache();
    void  PruneTfCache();

    // ------------------------------------------------------------ il2cpp
    Il2CppClass* FindClass(const char* name, const char* ns = nullptr);
    Il2CppObject* GetTransform(void* obj);
    Il2CppObject* SafeReadObjPub(void* obj, uint32_t off);
    bool         ValidInstance(Il2CppObject* o);
    Il2CppClass* SafeClass(Il2CppObject* o);

    uint32_t PickFieldOff(Il2CppClass* k, std::initializer_list<const char*> names,
        const char* typeSub = nullptr);

    void* MethodPtr(MethodInfo* m);
    const char* MethodReturnName(MethodInfo* m);

    // ------------------------------------------------------------ anti-hack
    Il2CppClass* AntiHackClass();
    Il2CppClass* FindAntiHackClass();
    Il2CppObject* AntiHackInstance();

    // ------------------------------------------------------------ debug
    void DumpFields(const char* className);
    void DumpFields(Il2CppClass* k, const char* tag);
    void DumpFieldsWithValues(Il2CppObject* o);
    void ListClassesContaining(const char* sub);
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
using SDK::g_cursorState;

using SDK::ScreenW;
using SDK::ScreenH;
using SDK::CameraObj;
using SDK::FovRadiusPx;
using SDK::CameraCotHalfFovY;
using SDK::RefreshCursorState;

using SDK::LockTargets;
using SDK::UnlockTargets;
using SDK::Snapshot;
using SDK::SnapshotInto;
using SDK::RefreshTargets;
using SDK::UpdateTargets;
using SDK::UpdateView;
using SDK::InvalidateRuntime;
using SDK::OnDomainReload;

using SDK::FindClass;
using SDK::GetTransform;
using SDK::SafeReadObjPub;
using SDK::ValidInstance;
using SDK::SafeClass;

using SDK::VisibleLine;
using SDK::HasLineOfSight;
using SDK::HasLineOfSightEx;
using SDK::HasLineOfSightExRaw;
using SDK::LosInvalidate;
using SDK::LosReady;
using SDK::LosBackend;
using SDK::TargetVisible;
using SDK::RaycastTest;

using SDK::BonePoint;
using SDK::FindBoneContains;
using SDK::GetBonePos;
using SDK::TargetAimPos;

using SDK::GetPos;
using SDK::SetPos;
using SDK::GetVelocity;
using SDK::MuzzlePos;
using SDK::GetHealth;
using SDK::GetHealthRaw;
using SDK::SetHealth;
using SDK::IsA;
using SDK::IsEnemy;
using SDK::GetTeamId;
using SDK::GetLocalTeam;
using SDK::TeamRelation;
using SDK::GetName;
using SDK::GetNameBuf;

using SDK::GetLocalPlayer;
using SDK::GetCamera;

using SDK::GetGunStats;
using SDK::GetGunStatsLive;
using SDK::GetWeapon;
using SDK::FindGunStatsObj;
using SDK::ResolveWeaponFields;

using SDK::CamTfObj;
using SDK::LookRot;
using SDK::GetRot;
using SDK::SetRot;
using SDK::SetPlayerRot;
using SDK::GetPlayerRot;

using SDK::FireBulletReady;
using SDK::FireBulletAt;
using SDK::AimLock;
using SDK::RestoreAim;

using SDK::MethodPtr;
using SDK::MethodReturnName;

using SDK::AntiHackClass;
using SDK::FindAntiHackClass;
using SDK::AntiHackInstance;
using SDK::ListClassesContaining;

using SDK::ClearTfCache;
using SDK::ClearBoneCache;
using SDK::PruneTfCache;

using SDK::DumpFields;
using SDK::DumpFieldsWithValues;