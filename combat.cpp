// ============================================================================
//  combat.cpp — UNICO orquestrador por frame. (v5)
//
//  v5 = v4 + endurecimento:
//    [1] INIT REAL: Hooks::g_sdkReady NAO e' mais setado na mao. Ele so vira
//        true depois de N frames consecutivos com estado minimo valido
//        (local player resolvido + passos criticos sem falha). Camera NAO
//        entra nessa conta — camera e' condicao de passo, nao de init.
//    [2] SEM RETURN POR CAMERA: UpdateView so marca SDK::g_viewProjOk.
//        Nenhum passo e' abortado por causa disso.
//    [3] PROFILER por passo via QPC, com media/last/min/max, escrito sob
//        lock (overlay le em outra thread).
//    [4] FALHA ISOLADA: __try/__except por passo, logando code+addr.
//        Um AV em um passo nao mata o frame nem os passos seguintes.
//    [5] SHUTDOWN LIMPO: g_unload -> espera tick em voo (bounded), limpa
//        alvo, stats e flag de ready, loga o resumo.
//    [6] ANTI-DUPLICIDADE: CAS em g_ticking. Chamada reentrante/concorrente
//        nao roda o frame duas vezes; contabiliza em g_skippedTicks.
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

    static std::atomic<bool> g_ticking{ false };
    static std::atomic<bool> g_unloaded{ false };  // shutdown ja executado

    static const uint32_t kWarmupFrames = 3;          // init real precisa disso

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

    float StepMinMs(int i) {
        if (i < 0 || i >= g_statsN) return 0.f;
        StatsLockShared(); float v = g_statMin[i]; StatsUnlockShared(); return v;
    }
    float StepMaxMs(int i) {
        if (i < 0 || i >= g_statsN) return 0.f;
        StatsLockShared(); float v = g_statMax[i]; StatsUnlockShared(); return v;
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
    static bool S_UpdateView() { SDK::UpdateView();          return true; }  // so marca viewProjOk
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

    static const StepDef kSteps[] = {
        { "Unlock",        S_Unlock        },
        { "UpdateTargets", S_UpdateTargets },  // nao depende da camera
        { "UpdateView",    S_UpdateView    },  // camera opcional aqui
        { "AimbotBest",    S_AimbotBest    },
        { "Misc",          S_Misc          },
        { "Aimbot",        S_Aimbot        },
        { "Triggerbot",    S_Triggerbot    },
        { "Weapons",       S_Weapons       },
        { "Movement",      S_Movement      },
        { "AntiCheat",     S_AntiCheat     },
    };
    static const int kStepsN = (int)(sizeof(kSteps) / sizeof(kSteps[0]));
    static_assert(sizeof(kSteps) / sizeof(kSteps[0]) <= kMaxSteps,
        "kMaxSteps pequeno para a tabela de passos");

    // ------------------------------------------------------- validacao init
    // Condicao REAL de prontidao. Sem atribuicao artificial de g_sdkReady.
    // Camera entra como informacao, nao como bloqueio.
    static bool ValidateCoreState() {
        if (!SDK::g_localPlayer) return false;   // ainda nao resolvido
        return true;
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

            // passos criticos para considerar o SDK "pronto"
            if (i == 0 || i == 1) allCoreOk &= ok;   // Unlock, UpdateTargets
        }

        // [1] prontidao real: N frames consecutivos validos
        if (allCoreOk && ValidateCoreState()) {
            if (g_okFrames < kWarmupFrames) ++g_okFrames;
        }
        else {
            g_okFrames = 0;
        }
        Hooks::g_sdkReady = (g_okFrames >= kWarmupFrames);

        ++g_frame;
        const double frameMs = QpcMs() - t0;
        g_tickMs = g_tickMs * 0.9f + (float)frameMs * 0.1f;

        // heartbeat 1x/s
        const uint64_t now = GetTickCount64();
        if (now - g_lastDbg >= 1000) {
            g_lastDbg = now;
            Log("dbg: alvos=%d local=%p team=%d viewProj=%d ready=%d tick=%.2fms "
                "frame=%u skip=%u crash=%u fase=%s obj=[ac=%d wp=%d]",
                SDK::g_targetCount, SDK::g_localPlayer, SDK::g_localTeam,
                (int)SDK::g_viewProjOk, (int)Hooks::g_sdkReady,
                (double)g_tickMs, g_frame, g_skippedTicks, g_crashCount,
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
        ResetStats();
        Hooks::g_sdkReady = false;   // unico ponto onde mexemos nele fora do tick
    }

} // namespace Combat