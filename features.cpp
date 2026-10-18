// ============================================================================
//  features.cpp — Menu::Render / Menu::ApplyStyle / Aimbot::Bone / Menu_WantsMouse
//  O menu de verdade vive em menu.cpp (Menu::Draw).
//
//  >>> UNICO dono de Menu::Render(). O menu.cpp NAO define Render, senao:
//        LNK2005 "void __cdecl Menu::Render(void)" ja definida em features.obj
//        LNK1169
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
//  Input — o hook de cursor consulta isto antes de deixar o mouse pro jogo.
// ---------------------------------------------------------------------------
bool Menu_WantsMouse() {
    if (!C.menuOpen)                 return false;
    if (!Hooks::g_imguiReady.load()) return false;
    if (!ImGui::GetCurrentContext()) return false;
    return ImGui::GetIO().WantCaptureMouse;
}

namespace Menu {

    void ApplyStyle() {
        if (!ImGui::GetCurrentContext()) return;

        ImGuiStyle& s = ImGui::GetStyle();

        // ORDEM IMPORTA: StyleColorsDark() zera os campos de rounding antes de
        // escrever as cores. Se ele vier DEPOIS, os overrides abaixo sao
        // sobrescritos de volta pra 0. Chame sempre primeiro.
        ImGui::StyleColorsDark();

        s.WindowRounding = 6.f;
        s.ChildRounding = 6.f;
        s.FrameRounding = 4.f;
        s.PopupRounding = 4.f;
        s.GrabRounding = 4.f;
        s.ScrollbarRounding = 4.f;
        s.TabRounding = 4.f;

        s.WindowBorderSize = 1.f;
        s.FrameBorderSize = 0.f;
        s.WindowPadding = ImVec2(8, 8);
        s.FramePadding = ImVec2(8, 4);
        s.ItemSpacing = ImVec2(8, 6);
        s.ItemInnerSpacing = ImVec2(6, 4);
        s.ScrollbarSize = 12.f;
        s.GrabMinSize = 10.f;

        s.WindowTitleAlign = ImVec2(0.5f, 0.5f);
        s.AntiAliasedLines = true;
        s.AntiAliasedFill = true;

        s.Colors[ImGuiCol_WindowBg] = ImVec4(0.06f, 0.06f, 0.07f, 0.94f);
        s.Colors[ImGuiCol_TitleBg] = ImVec4(0.10f, 0.10f, 0.12f, 1.00f);
    }

    // Ponto de entrada chamado pelo hk_wglSwapBuffers.
    // O menu.cpp define Draw() e chama o estilo proprio dele depois deste.
    void Render() {
        ImGuiContext* ctx = ImGui::GetCurrentContext();
        if (!ctx || !Hooks::g_imguiReady.load()) return;

        // Reaplica o estilo quando o contexto for (re)criado. O flag so passa
        // a valer depois que ApplyStyle() rodou de fato — antes ele era setado
        // mesmo com ApplyStyle() saindo cedo por !g_imguiReady.
        static ImGuiContext* styledCtx = nullptr;
        if (styledCtx != ctx) {
            ApplyStyle();
            styledCtx = ctx;
        }

        Draw();                        // menu.cpp
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