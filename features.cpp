// ============================================================================
//  features.cpp — Menu::Render / Aimbot::Bone / Menu_WantsMouse.
//  O menu de verdade vive em menu.cpp (Menu::Draw). O ESTILO tambem.
//
//  v6: sem mudanca de logica. Mantido integralmente o fix v5 (heal da pilha
//      do ImGui depois do Draw) e o __try do Menu::Render.
// ============================================================================
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <atomic>

#include "imgui.h"
#include "imgui_internal.h"

#include "il2cpp.h"
#include "il2cpp_api.h"
#include "common.h"
#include "memory.h"
#include "config.h"
#include "sdk.h"
#include "hooks.h"
#include "features.h"

bool Menu_WantsMouse() {
    if (!C.menuOpen)                 return false;
    if (!Hooks::g_imguiReady.load()) return false;
    if (!ImGui::GetCurrentContext()) return false;
    return ImGui::GetIO().WantCaptureMouse;
}

namespace Menu {

    static int s_winBase = 1;
    static int s_tabBase = 0;

    void CaptureStacks() {
        ImGuiContext* g = ImGui::GetCurrentContext();
        if (!g) return;
        s_winBase = g->CurrentWindowStack.Size;
        s_tabBase = g->CurrentTabBarStack.Size;
    }

    void HealImGuiStacks() {
        ImGuiContext* g = ImGui::GetCurrentContext();
        if (!g) return;

        int guard = 0;
        while (g->CurrentTabBarStack.Size > s_tabBase && ++guard < 64)
            ImGui::EndTabBar();

        guard = 0;
        while (g->CurrentWindowStack.Size > s_winBase && ++guard < 64)
            ImGui::End();
    }

    void Render() {
        if (!ImGui::GetCurrentContext()) return;
        if (!Hooks::g_imguiReady.load())    return;

        CaptureStacks();

        __try { Draw(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { Log("[crash] Menu::Draw"); }

        __try { HealImGuiStacks(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { Log("[crash] Menu::HealImGuiStacks"); }
    }

} // namespace Menu

namespace Aimbot {

    void Bone(Il2CppObject* p, Il2CppObject* target) {
        if (!p || !target || !SDK::O_rotation || !SDK::O_rotationIsQuat) return;

        __try {
            Vec3 me = SDK::GetPos(p);
            if (!me.isSane()) return;

            Vec3 tp = SDK::TargetAimPos(target, C.aimBone);
            if (!tp.isSane() || tp.zero())
                tp = SDK::GetPos(target);
            if (!tp.isSane()) return;

            Vec3 dir = tp - me;
            const float L = dir.len();
            if (L < 0.0001f) return;

            ::Quat q = SDK::LookRot(dir * (1.f / L));

            Il2CppObject* camTf = SDK::CamTfObj();
            if (camTf) SDK::SetRot(camTf, q);
            SDK::SetPlayerRot(p, q);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { Log("[crash] Aimbot::Bone"); }
    }

} // namespace Aimbot