// ============================================================================
//  aimbot.cpp — v18
//
//  FIX v18 (LINK):
//   [L1] Dist3D(Vec3,Vec3) agora casa com o features.h (extern, por valor).
//        Antes era static inline com const& -> overload distinto, e a versao
//        do header ficava SEM definicao (LNK2019 pra quem chamasse de fora).
//   [L2] Definidos os simbolos que o features.h declara e nao existiam em
//        lugar nenhum: Sub/Dot/Norm/LookRot/Slerp/LocalPos/CamForward.
//        >>> SE algum deles JA estiver definido em outro .cpp, apague daqui
//            (senao vira LNK2005).
//   [L3] <string> incluido (s_weaponStats usa std::string).
//   [L4] features.h incluido -> o compilador confere as assinaturas.
//
//  (mantidas: v17 com os fixes de campo C.aimbot/C.aimTeam, vis, team, etc.)
// ============================================================================

#include "aimbot.h"
#include "features.h"
#include "sdk.h"
#include "config.h"
#include "hooks.h"
#include "memory.h"
#include "common.h"

#include <cmath>
#include <cstring>
#include <string>
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
    //  math publica (casando com o features.h)   [L1][L2]
    // =======================================================================
    Vec3 Sub(Vec3 a, Vec3 b) { return Vec3(a.x - b.x, a.y - b.y, a.z - b.z); }

    float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

    Vec3 Norm(Vec3 v) {
        const float L = sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
        if (L < 0.000001f) return Vec3(0.f, 0.f, 0.f);
        return Vec3(v.x / L, v.y / L, v.z / L);
    }

    float Dist3D(Vec3 a, Vec3 b) {
        const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
        return sqrtf(dx * dx + dy * dy + dz * dz);
    }

    // yaw puro em torno de Y (Unity: frente = +Z)
    ::Quat LookRot(Vec3 fwd) {
        const float yaw = atan2f(fwd.x, fwd.z);
        const float h = yaw * 0.5f;
        return ::Quat{ 0.f, sinf(h), 0.f, cosf(h) };
    }

    ::Quat Slerp(::Quat a, ::Quat b, float t) {
        float d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
        if (d < 0.f) { b.x = -b.x; b.y = -b.y; b.z = -b.z; b.w = -b.w; d = -d; }

        if (d > 0.9995f) {
            ::Quat r{ a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t,
                      a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t };
            const float L = sqrtf(r.x * r.x + r.y * r.y + r.z * r.z + r.w * r.w);
            if (L > 0.000001f) { r.x /= L; r.y /= L; r.z /= L; r.w /= L; }
            return r;
        }

        const float th = acosf(d);
        const float s = sinf(th);
        const float wa = sinf((1.f - t) * th) / s;
        const float wb = sinf(t * th) / s;
        return ::Quat{ a.x * wa + b.x * wb, a.y * wa + b.y * wb,
                       a.z * wa + b.z * wb, a.w * wa + b.w * wb };
    }

    Vec3 LocalPos() {
        Il2CppObject* p = SDK::GetLocalPlayer();
        return p ? SDK::GetPos(p) : Vec3();
    }

    Vec3 CamForward() {
        Il2CppObject* cam = SDK::CamTfObj();
        if (!cam) return Vec3(0.f, 0.f, 1.f);
        const ::Quat q = SDK::GetPlayerRot(cam);
        // roda (0,0,1) por q
        const float x = q.x, y = q.y, z = q.z, w = q.w;
        return Vec3(2.f * (x * z + w * y),
            2.f * (y * z - w * x),
            1.f - 2.f * (x * x + y * y));
    }

    // =======================================================================
    //  helpers internos
    // =======================================================================
    static bool InFov(const Vec2& screen, float radiusPx) {
        const float cx = (float)SDK::ScreenW() * 0.5f;
        const float cy = (float)SDK::ScreenH() * 0.5f;
        const float dx = screen.x - cx;
        const float dy = screen.y - cy;
        return (dx * dx + dy * dy) <= (radiusPx * radiusPx);
    }

    static bool IsSameTeam(Il2CppObject* a, Il2CppObject* b) {
        if (!a || !b) return false;
        if (!SDK::IsEnemy(a)) return true;
        int ta = SDK::GetTeamId(a);
        int tb = SDK::GetTeamId(b);
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

            bool enemy = SDK::IsEnemy(t.player);
            if (C.aimTeam) {
                if (!enemy) continue;
                if (IsSameTeam(t.player, local)) continue;
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

            sn.visible = true;
            if (C.aimVisCheck) {
                if (!SDK::LosReady()) {
                    sn.visible = !C.aimStrictVis;
                }
                else {
                    sn.visible = SDK::TargetVisible(sn.slot, SDK::MuzzlePos(), sn.aimPos);
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
        if (!C.aimbot) { s_active = false; RestoreAimIfNeeded(); return; }

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
            C.aimBone, C.aimFov, (int)C.aimVisCheck, (int)C.aimStrictVis, (int)C.aimTeam);
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