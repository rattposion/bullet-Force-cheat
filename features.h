#pragma once
// ============================================================================
//  features.h — declaracoes publicas compartilhadas.
//  Cada simbolo tem UM dono. Nada e' definido aqui (exceto inlines triviais).
//
//  >>> v3
//    * Consistente com o hooks.cpp reconstruido (Reinstall, GetViewSize,
//      LogWndProcInfo sem param, SafeFont/FontCount, ViewW/ViewH).
//    * CONTRATO DE DONOS (defina em UM unico .cpp):
//        Menu::Draw / Menu::ApplyStyle   -> menu.cpp
//        Menu::Render                    -> features.cpp
//        Menu_WantsMouse / Aimbot::Bone  -> features.cpp
//        ESP::Tick/Draw/Latest/Reset     -> esp.cpp
//        Combat::Tick/Init/Shutdown/...  -> combat.cpp
//        Aimbot::Silent::Override        -> aimbot.cpp
//      g_open e' ALIAS de C.menuOpen: definir em menu.cpp como
//        bool& Menu::g_open = C.menuOpen;
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

// Numero de slots de anexo guardados no config (C.selAttach). Mantenha em
// sincronia com o tamanho do array no config.h.
constexpr int kMaxAttach = 3;

bool Menu_WantsMouse();          // consultada pelo hook de cursor

void DrawMenu();                 // alias de Menu::Draw()
void DrawOverlay();              // alias de Menu::Draw()

namespace Menu {
    extern bool& g_open;         // alias de C.menuOpen — mesma celula (menu.cpp)
    void ApplyStyle();           // dono: menu.cpp
    void Draw();                 // dono: menu.cpp
    void Render();               // dono: features.cpp
}

// ============================================================================
//  ESP
//  Tick() roda no thread do JOGO (dentro do Combat::Tick, depois de
//  UpdateTargets/UpdateView) e publica o Snap. Draw() roda no thread de render
//  e SO' consome Latest() — zero lock, zero chamada de SDK.
// ============================================================================
namespace ESP {
    constexpr int kMaxEspEnts = 64;
    constexpr int kGraceFrames = 12;   // evita piscar com frame vazio isolado

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
//  Stats escritas sob SRWLOCK. Leitura do overlay via CopyStats().
// ============================================================================
namespace Combat {
    constexpr int kMaxSteps = 16;

    struct StepStat {
        char  name[24]{};
        float ms = 0.0f;
        float last = 0.0f;
    };

    extern StepStat g_stats[kMaxSteps];
    extern int       g_statsN;            // sempre sob lock
    extern float     g_tickMs;
    extern std::atomic<bool> g_dataOk;    // estado transitorio (nao e' prontidao)

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
//  NAO declare 'struct Quat' aqui dentro: cria Aimbot::Quat, tipo distinto de
//  ::Quat, e quebra toda chamada de SDK que recebe Quat&.
// ============================================================================
namespace Aimbot {
    Il2CppObject* Best();
    bool  Install();
    void  Tick();
    void  Shutdown();
    void  Bone(Il2CppObject* p, Il2CppObject* target);

    Vec3  Sub(Vec3, Vec3);
    float Dot(Vec3, Vec3);
    Vec3  Norm(Vec3);
    ::Quat LookRot(Vec3);
    ::Quat Slerp(::Quat, ::Quat, float);
    Vec3  AimPoint(void*);
    Vec3  LocalPos();
    Vec3  CamForward();

    // Distancia entre dois pontos. A definicao vive fora daqui.
    float Dist3D(Vec3 a, Vec3 b);

    // Comprimento de um vetor. Inline: nao depende de link.
    // ATENCAO: dentro de namespace Aimbot isto esconde a versao global de
    // 2 argumentos — chame ::Dist3D(a, b) se precisar da global.
    inline float Dist3D(Vec3 v) {
        return sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
    }

    // silent aim — chamada no hook do FireBullet (hooks.cpp)
    // origin = posicao do cano (por valor), dir = direcao do tiro (por ref)
    namespace Silent { bool Override(Vec3 origin, Vec3& dir); }
}

namespace Recoil { void Install(); void Tick(); }
namespace Triggerbot { bool Install(); void Tick(); }
namespace AntiCheat { bool Install(); void Tick(); }