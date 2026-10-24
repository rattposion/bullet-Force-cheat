// ============================================================
//  menu.cpp - bfi / Bullet Force (ImGui)
//  v43.19: [ARMA] cada secao roda em __try proprio -> uma secao que
//          estoura nao mata mais a aba inteira (era isso que fazia os
//          seletores de skin/personagem/loadout/anexos "sumirem").
//          Tracer: g_sec marca a secao atual e o BFI_SAFE_TAB loga
//          "[crash] Tab X secao=Y" (throttled -> sem inundar o log).
//  v43.18: seletores sempre visiveis + botoes que aplicam. ComboList()
//          checa (*idx >= 0 && *idx < n) antes de indexar.
//  v43.17: <windows.h> PRIMEIRO; includes do projeto com "..\".
//          LineHas() compara dentro do len; MoneyName() protegido.
//          Cada aba em __try proprio (imgui.cpp:11200/11226).
//  v43.16: slots e anexos por NOME. BUILD chama Unlock::BuildAllNow().
// ============================================================

#include <windows.h>                 // >>> tem que ser o PRIMEIRO
#include <shellapi.h>
#include <cstdio>
#include <cstring>
#include <cstddef>
#include <string>
#include <vector>

#include "imgui.h"                   // esta' na mesma pasta (imgui\)

#include "../common.h"               // >>> um nivel acima
#include "../config.h"
#include "../features.h"
#include "../unlock.h"
#include "../dupe.h"

#pragma comment(lib, "shell32.lib")

#ifndef BFI_CONFIG_VER
#error "config.h ANTIGO sendo incluido (sem BFI_CONFIG_VER). Substitua o config.h da pasta do projeto."
#elif BFI_CONFIG_VER < 4315
#error "config.h desatualizado (BFI_CONFIG_VER < 4315). Substitua o config.h da pasta do projeto."
#endif

// Unlock API agora inteiramente exportada pelo unlock.h

namespace Menu {

    bool& g_open = C.menuOpen;

    // ==========================================================
    //  TRACER DE SECAO (usado pelo BFI_SAFE_TAB)
    // ==========================================================
    static const char* g_sec = "-";

    static void SecFail(const char* n)
    {
        static char last[64] = "";
        if (strcmp(last, n) != 0) {
            strcpy_s(last, n);
            Log("[arma] SECAO %s FALHOU - aba continua", n);
        }
    }

#define ARMA_SEC(_name, _call)                          \
        do { g_sec = _name;                             \
             __try { _call(); }                         \
             __except (EXCEPTION_EXECUTE_HANDLER) {     \
                 SecFail(_name);                        \
             } } while (0)

    // ==========================================================
    //  ESTILO
    // ==========================================================
    void ApplyStyle()
    {
        if (!ImGui::GetCurrentContext()) return;
        ImGui::StyleColorsDark();

        ImGuiStyle& s = ImGui::GetStyle();
        s.WindowRounding = 6.f; s.ChildRounding = 6.f; s.FrameRounding = 4.f;
        s.PopupRounding = 4.f; s.GrabRounding = 4.f; s.ScrollbarRounding = 4.f;
        s.TabRounding = 4.f;
        s.WindowBorderSize = 1.f; s.FrameBorderSize = 0.f;
        s.WindowPadding = ImVec2(10, 10); s.FramePadding = ImVec2(8, 4);
        s.ItemSpacing = ImVec2(8, 6); s.ItemInnerSpacing = ImVec2(6, 4);
        s.ScrollbarSize = 12.f; s.WindowTitleAlign = ImVec2(0.5f, 0.5f);

        ImVec4* c = s.Colors;
        c[ImGuiCol_WindowBg] = ImVec4(0.07f, 0.07f, 0.09f, 0.94f);
        c[ImGuiCol_TitleBg] = ImVec4(0.10f, 0.10f, 0.13f, 1.f);
        c[ImGuiCol_TitleBgActive] = ImVec4(0.13f, 0.13f, 0.17f, 1.f);
        c[ImGuiCol_TitleBgCollapsed] = ImVec4(0.10f, 0.10f, 0.13f, 0.80f);
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

    static void ApplyStyleOnce()
    {
        static void* lastCtx = nullptr;
        void* ctx = (void*)ImGui::GetCurrentContext();
        if (!ctx || ctx == lastCtx) return;
        lastCtx = ctx;
        ApplyStyle();
    }

    // ==========================================================
    //  KEYBIND
    // ==========================================================
    static const char* VkName(int vk)
    {
        static char     s_buf[8][32];
        static unsigned s_next = 0;
        char* b = s_buf[s_next++ & 7u];

        switch (vk) {
        case 0:           return "Nenhuma";
        case VK_LBUTTON:  return "Mouse Esq";
        case VK_RBUTTON:  return "Mouse Dir";
        case VK_MBUTTON:  return "Mouse Mei";
        case VK_XBUTTON1: return "Mouse 4";
        case VK_XBUTTON2: return "Mouse 5";
        case VK_SHIFT:    return "Shift";
        case VK_CONTROL:  return "Ctrl";
        case VK_MENU:     return "Alt";
        case VK_SPACE:    return "Espaco";
        case VK_TAB:      return "Tab";
        case VK_INSERT:   return "Insert";
        case VK_DELETE:   return "Delete";
        case VK_HOME:     return "Home";
        case VK_END:      return "End";
        case VK_PRIOR:    return "Page Up";
        case VK_NEXT:     return "Page Down";
        default: break;
        }
        if (vk >= VK_F1 && vk <= VK_F24) { sprintf_s(b, 32, "F%d", vk - VK_F1 + 1); return b; }
        if (vk >= '0' && vk <= '9') { sprintf_s(b, 32, "%c", (char)vk); return b; }
        if (vk >= 'A' && vk <= 'Z') { sprintf_s(b, 32, "%c", (char)vk); return b; }
        sprintf_s(b, 32, "0x%02X", vk);
        return b;
    }

    static int* g_waitKey = nullptr;

    static void KeyButton(const char* label, int* key)
    {
        const bool waiting = (g_waitKey == key);
        char buf[64];
        sprintf_s(buf, "%s: %s", label, waiting ? "<aperte>" : VkName(*key));

        if (waiting) {
            for (int vk = 1; vk < 256; ++vk) {
                if (GetAsyncKeyState(vk) & 0x8000) {
                    *key = vk;
                    g_waitKey = nullptr;
                    break;
                }
            }
        }

        if (ImGui::Button(buf, ImVec2(150, 0)))
            g_waitKey = waiting ? nullptr : key;

        if (!waiting && *key != 0) {
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) *key = 0;
        }
    }

