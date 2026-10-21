// ============================================================================
//  combat.cpp — UNICO orquestrador por frame. (v6)
//
//  v6 = v5 + correcao do ESP piscando/sumindo:
//    [A] ORDEM: UpdateView roda ANTES de UpdateTargets. Antes o onScreen era
//        calculado com a matriz do frame ANTERIOR e um frame sem camera
//        deixava todo o snapshot com onScreen=false.
//    [B] Passo critico identificado por NOME (IsCoreStep), nao por indice.
//    [C] PRONTIDAO LATCHED: exige kBadTolerance frames ruins CONSECUTIVOS
//        para derrubar. Antes, UM frame com excecao capturada zerava
//        g_okFrames e o render ficava >=3 frames sem desenhar -> piscada
//        sincronizada com spawn/morte/excecao.
//    [D] O local player deixou de ser condicao de prontidao. Ele e'
//        telemetria (SDK::g_localValid): estar morto/respawnando nao
//        significa que a partida parou nem que os alvos sao invalidos.
//
//  v5 = v4 + endurecimento (preservado):
//    [1] INIT REAL: Hooks::g_sdkReady NAO e' setado na mao.
//    [2] SEM RETURN POR CAMERA: UpdateView so marca SDK::g_viewProjOk.
//    [3] PROFILER por passo via QPC, com media/last/min/max, escrito sob lock.
//    [4] FALHA ISOLADA: __try/__except por passo, logando code+addr.
//    [5] SHUTDOWN LIMPO: g_unload -> espera tick em voo (bounded), limpa
//        alvo, stats e flag de ready, loga o resumo.
//    [6] ANTI-DUPLICIDADE: CAS em g_ticking.
// ============================================================================
#include "features.h"
#include "sdk.h"
#include "hooks.h"
#include "common.h"

#include <atomic>
#include <string.h>

extern "C" int bfi_anticheat_obj;
extern "C" int bfi_weapons_obj;

namespace Combat {

    // ---------------------------------------------------------------- estado
    static uint64_t g_lastDbg = 0;
    static uint32_t g_frame = 0;
    static uint32_t g_okFrames = 0;   // frames consecutivos "validos"
    static uint32_t g_skippedTicks = 0;   // chamadas descartadas pelo guard
    static uint32_t g_crashCount = 0;   // total de excecoes capturadas
    static uint32_t g_badStreak = 0;   // frames ruins CONSECUTIVOS

    static std::atomic<bool> g_ticking{ false };
    static std::atomic<bool> g_unloaded{ false };  // shutdown ja executado

    // Frames bons necessarios para PUBLICAR o ready.
    static const uint32_t kWarmupFrames = 3;
    // Frames ruins CONSECUTIVOS necessarios para DERRUBAR o ready.
    // Com 1, qualquer excecao capturada apagava o ESP por 3 frames.
    static const uint32_t kBadTolerance = 8;

    StepStat g_stats[kMaxSteps];
    int      g_statsN = 0;
    float    g_tickMs = 0.f;

    // min/max em arrays paralelos -> nao exige alterar a struct do header
    static float g_statMin[kMaxSteps];
    static float g_statMax[kMaxSteps];

    // lock das estatisticas (escrita no tick, leitura no overlay)
    static SRWLOCK g_statsLock = SRWLOCK_INIT;

    static void StatsLock() { AcquireSRWLockExclusive(&g_statsLock); }
    static void StatsUnlock() { ReleaseSRWLockExclusive(&g_statsLock); }
    static void StatsLockShared() { AcquireSRWLockShared(&g_statsLock); }
    static void StatsUnlockShared() { ReleaseSRWLockShared(&g_statsLock); }

    // ------------------------------------------------------------- relogio
    static double QpcMs() {
        static double inv = 0.0;
        if (inv == 0.0) {                    // escrita idempotente, benigna
            LARGE_INTEGER f; QueryPerformanceFrequency(&f);
            inv = (f.QuadPart > 0) ? (1000.0 / (double)f.QuadPart) : 0.0;
        }
        LARGE_INTEGER c; QueryPerformanceCounter(&c);
        return (double)c.QuadPart * inv;
    }

