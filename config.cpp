// ============================================================================
//  config.cpp — ini texto (chave=valor) via TABELA PLANA (sem C1061).
//  Campo novo = 1 linha na tabela. Chave ausente no arquivo = padrao mantido.
// ============================================================================
#include "config.h"
#include "common.h"

#include <cstdio>
#include <cstring>

extern HMODULE g_hModule;

Config  g_cfg;
Config& C = g_cfg;

static const char* CfgPath() {
    static char buf[MAX_PATH] = { 0 };
    if (!buf[0]) {
        GetModuleFileNameA(g_hModule, buf, MAX_PATH);
        char* p = strrchr(buf, '\\');
        if (p) *(p + 1) = 0;
        strcat_s(buf, "bfi.ini");
    }
    return buf;
}

// t: 0=bool 1=int 2=float 3=float[4] 4=char[n]
struct CfgE { const char* k; int t; void* p; int n; };

static CfgE kCfg[] = {
    { "menuOpen", 0, &g_cfg.menuOpen, 0 },{ "menuKey", 1, &g_cfg.menuKey, 0 },
    { "panicKey", 1, &g_cfg.panicKey, 0 },{ "saveKey", 1, &g_cfg.saveKey, 0 },
    { "unloadKey", 1, &g_cfg.unloadKey, 0 },

    { "esp", 0, &g_cfg.esp, 0 },{ "espBox", 0, &g_cfg.espBox, 0 },
    { "espFill", 0, &g_cfg.espFill, 0 },{ "espName", 0, &g_cfg.espName, 0 },
    { "espHealth", 0, &g_cfg.espHealth, 0 },{ "espHealthNum", 0, &g_cfg.espHealthNum, 0 },
    { "espWeapon", 0, &g_cfg.espWeapon, 0 },{ "espDistance", 0, &g_cfg.espDistance, 0 },
    { "espSnapline", 0, &g_cfg.espSnapline, 0 },{ "espSkeleton", 0, &g_cfg.espSkeleton, 0 },
    { "espNpc", 0, &g_cfg.espNpc, 0 },{ "teamCheck", 0, &g_cfg.teamCheck, 0 },
    { "fps", 0, &g_cfg.fps, 0 },
    { "espVisCheck", 0, &g_cfg.espVisCheck, 0 },{ "espVisLine", 0, &g_cfg.espVisLine, 0 },
    { "losPerFrame", 1, &g_cfg.losPerFrame, 0 },
    { "espThickness", 2, &g_cfg.espThickness, 0 },{ "espMaxDist", 2, &g_cfg.espMaxDist, 0 },
    { "espFontSize", 2, &g_cfg.espFontSize, 0 },{ "espOffY", 2, &g_cfg.espOffY, 0 },
    { "espScaleY", 2, &g_cfg.espScaleY, 0 },{ "espBodyH", 2, &g_cfg.espBodyH, 0 },
    { "espColor", 3, g_cfg.espColor, 0 },{ "espBoxColor", 3, g_cfg.espBoxColor, 0 },
    { "espTextColor", 3, g_cfg.espTextColor, 0 },{ "espSkelColor", 3, g_cfg.espSkelColor, 0 },
    { "visColor", 3, g_cfg.visColor, 0 },{ "noVisColor", 3, g_cfg.noVisColor, 0 },

    { "aimbot", 0, &g_cfg.aimbot, 0 },{ "aimKey", 1, &g_cfg.aimKey, 0 },
    { "aimFov", 2, &g_cfg.aimFov, 0 },{ "aimSmooth", 2, &g_cfg.aimSmooth, 0 },
    { "aimBone", 1, &g_cfg.aimBone, 0 },{ "aimMode", 1, &g_cfg.aimMode, 0 },
    { "aimVisible", 0, &g_cfg.aimVisible, 0 },
    { "aimVisCheck", 0, &g_cfg.aimVisCheck, 0 },{ "aimStrictVis", 0, &g_cfg.aimStrictVis, 0 },
    { "aimTeam", 0, &g_cfg.aimTeam, 0 },{ "aimRotate", 0, &g_cfg.aimRotate, 0 },
    { "showFovCircle", 0, &g_cfg.showFovCircle, 0 },{ "aimMaxDist", 2, &g_cfg.aimMaxDist, 0 },
    { "aimColor", 3, g_cfg.aimColor, 0 },
    { "aimSelect", 1, &g_cfg.aimSelect, 0 },{ "aimToggle", 0, &g_cfg.aimToggle, 0 },
    { "aimAutoFire", 0, &g_cfg.aimAutoFire, 0 },{ "aimFireDelayMs", 1, &g_cfg.aimFireDelayMs, 0 },
    { "silentAim", 0, &g_cfg.silentAim, 0 },{ "magicBullets", 0, &g_cfg.magicBullets, 0 },

    { "triggerbot", 0, &g_cfg.triggerbot, 0 },{ "triggerKey", 1, &g_cfg.triggerKey, 0 },
    { "triggerDelay", 2, &g_cfg.triggerDelay, 0 },{ "triggerRadius", 2, &g_cfg.triggerRadius, 0 },

    { "noRecoil", 0, &g_cfg.noRecoil, 0 },{ "noSpread", 0, &g_cfg.noSpread, 0 },
    { "infAmmo", 0, &g_cfg.infAmmo, 0 },{ "noReload", 0, &g_cfg.noReload, 0 },
    { "rapidFire", 0, &g_cfg.rapidFire, 0 },

    { "speedHack", 0, &g_cfg.speedHack, 0 },{ "speedValue", 2, &g_cfg.speedValue, 0 },
    { "fly", 0, &g_cfg.fly, 0 },{ "flySpeed", 2, &g_cfg.flySpeed, 0 },
    { "superJump", 0, &g_cfg.superJump, 0 },{ "jumpPower", 2, &g_cfg.jumpPower, 0 },
    { "teleport", 0, &g_cfg.teleport, 0 },

    { "bhop", 0, &g_cfg.bhop, 0 },{ "bhopKey", 1, &g_cfg.bhopKey, 0 },
    { "bhopKeepSpeed", 0, &g_cfg.bhopKeepSpeed, 0 },{ "bhopSkipMenu", 0, &g_cfg.bhopSkipMenu, 0 },
    { "bhopMode", 1, &g_cfg.bhopMode, 0 },{ "bhopHoldMs", 1, &g_cfg.bhopHoldMs, 0 },
    { "bhopRePressMs", 1, &g_cfg.bhopRePressMs, 0 },{ "bhopJumpSpeed", 2, &g_cfg.bhopJumpSpeed, 0 },
    { "shootInAir", 0, &g_cfg.shootInAir, 0 },{ "airFireKey", 1, &g_cfg.airFireKey, 0 },
    { "airFireDelayMs", 1, &g_cfg.airFireDelayMs, 0 },

    { "godmode", 0, &g_cfg.godmode, 0 },{ "godmodeValue", 2, &g_cfg.godmodeValue, 0 },
    { "hitmarker", 0, &g_cfg.hitmarker, 0 },{ "camFov", 2, &g_cfg.camFov, 0 },
    { "crosshair", 0, &g_cfg.crosshair, 0 },{ "crosshairSize", 2, &g_cfg.crosshairSize, 0 },
    { "crosshairColor", 3, g_cfg.crosshairColor, 0 },{ "watermark", 0, &g_cfg.watermark, 0 },

    { "showRiskAlert", 0, &g_cfg.showRiskAlert, 0 },{ "riskAlertBlink", 0, &g_cfg.riskAlertBlink, 0 },

    { "antiCheat", 0, &g_cfg.antiCheat, 0 },{ "blockedNoZone", 0, &g_cfg.blockedNoZone, 0 },
    { "blockedUnlock", 0, &g_cfg.blockedUnlock, 0 },{ "stage", 1, &g_cfg.stage, 0 },

    { "unlockAll", 0, &g_cfg.unlockAll, 0 },{ "unlockWeapons", 0, &g_cfg.unlockWeapons, 0 },
    { "unlockSkins", 0, &g_cfg.unlockSkins, 0 },{ "unlockAttachments", 0, &g_cfg.unlockAttachments, 0 },
    { "unlockCurrency", 0, &g_cfg.unlockCurrency, 0 },{ "unlockChars", 0, &g_cfg.unlockChars, 0 },
    { "fillLoadoutSlots", 0, &g_cfg.fillLoadoutSlots, 0 },{ "applySlots", 0, &g_cfg.applySlots, 0 },
    { "slotWeapon[0]", 1, &g_cfg.slotWeapon[0], 0 },
    { "slotWeapon[1]", 1, &g_cfg.slotWeapon[1], 0 },
    { "slotWeapon[2]", 1, &g_cfg.slotWeapon[2], 0 },
    { "currencyValue", 1, &g_cfg.currencyValue, 0 },{ "unlockInterval", 2, &g_cfg.unlockInterval, 0 },
    { "skinChanger", 0, &g_cfg.skinChanger, 0 },{ "skinIndex", 1, &g_cfg.skinIndex, 0 },
    { "unlockLevel", 1, &g_cfg.unlockLevel, 0 },

    { "forceNames", 0, &g_cfg.forceNames, 0 },
    { "weaponName", 4, g_cfg.weaponName, (int)sizeof(g_cfg.weaponName) },
    { "skinName", 4, g_cfg.skinName, (int)sizeof(g_cfg.skinName) },
    { "attachName", 4, g_cfg.attachName, (int)sizeof(g_cfg.attachName) },

    { "applySelection", 0, &g_cfg.applySelection, 0 },{ "loadoutSlot", 1, &g_cfg.loadoutSlot, 0 },
    { "selWeapon", 1, &g_cfg.selWeapon, 0 },{ "selCamo", 1, &g_cfg.selCamo, 0 },
    { "selChar", 1, &g_cfg.selChar, 0 },
    { "selAttach[0]", 1, &g_cfg.selAttach[0], 0 },
    { "selAttach[1]", 1, &g_cfg.selAttach[1], 0 },
    { "selAttach[2]", 1, &g_cfg.selAttach[2], 0 },
    { "charName", 4, g_cfg.charName, (int)sizeof(g_cfg.charName) },

    { "probeLog", 0, &g_cfg.probeLog, 0 },{ "beatLog", 0, &g_cfg.beatLog, 0 },
    { "showFpsGraph", 0, &g_cfg.showFpsGraph, 0 },

    // ---- CONTA / DINHEIRO ----
    { "moneyOnlyWallet", 0, &g_cfg.moneyOnlyWallet, 0 },
    { "moneyWritePrefs", 0, &g_cfg.moneyWritePrefs, 0 },
    { "moneyMode", 1, &g_cfg.moneyMode, 0 },
    { "moneyIntervalMs", 1, &g_cfg.moneyIntervalMs, 0 },
    { "accAuto", 0, &g_cfg.accAuto, 0 },
    { "accGold", 1, &g_cfg.accGold, 0 },
    { "accCases", 1, &g_cfg.accCases, 0 },

    { "logAutoScroll", 0, &g_cfg.logAutoScroll, 0 },
    { "logFilter", 1, &g_cfg.logFilter, 0 },
};

