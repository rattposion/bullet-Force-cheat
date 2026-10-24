// ============================================================================
//  triggerbot.cpp
//
//  >>> FIX:
//   1) Sem heap no caminho do tiro (antes: 6 std::vector POR TIRO).
//   2) So atira se a mira estiver EM CIMA do alvo (C.triggerRadius em px).
//      Antes bastava o alvo estar em qualquer lugar do FOV do aimbot.
//   3) Usa SDK::g_aimTarget (alvo unico do frame).
// ============================================================================
#include "features.h"
#include "sdk.h"
#include "hooks.h"
#include "common.h"

#include <windows.h>
#include <cstring>

namespace Triggerbot {

    static ULONGLONG g_next = 0;

    static void Fire() {
        if (!SDK::g_localPlayer) return;

        if (SDK::M_shoot) {
            __try { il2cpp_runtime_invoke(SDK::M_shoot, SDK::g_localPlayer, nullptr, nullptr); }
            __except (EXCEPTION_EXECUTE_HANDLER) { Log("[crash] Triggerbot::Fire(shoot)"); }
            return;
        }
        if (!SDK::M_FireBullet) return;

        uint32_t n = il2cpp_method_get_param_count ? il2cpp_method_get_param_count(SDK::M_FireBullet) : 0;
        if (n == 0) return;
        if (n > 16) n = 16;

        // >>> FIX: buffers no stack, sem alocacao.
        Vec3          v[16]{};
        float         f[16]{};
        int32_t       i[16]{};
        double        d[16]{};
        unsigned char b[16]{};
        void* args[16]{};

        for (uint32_t k = 0; k < n; ++k) {
            const Il2CppType* t = il2cpp_method_get_param ? il2cpp_method_get_param(SDK::M_FireBullet, k) : nullptr;
            const char* tn = (t && il2cpp_type_get_name) ? il2cpp_type_get_name(t) : nullptr;
            if (!tn) continue;
            if (strstr(tn, "Vector3"))      args[k] = &v[k];
            else if (strstr(tn, "Single"))  args[k] = &f[k];
            else if (strstr(tn, "Int32"))   args[k] = &i[k];
            else if (strstr(tn, "Double"))  args[k] = &d[k];
            else if (strstr(tn, "Boolean")) args[k] = &b[k];
        }

        __try { il2cpp_runtime_invoke(SDK::M_FireBullet, SDK::g_localPlayer, args, nullptr); }
        __except (EXCEPTION_EXECUTE_HANDLER) { Log("[crash] Triggerbot::Fire(bullet)"); }
    }

    void Tick() {
        if (!C.triggerbot) return;
        if (!SDK::g_localPlayer) return;
        if (C.triggerKey && !(GetAsyncKeyState(C.triggerKey) & 0x8000)) return;
        if (GetTickCount64() < g_next) return;

        if (!SDK::M_shoot && !SDK::M_FireBullet) {
            g_next = GetTickCount64() + (ULONGLONG)C.triggerDelay;
            return;
        }

        Il2CppObject* t = SDK::g_aimTarget;
        if (!t) return;

        // >>> FIX: so atira se a mira estiver EM CIMA do alvo.
        if (C.triggerRadius > 0.f) {
            Vec3 ap = Aimbot::AimPoint((void*)t);
            if (!ap.zero()) {
                Vec2 s{};
                if (SDK::WorldToScreen(ap, s)) {
                    float dx = s.x - (float)SDK::ScreenW() * 0.5f;
                    float dy = s.y - (float)SDK::ScreenH() * 0.5f;
                    if (sqrtf(dx * dx + dy * dy) > C.triggerRadius) return;
                }
            }
        }

        Fire();
        C.lastHitTime = (float)(GetTickCount64() / 1000.0);
        g_next = GetTickCount64() + (ULONGLONG)C.triggerDelay;
    }

    bool Install() {
        Log("[triggerbot] install: shoot=%p fire=%p raio=%.0fpx",
            SDK::M_shoot, SDK::M_FireBullet, (double)C.triggerRadius);
        return true;
    }

} // namespace Triggerbot