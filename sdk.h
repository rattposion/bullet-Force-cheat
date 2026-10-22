#pragma once
// ============================================================================
//  sdk.h — UMBRELLA.
//
//  REGRA DE THREAD:
//    [J] = so' no thread do jogo (dentro do Combat::Tick). Pode chamar il2cpp,
//          alocar, tomar lock, fazer raycast.
//    [R] = seguro no thread de render, mas le estado escrito pelo thread do
//          jogo -> capture no Tick se puder.
//    [L] = exige LockTargets()/UnlockTargets() em volta.
//
//  UpdateTargets() DEVE escrever g_targets/g_targetCount sob g_targetCs.
//
//  >>> v2: TeamRelation declarado (DEFINIR em sdk.cpp — ver bloco no fim).
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

// globais de escopo de arquivo (definidas em sdk.cpp)
bool         ValidInstance_(const void* o);
Il2CppClass* SafeClass_(Il2CppObject* o);

// helper de validacao de ponteiro — inline puro
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
    int           ScreenW();                       // [R] le Hooks::g_viewW
    int           ScreenH();                       // [R]
    float         FovRadiusPx(float deg);          // [J]
    float         CameraCotHalfFovY();             // [J]
    Il2CppObject* CameraObj();                     // [J]

    // ------------------------------------------------------------ cursor do JOGO
    extern std::atomic<int> g_cursorState;
    void RefreshCursorState();                     // [J] chamado pelo Combat

    // ------------------------------------------------------------ ciclo
    bool Init();                                   // [J]
    void Shutdown();                               // [J]
    void BuildCache();                             // [J]
    bool OnDomainReload();                         // [J]
    void InvalidateRuntime();                      // [J]

    // ------------------------------------------------------------ por frame
    void UpdateView();                             // [J] so' marca viewProjOk
    void UpdateTargets();                          // [J] escreve g_targets SOB LOCK
    void RefreshTargets();                         // [J] re-scan de entidades

    // ------------------------------------------------------------ alvos
    void  LockTargets();                           // [qualquer]
    void  UnlockTargets();                         // [qualquer]
    void  SnapshotInto(std::vector<Target>& out);  // [J]
    std::vector<Target> Snapshot();                // [J] aloca

    Il2CppObject* GetLocalPlayer();                // [J]
    Il2CppObject* GetCamera();                     // [J]
    bool  IsA(Il2CppObject* o, Il2CppClass* c);    // [J]
    bool  IsEnemy(Il2CppObject* o);                // [J]
    int   TeamRelation(Il2CppObject* o);           // [J] 1=inimigo 0=aliado -1=?
    int   GetTeamId(Il2CppObject* o);              // [J]
    int   GetLocalTeam();                          // [J]
    float GetHealth(Il2CppObject* o);              // [J]

    Vec3  GetPos(Il2CppObject* o);                 // [J]
    void  SetPos(Il2CppObject* o, Vec3 v);         // [J]
    Vec3  GetVelocity(Il2CppObject* o);            // [J]
    Vec3  MuzzlePos();                             // [J]

    Il2CppObject* FindBoneContains(Il2CppObject* root, const char* sub,
        int budget = 8192);                        // [J] caro
    Vec3  GetBonePos(void* bone);                  // [J]
    void  GetNameBuf(Il2CppObject* o, char* out, int n);   // [J] sem aloc
    std::string GetName(Il2CppObject* o);          // [J] aloca

    bool  WorldToScreen(Vec3 w, Vec2& s);          // [J]
    bool  VisibleLine(Vec3 a, Vec3 b);             // [J]
    Vec3  TargetAimPos(Il2CppObject* p, int boneMode);     // [J]
    Vec3  BonePoint(Vec3 head, Vec3 feet, int boneMode);   // [J]

    // ------------------------------------------------------------ rotacao
    Il2CppObject* CamTfObj();                      // [J]
    Quat  LookRot(Vec3 dir);                       // [puro]
    bool  GetRot(Il2CppObject* tf, Quat& out);     // [J]
    bool  SetRot(Il2CppObject* tf, Quat q);        // [J]
    void  SetPlayerRot(Il2CppObject* p, const Quat& q);    // [J]
    Quat  GetPlayerRot(Il2CppObject* p);           // [J]

    // ------------------------------------------------------------ arma
    Il2CppObject* GetGunStats(Il2CppObject* player);       // [J]
    Il2CppObject* GetGunStatsLive();               // [J]
    Il2CppObject* FindGunStatsObj();               // [J]
    Il2CppObject* GetWeapon(Il2CppObject* player);         // [J]
    int  ResolveWeaponFields(Il2CppObject* stats, uint32_t& cur, uint32_t& max,
        uint32_t& reserve, uint32_t& reload, uint32_t& spread,
        uint32_t& rate, uint32_t& dmg);            // [J]

    bool  AimLock(Il2CppObject* p, Quat& savedOriginal);   // [J]
    void  RestoreAim(Il2CppObject* p, const Quat& q);      // [J]
    bool  FireBulletReady();                       // [J]
    void  FireBulletAt(Vec3 dir);                  // [J]

    // ------------------------------------------------------------ LOS
    bool  HasLineOfSight(Vec3 eye, Vec3 aim);                      // [J]
    bool  HasLineOfSightEx(Vec3 eye, Vec3 aim,
        Il2CppObject* localTf, Il2CppObject* targetTf);            // [J]
    bool  HasLineOfSightExRaw(Vec3 eye, Vec3 aim,
        Il2CppObject* localTf, Il2CppObject* targetTf);            // [J]
    bool  TargetVisible(int idx, Vec3 eye, Vec3 aim);              // [J]
    bool  RaycastTest(Vec3 from, Vec3 to, float& outHitDist);      // [J]

    void  LosInvalidate();                         // [J]
    bool  LosReady();                              // [barato]
    const char* LosBackend();                      // [barato]

    void  ClearTfCache();                          // [J]
    void  ClearBoneCache();                        // [J]
    void  PruneTfCache();                          // [J]

    // ------------------------------------------------------------ il2cpp
    Il2CppClass* FindClass(const char* name, const char* ns = nullptr);  // [J]
    Il2CppObject* GetTransform(void* obj);         // [J]
    Il2CppObject* SafeReadObjPub(void* obj, uint32_t off);   // [J]
    bool         ValidInstance(Il2CppObject* o);   // [J]
    Il2CppClass* SafeClass(Il2CppObject* o);       // [J]

    uint32_t PickFieldOff(Il2CppClass* k, std::initializer_list<const char*> names,
        const char* typeSub = nullptr);            // [J]

    void* MethodPtr(MethodInfo* m);                // [puro]
    const char* MethodReturnName(MethodInfo* m);   // [puro]

    // ------------------------------------------------------------ anti-hack
    Il2CppClass* AntiHackClass();                 // [J]
    Il2CppClass* FindAntiHackClass();             // [J]
    Il2CppObject* AntiHackInstance();              // [J]

    // ------------------------------------------------------------ debug
    void DumpFields(const char* className);        // [J]
    void DumpFields(Il2CppClass* k, const char* tag);      // [J]
    void DumpFieldsWithValues(Il2CppObject* o);    // [J]
    void ListClassesContaining(const char* sub);   // [J]
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