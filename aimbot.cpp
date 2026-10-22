// ============================================================================
//  aimbot.cpp — v19
//
//  FIX v19 (PATCHES A3 + B1):
//   [A3] Silent::Override nao chama mais Best() (que re-varria tudo). Usa o
//        alvo JA' escolhido no frame — o mesmo que a camera usa. Antes a
//        direcao podia divergir do alvo da camera -> paralaxe/erro.
//   [B1] EnsureScan(): UM scan por frame, cacheado por SDK::g_frameId.
//        Best() tambem e' cacheado por frame. ScanTargets usa SnapshotInto em
//        vector ESTATICO (zero heap por frame). Antes: 2-3 varreduras/frame
//        cada uma alocando std::vector via SDK::Snapshot().
//   [B1b] IsActive() exposto para o gate do silent aim (hooks.cpp).
//
//  FIX v18 (LINK): [L1..L4] inalterados.
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
    static std::vector<Target>     s_raw;          // [B1] buffer reusado

    // [B1] cache de scan por frame
    static uint64_t s_scanFrame = ~0ull;
    static uint64_t s_bestFrame = ~0ull;
    static Il2CppObject* s_bestPlayer = nullptr;

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

        SDK::SnapshotInto(s_raw);                  // [B1] sem heap
        s_snap.reserve(s_raw.size());

        for (auto& t : s_raw) {
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

    // [B1] um scan por frame
    static void EnsureScan() {
        const uint64_t f = SDK::g_frameId;
        if (s_scanFrame == f) return;
        s_scanFrame = f;
        ScanTargets();
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

        EnsureScan();
        TargetSnap* best = PickBest();
        ApplyAim(best);
    }

    void Tick() { CoreTick(); }
    void Update() { CoreTick(); }

    bool IsActive() { return s_active; }        // [B1b]

    // [B1][A3] alvo unico do frame, cacheado. Consumido por combat (camera) e
    //          pelo silent aim (hooks) -> os DOIS usam o MESMO alvo.
    Il2CppObject* Best() {
        EnsureScan();
        const uint64_t f = SDK::g_frameId;
        if (s_bestFrame == f) return s_bestPlayer;
        s_bestFrame = f;
        TargetSnap* s = PickBest();
        s_bestPlayer = s ? s->player : nullptr;
        return s_bestPlayer;
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
        // [A3] mesma origem/target do frame; sem re-scan
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
        s_scanFrame = ~0ull;
        s_bestFrame = ~0ull;
        s_bestPlayer = nullptr;
        Log("[aimbot] Install OK (bone=%d fov=%.1f vis=%d strict=%d team=%d)",
            C.aimBone, C.aimFov, (int)C.aimVisCheck, (int)C.aimStrictVis, (int)C.aimTeam);
        return true;
    }

    void OnDomainReload() {
        SDK::ClearTfCache();
        SDK::ClearBoneCache();
        s_snap.clear();
        s_raw.clear();
        s_curTarget = nullptr;
        s_hasTarget = false;
        s_active = false;
        s_scanFrame = ~0ull;
        s_bestFrame = ~0ull;
        s_bestPlayer = nullptr;
        s_weaponStats.clear();
    }

    void Shutdown() {
        s_snap.clear();
        s_raw.clear();
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