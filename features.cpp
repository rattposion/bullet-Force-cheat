// ============================================================================
//  features.cpp — Menu::Render / Aimbot::Bone / Menu_WantsMouse
//  O menu de verdade vive em menu.cpp (Menu::Draw).
//
//  >>> UNICO dono de Menu::Render(). O menu.cpp NAO define Render, senao:
//        LNK2005 "void __cdecl Menu::Render(void)" ja definida em features.obj
//        LNK1169
//
//  >>> FIX (estilo duplicado): Menu::ApplyStyle() vivia AQUI e
//      menu.cpp::ApplyStyleOnce() aplicava o mesmo estilo. Como as duas rodam
//      no mesmo frame apos a criacao de um contexto novo, o estilo era
//      aplicado duas vezes (e os dois 'styledCtx' podiam divergir). Dono unico
//      agora e' menu.cpp, que aplica por dentro do Draw().
// ============================================================================
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cmath>

#include "imgui.h"

#include "il2cpp.h"
#include "il2cpp_api.h"
#include "common.h"
#include "memory.h"
#include "config.h"
#include "sdk.h"
#include "hooks.h"
#include "features.h"

// ---------------------------------------------------------------------------
//  Input — o hook de cursor e o WndProc consultam isto antes de deixar o mouse
//  pro jogo.
// ---------------------------------------------------------------------------
bool Menu_WantsMouse() {
    if (!C.menuOpen)                 return false;
    if (!Hooks::g_imguiReady.load()) return false;
    if (!ImGui::GetCurrentContext()) return false;
    return ImGui::GetIO().WantCaptureMouse;
}

namespace Menu {

    // Ponto de entrada chamado pelo hk_wglSwapBuffers.
    // O menu.cpp define Draw() e aplica o estilo proprio dele.
    void Render() {
        ImGuiContext* ctx = ImGui::GetCurrentContext();
        if (!ctx || !Hooks::g_imguiReady.load()) return;

        Draw();                        // menu.cpp (aplica o estilo por dentro)
    }

} // namespace Menu

// ===========================================================================
//  Aimbot::Bone — unico simbolo de Aimbot que vive aqui
// ===========================================================================
namespace Aimbot {

    void Bone(Il2CppObject* p, Il2CppObject* target) {
        if (!p || !target || !SDK::O_rotation || !SDK::O_rotationIsQuat) return;

        Vec3 me = SDK::GetPos(p);
        if (!me.isSane()) return;

        // TargetAimPos devolve Vec3() quando nao resolve o bone, mas (0,0,0)
        // tambem e um ponto VALIDO. Por isso as duas checagens: isSane() pega
        // NaN/Inf/valores absurdos, zero() pega o sentinela de falha.
        Vec3 tp = SDK::TargetAimPos(target, C.aimBone);
        if (!tp.isSane() || tp.zero())
            tp = SDK::GetPos(target);
        if (!tp.isSane()) return;

        Vec3 dir = tp - me;
        const float L = dir.len();
        if (L < 0.0001f) return;

        ::Quat q = SDK::LookRot(dir * (1.f / L));

        // SetPlayerRot trata o caso do campo ser SyncVar<Quaternion>
        SDK::SetPlayerRot(p, q);
    }

} // namespace Aimbot