// ============================================================
//  menu.cpp - bfi / Bullet Force (ImGui)
//
//  >>> v16:
//   - aba Aimbot: "So visivel (sem fallback)" -> C.aimStrictVis
//   - aba Visual: slider "LOS por frame" -> C.losPerFrame
//   - aba Trigger: slider "Raio da mira (px)" -> C.triggerRadius
//   - aba Misc: checkbox "Profiler (ms por passo)" -> C.showFpsGraph
//
//  >>> FIX (LNK2005): Menu::Render() NAO e definido aqui. Ele vive em
//      features.cpp (hooks.cpp chama Render -> features.cpp -> Draw).
//  >>> FIX C6011: ComboIdx checava 'list' na linha do 'cur' mas nao dentro do
//      loop do combo; com lista nula, list[i] estourava.
//  >>> FIX (v17): VkName() usava UM buffer estatico. Tres chamadas na mesma
//      expressao (rodape) devolviam o mesmo ponteiro e imprimiam o ultimo
//      valor. Agora usa buffer rotativo.
//  >>> FIX (v17): Menu::g_open agora e' REFERENCIA para C.menuOpen. Antes era
//      um bool proprio -> duas verdades sobre "menu aberto".
//  >>> FIX (v17): estilo aplicado 2x (aqui e em features.cpp). Dono unico agora
//      e' este arquivo; features.cpp::Render so chama Draw().
// ============================================================
#include "common.h"
#include "config.h"
#include "features.h"
#include "unlock.h"
#include "dupe.h"

#include <cstdio>
#include <cstring>
#include <imgui.h>

namespace Menu {

    // Mesma celula de memoria que C.menuOpen. Nao existe estado duplicado.
    bool& g_open = g_cfg.menuOpen;

    // ---------- estilo (dono unico) ----------
    static ImGuiContext* g_styledCtx = nullptr;

    static void ApplyStyleOnce()
    {
        // Reaplica quando o contexto do ImGui for (re)criado. So marca depois
        // de aplicar de fato.
        ImGuiContext* ctx = ImGui::GetCurrentContext();
        if (!ctx || g_styledCtx == ctx) return;
        g_styledCtx = ctx;

        // ORDEM IMPORTA: StyleColorsDark() ZERA os campos de rounding antes de
        // escrever as cores. Se vier depois, os overrides abaixo sao apagados.
        ImGui::StyleColorsDark();

        ImGuiStyle& s = ImGui::GetStyle();

        s.WindowRounding = 6.f;
        s.ChildRounding = 6.f;
        s.FrameRounding = 4.f;
        s.PopupRounding = 4.f;
        s.GrabRounding = 4.f;
        s.ScrollbarRounding = 4.f;
        s.TabRounding = 4.f;

        s.WindowBorderSize = 1.f;
        s.FrameBorderSize = 0.f;
        s.WindowPadding = ImVec2(10, 10);
        s.FramePadding = ImVec2(8, 4);
        s.ItemSpacing = ImVec2(8, 6);
        s.ItemInnerSpacing = ImVec2(6, 4);
        s.ScrollbarSize = 12.f;
        s.WindowTitleAlign = ImVec2(0.5f, 0.5f);

        ImVec4* c = s.Colors;
        c[ImGuiCol_WindowBg] = ImVec4(0.07f, 0.07f, 0.09f, 0.94f);
        c[ImGuiCol_TitleBg] = ImVec4(0.10f, 0.10f, 0.13f, 1.f);
        c[ImGuiCol_TitleBgActive] = ImVec4(0.13f, 0.13f, 0.17f, 1.f);
        c[ImGuiCol_Button] = ImVec4(0.17f, 0.17f, 0.22f, 1.f);
        c[ImGuiCol_ButtonHovered] = ImVec4(0.24f, 0.24f, 0.31f, 1.f);
        c[ImGuiCol_ButtonActive] = ImVec4(0.30f, 0.30f, 0.38f, 1.f);
        c[ImGuiCol_Header] = ImVec4(0.17f, 0.17f, 0.22f, 1.f);
        c[ImGuiCol_HeaderHovered] = ImVec4(0.24f, 0.24f, 0.31f, 1.f);
        c[ImGuiCol_HeaderActive] = ImVec4(0.30f, 0.30f, 0.38f, 1.f);
        c[ImGuiCol_CheckMark] = ImVec4(0.35f, 0.80f, 0.45f, 1.f);
        c[ImGuiCol_SliderGrab] = ImVec4(0.35f, 0.80f, 0.45f, 1.f);
        c[ImGuiCol_SliderGrabActive] = ImVec4(0.45f, 0.90f, 0.55f, 1.f);
        c[ImGuiCol_Tab] = ImVec4(0.12f, 0.12f, 0.15f, 1.f);
        c[ImGuiCol_TabHovered] = ImVec4(0.24f, 0.24f, 0.31f, 1.f);
        c[ImGuiCol_TabActive] = ImVec4(0.20f, 0.20f, 0.26f, 1.f);
        c[ImGuiCol_Separator] = ImVec4(0.24f, 0.24f, 0.30f, 1.f);
    }

