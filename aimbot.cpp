// ============================================================================
//  aimbot.cpp — bfi  (v11)
//
//  >>> CONTRATOS ASSUMIDOS DO SDK (nao confirmaveis neste arquivo):
//   [C1] SDK::g_targets / SDK::g_targetCount / SDK::g_aimTarget podem ser
//        lidos por outra thread durante o frame. NAO assumimos lock: o codigo
//        monta um SNAPSHOT por frame e so trabalha em cima dele.
//   [C2] Target::player e' a IDENTIDADE do alvo (usada em toda comparacao).
//        Target::obj e' aceito como alias na busca, mas nunca como identidade.
//   [C3] SDK::BonePoint(h, f, modo) devolve ponto valido ou zero.
//   [C4] SDK::GetPos/GetRot/SetRot/SetPlayerRot validam objeto internamente,
//        mas podem falhar; tratamos retorno e resultado.
//   [C5] SDK::HasLineOfSight pode falhar/AV -> sempre via SafeLos().
//   [C6] Silent::Override recebe dir possivelmente NAO normalizado -> o
//        contrato real de entrada e' "direcao qualquer != zero".
//
//  >>> CORRECOES DESTA VERSAO:
//   [1] SNAPSHOT por frame -> sem corrida em g_targets/g_targetCount.
//   [2] g_best/g_sticky com validacao de vida util contra o snapshot atual
//       + decaimento temporal (sticky expira).
//   [3] Clamp1 em todo Dot->acos, com validacao de nao-finito em todas as
//       entradas de score.
//   [4] __try apenas nos 3 pontos de entrada (Best/Tick/Silent::Override),
//       com locais POD -> evita C2712 do MSVC. Dependencias validadas antes
//       de desreferenciar.
//   [5] Teto de candidatos elevado e documentado; fallback de screenMode
//       agora EXPLICITO e logado, nao silencioso.
//   [6] Silent usa a MESMA funcao de score e o mesmo FOV do Best.
//   [7] Vetores: NormSafe devolve sucesso, LookRot valida, Slerp limita t,
//       PlausibleQ checa nao-finito.
//   [8] g_rotProbed re-testavel quando o player muda; shutdown sincronizado.
// ============================================================================
#include "common.h"
#include "sdk.h"
#include "config.h"
#include "features.h"

#include <cmath>
#include <cfloat>
#include <atomic>

namespace Aimbot {

    static_assert(sizeof(::Quat) == 16, "Quat deve ter 4 floats");

    // ------------------------------------------------------------- constantes
    static const int      kMaxSnap = 128;   // [5] era 64
    static const int      kMaxCand = 128;
    static const uint64_t kStickyMs = 300;   // validade da histerese
    static const uint64_t kSnapMaxAgeMs = 3;     // reuso do snapshot no frame
    static const int      kProbeRetries = 8;     // tentativas de probe de rot
    static const float    kEyeHeight = 1.55f;
    static const float    kRad2Deg = 57.2957795f;

    // ------------------------------------------------------------- util POD
    static inline bool IsFiniteF(float f) {
        return f == f && f <= FLT_MAX && f >= -FLT_MAX;
    }
    static inline bool Finite3(Vec3 v) {
        return IsFiniteF(v.x) && IsFiniteF(v.y) && IsFiniteF(v.z);
    }
    // "valido e nao-nulo". zero() sozinho e' ambiguo: (0,0,0) pode ser legitimo,
    // mas em pratica e' o sentinela de falha do SDK.
    static inline bool Ok3(Vec3 v) {
        return Finite3(v) && !(v.x == 0.f && v.y == 0.f && v.z == 0.f);
    }
    static inline float Clamp1(float v) { return v < -1.f ? -1.f : (v > 1.f ? 1.f : v); }

    // ---------------------------------------------------------------- vetores
    Vec3 Sub(Vec3 a, Vec3 b) { return Vec3{ a.x - b.x, a.y - b.y, a.z - b.z }; }
    float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    static float Len(Vec3 v) { return sqrtf(Dot(v, v)); }

