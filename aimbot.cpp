// ============================================================================
//  aimbot.cpp — v22
//
//  >>> v22 (ESTADO CONSISTENTE + PREDICAO POR TEMPO REAL)
//   [I1] Restauracao da rotacao usa o estado real de lock e limpa flags juntas.
//   [I2] Velocidade usa tempo decorrido real, nao FPS fixo de 60.
//   [I3] Historico de velocidade expira por tempo e reinicia em amostras invalidas.
//   [I4] Reset de dominio limpa tambem os indicadores de alvo/debug.
//
//  >>> v21 (ORIGEM DA MIRA CONSISTENTE + DIAGNOSTICO)
//   [H1] GetAimFrame(): origem E direcao vem do MESMO objeto. Antes era
//        LocalPos() (player) + CamForward() (camera) — dois objetos que, no
//        log, divergiam ~80 unidades. Angulo saia torto.
//   [H2] C.aimCamSrc: 0 = player (fresco, padrao) / 1 = camera. No log o
//        camPos estava ~1s atras do player.
//   [H3] WarnCamDivergence(): loga 1x/s se os dois divergem, pra decidir.
//   [H4] Predict/scan usam o mesmo frame.
//
//  >>> v20 (mantido): [G1] angulo em graus  [G2] cooldown nao larga a mira
//   [G3] fallback de bone  [G4] predicao  [G5] deadzone  [G6] cap de raycast
//  >>> v19 (mantido): [A3] silent usa o mesmo alvo  [B1] 1 scan/frame
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
#include <vector>
#include <algorithm>

extern HMODULE g_hModule;

namespace Aimbot {

    // =======================================================================
    //  estado
    // =======================================================================
    static bool  s_active = false;
    static bool  s_hasTarget = false;
    static bool  s_installed = false;
    static bool  s_aimLocked = false;
    static Il2CppObject* s_curTarget = nullptr;
    static Quat  s_savedRot{ 0.f, 0.f, 0.f, 1.f };
    static ULONGLONG s_lastFire = 0;
    static ULONGLONG s_lastTargetSwitch = 0;
    static float s_curAngle = 999.f;
    static int   s_curBone = -1;

    // =======================================================================
    //  snapshot de alvo
    // =======================================================================
    struct TargetSnap {
        Il2CppObject* player = nullptr;
        bool  alive = false;
        bool  enemy = false;
        bool  hasAim = false;
        bool  visible = false;
        bool  onScr = false;
        float dist = 0.f;
        float angle = 999.f;
        Vec3  aimPos{ 0.f, 0.f, 0.f };
        Vec3  aimRaw{ 0.f, 0.f, 0.f };
        Vec3  head{ 0.f, 0.f, 0.f };
        Vec3  feet{ 0.f, 0.f, 0.f };
        Vec2  scr{ 0.f, 0.f };
        int   slot = -1;
        int   bone = -1;
        char  name[32]{};
    };

    static std::vector<TargetSnap> s_snap;
    static std::vector<Target>     s_raw;

    static uint64_t s_scanFrame = ~0ull;
    static uint64_t s_bestFrame = ~0ull;
    static Il2CppObject* s_bestPlayer = nullptr;

    // =======================================================================
    //  math publica
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

    static Vec3 FwdFromQuat(const Quat& q) {
        const float x = q.x, y = q.y, z = q.z, w = q.w;
        return Vec3(2.f * (x * z + w * y),
            2.f * (y * z - w * x),
            1.f - 2.f * (x * x + y * y));
    }

    Vec3 LocalPos() {
        Il2CppObject* p = SDK::GetLocalPlayer();
        return p ? SDK::GetPos(p) : Vec3();
    }

    Vec3 CamForward() {
        Il2CppObject* cam = SDK::CamTfObj();
        if (!cam) return Vec3(0.f, 0.f, 1.f);
        return FwdFromQuat(SDK::GetPlayerRot(cam));
    }