    // --------------------------------------------------------------- stats
    static void Stat(const char* nome, double ms) {
        StatsLock();
        for (int i = 0; i < g_statsN; ++i) {
            if (strcmp(g_stats[i].name, nome) != 0) continue;
            g_stats[i].ms = g_stats[i].ms * 0.90f + (float)ms * 0.10f;
            g_stats[i].last = (float)ms;
            if (g_statMin[i] == 0.f || ms < g_statMin[i]) g_statMin[i] = (float)ms;
            if (ms > g_statMax[i]) g_statMax[i] = (float)ms;
            StatsUnlock();
            return;
        }
        if (g_statsN < kMaxSteps) {
            StepStat& s = g_stats[g_statsN];
            strncpy_s(s.name, nome, _TRUNCATE);
            s.ms = s.last = (float)ms;
            g_statMin[g_statsN] = (float)ms;
            g_statMax[g_statsN] = (float)ms;
            ++g_statsN;
        }
        StatsUnlock();
    }

    // leitura segura para o overlay (nao segure o lock enquanto desenha)
    int CopyStats(StepStat* out, int maxN) {
        if (!out || maxN <= 0) return 0;
        StatsLockShared();
        int n = (g_statsN < maxN) ? g_statsN : maxN;
        if (n > 0) memcpy(out, g_stats, sizeof(StepStat) * (size_t)n);
        StatsUnlockShared();
        return n;
    }

    void ResetStats() {
        StatsLock();
        g_statsN = 0;
        memset(g_stats, 0, sizeof(g_stats));
        memset(g_statMin, 0, sizeof(g_statMin));
        memset(g_statMax, 0, sizeof(g_statMax));
        StatsUnlock();
        g_tickMs = 0.f;
    }

    // -------------------------------------------------------------- crash
    // Filtro roda com a pilha ainda "suja" e ANTES do unwind: capture aqui,
    // logue depois. GetExceptionInformation() so e' valido no filtro.
    static bool RunStep(const char* nome, bool (*fn)(), double* outMs) {
        const double t0 = QpcMs();
        bool  ok = true;
        DWORD code = 0;
        void* addr = nullptr;

        __try {
            ok = fn();
        }
        __except ((code = GetExceptionCode(),
            addr = GetExceptionInformation()->ExceptionRecord->ExceptionAddress,
            EXCEPTION_EXECUTE_HANDLER)) {
            ok = false;
        }

        const double dt = QpcMs() - t0;
        if (!ok) {
            ++g_crashCount;
            Log("[crash] passo=%s code=0x%08X addr=%p dt=%.3fms", nome, code, addr, dt);
        }
        if (outMs) *outMs = dt;
        return ok;
    }

    // ----------------------------------------------------------- wrappers
    // Assinatura uniforme bool(). Se a funcao original nao devolve nada,
    // retornamos true (executou) — falha real vem do __except, nao do retorno.
    static bool S_Unlock() { Unlock::Tick();             return true; }
    static bool S_UpdateTargets() { SDK::UpdateTargets();       return true; }
    static bool S_UpdateView() { SDK::UpdateView();          return true; }
    static bool S_AimbotBest() {
        SDK::g_aimTarget = (C.aimbot || C.triggerbot) ? Aimbot::Best() : nullptr;
        return true;
    }
    static bool S_Misc() { Misc::Tick();               return true; }
    static bool S_Aimbot() { Aimbot::Tick();             return true; }
    static bool S_Triggerbot() { Triggerbot::Tick();         return true; }
    static bool S_Weapons() { Weapons::Tick();            return true; }
    static bool S_Movement() { Movement::Tick();           return true; }
    static bool S_AntiCheat() { AntiCheat::Tick();          return true; }

    // Tabela unica: execucao e profiler saem da mesma fonte.
    struct StepDef { const char* nome; bool (*fn)(); };

    // >>> [A] A ORDEM IMPORTA.
    //     UpdateView publica a matriz view*proj do frame ATUAL. UpdateTargets
    //     precisa dela para calcular onScreen. Com a ordem antiga
    //     (UpdateTargets antes), o onScreen saia com a matriz do frame
    //     anterior e um frame sem camera deixava o snapshot inteiro em
    //     onScreen=false -> ESP vazio.
    static const StepDef kSteps[] = {
        { "Unlock",        S_Unlock },
        { "UpdateView",    S_UpdateView },  // camera/viewProj PRIMEIRO
        { "UpdateTargets", S_UpdateTargets },  // usa a matriz DESTE frame
        { "AimbotBest",    S_AimbotBest },
        { "Misc",          S_Misc },
        { "Aimbot",        S_Aimbot },
        { "Triggerbot",    S_Triggerbot },
        { "Weapons",       S_Weapons },
        { "Movement",      S_Movement },
        { "AntiCheat",     S_AntiCheat },
    };
    static const int kStepsN = (int)(sizeof(kSteps) / sizeof(kSteps[0]));
    static_assert(sizeof(kSteps) / sizeof(kSteps[0]) <= kMaxSteps,
        "kMaxSteps pequeno para a tabela de passos");