    // [7] normalizacao com SUCESSO explicito + tratamento de overflow.
    static bool NormSafe(Vec3 v, Vec3& out)
    {
        float l = Len(v);
        if (!IsFiniteF(l)) {                       // overflow do produto escalar
            float m = fmaxf(fabsf(v.x), fmaxf(fabsf(v.y), fabsf(v.z)));
            if (!IsFiniteF(m) || m < 1e-6f) { out = v; return false; }
            Vec3 t{ v.x / m, v.y / m, v.z / m };
            float l2 = Len(t);
            if (!IsFiniteF(l2) || l2 < 1e-6f) { out = t; return false; }
            out = t * (1.f / l2);
            return true;
        }
        if (l < 1e-6f) { out = v; return false; }   // nulo -> direcao nao confiavel
        out = v * (1.f / l);
        return true;
    }
    Vec3 Norm(Vec3 v) { Vec3 o; NormSafe(v, o); return o; }   // compat

    // [7] PlausibleQ agora checa nao-finito.
    static bool PlausibleQ(const ::Quat& q)
    {
        if (!IsFiniteF(q.x) || !IsFiniteF(q.y) || !IsFiniteF(q.z) || !IsFiniteF(q.w))
            return false;
        float n = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
        return n > 0.25f && n < 2.25f;
    }

    static Vec3 FwdOfQ(const ::Quat& q)
    {
        Vec3 f{
            2.f * (q.x * q.z + q.w * q.y),
            2.f * (q.y * q.z - q.w * q.x),
            1.f - 2.f * (q.x * q.x + q.y * q.y)
        };
        Vec3 o; NormSafe(f, o);
        return o;
    }

    // [7] LookRot valida entrada e resultado. Unity YXZ (yaw Y, pitch X), sem roll.
    static bool LookRotChecked(Vec3 dir, ::Quat& out)
    {
        Vec3 d;
        if (!NormSafe(dir, d)) return false;
        float yaw = atan2f(d.x, d.z);
        float pitch = -asinf(Clamp1(d.y));
        if (!IsFiniteF(yaw) || !IsFiniteF(pitch)) return false;
        float sy = sinf(yaw * 0.5f), cy = cosf(yaw * 0.5f);
        float sp = sinf(pitch * 0.5f), cp = cosf(pitch * 0.5f);
        out = ::Quat{ cy * sp, sy * cp, -sy * sp, cy * cp };
        return true;
    }
    ::Quat LookRot(Vec3 dir) { ::Quat q{ 0,0,0,1 }; LookRotChecked(dir, q); return q; }

    // [7] Slerp limita t internamente e trata degenerados.
    ::Quat Slerp(::Quat a, ::Quat b, float t)
    {
        if (!PlausibleQ(a)) return b;
        if (!PlausibleQ(b)) return a;
        if (!IsFiniteF(t)) t = 1.f;
        if (t <= 0.f) return a;
        if (t >= 1.f) return b;

        float d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
        if (d < 0) { b.x = -b.x; b.y = -b.y; b.z = -b.z; b.w = -b.w; d = -d; }

        if (d > 0.9995f) {                                  // nlerp
            ::Quat r{ a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t,
                      a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t };
            float l = sqrtf(r.x * r.x + r.y * r.y + r.z * r.z + r.w * r.w);
            if (!IsFiniteF(l) || l < 1e-6f) return b;       // degenerado -> b
            r.x /= l; r.y /= l; r.z /= l; r.w /= l;
            return r;
        }
        float th = acosf(Clamp1(d)), s = sinf(th);
        if (!IsFiniteF(s) || fabsf(s) < 1e-6f) return b;
        float wa = sinf((1 - t) * th) / s, wb = sinf(t * th) / s;
        return ::Quat{ a.x * wa + b.x * wb, a.y * wa + b.y * wb,
                       a.z * wa + b.z * wb, a.w * wa + b.w * wb };
    }

    // =========================================================================
    //  SNAPSHOT  [1]
    //  Copia o que importa do frame e isola o resto do codigo da lista global.
    //  Nao segura lock: se o SDK publicar com lock, BuildSnapshot e' o unico
    //  lugar a ajustar. Validacao por entrada + rechecagem de contagem.
    // =========================================================================
    struct TargetSnap {
        Il2CppObject* player;
        Il2CppObject* obj;
        bool          alive;
        bool          enemy;
        float         dist;
        Vec3          head;
        Vec3          feet;
        Vec3          aim;
        bool          hasAim;
    };