    // =======================================================================
    //  [H1][H2] frame de mira: origem e direcao do MESMO objeto
    // =======================================================================
    struct AimFrame { Vec3 pos{ 0.f, 0.f, 0.f }; Vec3 fwd{ 0.f, 0.f, 1.f }; bool ok = false; };

    static AimFrame GetAimFrame() {
        AimFrame f;
        Il2CppObject* src = nullptr;

        if (C.aimCamSrc == 1) {
            src = SDK::CamTfObj();
            if (!src) src = SDK::GetLocalPlayer();
        }
        else {
            src = SDK::GetLocalPlayer();
            if (!src) src = SDK::CamTfObj();
        }
        if (!src) return f;

        f.pos = SDK::GetPos(src);
        if (f.pos.zero()) return f;
        f.fwd = FwdFromQuat(SDK::GetPlayerRot(src));
        f.ok = true;
        return f;
    }

    // [H3] diagnostico: 1x/s. Decide o aimCamSrc com dado, nao no chute.
    static void WarnCamDivergence() {
        static ULONGLONG next = 0;
        const ULONGLONG now = GetTickCount64();
        if (now < next) return;
        next = now + 1000;

        Il2CppObject* lp = SDK::GetLocalPlayer();
        Il2CppObject* cm = SDK::CamTfObj();
        if (!lp || !cm) return;

        const Vec3 a = SDK::GetPos(lp);
        const Vec3 b = SDK::GetPos(cm);
        const float d = fabsf(a.x - b.x) + fabsf(a.y - b.y) + fabsf(a.z - b.z);
        if (d > 2.f)
            Log("[aim] DIVERGENCIA player x camera: %.1f (player=%.1f,%.1f,%.1f cam=%.1f,%.1f,%.1f) src=%s",
                d, a.x, a.y, a.z, b.x, b.y, b.z, (C.aimCamSrc == 1) ? "CAMERA" : "PLAYER");
    }

    // =======================================================================
    //  angulo em GRAUS (espaco de mundo, sem projecao de tela)
    // =======================================================================
    static float AngleFrom(const Vec3& cam, const Vec3& fwd, const Vec3& p) {
        const Vec3 d = Norm(Sub(p, cam));
        float c = Dot(fwd, d);
        if (c > 1.f) c = 1.f;
        if (c < -1.f) c = -1.f;
        return acosf(c) * 57.29577951308232f;
    }

    float AngleTo(Vec3 world) {
        const AimFrame f = GetAimFrame();
        if (!f.ok) return 999.f;
        return AngleFrom(f.pos, f.fwd, world);
    }

    float AngleToTarget(void* target) {
        if (!target) return 999.f;
        return AngleTo(AimPoint(target));
    }

    // =======================================================================
    //  predicao — historico por frame, sem heap
    // =======================================================================
    struct VelEnt {
        Il2CppObject* p = nullptr;
        Vec3 last{ 0.f, 0.f, 0.f };
        Vec3 vel{ 0.f, 0.f, 0.f };
        uint64_t frame = 0;
        ULONGLONG sampleMs = 0;
    };
    static VelEnt s_vel[64];

