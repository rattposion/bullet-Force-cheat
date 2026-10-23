#pragma once
// ============================================================================
//  features.h — declaracoes publicas compartilhadas.
//  Cada simbolo tem UM dono. Nada e' definido aqui (exceto inlines triviais).
// ============================================================================
#include <atomic>
#include <cstdint>
#include <cmath>

#include "il2cpp.h"
#include "il2cpp_api.h"
#include "common.h"
#include "memory.h"
#include "hooks.h"
#include "sdk.h"
#include "config.h"
#include "unlock.h"

// ---------------------------------------------------------------------------
extern std::atomic<bool> g_unload;

constexpr int kMaxAttach = 3;

bool Menu_WantsMouse();

void DrawMenu();
void DrawOverlay();

namespace Menu {
    extern bool& g_open;
    void ApplyStyle();
    void Draw();
    void Render();
}

// ============================================================================
//  ESP
// ============================================================================
namespace ESP {
    constexpr int kMaxEspEnts = 64;
    constexpr int kGraceFrames = 12;

    struct Snap {
        uint64_t frame = 0;
        int      w = 0;
        int      h = 0;
        int      count = 0;

        struct Ent {
            Vec2  head{};
            Vec2  feet{};
            float dist = 0.0f;
            float health = 0.0f;
            int   team = 0;
            int   weapon = 0;
            bool  visible = false;
            bool  aim = false;
            bool  npc = false;
            char  name[24]{};
        } ents[kMaxEspEnts];
    };

    void        Tick();
    const Snap* Latest();
    void        Draw();
    int         Count();
    int         Invisiveis();
    void        Reset();
}
namespace Misc { void Panic(); void Draw(); void Tick(); }

// ============================================================================
//  COMBAT + PROFILER
// ============================================================================
namespace Combat {
    constexpr int kMaxSteps = 16;

    struct StepStat {
        char  name[24]{};
        float ms = 0.0f;
        float last = 0.0f;
    };

    extern StepStat g_stats[kMaxSteps];
    extern int       g_statsN;
    extern float     g_tickMs;
    extern std::atomic<bool> g_dataOk;

    void  Tick();
    void  Init();
    void  Shutdown();
    int   CopyStats(StepStat* out, int maxN);
    float StepMinMs(int i);
    float StepMaxMs(int i);
    void  ResetStats();
}

namespace Weapons { void Init(); void Tick(); }
namespace Movement { void Tick(); }

// ============================================================================
//  AIMBOT
// ============================================================================
namespace Aimbot {
    Il2CppObject* Best();
    bool  Install();
    void  Tick();
    void  Shutdown();
    void  Bone(Il2CppObject* p, Il2CppObject* target);

    bool  IsActive();

    Vec3  Sub(Vec3, Vec3);
    float Dot(Vec3, Vec3);
    Vec3  Norm(Vec3);
    ::Quat LookRot(Vec3);
    ::Quat Slerp(::Quat, ::Quat, float);
    Vec3  AimPoint(void*);
    Vec3  LocalPos();
    Vec3  CamForward();

    float Dist3D(Vec3 a, Vec3 b);

    inline float Dist3D(Vec3 v) {
        return sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
    }

    namespace Silent { bool Override(Vec3 origin, Vec3& dir); }
}

namespace Recoil { void Install(); void Tick(); }
namespace Triggerbot { bool Install(); void Tick(); }

namespace AntiCheat {
    bool Install();
    void Tick();

    // ---- alerta de risco de ban / desconexao ----
    int  RiskScore();                    // pontuacao bruta
    int  RiskLevel();                    // 0=baixo 1=medio 2=alto 3=critico
    const char* RiskText();              // "BAIXO"/"MEDIO"/"ALTO"/"CRITICO"
    void RiskReasons(char* out, int n);  // lista dos itens que somam risco
}