    // ==========================================================
    //  LOG
    // ==========================================================
    static char        g_logPath[MAX_PATH] = {};
    static bool        g_logPathDone = false;
    static ULONGLONG   g_logNext = 0;
    static std::string g_logText;
    static int         g_logErr = 0;
    static int         g_logType = 0;

    static void DirOfModule(char* out, size_t n)
    {
        out[0] = 0;
        HMODULE hm = nullptr;
        if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCSTR)&DirOfModule, &hm)) return;
        GetModuleFileNameA(hm, out, (DWORD)n);
        char* p = strrchr(out, '\\');
        if (p) p[1] = 0;
    }

    static const char* LogPath()
    {
        if (!g_logPathDone) {
            g_logPathDone = true;
            char dir[MAX_PATH]{};
            DirOfModule(dir, sizeof(dir));
            sprintf_s(g_logPath, "%sBFInternal.log", dir);
        }
        return g_logPath;
    }

    // comparacao DENTRO do len da linha (a linha nao tem NUL)
    static bool LineHas(const char* p, size_t len, const char* tag)
    {
        if (!p || !tag) return false;
        const size_t tl = strlen(tag);
        if (tl == 0 || len < tl) return false;
        for (size_t i = 0; i + tl <= len; ++i)
            if (memcmp(p + i, tag, tl) == 0) return true;
        return false;
    }

    static bool TypeMatch(const char* p, size_t len, int type)
    {
        if (C.logFilter[0] && !LineHas(p, len, C.logFilter)) return false;
        if (type == 0) return true;
        if (type == 5) return LineHas(p, len, "[money]") ||
            LineHas(p, len, "[mirror]") ||
            LineHas(p, len, "[conta]") ||
            LineHas(p, len, "[credit]");
        if (type == 6) return LineHas(p, len, "[shop]") ||
            LineHas(p, len, "[loja]");
        if (type == 1) return LineHas(p, len, "[info]");
        if (type == 2) return LineHas(p, len, "[ok]");
        if (type == 3) return LineHas(p, len, "[warn]") || LineHas(p, len, "[w]");
        if (type == 4) return LineHas(p, len, "[err]") || LineHas(p, len, "[erro]");
        return true;
    }

    static void LogRefresh()
    {
        const ULONGLONG now = GetTickCount64();
        if (now < g_logNext) return;
        g_logNext = now + 500;

        FILE* f = _fsopen(LogPath(), "rb", _SH_DENYNO);
        if (!f) { g_logErr = 1; return; }
        g_logErr = 0;

        fseek(f, 0, SEEK_END);
        const long long size = _ftelli64(f);
        long long from = size - 262144;
        if (from < 0) from = 0;
        _fseeki64(f, from, SEEK_SET);

        std::string buf;
        buf.resize((size_t)(size - from));
        const size_t rd = fread(&buf[0], 1, buf.size(), f);
        buf.resize(rd);
        fclose(f);

        if (from > 0) {
            const size_t nl = buf.find('\n');
            if (nl != std::string::npos) buf.erase(0, nl + 1);
        }
        g_logText.swap(buf);
    }

    static void OpenLogFolder()
    {
        char dir[MAX_PATH]{};
        DirOfModule(dir, sizeof(dir));
        ShellExecuteA(nullptr, "open", dir, nullptr, nullptr, SW_SHOWNORMAL);
    }

    static void SaveLogCopy()
    {
        char dir[MAX_PATH]{};
        DirOfModule(dir, sizeof(dir));
        char out[MAX_PATH]{};
        sprintf_s(out, "%sBFInternal_copia.log", dir);
        FILE* f = _fsopen(out, "wb", _SH_DENYNO);
        if (!f) return;
        fwrite(g_logText.data(), 1, g_logText.size(), f);
        fclose(f);
    }

    static bool CopyToClipboard(const char* text, size_t len)
    {
        if (!text || !len) return false;
        if (!OpenClipboard(nullptr)) return false;
        EmptyClipboard();

        HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, len + 1);
        if (!h) { CloseClipboard(); return false; }

        void* dst = GlobalLock(h);
        if (dst) { memcpy(dst, text, len); ((char*)dst)[len] = 0; GlobalUnlock(h); }

        if (!SetClipboardData(CF_TEXT, h)) { GlobalFree(h); CloseClipboard(); return false; }
        CloseClipboard();
        return true;
    }

    static int CopyLogToClipboard(bool onlyFiltered)
    {
        std::string out;
        out.reserve(g_logText.size() + 64);

        const char* p = g_logText.c_str();
        const char* end = p + g_logText.size();
        int lines = 0;

        while (p < end) {
            const char* nl = (const char*)memchr(p, '\n', (size_t)(end - p));
            const size_t len = nl ? (size_t)(nl - p) : (size_t)(end - p);

            if (len && (!onlyFiltered || TypeMatch(p, len, g_logType))) {
                out.append(p, len);
                out.push_back('\r');
                out.push_back('\n');
                ++lines;
            }
            if (!nl) break;
            p = nl + 1;
        }

        if (out.empty()) return 0;
        if (!CopyToClipboard(out.data(), out.size())) return -1;
        return lines;
    }

    // ==========================================================
    //  ABAS
    // ==========================================================
    static void TabAimbot();
    static void TabTrigger();
    static void TabVisual();
    static void TabWeapon();
    static void TabDupe();
    static void TabMovement();
    static void TabUnlock();
    static void TabConta();
    static void TabMisc();
    static void TabLog();

    static int RiskLevelLocal()
    {
        if (!C.antiCheat) return 0;
        const bool muitoDinheiro = (C.currencyValue >= 999999) ||
            (C.accGold >= 999999) || (C.accCases >= 999999);
        if (C.unlockCurrency && muitoDinheiro) return 2;
        return 1;
    }

    static void RiskBanner()
    {
        if (!C.showRiskAlert) return;

        const int lvl = RiskLevelLocal();
        ImVec4 col;
        const char* txt;
        switch (lvl) {
        case 2:  col = ImVec4(0.90f, 0.25f, 0.25f, 1.f); txt = "RISCO ALTO - valores fora do padrao"; break;
        case 1:  col = ImVec4(0.90f, 0.70f, 0.20f, 1.f); txt = "RISCO MEDIO - anti-cheat ativo"; break;
        default: col = ImVec4(0.35f, 0.80f, 0.45f, 1.f); txt = "OK - sem risco aparente"; break;
        }

        static float blink = 0.f;
        blink += ImGui::GetIO().DeltaTime;
        const float a = (lvl == 2 && C.riskAlertBlink && ((int)(blink * 3.f) & 1)) ? 0.35f : 1.f;

        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(col.x, col.y, col.z, a));
        ImGui::TextUnformatted(txt);
        ImGui::PopStyleColor();
        ImGui::Separator();
    }

    // ========================================================== AIMBOT
    static void TabAimbot()
    {
        ImGui::Checkbox("Aimbot", &C.aimbot);
        ImGui::SameLine(); KeyButton("Tecla", &C.aimKey);
        ImGui::SliderFloat("FOV", &C.aimFov, 1.f, 60.f, "%.0f");
        ImGui::SliderFloat("Suavidade", &C.aimSmooth, 1.f, 20.f, "%.1f");
        ImGui::Combo("Osso", &C.aimBone, "Cabeca\0Peito\0Corpo\0");
        ImGui::Combo("Modo", &C.aimMode, "Segurar\0Alternar\0Auto\0");
        ImGui::Checkbox("So visivel", &C.aimVisible);
        ImGui::SameLine(); ImGui::Checkbox("Visao estrita", &C.aimStrictVis);
        ImGui::Checkbox("Ignorar time", &C.aimTeam);
        ImGui::SameLine(); ImGui::Checkbox("Rotacionar", &C.aimRotate);
        ImGui::Checkbox("Circulo de FOV", &C.showFovCircle);
        ImGui::SameLine(); ImGui::SliderFloat("Dist. max", &C.aimMaxDist, 10.f, 500.f, "%.0f");
        ImGui::Checkbox("Silent aim", &C.silentAim);
        ImGui::SameLine(); ImGui::Checkbox("Balas magicas", &C.magicBullets);
        ImGui::ColorEdit4("Cor do FOV", C.aimColor, ImGuiColorEditFlags_NoInputs);

        ImGui::Separator();
        ImGui::Checkbox("Alternar", &C.aimToggle);
        ImGui::SameLine(); ImGui::Checkbox("Ignorar time", &C.aimTeamCheck);
        ImGui::Checkbox("Checar visao", &C.aimVisCheck);
        ImGui::SameLine(); ImGui::Checkbox("Visao estrita", &C.aimStrictVis);
        ImGui::Combo("Selecao", &C.aimSelect, "Distancia\0Menor FOV\0Menor vida\0");
        ImGui::Checkbox("Auto fire", &C.aimAutoFire);
        ImGui::SameLine(); ImGui::SliderInt("Delay tiro (ms)", &C.aimFireDelayMs, 0, 300);
    }

    // ========================================================== TRIGGER
    static void TabTrigger()
    {
        ImGui::Checkbox("Triggerbot", &C.triggerbot);
        ImGui::SameLine(); KeyButton("Tecla", &C.triggerKey);
        ImGui::SliderFloat("Delay (ms)", &C.triggerDelay, 0.f, 500.f, "%.0f");
        ImGui::SliderFloat("Raio (px)", &C.triggerRadius, 0.f, 60.f, "%.0f");
    }

    // ========================================================== VISUAL
    static void TabVisual()
    {
        ImGui::Checkbox("ESP", &C.esp);
        ImGui::Separator();

        ImGui::Checkbox("Caixa", &C.espBox);
        ImGui::SameLine(); ImGui::Checkbox("Preencher", &C.espFill);
        ImGui::SameLine(); ImGui::Checkbox("Nome", &C.espName);
        ImGui::Checkbox("Vida (barra)", &C.espHealth);
        ImGui::SameLine(); ImGui::Checkbox("Vida (numero)", &C.espHealthNum);
        ImGui::Checkbox("Distancia", &C.espDistance);
        ImGui::SameLine(); ImGui::Checkbox("Linha", &C.espSnapline);
        ImGui::Checkbox("Esqueleto", &C.espSkeleton);
        ImGui::SameLine(); ImGui::Checkbox("Arma", &C.espWeapon);
        ImGui::Checkbox("NPCs", &C.espNpc);
        ImGui::SameLine(); ImGui::Checkbox("Ignorar time", &C.teamCheck);

        ImGui::Separator();
        ImGui::Checkbox("Checar visao", &C.espVisCheck);
        ImGui::SameLine(); ImGui::Checkbox("Linha de visao", &C.espVisLine);
        ImGui::SliderInt("LOS por frame", &C.losPerFrame, 1, 32);

        ImGui::Separator();
        ImGui::SliderFloat("Espessura", &C.espThickness, 0.5f, 5.0f, "%.1f");
        ImGui::SliderFloat("Dist. max", &C.espMaxDist, 10.0f, 1000.0f, "%.0f");
        ImGui::SliderFloat("Fonte", &C.espFontSize, 8.0f, 24.0f, "%.0f");
        ImGui::SliderFloat("Offset Y", &C.espOffY, -50.0f, 50.0f, "%.0f");
        ImGui::SliderFloat("Escala Y", &C.espScaleY, 0.5f, 2.0f, "%.2f");
        ImGui::SliderFloat("Altura do corpo", &C.espBodyH, 1.0f, 3.0f, "%.2f");

        ImGui::Separator();
        ImGui::ColorEdit4("Cor base", C.espColor, ImGuiColorEditFlags_NoInputs);
        ImGui::ColorEdit4("Cor da caixa", C.espBoxColor, ImGuiColorEditFlags_NoInputs);
        ImGui::ColorEdit4("Cor do texto", C.espTextColor, ImGuiColorEditFlags_NoInputs);
        ImGui::ColorEdit4("Cor do esqueleto", C.espSkelColor, ImGuiColorEditFlags_NoInputs);
        ImGui::ColorEdit4("Cor visivel", C.visColor, ImGuiColorEditFlags_NoInputs);
        ImGui::ColorEdit4("Cor nao visivel", C.noVisColor, ImGuiColorEditFlags_NoInputs);

        ImGui::Separator();
        ImGui::Checkbox("Mira", &C.crosshair);
        ImGui::SameLine(); ImGui::SliderFloat("Tam. mira", &C.crosshairSize, 1.f, 20.f, "%.1f");
        ImGui::SameLine(); ImGui::ColorEdit4("Cor mira", C.crosshairColor, ImGuiColorEditFlags_NoInputs);
        ImGui::Checkbox("Hitmarker", &C.hitmarker);
        ImGui::SameLine(); ImGui::Checkbox("Contador de FPS", &C.fps);
        ImGui::SliderFloat("FOV camera", &C.camFov, 60.f, 130.f, "%.0f");
    }

    // ========================================================== ARMA
    //  Helpers (nao mexer): todos checam antes de indexar.
    static const char* RawName(const char* const* list, int idx)
    {
        if (!list || idx < 0) return "";
        const char* s = list[idx];
        if (!s) return "";
        if (s[0] && s[1] && s[2] && s[3] == ' ' && s[4] == '-' && s[5] == ' ')
            return s + 6;                       // tira o prefixo "NNN - "
        return s;
    }

    static bool ComboList(const char* label, const char* const* list, int n, int* idx,
        const char* noneTxt)
    {
        bool changed = false;
        const char* cur = (list && n > 0 && *idx >= 0 && *idx < n && list[*idx]) ? list[*idx] : noneTxt;
        if (ImGui::BeginCombo(label, cur)) {
            if (ImGui::Selectable(noneTxt, *idx < 0)) { *idx = -1; changed = true; }
            for (int i = 0; i < n; ++i) {
                ImGui::PushID(i);
                const char* nm = (list && list[i]) ? list[i] : "?";
                if (ImGui::Selectable(nm, *idx == i)) { *idx = i; changed = true; }
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        return changed;
    }

    static bool WeaponCombo(const char* label, int* idx)
    {
        return ComboList(label, Dupe::WeaponList(), Dupe::WeaponCount(), idx, "(nenhuma)");
    }

    static bool AttachCombo(const char* label, int slot, int* idx)
    {
        return ComboList(label, Dupe::AttachList(slot), Dupe::AttachCount(slot), idx, "(nenhum)");
    }

    static void SetName(char* dst, size_t n, const char* src)
    {
        if (!dst || !n) return;
        if (src && *src) { strncpy_s(dst, n, src, _TRUNCATE); C.forceNames = true; }
        else dst[0] = 0;
    }

    static void LimparSelecao()
    {
        C.selWeapon = -1;
        C.selCamo = -1;
        C.selChar = -1;
        C.selAttach[0] = C.selAttach[1] = C.selAttach[2] = -1;
        C.slotWeapon[0] = C.slotWeapon[1] = C.slotWeapon[2] = -1;
        C.weaponName[0] = C.skinName[0] = C.attachName[0] = C.charName[0] = 0;
        Log("[menu] selecao limpa");
    }

    // ---------------------------------------------------------- secoes
    static void SecBuild()
    {
        if (ImGui::Button("BUILD (coletar armas/skins/anexos)", ImVec2(300, 0)))
            Unlock::BuildAllNow();
        ImGui::SameLine();
        if (Dupe::WeaponCount() > 0)
            ImGui::TextColored(ImVec4(0.35f, 0.80f, 0.45f, 1.f), "OK: %d", Dupe::WeaponCount());
        else
            ImGui::TextColored(ImVec4(0.90f, 0.45f, 0.45f, 1.f), "vazio - aperte BUILD");
    }

    static void SecMods()
    {
        ImGui::Separator();
        ImGui::Checkbox("Sem recuo (no recoil)", &C.noRecoil);
        ImGui::SameLine(); ImGui::Checkbox("Sem dispersao", &C.noSpread);
        ImGui::Checkbox("Municao infinita", &C.infAmmo);
        ImGui::SameLine(); ImGui::Checkbox("Sem recarregar", &C.noReload);
        ImGui::Checkbox("Tiro rapido", &C.rapidFire);
    }

    static void SecSkin()
    {
        ImGui::Separator();
        ImGui::Text("Skin / camo");
        ImGui::Checkbox("Skin changer", &C.skinChanger);
        ImGui::SameLine(); ImGui::Checkbox("Aplicar selecao (a cada 4s)", &C.applySelection);

        ComboList("Skin / camo", Dupe::CamoList(), Dupe::CamoCount(), &C.selCamo, "(nenhuma)");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(160);
        ImGui::SliderInt("Skin (indice)", &C.skinIndex, 0, 64);
        if (Dupe::CamoCount() == 0) ImGui::TextDisabled("(sem camos - aperte BUILD)");

        if (ImGui::Button("Aplicar skin/camo agora", ImVec2(210, 0))) {
            C.skinChanger = true;
            C.applySelection = true;
            SetName(C.skinName, sizeof(C.skinName), RawName(Dupe::CamoList(), C.selCamo));
            Log("[menu] skin aplicar: camo=%d \"%s\"", C.selCamo, C.skinName);
        }
    }

    static void SecPersonagem()
    {
        ImGui::Separator();
        ImGui::Text("Personagem");
        ComboList("Personagem", Dupe::CharList(), Dupe::CharCount(), &C.selChar, "(nenhum)");
        ImGui::SameLine();
        if (ImGui::Button("Aplicar personagem", ImVec2(180, 0))) {
            C.unlockChars = true;
            SetName(C.charName, sizeof(C.charName), RawName(Dupe::CharList(), C.selChar));
            Log("[menu] personagem aplicar: idx=%d \"%s\"", C.selChar, C.charName);
        }
        if (Dupe::CharCount() == 0) ImGui::TextDisabled("(sem personagens - aperte BUILD)");
    }

    static void SecArmas()
    {
        ImGui::Separator();
        ImGui::Text("Arma e anexos (por nome)");
        WeaponCombo("Arma selecionada", &C.selWeapon);

        static const char* kSlotTxt[3] = { "1 - Primaria", "2 - Secundaria", "3 - Gadget" };
        if (C.loadoutSlot < 0 || C.loadoutSlot > 2) C.loadoutSlot = 0;
        if (ImGui::BeginCombo("Slot alvo", kSlotTxt[C.loadoutSlot])) {
            for (int s = 0; s < 3; ++s)
                if (ImGui::Selectable(kSlotTxt[s], C.loadoutSlot == s)) C.loadoutSlot = s;
            ImGui::EndCombo();
        }

        WeaponCombo("Slot 1", &C.slotWeapon[0]);
        WeaponCombo("Slot 2", &C.slotWeapon[1]);
        WeaponCombo("Slot 3", &C.slotWeapon[2]);
    }

    static void SecAnexos()
    {
        ImGui::TextDisabled("Anexos:");
        AttachCombo("Mira", 0, &C.selAttach[0]);
        AttachCombo("Acessorio", 1, &C.selAttach[1]);
        AttachCombo("Cano", 2, &C.selAttach[2]);
    }

    static void SecBotoes()
    {
        if (ImGui::Button("Aplicar loadout agora", ImVec2(210, 0))) {
            C.applySelection = true;
            C.fillLoadoutSlots = true;
            C.applySlots = true;
            SetName(C.weaponName, sizeof(C.weaponName), Dupe::WeaponName(C.selWeapon));
            SetName(C.attachName, sizeof(C.attachName), RawName(Dupe::AttachList(0), C.selAttach[0]));
            Log("[menu] loadout aplicar: arma=%d slot=%d anexoMira=%d",
                C.selWeapon, C.loadoutSlot, C.selAttach[0]);
        }
        ImGui::SameLine();
        if (ImGui::Button("Aplicar tudo (3 slots + anexos)", ImVec2(230, 0))) {
            C.applySelection = true;
            C.fillLoadoutSlots = true;
            C.applySlots = true;
            C.unlockAll = true; C.unlockSkins = true; C.unlockAttachments = true;
            Log("[menu] aplicar tudo");
        }
        ImGui::SameLine();
        if (ImGui::Button("Limpar selecao", ImVec2(130, 0))) LimparSelecao();
    }

    static void SecNomes()
    {
        ImGui::Separator();
        ImGui::Checkbox("Liberar customizacao", &C.gunCustomizeUnlock);
        ImGui::SameLine(); ImGui::Checkbox("Todos os slots", &C.gunCustomizeAllSlots);
        ImGui::Checkbox("Forcar nomes", &C.forceNames);
        ImGui::InputText("Nome arma", C.weaponName, sizeof(C.weaponName));
        ImGui::InputText("Nome skin", C.skinName, sizeof(C.skinName));
        ImGui::InputText("Nome anexo", C.attachName, sizeof(C.attachName));
        ImGui::InputText("Nome personagem", C.charName, sizeof(C.charName));
    }

    // ---------------------------------------------------------- aba
    static void TabWeapon()
    {
        ARMA_SEC("build", SecBuild);
        ARMA_SEC("mods", SecMods);
        ARMA_SEC("skin", SecSkin);
        ARMA_SEC("personagem", SecPersonagem);
        ARMA_SEC("arma", SecArmas);
        ARMA_SEC("anexos", SecAnexos);
        ARMA_SEC("botoes", SecBotoes);
        ARMA_SEC("nomes", SecNomes);
        g_sec = "-";
    }

    // ========================================================== DUPE / BUILD
    static void TabDupe()
    {
        ImGui::Text("Coleta de armas / camos / personagens");
        ImGui::TextDisabled("1) Build  2) escolha na aba Arma  3) Aplicar loadout");
        ImGui::Separator();

        if (ImGui::Button("BUILD (coletar agora)", ImVec2(230, 0))) {
            Unlock::BuildAllNow();
            Log("[menu] dupe: build pedida");
        }
        ImGui::SameLine();
        if (Dupe::Building())
            ImGui::TextColored(ImVec4(0.95f, 0.80f, 0.40f, 1.f), "construindo...");
        else if (Dupe::WeaponCount() > 0)
            ImGui::TextColored(ImVec4(0.35f, 0.80f, 0.45f, 1.f), "OK: %d armas", Dupe::WeaponCount());
        else
            ImGui::TextColored(ImVec4(0.90f, 0.45f, 0.45f, 1.f), "vazio - aperte Build");

        ImGui::Separator();
        ImGui::Text("armas=%d  camos=%d  chars=%d  epoch=%d  stage=%d",
            Dupe::WeaponCount(), Dupe::CamoCount(), Dupe::CharCount(),
            (int)Dupe::g_epoch.load(), Dupe::Stage());

        ImGui::Separator();
        ImGui::Text("Anexos:");
        for (int s = 0; s < 3; ++s) {
            static const char* kn[3] = { "Mira", "Acessorios", "Cano" };
            ImGui::Text("%s: %d", kn[s], Dupe::AttachCount(s));
        }

        ImGui::Separator();
        ImGui::Text("Armas:");
        ImGui::BeginChild("##dupeWpn", ImVec2(0, 220), true);
        const int wc = Dupe::WeaponCount();
        for (int i = 0; i < wc; ++i) {
            ImGui::PushID(1000 + i);
            if (ImGui::Selectable(Dupe::WeaponName(i), i == C.selWeapon)) C.selWeapon = i;
            ImGui::PopID();
        }
        if (!wc) ImGui::TextDisabled("(sem armas - entre numa partida e aperte Build)");
        ImGui::EndChild();
    }

    // ========================================================== MOVIMENTO
    static void TabMovement()
    {
        ImGui::Checkbox("Godmode", &C.godmode);
        ImGui::SameLine(); ImGui::SliderFloat("Vida", &C.godmodeValue, 1.f, 1000.f, "%.0f");
        ImGui::Separator();
        ImGui::Checkbox("Speed hack", &C.speedHack);
        ImGui::SameLine(); ImGui::SliderFloat("Velocidade", &C.speedValue, 1.f, 30.f, "%.1f");
        ImGui::Checkbox("Voar", &C.fly);
        ImGui::SameLine(); ImGui::SliderFloat("Vel. voo", &C.flySpeed, 1.f, 30.f, "%.1f");
        ImGui::Checkbox("Super pulo", &C.superJump);
        ImGui::SameLine(); ImGui::SliderFloat("Forca pulo", &C.jumpPower, 1.f, 20.f, "%.1f");
        ImGui::Checkbox("Teleporte", &C.teleport);
        ImGui::Separator();
        ImGui::Checkbox("Bhop", &C.bhop);
        ImGui::SameLine(); KeyButton("Tecla", &C.bhopKey);
        ImGui::Checkbox("Manter velocidade", &C.bhopKeepSpeed);
        ImGui::Checkbox("Bhop (so fora do menu)", &C.bhopSkipMenu);
        ImGui::SliderInt("Modo", &C.bhopMode, 0, 2);
        ImGui::SliderInt("Hold (ms)", &C.bhopHoldMs, 10, 300);
        ImGui::SliderInt("Re-press (ms)", &C.bhopRePressMs, 10, 300);
        ImGui::SliderFloat("Vel. pulo", &C.bhopJumpSpeed, 0.0f, 3.f, "%.2f");
        ImGui::Separator();
        ImGui::Checkbox("Atirar no ar", &C.shootInAir);
        ImGui::SameLine(); KeyButton("Tecla", &C.airFireKey);
        ImGui::SliderInt("Delay tiro (ms)", &C.airFireDelayMs, 0, 500);
    }

    // ========================================================== UNLOCK
    static void TabUnlock()
    {
        ImGui::Checkbox("Unlock all", &C.unlockAll);
        ImGui::Checkbox("Armas", &C.unlockWeapons);
        ImGui::SameLine(); ImGui::Checkbox("Skins", &C.unlockSkins);
        ImGui::SameLine(); ImGui::Checkbox("Anexos", &C.unlockAttachments);
        ImGui::Checkbox("Personagens", &C.unlockChars);
        ImGui::SameLine(); ImGui::Checkbox("Moeda", &C.unlockCurrency);
        ImGui::Separator();
        ImGui::Checkbox("Preencher slots", &C.fillLoadoutSlots);
        ImGui::SameLine(); ImGui::Checkbox("Aplicar slots", &C.applySlots);
        ImGui::SliderFloat("Intervalo", &C.unlockInterval, 0.05f, 5.f, "%.2f");
        ImGui::Separator();
        if (ImGui::Button("Aplicar tudo agora")) {
            C.unlockAll = true; C.unlockWeapons = true; C.unlockSkins = true;
            C.unlockAttachments = true; C.unlockChars = true;
            C.fillLoadoutSlots = true; C.applySlots = true;
            Log("[menu] unlock: tudo marcado");
        }
    }

    // ========================================================== CONTA
    static void TabConta()
    {
        ImGui::Text("Carteira / conta");
        ImGui::TextDisabled("Alvo real: accountInfo -> customInfo -> bfAccountInfo");
        ImGui::Separator();

        ImGui::Checkbox("So carteira (evita quebrar FishNet)", &C.moneyOnlyWallet);
        ImGui::SameLine();
        ImGui::Checkbox("Auto (trava)", &C.accAuto);

        ImGui::TextColored(ImVec4(0.55f, 0.85f, 1.f, 1.f),
            "Creditos reais (JSON da conta): %lld", Unlock::MoneyReal());

        ImGui::PushItemWidth(150);

        ImGui::InputInt("Creditos", &C.currencyValue);
        ImGui::SameLine();
        if (ImGui::Button("Aplicar##cr")) {
            const long long v = (long long)C.currencyValue;
            int n = 0;
            n += Unlock::MoneyLockByName("CreditsManager", "LDOGMJMLIOP", v);
            n += Unlock::MoneyLockByName("AccountManager", "money", v);
            Log("[conta] creditos=%d -> %d campo(s)", C.currencyValue, n);
        }

        ImGui::InputInt("Ouro", &C.accGold);
        ImGui::SameLine();
        if (ImGui::Button("Aplicar##ou")) {
            const long long v = (long long)C.accGold;
            int n = 0;
            n += Unlock::MoneyLockByName("AccountManager", "gold", v);
            n += Unlock::MoneyLockByName("CamoCaseManager", "goldCases", v);
            n += Unlock::MoneyLockByName("LoadoutManager", "goldAmount", v);
            n += Unlock::MoneyLockByName("LoadoutAndShop", "goldAmount", v);
            Log("[conta] ouro=%d -> %d campo(s)", C.accGold, n);
        }

        ImGui::InputInt("Nivel (xp)", &C.unlockLevel);
        ImGui::SameLine();
        if (ImGui::Button("Aplicar##lv")) {
            const int n = Unlock::MoneyLockByName("AccountManager", "xp", (long long)C.unlockLevel);
            Log("[conta] xp=%d -> %d alvo(s)", C.unlockLevel, n);
        }

        ImGui::InputInt("Caixas (cases)", &C.accCases);
        ImGui::SameLine();
        if (ImGui::Button("Aplicar##cs")) {
            const int n = Unlock::MoneyLockByName("AccountManager", "cases", (long long)C.accCases);
            Log("[conta] cases=%d -> %d alvo(s)", C.accCases, n);
        }

        ImGui::PopItemWidth();

        ImGui::Separator();

        if (ImGui::Button("FORCAR todos os espelhos")) {
            C.moneyOnlyWallet = true;
            const int n = Unlock::ForceMirrors((long long)C.currencyValue, 0);
            Log("[conta] ForceMirrors(todos): %d campo(s)", n);
        }
        ImGui::SameLine();
        if (ImGui::Button("FORCAR so travados")) {
            const int n = Unlock::ForceMirrors((long long)C.currencyValue, 1);
            Log("[conta] ForceMirrors(travados): %d campo(s)", n);
        }
        ImGui::SameLine();
        if (ImGui::Button("Ver espelhos no log")) Unlock::DumpMirrors();

        ImGui::Separator();

        if (ImGui::Button("Forcar loja (ShopForceAll)")) {
            Unlock::ShopForceAll();
            Log("[conta] ShopForceAll()");
        }
        ImGui::SameLine();
        if (ImGui::Button("Dump loja no log")) Unlock::DumpShop();
        ImGui::SameLine();
        if (ImGui::Button("Dump CREDITOS no log")) Unlock::DumpCredits();

        ImGui::Separator();

        if (ImGui::Button("Re-detectar alvos (walk)")) { C.moneyRescan = true; }
        ImGui::SameLine();
        if (ImGui::Button("Atualizar valores")) Unlock::MoneyRefresh();
        ImGui::SameLine();
        if (ImGui::Button("Destravar tudo")) Unlock::MoneyUnlockAll();

        ImGui::Separator();
        ImGui::Text("Alvos do walk: %d", Unlock::MoneyCount());
        if (!Unlock::MoneyReady())
            ImGui::TextDisabled("(vazio - entre numa partida ou clique Re-detectar)");

        ImGui::BeginChild("##alvos", ImVec2(0, 190), true);
        const int n = Unlock::MoneyCount();
        for (int i = 0; i < n; ++i) {
            ImGui::PushID(i);
            bool lk = Unlock::MoneyLocked(i);
            if (ImGui::Checkbox("##lk", &lk)) Unlock::MoneySetLock(i, lk);
            ImGui::SameLine();
            const char* mn = Unlock::MoneyName(i);
            ImGui::Text("%-38s", mn ? mn : "?");
            ImGui::SameLine();
            long long v = Unlock::MoneyValue(i);
            ImGui::SetNextItemWidth(120);
            if (ImGui::InputScalar("##v", ImGuiDataType_S64, &v)) Unlock::MoneySet(i, v);
            ImGui::PopID();
        }
        ImGui::EndChild();

        ImGui::Checkbox("Gravar tambem em PlayerPrefs", &C.moneyWritePrefs);
        static const char* kModes[] = { "Todo tick", "Uma vez por partida", "Por intervalo" };
        ImGui::Combo("Modo", &C.moneyMode, kModes, IM_ARRAYSIZE(kModes));
        if (C.moneyMode == 2) {
            ImGui::SameLine();
            ImGui::SliderInt("ms", &C.moneyIntervalMs, 200, 10000);
        }
        if (ImGui::Button("Aplicar agora")) C.moneyApplyNow = true;
    }

    // ========================================================== MISC
    static void TabMisc()
    {
        ImGui::Checkbox("Watermark", &C.watermark);
        ImGui::SameLine(); ImGui::Checkbox("Grafico de FPS", &C.showFpsGraph);
        ImGui::Checkbox("Aviso de risco", &C.showRiskAlert);
        ImGui::SameLine(); ImGui::Checkbox("Piscar", &C.riskAlertBlink);
        ImGui::Separator();
        ImGui::Checkbox("Anti-cheat (trava valores)", &C.antiCheat);
        ImGui::Checkbox("Bloquear fora de zona", &C.blockedNoZone);
        ImGui::SameLine(); ImGui::Checkbox("Bloquear unlock", &C.blockedUnlock);
        ImGui::SliderInt("Stage", &C.stage, 0, 5);
        ImGui::Separator();
        ImGui::Checkbox("Log do probe", &C.probeLog);
        ImGui::SameLine(); ImGui::Checkbox("Log de beat", &C.beatLog);
        ImGui::Checkbox("Verbose unlock", &C.verboseUnlock);
        ImGui::SameLine(); ImGui::SliderInt("Linhas watch", &C.watchMaxLines, 1, 40);
        ImGui::Checkbox("Dump automatico", &C.dumpOnce);
        ImGui::SameLine(); ImGui::SliderInt("Intervalo dump (ms)", &C.dumpIntervalMs, 500, 30000);
        ImGui::Separator();
        KeyButton("Menu", &C.menuKey);
        ImGui::SameLine(); KeyButton("Panico", &C.panicKey);
        ImGui::SameLine(); KeyButton("Salvar cfg", &C.saveKey);
        ImGui::SameLine(); KeyButton("Descarregar", &C.unloadKey);
        if (ImGui::Button("Salvar config agora")) ConfigIO::Save();
        ImGui::SameLine();
        if (ImGui::Button("Carregar config")) ConfigIO::Load();
    }

    // ========================================================== LOG
    static void TabLog()
    {
        LogRefresh();

        if (ImGui::Button("Abrir pasta do log")) OpenLogFolder();
        ImGui::SameLine();
        if (ImGui::Button("Copiar tudo")) {
            const int n = CopyLogToClipboard(false);
            Log("[log] copiado: %d linha(s)%s", n, n < 0 ? " (FALHOU)" : "");
        }
        ImGui::SameLine();
        if (ImGui::Button("Copiar filtrado")) {
            const int n = CopyLogToClipboard(true);
            Log("[log] copiado (filtro): %d linha(s)%s", n, n < 0 ? " (FALHOU)" : "");
        }

        if (ImGui::Button("Salvar copia")) SaveLogCopy();
        ImGui::SameLine();
        if (ImGui::Button("Ver espelhos")) Unlock::DumpMirrors();
        ImGui::SameLine();
        ImGui::Checkbox("Auto-scroll", &C.logAutoScroll);

        ImGui::SetNextItemWidth(240);
        ImGui::InputText("Filtro", C.logFilter, sizeof(C.logFilter));

        ImGui::TextDisabled("%s", LogPath());
        if (g_logErr) ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.4f, 1.f), "falha ao ler o log");

        static const char* kTypes[] = { "Todos", "Info", "Ok", "Warn", "Erro", "Money", "Shop" };
        for (int i = 0; i < 7; ++i) {
            if (i) ImGui::SameLine();
            const bool on = (g_logType == i);
            if (on) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.45f, 0.25f, 1.f));
            if (ImGui::SmallButton(kTypes[i])) g_logType = i;
            if (on) ImGui::PopStyleColor();
        }

        ImGui::Separator();
        ImGui::BeginChild("##logtxt", ImVec2(0, 0), true);

        const char* p = g_logText.c_str();
        const char* end = p + g_logText.size();
        while (p < end) {
            const char* nl = (const char*)memchr(p, '\n', (size_t)(end - p));
            const size_t len = nl ? (size_t)(nl - p) : (size_t)(end - p);
            if (len && TypeMatch(p, len, g_logType)) {
                if (LineHas(p, len, "[money]") || LineHas(p, len, "[mirror]") ||
                    LineHas(p, len, "[credit]") || LineHas(p, len, "[shop]") ||
                    LineHas(p, len, "[loja]") || LineHas(p, len, "[conta]"))
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.85f, 1.f, 1.f));
                else if (LineHas(p, len, "[err]") || LineHas(p, len, "[erro]") ||
                    LineHas(p, len, "!!!!"))
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.45f, 0.45f, 1.f));
                else if (LineHas(p, len, "[warn]"))
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.80f, 0.40f, 1.f));
                else
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.85f, 0.88f, 1.f));

                ImGui::TextUnformatted(p, p + len);
                ImGui::PopStyleColor();
            }
            if (!nl) break;
            p = nl + 1;
        }

        if (C.logAutoScroll) ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();
    }

    // ========================================================== DRAW
    //  Cada aba em __try proprio; EndTabItem/EndTabBar/End SEMPRE rodam.
    //  O log de crash e' THROTTLED (so quando muda) -> sem inundar o arquivo.