    static Vec3 VelOf(Il2CppObject* p, const Vec3& now) {
        if (!p) return Vec3(0.f, 0.f, 0.f);

        const uint64_t frame = SDK::g_frameId;
        const ULONGLONG nowMs = GetTickCount64();

        for (int i = 0; i < 64; ++i) {
            VelEnt& e = s_vel[i];
            if (e.p != p) continue;

            // Nao atualiza duas vezes a mesma entidade no mesmo frame.
            if (e.frame == frame) return e.vel;

            const ULONGLONG elapsedMs = (nowMs >= e.sampleMs) ? (nowMs - e.sampleMs) : 0;
            const float dt = static_cast<float>(elapsedMs) * 0.001f;

            // Pausa longa ou timestamp inconsistente: reinicia a amostra em vez
            // de fabricar uma velocidade usando um dt arbitrario.
            if (dt < 0.001f || dt > 0.5f) {
                e.last = now;
                e.vel = Vec3(0.f, 0.f, 0.f);
                e.frame = frame;
                e.sampleMs = nowMs;
                return e.vel;
            }

            const Vec3 inst{ (now.x - e.last.x) / dt,
                             (now.y - e.last.y) / dt,
                             (now.z - e.last.z) / dt };

            // Ignora amostras nao finitas para nao contaminar o filtro.
            if (!std::isfinite(inst.x) || !std::isfinite(inst.y) || !std::isfinite(inst.z)) {
                e.last = now;
                e.vel = Vec3(0.f, 0.f, 0.f);
            }
            else {
                e.vel.x = e.vel.x * 0.55f + inst.x * 0.45f;
                e.vel.y = e.vel.y * 0.55f + inst.y * 0.45f;
                e.vel.z = e.vel.z * 0.55f + inst.z * 0.45f;
                e.last = now;
            }

            e.frame = frame;
            e.sampleMs = nowMs;
            return e.vel;
        }

        // Usa uma entrada livre ou o historico mais antigo. A expiracao e por
        // tempo real, sem assumir que o jogo roda a 60 FPS.
        int slot = -1;
        ULONGLONG oldest = ~static_cast<ULONGLONG>(0);
        for (int i = 0; i < 64; ++i) {
            if (!s_vel[i].p) { slot = i; break; }
            if (nowMs >= s_vel[i].sampleMs && nowMs - s_vel[i].sampleMs >= 500) {
                slot = i;
                break;
            }
            if (s_vel[i].sampleMs < oldest) {
                oldest = s_vel[i].sampleMs;
                slot = i;
            }
        }

        VelEnt& e = s_vel[slot];
        e.p = p;
        e.last = now;
        e.vel = Vec3(0.f, 0.f, 0.f);
        e.frame = frame;
        e.sampleMs = nowMs;
        return e.vel;
    }

    Vec3 Predict(Il2CppObject* e, Vec3 p) {
        if (!C.aimPredict || !e) return p;
        const Vec3 v = VelOf(e, p);
        if (v.x == 0.f && v.y == 0.f && v.z == 0.f) return p;

        const AimFrame f = GetAimFrame();
        const float from = f.ok ? Dist3D(f.pos, p) : Dist3D(LocalPos(), p);
        const float sp = (C.aimBulletSpeed > 1.f && std::isfinite(C.aimBulletSpeed))
            ? C.aimBulletSpeed : 400.f;
        if (!std::isfinite(from) || !std::isfinite(C.aimPredictAmt) || C.aimPredictAmt <= 0.f)
            return p;
        const float t = (from / sp) * C.aimPredictAmt;
        if (!std::isfinite(t) || t <= 0.f || t > 0.6f) return p;
        const Vec3 predicted(p.x + v.x * t, p.y + v.y * t, p.z + v.z * t);
        if (!std::isfinite(predicted.x) || !std::isfinite(predicted.y) || !std::isfinite(predicted.z))
            return p;
        return predicted;
    }

    // =======================================================================
    //  bone com fallback
    // =======================================================================
    bool BonePos(Il2CppObject* e, int bone, Vec3& out) {
        if (!e) return false;
        const Vec3 v = SDK::TargetAimPos(e, bone);
        if (v.zero()) return false;
        out = v;
        return true;
    }

    static bool AimPosOf(Il2CppObject* p, int slot, const Vec3& muzzle,
        bool losReady, Vec3& out, int& boneUsed) {
        int order[5]; int n = 0;
        order[n++] = C.aimBone;
        for (int b = 0; b < 4 && n < 5; ++b)
            if (b != C.aimBone) order[n++] = b;

        const bool needVis = C.aimVisCheck && C.aimStrictVis && losReady && slot >= 0;

        Vec3 firstAny{ 0.f, 0.f, 0.f };
        int  firstBone = -1;

        for (int i = 0; i < n; ++i) {
            const Vec3 v = SDK::TargetAimPos(p, order[i]);
            if (v.zero()) continue;
            if (firstBone < 0) { firstAny = v; firstBone = order[i]; }
            if (!needVis) { out = v; boneUsed = order[i]; return true; }
            if (SDK::TargetVisible(slot, muzzle, v)) { out = v; boneUsed = order[i]; return true; }
        }
        if (firstBone >= 0) { out = firstAny; boneUsed = firstBone; return true; }
        return false;
    }

