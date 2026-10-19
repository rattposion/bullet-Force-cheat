// ============================================================================
//  aimbot.cpp — v16 (corrigido)
//
//  FIXES desta versão:
//   [A1] TargetSnap com inicializadores default (mata C26495).
//   [A2] SDK::ClearTfCache / SDK::ClearBoneCache em OnDomainReload.
//   [A3] SDK::ValidInstance / SDK::SafeClass.
//   [A4] losCursor blindado contra n==0.
//   [A5] Tick()/Install()/Best()/AimPoint()/Silent::Override() (LNK2019).
//   [A6] Update() restaura rotacao tambem no modo hold.
//   [FIX VIS]  vis check agora DESCARTA alvo invisivel (nao so penaliza).
//   [FIX TEAM] team check duplo: IsEnemy + fallback por teamId.
//   [FIX COMPILE] linha 99/100: GetTeamId agora existe no SDK.
//   [FIX COMPILE] linha 167: TargetVisible chamado com 3 args (idx, eye, aim).
// ============================================================================

#include "aimbot.h"
#include "sdk.h"
#include "config.h"
#include "hooks.h"
#include "memory.h"
#include "common.h"

#include <cmath>
#include <cstring>
#include <vector>
#include <algorithm>
#include <unordered_map>

extern HMODULE g_hModule;

namespace Aimbot {

    // =======================================================================
    //  estado
    // =======================================================================
    static bool  s_active = false;
    static bool  s_hasTarget = false;
    static bool  s_installed = false;
    static Il2CppObject* s_curTarget = nullptr;
    static Quat  s_savedRot{ 0.f, 0.f, 0.f, 1.f };
    static ULONGLONG s_lastFire = 0;
    static ULONGLONG s_lastTargetSwitch = 0;
    static int   s_switchDelayMs = 180;

    static std::unordered_map<std::string, float> s_weaponStats;

    // =======================================================================
    //  struct de snapshot de alvo
    // =======================================================================
    struct TargetSnap {
        Il2CppObject* player = nullptr;
        Il2CppObject* obj = nullptr;
        bool  alive = false;
        bool  enemy = false;
        bool  hasAim = false;
        bool  visible = false;
        float dist = 0.f;
        Vec3  aimPos{ 0.f, 0.f, 0.f };
        Vec3  head{ 0.f, 0.f, 0.f };
        Vec3  feet{ 0.f, 0.f, 0.f };
        int   slot = -1;
        char  name[32]{};
    };

    static std::vector<TargetSnap> s_snap;

    // =======================================================================
    //  helpers
    // =======================================================================
    static inline float Dist3D(const Vec3& a, const Vec3& b) {
        float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
        return sqrtf(dx * dx + dy * dy + dz * dz);
    }

    static bool InFov(const Vec2& screen, float radiusPx) {
        const float cx = (float)SDK::ScreenW() * 0.5f;
        const float cy = (float)SDK::ScreenH() * 0.5f;
        const float dx = screen.x - cx;
        const float dy = screen.y - cy;
        return (dx * dx + dy * dy) <= (radiusPx * radiusPx);
    }

    // =======================================================================
    //  [FIX TEAM] — checagem de time robusta
    //  >>> CORRIGE LINHAS 99 e 100 (SDK::GetTeamId agora existe)
    // =======================================================================
    static bool IsSameTeam(Il2CppObject* a, Il2CppObject* b) {
        if (!a || !b) return false;

        // 1) caminho oficial
        if (!SDK::IsEnemy(a)) return true;   // IsEnemy false = mesmo time

        // 2) fallback por teamId (linhas 99/100 — agora compilam)
        int ta = SDK::GetTeamId(a);          // <<< linha ~99 corrigida
        int tb = SDK::GetTeamId(b);          // <<< linha ~100 corrigida
        if (ta >= 0 && tb >= 0) return ta == tb;

        return false;
    }