#define BFI_SAFE_TAB(_label, _fn)                                   \
        if (ImGui::BeginTabItem(_label)) {                          \
            __try { g_sec = "-"; _fn(); }                           \
            __except (EXCEPTION_EXECUTE_HANDLER) {                  \
                static char _last[96] = "";                         \
                char _t[96];                                        \
                sprintf_s(_t, "%s|%s", _label, g_sec);              \
                if (strcmp(_t, _last) != 0) {                       \
                    strcpy_s(_last, _t);                            \
                    Log("[crash] Tab %s secao=%s", _label, g_sec);  \
                }                                                   \
            }                                                       \
            __try { ImGui::EndTabItem(); }                          \
            __except (EXCEPTION_EXECUTE_HANDLER) {                  \
                Log("[crash] EndTabItem %s", _label);               \
            }                                                       \
        }

    void Draw()
    {
        if (!g_open) return;
        ApplyStyleOnce();

        ImGui::SetNextWindowSize(ImVec2(780, 880), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2(40, 40), ImGuiCond_FirstUseEver);

        if (!ImGui::Begin("bfi - Bullet Force", &g_open)) {
            ImGui::End();
            return;
        }

        RiskBanner();

        if (ImGui::BeginTabBar("##tabs")) {
            BFI_SAFE_TAB("Aimbot", TabAimbot);
            BFI_SAFE_TAB("Trigger", TabTrigger);
            BFI_SAFE_TAB("Visual", TabVisual);
            BFI_SAFE_TAB("Arma", TabWeapon);
            BFI_SAFE_TAB("Dupe", TabDupe);
            BFI_SAFE_TAB("Movimento", TabMovement);
            BFI_SAFE_TAB("Unlock", TabUnlock);
            BFI_SAFE_TAB("Conta", TabConta);
            BFI_SAFE_TAB("Misc", TabMisc);
            BFI_SAFE_TAB("Log", TabLog);

            __try { ImGui::EndTabBar(); }
            __except (EXCEPTION_EXECUTE_HANDLER) { Log("[crash] EndTabBar"); }
        }

        ImGui::Separator();
        ImGui::TextDisabled("Insert = menu | Delete = panico | F5 = salvar | F8 = descarregar");

        __try { ImGui::End(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { Log("[crash] End"); }
    }

#undef BFI_SAFE_TAB
#undef ARMA_SEC

} // namespace Menu
// ==== FIM menu.cpp ====