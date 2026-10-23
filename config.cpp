// ============================================================================
//  config.cpp — ini texto (chave=valor). Campo novo = chave nova; se a chave
//  nao existir no arquivo, o valor PADRAO do struct e' mantido.
// ============================================================================
#include "config.h"
#include "common.h"

#include <cstdio>
#include <cstring>

extern HMODULE g_hModule;

Config  g_cfg;
Config& C = g_cfg;

// ---------------------------------------------------------------------
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

static void WCol(FILE* f, const char* k, const float c[4]) {
    fprintf(f, "%s=%.4f %.4f %.4f %.4f\n", k, c[0], c[1], c[2], c[3]);
}
static void RCol(const char* v, float c[4]) {
    sscanf_s(v, "%f %f %f %f", &c[0], &c[1], &c[2], &c[3]);
}

namespace ConfigIO {

    void Save() {
        FILE* f = nullptr;
        if (fopen_s(&f, CfgPath(), "w") != 0 || !f) return;

#define S_B(v) fprintf(f, #v "=%d\n", g_cfg.v ? 1 : 0)
#define S_I(v) fprintf(f, #v "=%d\n", g_cfg.v)
#define S_F(v) fprintf(f, #v "=%.4f\n", g_cfg.v)

        S_B(menuOpen); S_I(menuKey); S_I(panicKey); S_I(saveKey); S_I(unloadKey);

        S_B(esp); S_B(espBox); S_B(espFill); S_B(espName); S_B(espHealth);
        S_B(espHealthNum); S_B(espWeapon); S_B(espDistance); S_B(espSnapline);
        S_B(espSkeleton); S_B(espNpc); S_B(teamCheck); S_B(fps);
        S_B(espVisCheck); S_B(espVisLine);
        S_I(losPerFrame);
        S_F(espThickness); S_F(espMaxDist); S_F(espFontSize);
        S_F(espOffY); S_F(espScaleY); S_F(espBodyH);
        WCol(f, "espColor", g_cfg.espColor);
        WCol(f, "espBoxColor", g_cfg.espBoxColor);
        WCol(f, "espTextColor", g_cfg.espTextColor);
        WCol(f, "espSkelColor", g_cfg.espSkelColor);
        WCol(f, "visColor", g_cfg.visColor);
        WCol(f, "noVisColor", g_cfg.noVisColor);

        S_B(aimbot); S_I(aimKey); S_F(aimFov); S_F(aimSmooth); S_I(aimBone);
        S_I(aimMode); S_B(aimVisible); S_B(aimVisCheck); S_B(aimStrictVis);
        S_B(aimTeam); S_B(aimRotate);
        S_B(showFovCircle); S_F(aimMaxDist);
        WCol(f, "aimColor", g_cfg.aimColor);

        S_I(aimSelect); S_B(aimToggle); S_B(aimAutoFire); S_I(aimFireDelayMs);

        S_B(silentAim); S_B(magicBullets);

        S_B(triggerbot); S_I(triggerKey); S_F(triggerDelay); S_F(triggerRadius);

        S_B(noRecoil); S_B(noSpread); S_B(infAmmo); S_B(noReload); S_B(rapidFire);

        S_B(speedHack); S_F(speedValue); S_B(fly); S_F(flySpeed);
        S_B(superJump); S_F(jumpPower); S_B(teleport);

        // ---- BUNNYHOP / TIRO NO AR ----
        S_B(bhop); S_I(bhopKey); S_B(bhopKeepSpeed); S_B(bhopSkipMenu);
        S_I(bhopMode); S_I(bhopHoldMs); S_I(bhopRePressMs); S_F(bhopJumpSpeed);
        S_B(shootInAir); S_I(airFireKey); S_I(airFireDelayMs);

        S_B(godmode); S_F(godmodeValue);
        S_B(hitmarker); S_F(camFov);
        S_B(crosshair); S_F(crosshairSize);
        WCol(f, "crosshairColor", g_cfg.crosshairColor);
        S_B(watermark);

        // ---- ALERTA DE RISCO ----
        S_B(showRiskAlert); S_B(riskAlertBlink);

        S_B(antiCheat); S_B(blockedNoZone); S_B(blockedUnlock); S_I(stage);

        // ---- UNLOCK ----
        S_B(unlockAll); S_B(unlockWeapons); S_B(unlockSkins);
        S_B(unlockAttachments); S_B(unlockCurrency);
        S_B(unlockChars);
        S_B(fillLoadoutSlots); S_B(applySlots);
        S_I(slotWeapon[0]); S_I(slotWeapon[1]); S_I(slotWeapon[2]);
        S_I(currencyValue); S_F(unlockInterval);
        S_B(skinChanger); S_I(skinIndex); S_I(unlockLevel);

        // ---- SELECAO / LOG ----
        S_B(applySelection); S_I(loadoutSlot);
        S_I(selWeapon); S_I(selCamo); S_I(selChar);
        S_I(selAttach[0]); S_I(selAttach[1]); S_I(selAttach[2]);
        S_B(probeLog); S_B(beatLog); S_B(showFpsGraph);

#undef S_B
#undef S_I
#undef S_F

        fclose(f);
        Log("[cfg] salvo em %s", CfgPath());
    }

