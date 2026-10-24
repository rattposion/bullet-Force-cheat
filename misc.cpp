#include "features.h"
#include "sdk.h"
#include "hooks.h"
#include "common.h"

#include <windows.h>
#include <cmath>

namespace Misc {

    void Panic() {
        C.esp = false;
        C.aimbot = false;
        C.silentAim = false;
        C.magicBullets = false;
        C.triggerbot = false;
        C.godmode = false;
        C.speedHack = false;
        C.fly = false;
        C.superJump = false;
        C.teleport = false;
        C.crosshair = false;
        C.noRecoil = false;
        C.noSpread = false;
        C.infAmmo = false;
        C.noReload = false;
        C.rapidFire = false;
        C.bhop = false;
        C.shootInAir = false;
        C.hitmarker = false;
        Log("PANIC: tudo desligado");
    }

    // --------------------------------------------------------------- godmode
    static void GodMode() {
        if (!C.godmode) return;
        Il2CppObject* p = SDK::g_localPlayer;
        if (!p) return;

        const float want = (C.godmodeValue > 0.f) ? C.godmodeValue : 100.f;
        const float hp = SDK::GetHealth(p);
        if (hp < want - 0.5f)
            SDK::SetHealth(p, want);
    }

    void Tick() {
        // godmode primeiro: nao depende do FOV da camera
        __try { GodMode(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { Log("[crash] Misc::GodMode"); }

        static float last = -1.f;
        if (C.camFov <= 0.f) { last = -1.f; return; }
        if (C.camFov == last) return;
        if (!SDK::M_set_fov) return;

        Il2CppObject* cam = SDK::CameraObj();
        if (!cam) return;

        last = C.camFov;
        float v = C.camFov;
        void* a[1];
        a[0] = &v;
        __try { il2cpp_runtime_invoke(SDK::M_set_fov, cam, a, nullptr); }
        __except (EXCEPTION_EXECUTE_HANDLER) { Log("[crash] Misc::set_fov"); }
    }

} // namespace Misc