// ============================================================================
//  combat.cpp — UNICO orquestrador por frame. (v11)
// ============================================================================
#include <windows.h>

#include "features.h"
#include "sdk.h"
#include "hooks.h"
#include "common.h"

#include <atomic>
#include <string.h>

namespace Combat {

    // ---------------------------------------------------------------- estado
    static uint64_t g_lastDbg = 0;
    static uint32_t g_frame = 0;
    static uint32_t g_okFrames = 0;
    static uint32_t g_skippedTicks = 0;
    static uint32_t g_crashCount = 0;

    static std::atomic<bool> g_ticking{ false };
    static std::atomic<bool> g_unloaded{ false };
    static bool              g_ready = false;

    static const uint32_t kWarmupFrames = 3;

    StepStat g_stats[kMaxSteps];
    int      g_statsN = 0;
    float    g_tickMs = 0.f;
    std::atomic<bool> g_dataOk{ false };

    static float g_statMin[kMaxSteps];
    static float g_statMax[kMaxSteps];

    static SRWLOCK g_statsLock = SRWLOCK_INIT;

    _Acquires_lock_(g_statsLock)
        static void StatsLock() { AcquireSRWLockExclusive(&g_statsLock); }
    _Releases_lock_(g_statsLock)
        static void StatsUnlock() { ReleaseSRWLockExclusive(&g_statsLock); }
    _Acquires_lock_(g_statsLock)
        static void StatsLockShared() { AcquireSRWLockShared(&g_statsLock); }
    _Releases_lock_(g_statsLock)
        static void StatsUnlockShared() { ReleaseSRWLockShared(&g_statsLock); }

