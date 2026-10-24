// ============================================================
//  menu.cpp - bfi / Bullet Force (ImGui)
//  v43: aba Conta com espelhos DIRETOS (ForceMirrors/DumpMirrors)
//       + "So carteira" ligado ao forcar (nao quebra o FishNet)
// ============================================================
#include "common.h"
#include "config.h"
#include "features.h"
#include "unlock.h"
#include "dupe.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <imgui.h>
#include <shellapi.h>
#include <windows.h>

#pragma comment(lib, "shell32.lib")

namespace Unlock {
    void        DumpShop();
    int         ShopForceAll(long long money, long long gold);
    int         ForceMirrors(long long credits, long long gold);
    void        DumpMirrors();

    int         MoneyCount();
    const char* MoneyName(int i);
    long long   MoneyValue(int i);
    void        MoneySet(int i, long long v);
    bool        MoneyLocked(int i);
    void        MoneySetLock(int i, bool on);
    int         MoneyLockByName(const char* cls, const char* fld, long long v);
    void        MoneyUnlockAll();
    void        MoneyRefresh();
    bool        MoneyReady();
}

namespace Menu {

    bool& g_open = C.menuOpen;

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
        static uint32_t styledGen = 0;
        const uint32_t gen = Hooks::g_imguiGeneration.load(std::memory_order_acquire);
        if (gen == styledGen) return;
        if (!ImGui::GetCurrentContext()) return;
        ApplyStyle();
        styledGen = gen;
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
    static int         g_logType = 0;   // 0=todos 1=info 2=ok 3=warn 4=erro 5=money 6=shop

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