    static TargetSnap g_snap[kMaxSnap];
    static int        g_snapN = 0;
    static uint64_t   g_snapMs = 0;
    static bool       g_snapClamp = false;

    static int BuildSnapshot()
    {
        int n = 0;
        __try {
            int cnt = SDK::g_targetCount;
            if (cnt < 0) cnt = 0;
            g_snapClamp = (cnt > kMaxSnap);
            if (cnt > kMaxSnap) cnt = kMaxSnap;

            for (int i = 0; i < cnt; ++i) {
                const Target& t = SDK::g_targets[i];
                if (!t.player) continue;

                TargetSnap& s = g_snap[n];
                s.player = t.player;
                s.obj = t.obj;
                s.alive = t.alive;
                s.enemy = t.enemy;
                s.dist = t.dist;
                s.head = t.head;
                s.feet = t.feet;

                Vec3 h = t.head;
                if (t.headBone) { Vec3 b = SDK::GetPos(t.headBone); if (Ok3(b)) h = b; }
                Vec3 f = t.feet;
                if (t.footBone) { Vec3 b = SDK::GetPos(t.footBone); if (Ok3(b)) f = b; }

                Vec3 a = SDK::BonePoint(h, f, C.aimBone);
                if (!Ok3(a)) a = h;                     // [C3] fallback -> nunca zero
                s.aim = a;
                s.hasAim = Ok3(a);
                ++n;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            // mantem o que ja copiou; snapshot parcial e' melhor que nada
        }
        g_snapN = n;
        g_snapMs = GetTickCount64();
        return n;
    }

    static void EnsureSnapshot()
    {
        if (g_snapN > 0 && (GetTickCount64() - g_snapMs) <= kSnapMaxAgeMs) return;
        BuildSnapshot();
    }

    // busca por identidade [C2]
    static int FindSnap(Il2CppObject* player)
    {
        if (!player) return -1;
        for (int i = 0; i < g_snapN; ++i)
            if (g_snap[i].player == player) return i;
        return -1;
    }
    static bool TargetLive(Il2CppObject* player)
    {
        int i = FindSnap(player);
        return i >= 0 && g_snap[i].alive;
    }

    // =========================================================================
    //  camadas seguras (poucas linhas, __try pontual)  [4]
    // =========================================================================
    static bool SafeLos(Vec3 a, Vec3 b)
    {
        if (!Ok3(a) || !Ok3(b)) return false;
        __try { return SDK::HasLineOfSight(a, b); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    // =========================================================================
    //  eye / forward com sucesso explicito  [5][5.3][5.4]
    // =========================================================================
    static bool EyePosChecked(Vec3& out)
    {
        if (!SDK::g_localPlayer) return false;

        if (Il2CppObject* ct = SDK::CamTfObj()) {
            Vec3 p = SDK::GetPos(ct);
            if (Ok3(p)) { out = p; return true; }
        }
        Vec3 p = SDK::GetPos(SDK::g_localPlayer);
        if (Ok3(p)) { out = Vec3{ p.x, p.y + kEyeHeight, p.z }; return true; }
        if (Ok3(SDK::g_localPos)) { out = SDK::g_localPos; return true; }
        return false;
    }
    Vec3 EyePos() { Vec3 o{}; EyePosChecked(o); return o; }
    Vec3 LocalPos() { return EyePos(); }

    static bool CamForwardChecked(Vec3& out)
    {
        if (Il2CppObject* ct = SDK::CamTfObj()) {
            ::Quat q{};
            if (SDK::GetRot(ct, q) && PlausibleQ(q)) {
                Vec3 f = FwdOfQ(q);
                if (Ok3(f)) { out = f; return true; }
            }
        }
        if (SDK::g_localPlayer) {
            ::Quat q = SDK::GetPlayerRot(SDK::g_localPlayer);
            if (PlausibleQ(q)) {
                Vec3 f = FwdOfQ(q);
                if (Ok3(f)) { out = f; return true; }
            }
        }
        out = Vec3{ 0.f, 0.f, 1.f };       // fallback assumido
        return false;                       // chamador sabe que e' presumido
    }
    Vec3 CamForward() { Vec3 o{}; CamForwardChecked(o); return o; }

    // =========================================================================
    //  estado de selecao  [2]
    // =========================================================================
    static Il2CppObject* g_best = nullptr;
    static Il2CppObject* g_sticky = nullptr;
    static uint64_t      g_stickyMs = 0;
    static uint32_t      g_crashCount = 0;
    static uint64_t      g_lastAimLog = 0;

    static bool StickyValid() {
        return g_sticky && (GetTickCount64() - g_stickyMs) <= kStickyMs;
    }

    // guardas de reentrancia / shutdown
    static std::atomic<bool> g_busyBest{ false };
    static std::atomic<bool> g_busyTick{ false };
    static std::atomic<bool> g_busySilent{ false };
    static std::atomic<bool> g_shutting{ false };

    // probe de rotacao do player  [8]
    static bool          g_rotProbed = false;
    static Il2CppObject* g_probePlayer = nullptr;
    static int           g_probeTries = 0;
    static bool          g_writePlayerRot = true;

    static bool g_screenFallbackLogged = false;
    static bool g_fwdFallbackLogged = false;

    // =========================================================================
    //  score unico compartilhado por Best e Silent  [6]
    // =========================================================================
    struct ScoreCtx {
        Vec3  eye;
        Vec3  fwd;
        float fovDeg;
        float radiusPx;
        bool  screenMode;
        float cx, cy;
    };

    static float ScoreSnap(const TargetSnap& s, const ScoreCtx& c)
    {
        if (!s.player || s.player == SDK::g_localPlayer) return -1.f;
        if (!s.alive) return -1.f;
        if (C.aimTeam && !s.enemy) return -1.f;
        if (C.aimMaxDist > 0.f && s.dist > C.aimMaxDist) return -1.f;
        if (!s.hasAim || !Ok3(s.aim)) return -1.f;

        float score;
        if (c.screenMode) {
            Vec2 p{};
            if (!SDK::WorldToScreen(s.aim, p)) return -1.f;
            if (!IsFiniteF(p.x) || !IsFiniteF(p.y)) return -1.f;
            float dx = p.x - c.cx, dy = p.y - c.cy;
            score = sqrtf(dx * dx + dy * dy);
            if (!IsFiniteF(score) || score > c.radiusPx) return -1.f;
        }
        else {
            Vec3 d;
            if (!NormSafe(Sub(s.aim, c.eye), d)) return -1.f;
            float dot = Clamp1(Dot(c.fwd, d));              // [3]
            float ang = acosf(dot) * kRad2Deg;
            if (!IsFiniteF(ang) || ang > c.fovDeg) return -1.f;
            score = ang;
        }

        score += 0.5f;
        if (s.player == g_sticky && StickyValid()) score *= 0.75f;
        return score;
    }

    struct Cand { float score; int idx; };

    // =========================================================================
    //  selecao (sem __try: roda dentro do __try de Best)
    // =========================================================================
    static bool BuildCtx(ScoreCtx& c, Vec3 origin, Vec3 fwd, bool forceAngle)
    {
        c.eye = origin;
        c.fwd = fwd;
        c.fovDeg = C.aimFov;
        c.cx = (float)SDK::ScreenW() * 0.5f;
        c.cy = (float)SDK::ScreenH() * 0.5f;
        c.radiusPx = SDK::FovRadiusPx(C.aimFov);

        bool screen = !forceAngle && (C.aimMode != 1) && SDK::g_viewProjOk;
        if (screen && !(IsFiniteF(c.radiusPx) && c.radiusPx > 2.f && c.radiusPx < 100000.f)) {
            screen = false;
            if (!g_screenFallbackLogged) {
                g_screenFallbackLogged = true;
                Log("[aim] modo camera indisponivel (viewProj=%d radius=%.1f) -> modo angulo",
                    (int)SDK::g_viewProjOk, c.radiusPx);
            }
        }
        c.screenMode = screen;
        return true;
    }

    static Il2CppObject* SelectFromSnapshot(const ScoreCtx& ctx)
    {
        if (g_snapN <= 0) return nullptr;

        Cand cand[kMaxCand];
        int nc = 0;
        for (int i = 0; i < g_snapN && nc < kMaxCand; ++i) {
            float s = ScoreSnap(g_snap[i], ctx);
            if (s < 0.f) continue;
            cand[nc].score = s;
            cand[nc].idx = i;
            ++nc;
        }
        if (nc == 0) return nullptr;

        // insertion sort ascendente (nc <= 128, custo previsivel)
        for (int i = 1; i < nc; ++i) {
            Cand key = cand[i];
            int j = i - 1;
            while (j >= 0 && cand[j].score > key.score) { cand[j + 1] = cand[j]; --j; }
            cand[j + 1] = key;
        }

        const bool filtrar = (C.aimVisCheck != 0) && SDK::LosReady();
        if (!filtrar) return g_snap[cand[0].idx].player;

        for (int i = 0; i < nc; ++i) {
            const TargetSnap& s = g_snap[cand[i].idx];
            if (SafeLos(ctx.eye, s.aim)) return s.player;
        }
        return C.aimStrictVis ? nullptr : g_snap[cand[0].idx].player;
    }

    static Il2CppObject* BestUnsafe()
    {
        EnsureSnapshot();
        if (g_snapClamp && g_snapN >= kMaxSnap) {
            static uint64_t last = 0;
            uint64_t now = GetTickCount64();
            if (now - last >= 1000) {
                last = now;
                Log("[aim] alvos truncados: %d > %d", SDK::g_targetCount, kMaxSnap);
            }
        }
        if (g_snapN <= 0) return nullptr;

        Vec3 eye{};
        if (!EyePosChecked(eye)) return nullptr;

        Vec3 fwd{};
        if (!CamForwardChecked(fwd) && !g_fwdFallbackLogged) {
            g_fwdFallbackLogged = true;
            Log("[aim] forward da camera indisponivel -> usando fallback");
        }

        ScoreCtx ctx{};
        BuildCtx(ctx, eye, fwd, /*forceAngle=*/false);
        return SelectFromSnapshot(ctx);
    }

    // =========================================================================
    //  API publica
    // =========================================================================
    Il2CppObject* Best()
    {
        if (g_shutting.load()) return nullptr;

        if ((!C.aimbot && !C.triggerbot) || !SDK::g_localPlayer) {
            g_best = nullptr;
            return nullptr;
        }
        if (g_busyBest.exchange(true, std::memory_order_acquire)) return g_best;

        Il2CppObject* pick = nullptr;
        __try {
            pick = BestUnsafe();
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            ++g_crashCount;
            Log("[crash] Aimbot::Best");
            pick = nullptr;
        }

        // [2] publica SOMENTE se o ponteiro ainda consta no snapshot atual
        if (pick && TargetLive(pick)) {
            g_best = pick;
            g_sticky = pick;
            g_stickyMs = GetTickCount64();
        }
        else {
            g_best = nullptr;
            if (!StickyValid()) g_sticky = nullptr;
        }

        if (C.probeLog && g_best) {
            uint64_t now = GetTickCount64();
            if (now - g_lastAimLog >= 250) {
                g_lastAimLog = now;
                Vec3 ap = AimPoint((void*)g_best);
                Vec3 eye{};
                EyePosChecked(eye);
                Vec3 fwd{}; CamForwardChecked(fwd);
                float dist = Len(Sub(ap, eye));
                Vec3 d{}; bool dOk = NormSafe(Sub(ap, eye), d);
                float ang = dOk ? acosf(Clamp1(Dot(fwd, d))) * kRad2Deg : -1.f;
                Vec2 sp{}; bool on = SDK::WorldToScreen(ap, sp);
                Log("[aim] alvo=%p dist=%.1f ang=%.1f onScreen=%d visCheck=%d strict=%d",
                    g_best, dist, ang, (int)on, (int)C.aimVisCheck, (int)C.aimStrictVis);
            }
        }

        g_busyBest.store(false, std::memory_order_release);
        return g_best;
    }

    Vec3 AimPoint(void* obj)
    {
        if (!obj) return Vec3{};
        EnsureSnapshot();
        for (int i = 0; i < g_snapN; ++i)
            if (g_snap[i].player == obj || g_snap[i].obj == obj)   // [C2] identidade + alias
                return g_snap[i].aim;
        return Vec3{};
    }

    static bool KeyDown() {
        if (!C.aimKey) return true;                 // 0 = sempre ativo
        return (GetAsyncKeyState(C.aimKey) & 0x8000) != 0;
    }

    // [7] suavizacao estavel em relacao ao framerate, preservando o "feel"
    // original a 60fps (t = 1/smooth).
    static float SmoothT()
    {
        if (C.aimSmooth <= 0.f) return 1.f;
        static uint64_t last = 0;
        uint64_t now = GetTickCount64();
        float dt = (last == 0) ? (1.f / 60.f) : (float)(now - last) * 0.001f;
        last = now;
        if (!IsFiniteF(dt) || dt <= 0.f || dt > 0.25f) dt = 1.f / 60.f;

        float base = 1.f / C.aimSmooth;
        if (base > 1.f) base = 1.f;
        float t = 1.f - powf(1.f - base, dt * 60.f);
        return (t < 0.f) ? 0.f : (t > 1.f ? 1.f : t);
    }

    static void ProbePlayerRot()
    {
        if (!SDK::g_localPlayer) return;
        if (g_rotProbed && g_probePlayer == SDK::g_localPlayer) return;

        ::Quat q = SDK::GetPlayerRot(SDK::g_localPlayer);
        bool ok = PlausibleQ(q);

        // [8] falha transitoria NAO vira resultado definitivo
        if (!ok && g_probeTries < kProbeRetries) { ++g_probeTries; return; }

        g_rotProbed = true;
        g_probePlayer = SDK::g_localPlayer;
        g_writePlayerRot = ok;

        if (ok)
            Log("[aim] playerRot legivel (%.3f,%.3f,%.3f,%.3f) -> escreve no player tambem",
                q.x, q.y, q.z, q.w);
        else
            Log("[aim] playerRot ILEGIVEL apos %d tentativas -> SO camTf", g_probeTries);
    }

    static void TickUnsafe()
    {
        // [5] alvo revalidado contra o snapshot do frame corrente
        Il2CppObject* best = SDK::g_aimTarget ? SDK::g_aimTarget : g_best;
        EnsureSnapshot();
        if (best && !TargetLive(best)) {
            if (best != g_best && g_best && TargetLive(g_best)) best = g_best;
            else best = nullptr;
        }
        if (!best) return;
        if (!KeyDown()) return;

        Vec3 aim = AimPoint(best);
        if (!Ok3(aim)) return;

        Vec3 eye{};
        if (!EyePosChecked(eye)) return;

        Vec3 dir{};
        if (!NormSafe(Sub(aim, eye), dir)) return;

        ::Quat want{};
        if (!LookRotChecked(dir, want)) return;

        Il2CppObject* camTf = SDK::CamTfObj();

        ::Quat cur{ 0,0,0,1 };
        bool haveCur = false;
        if (camTf) {
            ::Quat q{};
            if (SDK::GetRot(camTf, q) && PlausibleQ(q)) { cur = q; haveCur = true; }
        }
        // [5.5] sem camTf ainda da' pra usar a rotacao do player
        if (!haveCur && SDK::g_localPlayer) {
            ::Quat q = SDK::GetPlayerRot(SDK::g_localPlayer);
            if (PlausibleQ(q)) { cur = q; haveCur = true; }
        }
        if (!haveCur) cur = ::Quat{ 0,0,0,1 };

        ::Quat q = Slerp(cur, want, SmoothT());

        if (camTf) SDK::SetRot(camTf, q);
        if (g_writePlayerRot && SDK::g_localPlayer) SDK::SetPlayerRot(SDK::g_localPlayer, q);
    }

    void Tick()
    {
        if (g_shutting.load()) return;
        if (!SDK::g_localPlayer) return;

        ProbePlayerRot();

        if (!C.aimbot || !C.aimRotate) return;
        if (g_busyTick.exchange(true, std::memory_order_acquire)) return;

        __try {
            TickUnsafe();
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            ++g_crashCount;
            Log("[crash] Aimbot::Tick");
        }

        g_busyTick.store(false, std::memory_order_release);
    }

    // =========================================================================
    //  SILENT AIM / MAGIC BULLETS  [6]
    //  Mesmo score, mesmo FOV, mesma politica de LOS do Best. Unica diferenca:
    //  referencia angular e' o eixo do projetil (origin/dir), nao a camera.
    // =========================================================================
    namespace Silent {

        static bool SilentUnsafe(Vec3 origin, Vec3 dirIn, Vec3& out)
        {
            if (!Ok3(origin)) return false;

            Vec3 dir{};
            if (!NormSafe(dirIn, dir)) return false;   // [C6] entrada pode vir crua

            EnsureSnapshot();
            if (g_snapN <= 0) return false;

            ScoreCtx c{};
            c.eye = origin;
            c.fwd = dir;
            c.fovDeg = C.aimFov;                        // mesmo FOV
            c.screenMode = false;                       // silent e' sempre 3D
            c.radiusPx = 0.f; c.cx = 0.f; c.cy = 0.f;

            const bool filtrar = (C.aimVisCheck != 0) && SDK::LosReady();

            float bestVis = FLT_MAX, bestBlind = FLT_MAX;
            int   idxVis = -1, idxBlind = -1;

            for (int i = 0; i < g_snapN; ++i) {
                float s = ScoreSnap(g_snap[i], c);      // MESMO score
                if (s < 0.f) continue;

                if (s < bestBlind) { bestBlind = s; idxBlind = i; }

                if (filtrar && !SafeLos(origin, g_snap[i].aim)) continue;
                if (s < bestVis) { bestVis = s; idxVis = i; }
            }

            int escolhido = -1;
            if (filtrar) {
                if (idxVis >= 0) escolhido = idxVis;
                else if (!C.aimStrictVis) escolhido = idxBlind;
            }
            else {
                escolhido = idxBlind;
            }
            if (escolhido < 0) return false;

            return NormSafe(Sub(g_snap[escolhido].aim, origin), out);
        }

        bool Override(Vec3 origin, Vec3& dir)
        {
            if (!C.silentAim && !C.magicBullets) return false;
            if (g_shutting.load()) return false;
            if (!SDK::g_localPlayer) return false;
            if (g_busySilent.exchange(true, std::memory_order_acquire)) return false;

            bool applied = false;
            Vec3 newDir{};
            __try {
                applied = SilentUnsafe(origin, dir, newDir);
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                ++g_crashCount;
                Log("[crash] Aimbot::Silent::Override");
                applied = false;
            }

            if (applied) {
                Vec3 n{};
                if (NormSafe(newDir, n)) dir = n;
                else applied = false;
            }

            g_busySilent.store(false, std::memory_order_release);
            return applied;
        }
    }

    // =========================================================================
    //  init / shutdown
    // =========================================================================
    bool Install()
    {
        Il2CppObject* ct = SDK::CamTfObj();
        bool ok = (ct != nullptr) && (SDK::g_localPlayer != nullptr);

        g_best = nullptr;
        g_sticky = nullptr;
        g_stickyMs = 0;
        g_rotProbed = false;
        g_probePlayer = nullptr;
        g_probeTries = 0;
        g_writePlayerRot = true;
        g_snapN = 0; g_snapMs = 0; g_snapClamp = false;
        g_screenFallbackLogged = false;
        g_fwdFallbackLogged = false;

        Log("[aimbot] install: ok=%d fov=%.1f tecla=0x%02X camTf=%p bone=%d smooth=%.1f "
            "visCheck=%d strict=%d modo=%d rotate=%d",
            (int)ok, C.aimFov, C.aimKey, ct, C.aimBone, C.aimSmooth,
            (int)C.aimVisCheck, (int)C.aimStrictVis, C.aimMode, (int)C.aimRotate);
        return ok;                                  // [Install] nao mente mais
    }

    void Shutdown()
    {
        g_shutting.store(true, std::memory_order_release);

        // espera chamadas em voo (bounded, nunca trava)
        for (int i = 0; i < 500 &&
            (g_busyBest.load() || g_busyTick.load() || g_busySilent.load()); ++i)
            Sleep(0);

        g_best = nullptr;
        g_sticky = nullptr;
        g_stickyMs = 0;
        g_rotProbed = false;
        g_probePlayer = nullptr;
        g_probeTries = 0;
        g_writePlayerRot = true;
        g_snapN = 0;
        g_snapMs = 0;

        g_shutting.store(false, std::memory_order_release);
    }

} // namespace Aimbot