    int   CurrentBone() { return s_curBone; }
    float CurrentAngle() { return s_curAngle; }

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
    //  scan de alvos (1x por frame)
    // =======================================================================
    struct Pre { int idx; float ang; };
    static Pre s_pre[128];

    static void ScanTargets() {
        s_snap.clear();

        Il2CppObject* local = SDK::GetLocalPlayer();
        if (!local) return;

        WarnCamDivergence();

        const AimFrame af = GetAimFrame();
        const Vec3  localPos = SDK::GetPos(local);
        const Vec3  camPos = af.ok ? af.pos : localPos;          // [H1]
        const Vec3  fwd = af.ok ? af.fwd : CamForward();         // [H1]
        const Vec3  muzzle = SDK::MuzzlePos();
        const bool  losReady = SDK::LosReady();
        const bool  teamOnly = (C.aimTeam || C.aimTeamCheck);
        const float maxD = (C.aimMaxDist > 1.f) ? C.aimMaxDist : 250.f;
        const float maxAng = (C.aimFov > 0.05f) ? C.aimFov : 8.f;
        const float maxD2 = maxD * maxD;
        const bool  needScr = (C.aimSelect == 3);
        const float fovPx = needScr ? SDK::FovRadiusPx(C.aimFov) : 0.f;

        SDK::SnapshotInto(s_raw);
        if (s_snap.capacity() < s_raw.size()) s_snap.reserve(s_raw.size());

        int np = 0;
        for (auto& t : s_raw) {
            if (!t.player) continue;
            if (!SDK::ValidInstance(t.player)) continue;
            if (SDK::GetHealth(t.player) <= 0.5f) continue;

            const bool enemy = SDK::IsEnemy(t.player);
            if (teamOnly) {
                if (!enemy) continue;
                if (IsSameTeam(t.player, local)) continue;
            }

            const float dx = t.head.x - localPos.x;
            const float dy = t.head.y - localPos.y;
            const float dz = t.head.z - localPos.z;
            const float d2 = dx * dx + dy * dy + dz * dz;
            if (d2 > maxD2) continue;

            const float ang = AngleFrom(camPos, fwd, t.head);
            if (ang > maxAng + 3.f) continue;
            if (np >= 128) continue;

            TargetSnap sn;
            sn.player = t.player;
            sn.alive = true;
            sn.enemy = enemy;
            sn.dist = sqrtf(d2);
            sn.head = t.head;
            sn.feet = t.feet;
            sn.slot = t.slot;
            strncpy_s(sn.name, sizeof(sn.name), t.name, _TRUNCATE);
            s_snap.push_back(sn);

            s_pre[np].idx = (int)s_snap.size() - 1;
            s_pre[np].ang = ang;
            ++np;
        }
        if (np == 0) return;

        std::sort(s_pre, s_pre + np, [](const Pre& a, const Pre& b) { return a.ang < b.ang; });

        const int topN = (C.aimVisCheck && C.aimStrictVis)
            ? ((C.aimVisTopN > 0) ? C.aimVisTopN : 8) : np;

        for (int i = 0; i < np && i < topN; ++i) {
            TargetSnap& sn = s_snap[s_pre[i].idx];
            Vec3 aim{ 0.f, 0.f, 0.f };
            int bone = -1;
            if (!AimPosOf(sn.player, sn.slot, muzzle, losReady, aim, bone)) continue;

            const float ang = AngleFrom(camPos, fwd, aim);
            if (ang > maxAng) continue;

            sn.aimRaw = aim;
            sn.aimPos = Predict(sn.player, aim);
            sn.angle = ang;
            sn.bone = bone;
            sn.hasAim = true;
            sn.visible = true;
            if (C.aimVisCheck && C.aimStrictVis && losReady && sn.slot >= 0)
                sn.visible = SDK::TargetVisible(sn.slot, muzzle, aim);
            if (C.aimVisCheck && C.aimStrictVis && !sn.visible) sn.hasAim = false;

            if (needScr && sn.hasAim) {
                Vec2 scr{ 0.f, 0.f };
                sn.onScr = SDK::WorldToScreen(aim, scr);
                sn.scr = scr;
                if (sn.onScr && !InFov(scr, fovPx)) sn.hasAim = false;
            }
        }

        for (int i = topN; i < np; ++i) {
            TargetSnap& sn = s_snap[s_pre[i].idx];
            if (sn.hasAim) continue;
            Vec3 aim{ 0.f, 0.f, 0.f };
            int bone = -1;
            if (!AimPosOf(sn.player, sn.slot, muzzle, false, aim, bone)) continue;

            const float ang = AngleFrom(camPos, fwd, aim);
            if (ang > maxAng) continue;

            sn.aimRaw = aim;
            sn.aimPos = Predict(sn.player, aim);
            sn.angle = ang;
            sn.bone = bone;
            sn.hasAim = true;
            sn.visible = true;
        }

        bool any = false;
        for (auto& s : s_snap) if (s.hasAim) { any = true; break; }
        if (!any && C.aimVisCheck && C.aimStrictVis) {
            for (int i = 0; i < np; ++i) {
                TargetSnap& sn = s_snap[s_pre[i].idx];
                Vec3 aim{ 0.f, 0.f, 0.f };
                int bone = -1;
                if (!AimPosOf(sn.player, sn.slot, muzzle, false, aim, bone)) continue;
                const float ang = AngleFrom(camPos, fwd, aim);
                if (ang > maxAng) continue;
                sn.aimRaw = aim;
                sn.aimPos = Predict(sn.player, aim);
                sn.angle = ang;
                sn.bone = bone;
                sn.hasAim = true;
                sn.visible = false;
                break;
            }
        }
    }

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