    // ---------- keybind ----------
    // Buffer ROTATIVO: VkName() e' chamada varias vezes na MESMA expressao
    // (ex.: rodape com menu/panic/salvar). Com um buffer unico, os tres "%s"
    // recebiam o mesmo ponteiro e imprimiam o ultimo valor calculado.
    static const char* VkName(int vk)
    {
        static char     s_buf[8][32];
        static unsigned s_next = 0;
        char* b = s_buf[s_next++ & 7u];

        switch (vk) {
        case 0:            return "Nenhuma";
        case VK_LBUTTON:   return "Mouse Esq";
        case VK_RBUTTON:   return "Mouse Dir";
        case VK_MBUTTON:   return "Mouse Mei";
        case VK_XBUTTON1:  return "Mouse 4";
        case VK_XBUTTON2:  return "Mouse 5";
        case VK_SHIFT:     return "Shift";
        case VK_CONTROL:   return "Ctrl";
        case VK_MENU:      return "Alt";
        case VK_SPACE:     return "Espaco";
        case VK_TAB:       return "Tab";
        case VK_INSERT:    return "Insert";
        case VK_DELETE:    return "Delete";
        case VK_HOME:      return "Home";
        case VK_END:       return "End";
        case VK_PRIOR:     return "PgUp";
        case VK_NEXT:      return "PgDn";
        case VK_F1:  return "F1";  case VK_F2:  return "F2";  case VK_F3:  return "F3";
        case VK_F4:  return "F4";  case VK_F5:  return "F5";  case VK_F6:  return "F6";
        case VK_F7:  return "F7";  case VK_F8:  return "F8";  case VK_F9:  return "F9";
        case VK_F10: return "F10"; case VK_F11: return "F11"; case VK_F12: return "F12";
        }
        if (vk >= '0' && vk <= '9') { b[0] = (char)vk; b[1] = 0; return b; }
        if (vk >= 'A' && vk <= 'Z') { b[0] = (char)vk; b[1] = 0; return b; }
        sprintf_s(b, 32, "0x%02X", vk);
        return b;
    }

    static int* g_waitKey = nullptr;
    static bool  g_waitArm = false;   // pula o frame do clique (LBUTTON ainda pode estar down)

    static void Keybind(const char* label, int* key)
    {
        char buf[96];
        sprintf_s(buf, "%s: %s", label, (g_waitKey == key) ? "..." : VkName(*key));

        if (ImGui::Button(buf, ImVec2(170, 0))) {
            g_waitKey = key;
            g_waitArm = false;
        }

        if (g_waitKey != key) return;

        if (!g_waitArm) { g_waitArm = true; return; }   // arma no frame seguinte

        for (int vk = 1; vk < 256; vk++) {
            if (GetAsyncKeyState(vk) & 0x8000) {
                *key = vk;
                g_waitKey = nullptr;
                break;
            }
        }
    }

    // ---------- abas ----------
    static void TabAimbot();
    static void TabTrigger();
    static void TabVisual();
    static void TabWeapon();
    static void TabMovement();
    static void TabUnlock();
    static void TabMisc();