    // ------------------------------------------------------------- relogio
    static double QpcMs() {
        static double inv = 0.0;
        static bool   inited = false;
        if (!inited) {
            LARGE_INTEGER f{};
            QueryPerformanceFrequency(&f);
            inv = (f.QuadPart > 0) ? (1000.0 / (double)f.QuadPart) : 1.0;
            inited = true;
        }
        LARGE_INTEGER c{};
        QueryPerformanceCounter(&c);
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

    int CopyStats(StepStat* out, int maxN) {
        if (!out || maxN <= 0) return 0;
        StatsLockShared();
        const int n = (g_statsN < maxN) ? g_statsN : maxN;
        if (n > 0) memcpy(out, g_stats, sizeof(StepStat) * (size_t)n);
        StatsUnlockShared();
        return n;
    }

    float StepMinMs(int i) {
        StatsLockShared();
        const float v = (i >= 0 && i < g_statsN) ? g_statMin[i] : 0.f;
        StatsUnlockShared();
        return v;
    }
    float StepMaxMs(int i) {
        StatsLockShared();
        const float v = (i >= 0 && i < g_statsN) ? g_statMax[i] : 0.f;
        StatsUnlockShared();
        return v;
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
    static bool RunStep(const char* nome, bool (*fn)(), double* outMs) {
        const double t0 = QpcMs();
        bool  ok = true;
        DWORD code = 0;
        void* addr = nullptr;

        __try { ok = fn(); }
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
    static bool S_Unlock() { Unlock::Tick();            return true; }
    static bool S_UpdateTargets() { SDK::UpdateTargets();      return true; }
    static bool S_UpdateView() { SDK::UpdateView();         return true; }
    static bool S_Cursor() { SDK::RefreshCursorState(); return true; }
    static bool S_ESP() { ESP::Tick();               return true; }

    static bool S_AimbotBest() {
        const bool querAlvo = C.aimbot || C.triggerbot || C.magicBullets || C.silentAim;
        SDK::g_aimTarget = querAlvo ? Aimbot::Best() : nullptr;
        return true;
    }

    static bool S_Misc() { Misc::Tick();         return true; }
    static bool S_Aimbot() { Aimbot::Tick();       return true; }
    static bool S_Triggerbot() { Triggerbot::Tick();   return true; }
    static bool S_Weapons() { Weapons::Tick();      return true; }
    static bool S_Movement() { Movement::Tick();     return true; }
    static bool S_AntiCheat() { AntiCheat::Tick();    return true; }

    struct StepDef { const char* nome; bool (*fn)(); bool critico; int everyN; };

    static const StepDef kSteps[] = {
        { "Unlock",        S_Unlock,        false, 4 },
        { "UpdateTargets", S_UpdateTargets, true,  1 },
        { "UpdateView",    S_UpdateView,    false, 1 },
        { "AimbotBest",    S_AimbotBest,    false, 1 },
        { "Cursor",        S_Cursor,        false, 4 },
        { "ESP",           S_ESP,           false, 1 },
        { "Misc",          S_Misc,          false, 2 },
        { "Aimbot",        S_Aimbot,        false, 1 },
        { "Triggerbot",    S_Triggerbot,    false, 1 },
        { "Weapons",       S_Weapons,       false, 4 },
        { "Movement",      S_Movement,      false, 1 },
        { "AntiCheat",     S_AntiCheat,     false, 8 },
    };
    static const int kStepsN = (int)(sizeof(kSteps) / sizeof(kSteps[0]));
    static_assert(sizeof(kSteps) / sizeof(kSteps[0]) <= kMaxSteps,
        "kMaxSteps pequeno para a tabela de passos");

    static bool ValidateCoreState() { return SDK::g_localPlayer != nullptr; }

    // ------------------------------------------------------------ shutdown
    static void ShutdownOnce() {
        if (g_unloaded.exchange(true)) return;
        for (int i = 0; i < 500 && g_ticking.load(std::memory_order_acquire); ++i)
            Sleep(0);

        g_ready = false;
        Hooks::g_sdkReady.store(false, std::memory_order_release);
        g_dataOk.store(false, std::memory_order_release);
        SDK::g_aimTarget = nullptr;
        ResetStats();
        g_okFrames = 0;

        Log("combat: shutdown limpo frames=%u skipped=%u crashes=%u",
            g_frame, g_skippedTicks, g_crashCount);
    }

    void Shutdown() { ShutdownOnce(); }

    // ---------------------------------------------------------------- tick
    void Tick() {
        if (g_unload.load(std::memory_order_acquire)) { ShutdownOnce(); return; }

        bool expected = false;
        if (!g_ticking.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            ++g_skippedTicks;
            return;
        }

        const double t0 = QpcMs();
        PHASE("Combat::Tick");

        struct Hot { const char* n; double ms; } hot[3] = { { nullptr,0.0 },{ nullptr,0.0 },{ nullptr,0.0 } };
        auto pushHot = [&](const char* n, double ms) {
            if (ms <= hot[2].ms) return;
            hot[2] = { n, ms };
            if (hot[2].ms > hot[1].ms) { Hot t = hot[1]; hot[1] = hot[2]; hot[2] = t; }
            if (hot[1].ms > hot[0].ms) { Hot t = hot[0]; hot[0] = hot[1]; hot[1] = t; }
            };

        int ran = 0;
        bool allCoreOk = true;
        for (int i = 0; i < kStepsN; ++i) {
            const int n = kSteps[i].everyN;
            if (n > 1 && ((g_frame + (uint32_t)i) % (uint32_t)n) != 0) continue;

            ++ran;
            PHASE(kSteps[i].nome);
            double dt = 0.0;
            const bool ok = RunStep(kSteps[i].nome, kSteps[i].fn, &dt);
            Stat(kSteps[i].nome, dt);
            pushHot(kSteps[i].nome, dt);
            if (kSteps[i].critico) allCoreOk &= ok;
        }

        const bool dadosOk = allCoreOk && ValidateCoreState();
        if (dadosOk && g_okFrames < kWarmupFrames) ++g_okFrames;
        if (g_okFrames >= kWarmupFrames) g_ready = true;

        Hooks::g_sdkReady.store(g_ready, std::memory_order_release);
        g_dataOk.store(dadosOk, std::memory_order_release);

        ++g_frame;
        const double frameMs = QpcMs() - t0;
        g_tickMs = g_tickMs * 0.9f + (float)frameMs * 0.1f;

        const uint64_t now = GetTickCount64();
        if (now - g_lastDbg >= 1000) {
            g_lastDbg = now;
            Log("dbg: alvos=%d local=%p team=%d ready=%d data=%d cursor=%d "
                "tick=%.2fms frame=%u passos=%d/%d skip=%u crash=%u top=%s:%.2f %s:%.2f %s:%.2f",
                SDK::g_targetCount, SDK::g_localPlayer, SDK::g_localTeam,
                (int)g_ready, (int)dadosOk,
                SDK::g_cursorState.load(std::memory_order_relaxed),
                (double)g_tickMs, g_frame, ran, kStepsN, g_skippedTicks, g_crashCount,
                hot[0].n ? hot[0].n : "-", hot[0].ms,
                hot[1].n ? hot[1].n : "-", hot[1].ms,
                hot[2].n ? hot[2].n : "-", hot[2].ms);
        }

        g_ticking.store(false, std::memory_order_release);
    }

    void Init() {
        g_unloaded.store(false);
        g_ticking.store(false);
        g_ready = false;
        g_lastDbg = GetTickCount64();
        g_frame = g_okFrames = g_skippedTicks = g_crashCount = 0;
        ResetStats();
        Hooks::g_sdkReady.store(false, std::memory_order_release);
        g_dataOk.store(false, std::memory_order_release);
    }

} // namespace Combat