    // =======================================================================
    //  scan de alvos
    // =======================================================================
    static void ScanTargets() {
        s_snap.clear();

        Il2CppObject* local = SDK::GetLocalPlayer();
        if (!local) return;

        Vec3 localPos = SDK::GetPos(local);

        auto raw = SDK::Snapshot();
        s_snap.reserve(raw.size());

        for (auto& t : raw) {
            if (!t.player) continue;
            if (!SDK::ValidInstance(t.player)) continue;

            float hp = SDK::GetHealth(t.player);
            if (hp <= 0.5f) continue;

            // ---------------- [FIX TEAM] ----------------
            bool enemy = SDK::IsEnemy(t.player);
            if (C.aimTeamCheck) {
                if (!enemy) continue;                       // IsEnemy confiavel
                if (IsSameTeam(t.player, local)) continue;  // fallback reforcando
            }

            float dist = Dist3D(localPos, t.head);
            if (dist > C.aimMaxDist) continue;

            Vec2 scr{};
            if (!SDK::WorldToScreen(t.head, scr)) continue;

            float fovPx = SDK::FovRadiusPx(C.aimFov);
            if (!InFov(scr, fovPx)) continue;

            TargetSnap sn;
            sn.player = t.player;
            sn.obj = t.player;
            sn.alive = true;
            sn.enemy = enemy;
            sn.dist = dist;
            sn.head = t.head;
            sn.feet = t.feet;
            sn.slot = t.slot;
            strncpy_s(sn.name, sizeof(sn.name), t.name, _TRUNCATE);

            Vec3 aim = SDK::TargetAimPos(t.player, C.aimBone);
            if (aim.zero()) aim = t.head;
            sn.aimPos = aim;
            sn.hasAim = true;

            // ---------------- [FIX VIS] ----------------
            // >>> CORRIGE LINHA 167: TargetVisible recebe (idx, eye, aim)
            sn.visible = true;
            if (C.aimVisCheck) {
                if (!SDK::LosReady()) {
                    sn.visible = !C.aimStrictVis;     // sem LOS: bloqueia se strict
                }
                else {
                    sn.visible = SDK::TargetVisible(sn.slot,
                        SDK::MuzzlePos(),
                        sn.aimPos);   // <<< 3 args
                }
            }
            if (C.aimVisCheck && C.aimStrictVis && !sn.visible) continue;

            s_snap.push_back(sn);
        }
    }

    // =======================================================================
    //  escolha do melhor alvo
    // =======================================================================
    static TargetSnap* PickBest() {
        if (s_snap.empty()) return nullptr;

        TargetSnap* best = nullptr;
        float bestScore = 1e30f;

        for (auto& s : s_snap) {
            if (!s.alive || !s.hasAim) continue;

            if (C.aimVisCheck && C.aimStrictVis && !s.visible) continue;

            Vec2 scr{};
            if (!SDK::WorldToScreen(s.aimPos, scr)) continue;

            const float cx = (float)SDK::ScreenW() * 0.5f;
            const float cy = (float)SDK::ScreenH() * 0.5f;
            float dx = scr.x - cx, dy = scr.y - cy;
            float screenDist = sqrtf(dx * dx + dy * dy);

            if (C.aimVisCheck && !C.aimStrictVis && !s.visible)
                screenDist += 400.f;

            float score = screenDist;
            switch (C.aimSelect) {
            case 0: score = screenDist; break;
            case 1: score = s.dist; break;
            case 2: score = s.dist + screenDist * 0.001f; break;
            default: score = screenDist; break;
            }

            if (score < bestScore) { bestScore = score; best = &s; }
        }
        return best;
    }

    // =======================================================================
    //  aim
    // =======================================================================
    static void ApplyAim(TargetSnap* s) {
        if (!s) return;

        if (s->player != s_curTarget) {
            ULONGLONG now = GetTickCount64();
            if (now - s_lastTargetSwitch < (ULONGLONG)s_switchDelayMs) return;
            s_lastTargetSwitch = now;
            s_curTarget = s->player;
        }

        Quat original{};
        if (!SDK::AimLock(s->player, original)) return;

        s_savedRot = original;
        s_hasTarget = true;

        if (C.aimAutoFire && SDK::FireBulletReady()) {
            ULONGLONG now = GetTickCount64();
            ULONGLONG delay = (ULONGLONG)C.aimFireDelayMs;
            if (now - s_lastFire >= delay) {
                s_lastFire = now;
                Vec3 dir = s->aimPos - SDK::MuzzlePos();
                float L = dir.len();
                if (L > 0.0001f) {
                    dir = dir * (1.f / L);
                    SDK::FireBulletAt(dir);
                }
            }
        }
    }