    static bool TypeMatch(const char* line, int type)
    {
        if (type == 0) return true;
        if (type == 5) return strstr(line, "[money]") != nullptr ||
            strstr(line, "[mirror]") != nullptr ||
            strstr(line, "[conta]") != nullptr;
        if (type == 6) return strstr(line, "[shop]") != nullptr;
        if (type == 1) return strstr(line, "[info]") != nullptr;
        if (type == 2) return strstr(line, "[ok]") != nullptr;
        if (type == 3) return strstr(line, "[warn]") != nullptr || strstr(line, "[w]") != nullptr;
        if (type == 4) return strstr(line, "[err]") != nullptr || strstr(line, "[erro]") != nullptr;
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

    // ----------------------------------------------------------
    //  COPIAR - via Win32 (nao depende do backend do ImGui)
    // ----------------------------------------------------------
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

            if (len && (!onlyFiltered || TypeMatch(p, g_logType))) {
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
    static void TabMovement();
    static void TabUnlock();
    static void TabConta();
    static void TabMisc();
    static void TabLog();

    static void RiskBanner()
    {
        if (!C.showRiskAlert) return;

        const int lvl = AntiCheat::RiskLevel();
        ImVec4 col;
        const char* txt;
        switch (lvl) {
        case 2:  col = ImVec4(0.90f, 0.25f, 0.25f, 1.f); txt = "RISCO ALTO - anti-cheat ativo"; break;
        case 1:  col = ImVec4(0.90f, 0.70f, 0.20f, 1.f); txt = "RISCO MEDIO - valores fora do normal"; break;
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
    }

    // ========================================================== TRIGGER
    static void TabTrigger()
    {
        ImGui::Checkbox("Triggerbot", &C.triggerbot);
        ImGui::SameLine(); KeyButton("Tecla", &C.triggerKey);
        ImGui::SliderFloat("Delay (s)", &C.triggerDelay, 0.f, 0.5f, "%.3f");
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
    static void TabWeapon()
    {
        ImGui::Checkbox("Skin changer", &C.skinChanger);
        ImGui::SameLine(); ImGui::SliderInt("Skin", &C.skinIndex, 0, 64);
        ImGui::Checkbox("Aplicar selecao (a cada 4s)", &C.applySelection);

        const int wc = Dupe::WeaponCount();
        if (wc > 0) {
            if (C.selWeapon >= wc) C.selWeapon = 0;
            if (ImGui::BeginCombo("Arma", Dupe::WeaponName(C.selWeapon))) {
                for (int i = 0; i < wc; ++i)
                    if (ImGui::Selectable(Dupe::WeaponName(i), i == C.selWeapon))
                        C.selWeapon = i;
                ImGui::EndCombo();
            }
        }
        else {
            ImGui::TextDisabled("(armas: entre numa partida)");
        }

        const int cc = Dupe::CamoCount();
        const char* const* cams = Dupe::CamoList();
        if (cc > 0 && cams) {
            if (C.selCamo >= cc) C.selCamo = 0;
            const char* cur = cams[C.selCamo] ? cams[C.selCamo] : "?";
            if (ImGui::BeginCombo("Skin/pattern", cur)) {
                for (int i = 0; i < cc; ++i)
                    if (ImGui::Selectable(cams[i] ? cams[i] : "?", i == C.selCamo))
                        C.selCamo = i;
                ImGui::EndCombo();
            }
        }

        const int hc = Dupe::CharCount();
        const char* const* chrs = Dupe::CharList();
        if (hc > 0 && chrs) {
            if (C.selChar >= hc) C.selChar = 0;
            const char* cur = chrs[C.selChar] ? chrs[C.selChar] : "?";
            if (ImGui::BeginCombo("Personagem", cur)) {
                for (int i = 0; i < hc; ++i)
                    if (ImGui::Selectable(chrs[i] ? chrs[i] : "?", i == C.selChar))
                        C.selChar = i;
                ImGui::EndCombo();
            }
        }

        ImGui::Separator();
        ImGui::SliderInt("Slot", &C.loadoutSlot, 0, 2);
        ImGui::SliderInt("Slot 1", &C.slotWeapon[0], -1, 64);
        ImGui::SliderInt("Slot 2", &C.slotWeapon[1], -1, 64);
        ImGui::SliderInt("Slot 3", &C.slotWeapon[2], -1, 64);

        if (ImGui::Button("Aplicar loadout agora")) {
            C.applySelection = true;
            Log("[menu] loadout marcado para aplicar (lista=%d slot=%d)", C.selWeapon, C.loadoutSlot);
        }
    }

    // ========================================================== MOVIMENTO
    static void TabMovement()
    {
        ImGui::Checkbox("Godmode", &C.godmode);
        ImGui::SameLine(); ImGui::SliderFloat("Vida", &C.godmodeValue, 1.f, 1000.f, "%.0f");
        ImGui::Separator();
        ImGui::Checkbox("Bhop", &C.bhopSkipMenu);
        ImGui::SliderInt("Modo", &C.bhopMode, 0, 2);
        ImGui::SliderInt("Hold (ms)", &C.bhopHoldMs, 10, 300);
        ImGui::SliderInt("Re-press (ms)", &C.bhopRePressMs, 10, 300);
        ImGui::SliderFloat("Vel. pulo", &C.bhopJumpSpeed, 0.5f, 2.f, "%.2f");
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
        ImGui::TextDisabled("Entre numa partida. Use os espelhos DIRETOS abaixo.");
        ImGui::Separator();

        ImGui::Checkbox("So carteira (evita quebrar FishNet)", &C.moneyOnlyWallet);
        ImGui::SameLine();
        ImGui::Checkbox("Auto (trava)", &C.accAuto);

        ImGui::PushItemWidth(150);

        ImGui::InputInt("Creditos", &C.currencyValue);
        ImGui::SameLine();
        if (ImGui::Button("Aplicar##cr")) {
            int n = Unlock::ForceMirrors((long long)C.currencyValue, (long long)C.accGold);
            n += Unlock::MoneyLockByName("BF_AccountInfo", "money", (long long)C.currencyValue);
            Log("[conta] creditos=%d -> %d campo(s)", C.currencyValue, n);
        }

        ImGui::InputInt("Ouro", &C.accGold);
        ImGui::SameLine();
        if (ImGui::Button("Aplicar##ou")) {
            int n = Unlock::ForceMirrors((long long)C.currencyValue, (long long)C.accGold);
            n += Unlock::MoneyLockByName("BF_AccountInfo", "gold", (long long)C.accGold);
            n += Unlock::MoneyLockByName("LoadoutManager", "goldAmount", (long long)C.accGold);
            n += Unlock::MoneyLockByName("Sidebar", "goldAmount", (long long)C.accGold);
            Log("[conta] ouro=%d -> %d campo(s)", C.accGold, n);
        }

        ImGui::InputInt("Nivel (xp)", &C.unlockLevel);
        ImGui::SameLine();
        if (ImGui::Button("Aplicar##lv")) {
            const int n = Unlock::MoneyLockByName("BF_AccountInfo", "xp", (long long)C.unlockLevel);
            Log("[conta] nivel=%d -> %d alvo(s)", C.unlockLevel, n);
        }

        ImGui::InputInt("Caixas (cases)", &C.accCases);
        ImGui::SameLine();
        if (ImGui::Button("Aplicar##cs")) {
            const int n = Unlock::MoneyLockByName("BF_AccountInfo", "cases_CREDIT", (long long)C.accCases);
            Log("[conta] cases=%d -> %d alvo(s)", C.accCases, n);
        }

        ImGui::PopItemWidth();

        ImGui::Separator();

        // ---- ESPELHOS DIRETOS (nao dependem do walk) ----
        if (ImGui::Button("FORCAR espelhos (creditos/ouro)")) {
            C.moneyOnlyWallet = true;
            const int n = Unlock::ForceMirrors((long long)C.currencyValue, (long long)C.accGold);
            Log("[conta] espelhos diretos: %d campo(s)", n);
        }
        ImGui::SameLine();
        if (ImGui::Button("Ver espelhos no log")) Unlock::DumpMirrors();

        ImGui::Separator();

        if (ImGui::Button("Re-detectar alvos (walk)")) { C.moneyRescan = true; }
        ImGui::SameLine();
        if (ImGui::Button("Atualizar valores")) Unlock::MoneyRefresh();
        ImGui::SameLine();
        if (ImGui::Button("Destravar tudo")) Unlock::MoneyUnlockAll();

        if (ImGui::Button("Dump loja no log")) Unlock::DumpShop();

        ImGui::Separator();
        ImGui::Text("Alvos do walk: %d (a maioria e' lixo)", Unlock::MoneyCount());
        if (!Unlock::MoneyReady())
            ImGui::TextDisabled("(vazio - entre numa partida ou clique Re-detectar)");

        ImGui::BeginChild("##alvos", ImVec2(0, 190), true);
        const int n = Unlock::MoneyCount();
        for (int i = 0; i < n; ++i) {
            ImGui::PushID(i);
            bool lk = Unlock::MoneyLocked(i);
            if (ImGui::Checkbox("##lk", &lk)) Unlock::MoneySetLock(i, lk);
            ImGui::SameLine();
            ImGui::Text("%-38s", Unlock::MoneyName(i));
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
        ImGui::SameLine();
        if (ImGui::Button("Salvar copia")) SaveLogCopy();
        ImGui::SameLine();
        if (ImGui::Button("Ver espelhos")) Unlock::DumpMirrors();
        ImGui::SameLine();
        ImGui::Checkbox("Auto-scroll", &C.logAutoScroll);

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
            if (len && TypeMatch(p, g_logType)) {
                if (strstr(p, "[money]") || strstr(p, "[mirror]") ||
                    strstr(p, "[shop]") || strstr(p, "[conta]"))
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.55f, 0.85f, 1.f, 1.f));
                else if (strstr(p, "[err]") || strstr(p, "[erro]") || strstr(p, "!!!!"))
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.45f, 0.45f, 1.f));
                else if (strstr(p, "[warn]"))
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
    //  Menu::Render() fica no features.cpp - nao definir aqui.
    void Draw()
    {
        if (!g_open) return;
        ApplyStyleOnce();

        ImGui::SetNextWindowSize(ImVec2(690, 680), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2(60, 60), ImGuiCond_FirstUseEver);

        if (!ImGui::Begin("bfi - Bullet Force", &g_open)) {
            ImGui::End();
            return;
        }

        RiskBanner();

        if (ImGui::BeginTabBar("##tabs")) {
            if (ImGui::BeginTabItem("Aimbot")) { TabAimbot();   ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Trigger")) { TabTrigger();  ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Visual")) { TabVisual();   ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Arma")) { TabWeapon();   ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Movimento")) { TabMovement(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Unlock")) { TabUnlock();   ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Conta")) { TabConta();    ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Misc")) { TabMisc();     ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Log")) { TabLog();      ImGui::EndTabItem(); }
            ImGui::EndTabBar();
        }

        ImGui::Separator();
        ImGui::TextDisabled("Insert = menu | Delete = panico | F5 = salvar | F8 = descarregar");
        ImGui::End();
    }

} // namespace Menu
// ==== FIM menu.cpp ====