            float score;
            switch (C.aimSelect) {
            case 0:  score = s.angle; break;
            case 1:  score = s.dist; break;
            case 2:  score = s.angle + s.dist * 0.02f; break;
            default: {
                if (!s.onScr) continue;
                const float cx = (float)SDK::ScreenW() * 0.5f;
                const float cy = (float)SDK::ScreenH() * 0.5f;
                const float ex = s.scr.x - cx, ey = s.scr.y - cy;
                score = sqrtf(ex * ex + ey * ey);
                break;
            }
            }
            if (C.aimVisCheck && !C.aimStrictVis && !s.visible) score += C.aimInvisPenalty;

            if (score < bestScore) { bestScore = score; best = &s; }
        }
        return best;
    }

    // =======================================================================
    //  aim
    // =======================================================================
    static void RestoreAimIfNeeded() {
        // s_aimLocked indica que s_savedRot foi capturada e pode ser restaurada.
        // Nao depende de s_hasTarget, que e um indicador auxiliar de estado.
        if (s_aimLocked && s_curTarget) {
            if (SDK::ValidInstance(s_curTarget)) {
                SDK::RestoreAim(s_curTarget, s_savedRot);
            }
            else {
                // A instancia ja nao e valida: nao chamar metodos de rotacao nela.
                s_curTarget = nullptr;
            }
        }
        s_hasTarget = false;
        s_aimLocked = false;
    }

    static void ApplyAim(TargetSnap* s) {
        if (!s) return;

        const ULONGLONG now = GetTickCount64();

        if (s->player != s_curTarget) {
            const ULONGLONG cd = (ULONGLONG)((C.aimSwitchMs > 0) ? C.aimSwitchMs : 180);
            const bool cooling = (now - s_lastTargetSwitch) < cd;

            if (s_curTarget && cooling) {
                for (auto& q : s_snap) {
                    if (q.player == s_curTarget && q.hasAim) { s = &q; break; }
                }
            }
            if (s->player != s_curTarget) {
                RestoreAimIfNeeded();
                s_curTarget = s->player;
                s_lastTargetSwitch = now;
                s_curBone = -1;
            }
        }

        s_curAngle = s->angle;
        s_curBone = s->bone;

        const bool inDeadzone = s_aimLocked && (s->angle < C.aimDeadzone);
        const bool wantRotate = (C.aimRotate || C.aimAutoFire);

        if (wantRotate && !inDeadzone) {
            Quat original{};
            if (SDK::AimLock(s->player, original)) {
                if (!s_aimLocked) { s_savedRot = original; s_aimLocked = true; }
                s_hasTarget = true;
            }
        }
        else if (s_aimLocked) {
            s_hasTarget = true;
        }

        if (C.aimAutoFire && SDK::FireBulletReady()) {
            const ULONGLONG delay = (ULONGLONG)((C.aimFireDelayMs > 0) ? C.aimFireDelayMs : 50);
            if (now - s_lastFire >= delay) {
                s_lastFire = now;
                Vec3 dir = s->aimPos - SDK::MuzzlePos();
                const float L = dir.len();
                if (L > 0.0001f) SDK::FireBulletAt(dir * (1.f / L));
            }
        }
    }

    // =======================================================================
    //  loop interno + API publica
    // =======================================================================
    static void CoreTick() {
        if (!C.aimbot) { s_active = false; RestoreAimIfNeeded(); return; }

        const bool key = (GetAsyncKeyState(C.aimKey) & 0x8000) != 0;

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
        ApplyAim(PickBest());
    }

    void Tick() { CoreTick(); }

    bool IsActive() { return s_active; }

    Il2CppObject* Best() {
        if (!s_installed) return nullptr;
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

        Vec3 aim{ 0.f, 0.f, 0.f };
        if (BonePos(p, C.aimBone, aim)) return Predict(p, aim);

        const Vec3 pos = SDK::GetPos(p);
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

    void ClearTarget() {
        RestoreAimIfNeeded();
        s_curTarget = nullptr;
        s_curBone = -1;
        s_curAngle = 999.f;
    }

    void ForceReacquire() {
        ClearTarget();
        s_lastTargetSwitch = 0;
        s_scanFrame = ~0ull;
        s_bestFrame = ~0ull;
        s_bestPlayer = nullptr;
    }

    bool Install() {
        s_installed = true;
        s_active = false;
        s_hasTarget = false;
        s_aimLocked = false;
        s_curTarget = nullptr;
        s_lastFire = 0;
        s_lastTargetSwitch = 0;
        s_scanFrame = ~0ull;
        s_bestFrame = ~0ull;
        s_bestPlayer = nullptr;
        Log("[aimbot] Install OK v22 (bone=%d fov=%.1fdeg src=%s vis=%d strict=%d team=%d pred=%d topN=%d)",
            C.aimBone, C.aimFov, (C.aimCamSrc == 1) ? "CAMERA" : "PLAYER",
            (int)C.aimVisCheck, (int)C.aimStrictVis,
            (int)(C.aimTeam || C.aimTeamCheck), (int)C.aimPredict, C.aimVisTopN);
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
        s_aimLocked = false;
        s_curBone = -1;
        s_curAngle = 999.f;
        s_lastFire = 0;
        s_lastTargetSwitch = 0;
        s_scanFrame = ~0ull;
        s_bestFrame = ~0ull;
        s_bestPlayer = nullptr;
        memset(s_vel, 0, sizeof(s_vel));
    }

    void Shutdown() {
        RestoreAimIfNeeded();
        s_snap.clear();
        s_raw.clear();
        s_curTarget = nullptr;
        s_hasTarget = false;
        s_active = false;
        s_aimLocked = false;
        s_installed = false;
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