    void Load() {
        FILE* f = nullptr;
        if (fopen_s(&f, CfgPath(), "r") != 0 || !f) { Log("[cfg] sem arquivo, usando padroes"); return; }

        char line[256];
        while (fgets(line, sizeof(line), f)) {
            char* nl = strpbrk(line, "\r\n");
            if (nl) *nl = 0;
            char* eq = strchr(line, '=');
            if (!eq) continue;
            *eq = 0;
            const char* k = line;
            const char* v = eq + 1;

#define L_B(x) else if (!strcmp(k, #x)) g_cfg.x = (atoi(v) != 0)
#define L_I(x) else if (!strcmp(k, #x)) g_cfg.x = atoi(v)
#define L_F(x) else if (!strcmp(k, #x)) g_cfg.x = (float)atof(v)
#define L_C(x) else if (!strcmp(k, #x)) RCol(v, g_cfg.x)

            if (0) {}

            L_B(menuOpen); L_I(menuKey); L_I(panicKey); L_I(saveKey); L_I(unloadKey);

            L_B(esp); L_B(espBox); L_B(espFill); L_B(espName); L_B(espHealth);
            L_B(espHealthNum); L_B(espWeapon); L_B(espDistance); L_B(espSnapline);
            L_B(espSkeleton); L_B(espNpc); L_B(teamCheck); L_B(fps);
            L_B(espVisCheck); L_B(espVisLine);
            L_I(losPerFrame);
            L_F(espThickness); L_F(espMaxDist); L_F(espFontSize);
            L_F(espOffY); L_F(espScaleY); L_F(espBodyH);
            L_C(espColor); L_C(espBoxColor); L_C(espTextColor); L_C(espSkelColor);
            L_C(visColor); L_C(noVisColor);

            L_B(aimbot); L_I(aimKey); L_F(aimFov); L_F(aimSmooth); L_I(aimBone);
            L_I(aimMode); L_B(aimVisible); L_B(aimVisCheck); L_B(aimStrictVis);
            L_B(aimTeam); L_B(aimRotate);
            L_B(showFovCircle); L_F(aimMaxDist);
            L_C(aimColor);

            L_I(aimSelect); L_B(aimToggle); L_B(aimAutoFire); L_I(aimFireDelayMs);

            L_B(silentAim); L_B(magicBullets);

            L_B(triggerbot); L_I(triggerKey); L_F(triggerDelay); L_F(triggerRadius);

            L_B(noRecoil); L_B(noSpread); L_B(infAmmo); L_B(noReload); L_B(rapidFire);

            L_B(speedHack); L_F(speedValue); L_B(fly); L_F(flySpeed);
            L_B(superJump); L_F(jumpPower); L_B(teleport);

            L_B(bhop); L_I(bhopKey); L_B(bhopKeepSpeed); L_B(bhopSkipMenu);
            L_I(bhopMode); L_I(bhopHoldMs); L_I(bhopRePressMs); L_F(bhopJumpSpeed);
            L_B(shootInAir); L_I(airFireKey); L_I(airFireDelayMs);

            L_B(godmode); L_F(godmodeValue);
            L_B(hitmarker); L_F(camFov);
            L_B(crosshair); L_F(crosshairSize);
            L_C(crosshairColor);
            L_B(watermark);

            L_B(showRiskAlert); L_B(riskAlertBlink);

            L_B(antiCheat); L_B(blockedNoZone); L_B(blockedUnlock); L_I(stage);

            L_B(unlockAll); L_B(unlockWeapons); L_B(unlockSkins);
            L_B(unlockAttachments); L_B(unlockCurrency);
            L_B(unlockChars);
            L_B(fillLoadoutSlots); L_B(applySlots);
            L_I(slotWeapon[0]); L_I(slotWeapon[1]); L_I(slotWeapon[2]);
            L_I(currencyValue); L_F(unlockInterval);
            L_B(skinChanger); L_I(skinIndex); L_I(unlockLevel);

            L_B(applySelection); L_I(loadoutSlot);
            L_I(selWeapon); L_I(selCamo); L_I(selChar);
            L_I(selAttach[0]); L_I(selAttach[1]); L_I(selAttach[2]);
            L_B(probeLog); L_B(beatLog); L_B(showFpsGraph);

#undef L_B
#undef L_I
#undef L_F
#undef L_C
        }
        fclose(f);
        Log("[cfg] carregado de %s", CfgPath());
    }

    void Init() { Load(); }

} // namespace ConfigIO