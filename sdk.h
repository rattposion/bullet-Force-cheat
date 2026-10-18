#pragma once
// ============================================================================
//  sdk.h — UMBRELLA. Mantem compatibilidade total.
//
//  A responsabilidade esta' separada em:
//      sdk_types.h  -> Target, MAX_TARGETS
//      sdk_state.h  -> todo o estado mutavel + regra de concorrencia
//      sdk.h        -> funcoes + o bloco 'using' global
//
//  Se um arquivo so' precisa de 'Target', inclua sdk_types.h.
//  Se so' precisa do estado, inclua sdk_state.h.
//  O bloco 'using' no fim e' o motivo de quase tudo incluir este header —
//  ele joga ~60 nomes no escopo global.
// ============================================================================

#include "common.h"
#include "il2cpp.h"
#include "il2cpp_api.h"
#include "memory.h"
#include "config.h"
#include "hooks.h"          // dono de Hooks::g_viewW/g_viewH (nao redeclare aqui)
#include "sdk_types.h"
#include "sdk_state.h"

#include <vector>
#include <string>
#include <initializer_list>

extern const char* g_phase;
#ifndef PHASE
#define PHASE(x) (g_phase = (x))
#endif

namespace SDK {

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
    std::vector<Target> Snapshot();     // copia sob lock; seguro fora do lock

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
    void  GetNameBuf(Il2CppObject* o, char* out, int n);   // respeita 'n' e termina
    std::string GetName(Il2CppObject* o);

    bool  WorldToScreen(Vec3 w, Vec2& s);
    bool  VisibleLine(Vec3 a, Vec3 b);
    Vec3  TargetAimPos(Il2CppObject* p, int boneMode);

    // mapeamento de osso compartilhado (aimbot.cpp e TargetAimPos usam o MESMO).
    // 0=auto 1=cabeca 2=peito 3=estomago 4=perna 5=aleatorio.
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

// ---------------------------------------------------------------------------
//  BLOCO 'using' — joga os nomes do SDK no escopo global. Mantido por
//  compatibilidade. Se um dia quiser reduzir o acoplamento, e' aqui que se
//  corta: cada arquivo passaria a qualificar com SDK:: o que usa.
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