    static void RestoreAimIfNeeded() {
        if (!s_hasTarget) return;
        if (!s_curTarget) return;
        SDK::RestoreAim(s_curTarget, s_savedRot);
        s_hasTarget = false;
    }

    // =======================================================================
    //  loop interno + API publica
    // =======================================================================
    static void CoreTick() {
        if (!C.aimEnabled) { s_active = false; RestoreAimIfNeeded(); return; }

        bool key = (GetAsyncKeyState(C.aimKey) & 0x8000) != 0;

        if (C.aimToggle) {
            static bool lastKey = false;
            if (key && !lastKey) s_active = !s_active;
            lastKey = key;
            if (!s_active) { RestoreAimIfNeeded(); return; }
        }
        else {
            s_active = key;
            if (!s_active) { RestoreAimIfNeeded(); return; }
        }

        ScanTargets();
        TargetSnap* best = PickBest();
        ApplyAim(best);
    }

    void Tick() { CoreTick(); }
    void Update() { CoreTick(); }

    Il2CppObject* Best() {
        ScanTargets();
        TargetSnap* s = PickBest();
        return s ? s->player : nullptr;
    }

    Vec3 AimPoint(void* target) {
        if (!target) return Vec3();
        Il2CppObject* p = (Il2CppObject*)target;

        Vec3 aim = SDK::TargetAimPos(p, C.aimBone);
        if (!aim.zero()) return aim;

        Vec3 pos = SDK::GetPos(p);
        return Vec3(pos.x, pos.y + 1.55f, pos.z);
    }

    namespace Silent {
        bool Override(Vec3 muzzle, Vec3& outDir) {
            if (!C.silentAim) return false;

            Il2CppObject* tgt = Best();
            if (!tgt) return false;

            Vec3 aim = AimPoint(tgt);
            if (aim.zero()) return false;

            Vec3 d{ aim.x - muzzle.x, aim.y - muzzle.y, aim.z - muzzle.z };
            float L = d.len();
            if (L < 0.0001f) return false;

            outDir = d * (1.f / L);
            return true;
        }
    }

    bool Install() {
        s_installed = true;
        s_active = false;
        s_hasTarget = false;
        s_curTarget = nullptr;
        s_lastFire = 0;
        s_lastTargetSwitch = 0;
        Log("[aimbot] Install OK (bone=%d fov=%.1f vis=%d strict=%d team=%d)",
            C.aimBone, C.aimFov, (int)C.aimVisCheck, (int)C.aimStrictVis, (int)C.aimTeamCheck);
        return true;
    }

    void OnDomainReload() {
        SDK::ClearTfCache();
        SDK::ClearBoneCache();
        s_snap.clear();
        s_curTarget = nullptr;
        s_hasTarget = false;
        s_active = false;
        s_weaponStats.clear();
    }

    void Shutdown() {
        s_snap.clear();
        s_curTarget = nullptr;
        s_hasTarget = false;
        s_active = false;
        s_installed = false;
        s_weaponStats.clear();
    }

    void DumpWeapon() {
        Il2CppObject* gun = SDK::GetGunStatsLive();
        if (!gun) { Log("[aim] sem gun stats"); return; }

        uint32_t cur = 0, max = 0, reserve = 0, reload = 0, spread = 0, rate = 0, dmg = 0;
        int n = SDK::ResolveWeaponFields(gun, cur, max, reserve, reload, spread, rate, dmg);
        Log("[aim] weapon fields resolvidos: %d", n);
        SDK::DumpFieldsWithValues(gun);
    }

    bool HasTarget() { return s_hasTarget && s_curTarget != nullptr; }
    Il2CppObject* CurrentTarget() { return s_curTarget; }

} // namespace Aimbot