    // >>> [B] Passo critico por NOME: reordenar kSteps nao muda a semantica.
    static bool IsCoreStep(const char* nome) {
        return strcmp(nome, "Unlock") == 0 || strcmp(nome, "UpdateTargets") == 0;
    }

    // ------------------------------------------------------------ shutdown
    static void ShutdownOnce() {
        if (g_unloaded.exchange(true)) return;

        // espera um tick eventualmente em voo (bounded, nunca trava)
        for (int i = 0; i < 500 && g_ticking.load(std::memory_order_acquire); ++i)
            Sleep(0);

        Hooks::g_sdkReady = false;
        SDK::g_aimTarget = nullptr;
        ResetStats();
        g_okFrames = 0;
        g_badStreak = 0;

        Log("combat: shutdown limpo frames=%u skipped=%u crashes=%u",
            g_frame, g_skippedTicks, g_crashCount);
    }

    void Shutdown() { ShutdownOnce(); }

    // ---------------------------------------------------------------- tick
    void Tick() {
        // [4] unload tem prioridade e nao depende de nada abaixo
        if (g_unload.load(std::memory_order_acquire)) {
            ShutdownOnce();
            return;
        }

        // [6] guard anti-duplicidade / reentrancia
        bool expected = false;
        if (!g_ticking.compare_exchange_strong(expected, true,
            std::memory_order_acq_rel)) {
            ++g_skippedTicks;
            return;
        }

        const double t0 = QpcMs();
        PHASE("Combat::Tick");

        bool allCoreOk = true;
        for (int i = 0; i < kStepsN; ++i) {
            PHASE(kSteps[i].nome);          // fase corrente para o dump
            double dt = 0.0;
            const bool ok = RunStep(kSteps[i].nome, kSteps[i].fn, &dt);
            Stat(kSteps[i].nome, dt);       // [3] sempre mede, mesmo em falha

            if (IsCoreStep(kSteps[i].nome)) allCoreOk &= ok;
        }

        // =============================================================
        //  >>> [C] PRONTIDAO LATCHED.
        //  Liga: kWarmupFrames frames bons consecutivos.
        //  Desliga: kBadTolerance frames ruins CONSECUTIVOS.
        //  Com o reset imediato antigo, UMA excecao capturada zerava
        //  g_okFrames e o render ficava >=3 frames sem desenhar.
        //
        //  >>> [D] O local player NAO entra mais nesta conta. Estar
        //  morto/respawnando nao significa que a partida parou nem que
        //  os alvos sao invalidos. A validade dele e' telemetria
        //  (SDK::g_localValid).
        // =============================================================
        if (allCoreOk) {
            g_badStreak = 0;
            if (g_okFrames < kWarmupFrames) ++g_okFrames;
        }
        else if (g_badStreak < 1000000u) {
            ++g_badStreak;
            if (g_badStreak >= kBadTolerance) g_okFrames = 0;
        }
        Hooks::g_sdkReady = (g_okFrames >= kWarmupFrames);

        ++g_frame;
        const double frameMs = QpcMs() - t0;
        g_tickMs = g_tickMs * 0.9f + (float)frameMs * 0.1f;

        // heartbeat 1x/s
        const uint64_t now = GetTickCount64();
        if (now - g_lastDbg >= 1000) {
            g_lastDbg = now;
            Log("dbg: alvos=%d local=%p localValid=%d team=%d viewProj=%d ready=%d "
                "bad=%u tick=%.2fms frame=%u skip=%u crash=%u fase=%s obj=[ac=%d wp=%d]",
                SDK::g_targetCount, SDK::g_localPlayer,
                (int)SDK::g_localValid.load(std::memory_order_relaxed),
                SDK::g_localTeam, (int)SDK::g_viewProjOk, (int)Hooks::g_sdkReady,
                g_badStreak, (double)g_tickMs, g_frame, g_skippedTicks, g_crashCount,
                g_phase ? g_phase : "?", bfi_anticheat_obj, bfi_weapons_obj);
        }

        g_ticking.store(false, std::memory_order_release);
    }

    // chamar no attach da DLL, antes do primeiro Tick
    void Init() {
        g_unloaded.store(false);
        g_ticking.store(false);
        g_lastDbg = GetTickCount64();
        g_frame = g_okFrames = g_skippedTicks = g_crashCount = 0;
        g_badStreak = 0;
        ResetStats();
        Hooks::g_sdkReady = false;   // unico ponto onde mexemos nele fora do tick
    }

} // namespace Combat