namespace ConfigIO {

    void Save() {
        FILE* f = nullptr;
        if (fopen_s(&f, CfgPath(), "w") != 0 || !f) return;
        for (size_t i = 0; i < sizeof(kCfg) / sizeof(kCfg[0]); ++i) {
            CfgE& e = kCfg[i];
            switch (e.t) {
            case 0: fprintf(f, "%s=%d\n", e.k, (*(bool*)e.p) ? 1 : 0); break;
            case 1: fprintf(f, "%s=%d\n", e.k, *(int*)e.p); break;
            case 2: fprintf(f, "%s=%.4f\n", e.k, *(float*)e.p); break;
            case 3: {
                float* c = (float*)e.p;
                fprintf(f, "%s=%.4f %.4f %.4f %.4f\n", e.k, c[0], c[1], c[2], c[3]); break;
            }
            case 4: fprintf(f, "%s=%s\n", e.k, (const char*)e.p); break;
            }
        }
        fclose(f);
        Log("[cfg] salvo em %s", CfgPath());
    }

    void Load() {
        FILE* f = nullptr;
        if (fopen_s(&f, CfgPath(), "r") != 0 || !f) { Log("[cfg] sem arquivo, usando padroes"); return; }
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            char* nl = strpbrk(line, "\r\n");
            if (nl) *nl = 0;
            char* eq = strchr(line, '=');
            if (!eq) continue;
            *eq = 0;
            const char* k = line;
            const char* v = eq + 1;
            for (size_t i = 0; i < sizeof(kCfg) / sizeof(kCfg[0]); ++i) {
                CfgE& e = kCfg[i];
                if (strcmp(e.k, k) != 0) continue;
                switch (e.t) {
                case 0: *(bool*)e.p = (atoi(v) != 0); break;
                case 1: *(int*)e.p = atoi(v); break;
                case 2: *(float*)e.p = (float)atof(v); break;
                case 3: {
                    float* c = (float*)e.p;
                    sscanf_s(v, "%f %f %f %f", &c[0], &c[1], &c[2], &c[3]); break;
                }
                case 4: strncpy_s((char*)e.p, (size_t)e.n, v, _TRUNCATE); break;
                }
                break;
            }
        }
        fclose(f);
        Log("[cfg] carregado de %s", CfgPath());
    }

    void Init() { Load(); }

} // namespace ConfigIO