    void Draw()
    {
        ImGuiContext* ctx = ImGui::GetCurrentContext();
        if (!ctx) return;

        ApplyStyleOnce();
        if (!C.menuOpen) return;

        ImGui::SetNextWindowSize(ImVec2(600, 560), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("bfi##main", &C.menuOpen))
        {
            ImGui::End();
            return;
        }

        ImGui::Text("bfi | Bullet Force internal");
        ImGui::Separator();

        if (ImGui::BeginTabBar("tabs"))
        {
            if (ImGui::BeginTabItem("Aimbot")) { TabAimbot();   ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Trigger")) { TabTrigger();  ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Visual")) { TabVisual();   ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Arma")) { TabWeapon();   ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Movimento")) { TabMovement(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Unlock")) { TabUnlock();   ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Misc")) { TabMisc();     ImGui::EndTabItem(); }
            ImGui::EndTabBar();
        }

        ImGui::Separator();
        ImGui::TextDisabled("%s = menu | %s = panic | %s = salvar",
            VkName(C.menuKey), VkName(C.panicKey), VkName(C.saveKey));
        ImGui::End();
    }

    // ------------------------------------------------------------
    static void TabAimbot()
    {
        ImGui::Checkbox("Aimbot", &C.aimbot);
        ImGui::SameLine();
        ImGui::Checkbox("Ignorar time", &C.aimTeam);

        ImGui::Checkbox("Checar visao (LOS) - so visivel", &C.aimVisCheck);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Marcado: so mira em alvo visivel.\n"
                "Desmarcado: mira atraves da parede.");

        ImGui::Checkbox("So visivel (sem fallback)", &C.aimStrictVis);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Marcado: se ninguem estiver visivel, nao mira\n"
                "em ninguem. Nada de mirar atras do muro.\n"
                "Desmarcado: cai no mais proximo da mira.");

        ImGui::Checkbox("Rotacionar camera", &C.aimRotate);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Desmarcado: o aimbot NAO mexe na camera\n"
                "(util com silent aim).");

        ImGui::Checkbox("Circulo de FOV", &C.showFovCircle);

        static const char* kModes[] = { "Camera (screen)", "Angulo (3D)", "Mouse" };
        ImGui::Combo("Modo", &C.aimMode, kModes, IM_ARRAYSIZE(kModes));
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Camera: FOV medido em pixels na tela.\n"
                "Angulo: FOV medido em graus no mundo (independe do zoom).\n"
                "Mouse: sem suporte -> cai em Camera.");

        static const char* kBones[] = { "Auto", "Cabeca", "Peito", "Estomago", "Perna", "Aleatorio" };
        ImGui::Combo("Osso", &C.aimBone, kBones, IM_ARRAYSIZE(kBones));

        ImGui::SliderFloat("FOV", &C.aimFov, 1.0f, 90.0f, "%.1f");
        ImGui::SliderFloat("Suavidade", &C.aimSmooth, 1.0f, 20.0f, "%.1f");
        ImGui::SliderFloat("Dist. max", &C.aimMaxDist, 10.0f, 500.0f, "%.0f");
        ImGui::ColorEdit4("Cor do FOV", C.aimColor);
        Keybind("Tecla do aimbot", &C.aimKey);

        ImGui::Separator();
        ImGui::Checkbox("Silent aim", &C.silentAim);
        ImGui::Checkbox("Magic bullets", &C.magicBullets);
    }

    // ------------------------------------------------------------
    static void TabTrigger()
    {
        ImGui::Checkbox("Triggerbot", &C.triggerbot);
        ImGui::SliderFloat("Delay (ms)", &C.triggerDelay, 0.0f, 300.0f, "%.0f");

        // >>> NOVO: so atira se a mira estiver EM CIMA do alvo.
        ImGui::SliderFloat("Raio da mira (px)", &C.triggerRadius, 0.0f, 60.0f, "%.0f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("So atira se o alvo estiver a menos de N pixels\n"
                "do centro da tela. 0 = desliga (atira com o alvo em\n"
                "qualquer lugar do FOV do aimbot).");

        Keybind("Tecla do trigger", &C.triggerKey);
    }

    // ------------------------------------------------------------
    static void TabVisual()
    {
        ImGui::Checkbox("ESP", &C.esp);
        ImGui::Separator();

        ImGui::Checkbox("Caixa", &C.espBox);
        ImGui::Checkbox("Preencher", &C.espFill);
        ImGui::Checkbox("Nome", &C.espName);
        ImGui::Checkbox("Vida (barra)", &C.espHealth);
        ImGui::Checkbox("Vida (numero)", &C.espHealthNum);
        ImGui::Checkbox("Arma", &C.espWeapon);
        ImGui::Checkbox("Distancia", &C.espDistance);
        ImGui::Checkbox("Linha", &C.espSnapline);
        ImGui::Checkbox("Esqueleto", &C.espSkeleton);
        ImGui::Checkbox("NPCs", &C.espNpc);
        ImGui::Checkbox("Ignorar time", &C.teamCheck);

        ImGui::Separator();
        ImGui::Checkbox("Checar visao", &C.espVisCheck);
        ImGui::Checkbox("Linha de visao", &C.espVisLine);

        ImGui::SliderInt("LOS por frame", &C.losPerFrame, 1, 32);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Quantos alvos sao testados por frame (rodizio).\n"
                "Baixo = mais FPS. A visibilidade atualiza em poucos frames.\n"
                "Isso e' o custo real do 'Checar visao'.");

        ImGui::SliderFloat("Espessura", &C.espThickness, 0.5f, 5.0f, "%.1f");
        ImGui::SliderFloat("Dist. max", &C.espMaxDist, 10.0f, 1000.0f, "%.0f");
        ImGui::SliderFloat("Tamanho da fonte", &C.espFontSize, 8.0f, 24.0f, "%.0f");
        ImGui::SliderFloat("Offset Y", &C.espOffY, -50.0f, 50.0f, "%.0f");
        ImGui::SliderFloat("Escala Y", &C.espScaleY, 0.5f, 2.0f, "%.2f");
        ImGui::SliderFloat("Altura do corpo", &C.espBodyH, 1.0f, 3.0f, "%.2f");

        ImGui::Separator();
        ImGui::ColorEdit4("Cor base", C.espColor);
        ImGui::ColorEdit4("Cor da caixa", C.espBoxColor);
        ImGui::ColorEdit4("Cor do texto", C.espTextColor);
        ImGui::ColorEdit4("Cor do esqueleto", C.espSkelColor);
        ImGui::ColorEdit4("Cor visivel", C.visColor);
        ImGui::ColorEdit4("Cor nao visivel", C.noVisColor);
    }

    // ------------------------------------------------------------
    static void TabWeapon()
    {
        ImGui::Checkbox("Sem recoil", &C.noRecoil);
        ImGui::Checkbox("Sem spread", &C.noSpread);
        ImGui::Checkbox("Municao infinita", &C.infAmmo);
        ImGui::Checkbox("Sem reload", &C.noReload);
        ImGui::Checkbox("Rapid fire", &C.rapidFire);
    }

    // ------------------------------------------------------------
    static void TabMovement()
    {
        ImGui::Checkbox("Speed hack", &C.speedHack);
        ImGui::SliderFloat("Velocidade", &C.speedValue, 1.0f, 30.0f, "%.1f");

        ImGui::Checkbox("Fly", &C.fly);
        ImGui::SliderFloat("Velocidade do voo", &C.flySpeed, 1.0f, 30.0f, "%.1f");

        ImGui::Checkbox("Super pulo", &C.superJump);
        ImGui::SliderFloat("Forca do pulo", &C.jumpPower, 1.0f, 30.0f, "%.1f");

        ImGui::Checkbox("Teleport", &C.teleport);
        ImGui::Checkbox("God mode", &C.godmode);
    }

    // ------------------------------------------------------------
    // Item seguro: devolve 'fallback' quando indice fora da faixa OU string nula.
    static const char* ItemOr(const char* const* list, int idx, int count,
        const char* fallback)
    {
        if (!list || idx < 0 || idx >= count) return fallback;
        return list[idx] ? list[idx] : fallback;
    }

    static bool ComboIdx(const char* label, int* sel, int count, const char* const* list,
        const char* vazio)
    {
        if (count > 0 && !list) return false;

        const char* cur = ItemOr(list, *sel, count, vazio);
        bool mudou = false;
        if (ImGui::BeginCombo(label, cur)) {
            for (int i = 0; i < count; i++) {
                const char* item = (list && list[i]) ? list[i] : "(?)";
                if (ImGui::Selectable(item, *sel == i)) { *sel = i; mudou = true; }
            }
            ImGui::EndCombo();
        }
        return mudou;
    }

    // ------------------------------------------------------------
    static void TabUnlock()
    {
        ImGui::Checkbox("Unlock All", &C.unlockAll);
        ImGui::SameLine(); ImGui::Checkbox("Armas", &C.unlockWeapons);
        ImGui::SameLine(); ImGui::Checkbox("Skins", &C.unlockSkins);
        ImGui::Checkbox("Acessorios", &C.unlockAttachments);
        ImGui::SameLine(); ImGui::Checkbox("Personagens", &C.unlockChars);
        ImGui::SameLine(); ImGui::Checkbox("Dinheiro", &C.unlockCurrency);

        ImGui::SliderInt("Nivel", &C.unlockLevel, 0, 9999);
        ImGui::InputInt("Dinheiro (valor)", &C.currencyValue);
        ImGui::SliderFloat("Intervalo", &C.unlockInterval, 0.05f, 2.0f, "%.2f");

        ImGui::Separator();
        ImGui::Text("Catalogo: %s", Dupe::HasFile() ? "bfi_dupe.json OK" : "AUSENTE");

        if (Dupe::Building()) {
            ImGui::TextColored(ImVec4(1.f, 0.8f, 0.2f, 1.f), "gerando... etapa %d/5", Dupe::Stage());
        }
        else {
            ImGui::TextDisabled("%s", Dupe::Status());
        }

        if (ImGui::Button("DUPE COMPLETO (gerar JSON)")) Dupe::RequestBuild();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Varre armas, anexos, skins e personagens UMA vez\n"
                "e salva em bfi_dupe.json, ao lado da DLL.\n"
                "Roda em 5 etapas, 1 por frame. Entre numa partida antes.");
        ImGui::SameLine();
        if (ImGui::Button("Recarregar JSON")) Dupe::RequestReload();
        ImGui::SameLine();
        if (ImGui::Button("Cancelar") && Dupe::Building()) Dupe::Cancel();
        ImGui::SameLine();
        if (ImGui::Button("Dump no log")) Unlock::Dump();

        ImGui::TextDisabled("armas=%d skins=%d chars=%d anexos=%d/%d/%d",
            Dupe::WeaponCount(), Dupe::CamoCount(), Dupe::CharCount(),
            Dupe::AttachCount(0), Dupe::AttachCount(1), Dupe::AttachCount(2));

        if (!Dupe::HasFile() && Dupe::WeaponCount() == 0)
            ImGui::TextColored(ImVec4(1.f, 0.5f, 0.4f, 1.f),
                "sem catalogo: aperte DUPE COMPLETO dentro de uma partida");

        ImGui::SameLine();
        ImGui::Checkbox("Aplicar selecao", &C.applySelection);

        if (ImGui::CollapsingHeader("Loadout", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::SliderInt("Slot do loadout", &C.loadoutSlot, 0, 2);

            int wn = Unlock::WeaponCount();
            const char* const* wl = Unlock::WeaponList();
            if (wn > 0 && wl)
                ComboIdx("Arma", &C.selWeapon, wn, wl, "(nenhuma)");
            else
                ImGui::TextDisabled("Arma: catalogo vazio (aperte DUPE COMPLETO)");

            for (int s = 0; s < Unlock::AttachSlotCount(); ++s) {
                int an = Unlock::AttachCount(s);
                const char* const* al = Unlock::AttachList(s);
                ImGui::PushID(s);
                if (an > 0 && al) {
                    ComboIdx(Unlock::AttachSlotName(s), &C.selAttach[s], an, al, "(nenhum)");
                    ImGui::SameLine();
                    if (ImGui::SmallButton("limpar")) C.selAttach[s] = -1;
                }
                else {
                    ImGui::TextDisabled("%s: vazio", Unlock::AttachSlotName(s));
                }
                ImGui::PopID();
            }
        }

        if (ImGui::CollapsingHeader("Skins", ImGuiTreeNodeFlags_DefaultOpen))
        {
            int cn = Unlock::CamoCount();
            const char* const* cl = Unlock::CamoList();
            if (cn > 0 && cl) {
                if (ComboIdx("Skin / Camo", &C.selCamo, cn, cl, "(nenhuma)")) {
                    C.skinIndex = C.selCamo;
                    // cl[i] pode ser nulo -> strncpy_s com src nulo e' UB.
                    strncpy_s(C.skinName, sizeof(C.skinName),
                        ItemOr(cl, C.selCamo, cn, ""), _TRUNCATE);
                }
            }
            else {
                ImGui::TextDisabled("Skin: catalogo vazio");
            }
            ImGui::Checkbox("Skin changer", &C.skinChanger);
            ImGui::SliderInt("Indice da skin", &C.skinIndex, 0, 255);
            ImGui::InputText("Nome da skin", C.skinName, sizeof(C.skinName));
        }

        if (ImGui::CollapsingHeader("Personagens", ImGuiTreeNodeFlags_DefaultOpen))
        {
            int pn = Unlock::CharCount();
            const char* const* pl = Unlock::CharList();
            if (pn > 0 && pl) {
                if (ComboIdx("Personagem", &C.selChar, pn, pl, "(nenhum)"))
                    strncpy_s(C.charName, sizeof(C.charName),
                        ItemOr(pl, C.selChar, pn, ""), _TRUNCATE);
            }
            else {
                ImGui::TextDisabled("Personagem: catalogo vazio");
            }
        }

        ImGui::Separator();
        ImGui::TextDisabled("sel: arma=%d skin=%d char=%d anexos=%d/%d/%d",
            C.selWeapon, C.selCamo, C.selChar,
            C.selAttach[0], C.selAttach[1], C.selAttach[2]);

        if (ImGui::CollapsingHeader("Slots manuais"))
        {
            ImGui::Checkbox("Preencher slots", &C.fillLoadoutSlots);
            ImGui::Checkbox("Aplicar slots", &C.applySlots);
            ImGui::SliderInt("Slot 1", &C.slotWeapon[0], -1, 64);
            ImGui::SliderInt("Slot 2", &C.slotWeapon[1], -1, 64);
            ImGui::SliderInt("Slot 3", &C.slotWeapon[2], -1, 64);
        }

        if (ImGui::CollapsingHeader("Forcar nomes (string)"))
        {
            ImGui::Checkbox("Ativo", &C.forceNames);
            ImGui::InputText("Arma", C.weaponName, sizeof(C.weaponName));
            ImGui::InputText("Skin", C.skinName, sizeof(C.skinName));
            ImGui::InputText("Anexo", C.attachName, sizeof(C.attachName));
        }
    }

    // ------------------------------------------------------------
    static void TabMisc()
    {
        ImGui::Checkbox("Watermark", &C.watermark);
        ImGui::Checkbox("Mostrar FPS", &C.fps);

        // >>> NOVO: profiler por passo
        ImGui::Checkbox("Profiler (ms por passo)", &C.showFpsGraph);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Mostra o tempo de CADA passo do tick.\n"
                "Use pra achar o gargalo em vez de chutar.");

        ImGui::Checkbox("Crosshair", &C.crosshair);
        ImGui::SliderFloat("Tamanho da mira", &C.crosshairSize, 1.0f, 20.0f, "%.0f");
        ImGui::ColorEdit4("Cor da mira", C.crosshairColor);
        ImGui::SliderFloat("FOV da camera", &C.camFov, 60.0f, 140.0f, "%.0f");
        ImGui::Checkbox("Hitmarker", &C.hitmarker);

        ImGui::Separator();
        ImGui::Checkbox("Anti-cheat bypass", &C.antiCheat);
        ImGui::Text("Stage: %d", C.stage);

        ImGui::Separator();
        ImGui::Checkbox("Log detalhado (probeLog)", &C.probeLog);
        ImGui::Checkbox("Beat no log", &C.beatLog);

        ImGui::Separator();
        Keybind("Tecla do menu", &C.menuKey);
        Keybind("Tecla de panic", &C.panicKey);
        Keybind("Tecla de salvar", &C.saveKey);
        Keybind("Tecla de unload", &C.unloadKey);

        if (ImGui::Button("Salvar config"))   ConfigIO::Save();
        ImGui::SameLine();
        if (ImGui::Button("Carregar config")) ConfigIO::Load();
    }

} // namespace Menu

// ---------- aliases de entrada ----------
void DrawMenu() { Menu::Draw(); }
void DrawOverlay() { Menu::Draw(); }