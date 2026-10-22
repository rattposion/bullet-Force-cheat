// ============================================================================
//  features.cpp — Menu::Render / Aimbot::Bone / Menu_WantsMouse.
//  O menu de verdade vive em menu.cpp (Menu::Draw). O ESTILO tambem: dono
//  unico e' menu.cpp::ApplyStyle. Aqui nao existe mais ApplyStyle.
//
//  v3: Menu::Render blindado (__try) — um AV no Draw do menu nao sobe pro
//      frame. O hooks.cpp tambem ja envolve a chamada; isto e' 2a camada.
// ============================================================================
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <atomic>

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
//  Input — o hook de cursor consulta isto ANTES de engolir o SetCursorPos.
// ---------------------------------------------------------------------------
bool Menu_WantsMouse() {
    if (!C.menuOpen)                 return false;
    if (!Hooks::g_imguiReady.load()) return false;
    if (!ImGui::GetCurrentContext()) return false;
    return ImGui::GetIO().WantCaptureMouse;
}

namespace Menu {

    void Render() {
        if (!ImGui::GetCurrentContext()) return;
        if (!Hooks::g_imguiReady.load())    return;

        __try { Draw(); }   // menu.cpp; o estilo e' responsabilidade dele
        __except (EXCEPTION_EXECUTE_HANDLER) { Log("[crash] Menu::Draw"); }
    }

} // namespace Menu

// ===========================================================================
//  Aimbot::Bone — unico simbolo de Aimbot que vive aqui
// ===========================================================================
namespace Aimbot {

    void Bone(Il2CppObject* p, Il2CppObject* target) {
        if (!p || !target || !SDK::O_rotation || !SDK::O_rotationIsQuat) return;

        // SEH: 'target' pode estar reciclado. Sem isto, um AV aqui derruba o
        // passo (e o Bone pode ser chamado do thread de render).
        __try {
            Vec3 me = SDK::GetPos(p);
            if (!me.isSane()) return;

            // TargetAimPos devolve Vec3() quando nao resolve o bone, mas
            // (0,0,0) tambem e' ponto valido: por isso as duas checagens.
            Vec3 tp = SDK::TargetAimPos(target, C.aimBone);
            if (!tp.isSane() || tp.zero())
                tp = SDK::GetPos(target);
            if (!tp.isSane()) return;

            Vec3 dir = tp - me;
            const float L = dir.len();
            if (L < 0.0001f) return;

            ::Quat q = SDK::LookRot(dir * (1.f / L));

            // Caminho da camera E do player: escrever so' o player nao converge
            // quando a mira do jogo e' dirigida pela camera.
            Il2CppObject* camTf = SDK::CamTfObj();
            if (camTf) SDK::SetRot(camTf, q);
            SDK::SetPlayerRot(p, q);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { Log("[crash] Aimbot::Bone"); }
    }

} // namespace Aimbot