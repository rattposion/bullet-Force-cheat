#pragma once
#ifndef BFI_CONFIG_H
#define BFI_CONFIG_H

#include <windows.h>

struct Config {

    // ---- JANELA / MENU ----
    bool  menuOpen = true;
    int   menuKey = VK_INSERT;
    int   panicKey = VK_DELETE;
    int   saveKey = VK_F5;
    int   unloadKey = VK_F8;

    // ---- ESP ----
    bool  esp = true;
    bool  espBox = true;
    bool  espFill = false;
    bool  espName = true;
    bool  espHealth = true;
    bool  espHealthNum = true;
    bool  espWeapon = false;
    bool  espDistance = true;
    bool  espSnapline = false;
    bool  espSkeleton = false;
    bool  espNpc = false;
    bool  teamCheck = false;
    float espThickness = 1.5f;
    float espMaxDist = 300.f;
    float espFontSize = 13.f;
    float espOffY = 0.f;
    float espScaleY = 1.f;
    float espBodyH = 1.8f;
    float espColor[4] = { 1.f, 0.15f, 0.15f, 1.f };
    float espBoxColor[4] = { 1.f, 1.f, 1.f, 1.f };
    float espTextColor[4] = { 1.f, 1.f, 1.f, 1.f };
    float espSkelColor[4] = { 1.f, 1.f, 0.f, 1.f };

    bool  espVisCheck = true;
    bool  espVisLine = true;
    float visColor[4] = { 0.f, 1.f, 0.f, 1.f };
    float noVisColor[4] = { 1.f, 0.f, 0.f, 1.f };

    bool  fps = false;

    // ---- PERFORMANCE ----
    int   losPerFrame = 4;

    // ---- AIMBOT ----
    bool  aimbot = false;
    int   aimKey = VK_RBUTTON;
    float aimFov = 8.f;
    float aimSmooth = 5.f;
    int   aimBone = 0;
    int   aimMode = 0;              // 0=Camera(screen) 1=Angulo(3D) 2=Mouse(->Camera)
    bool  aimVisible = true;

    // Checar visao (LOS): MARCADO -> mira SOMENTE em quem esta visivel.
    bool  aimVisCheck = true;

    // Nao mirar atras do muro: true = descarta invisivel (padrao).
    bool  aimStrictVis = true;

    bool  aimTeam = false;
    bool  aimRotate = true;
    bool  showFovCircle = false;
    float aimMaxDist = 250.f;
    float aimColor[4] = { 0.f, 1.f, 0.f, 1.f };

    bool  silentAim = false;
    bool  magicBullets = false;

    // =======================================================================
    //  ALIASES / CAMPOS NOVOS  (nome exato usado pelo aimbot.cpp)
    // =======================================================================
    bool  aimEnabled = true;
    bool  aimTeamCheck = false;     // true = nao mira em companheiro de time
    int   aimSelect = 0;            // 0=mais perto da mira  1=mais perto  2=hibrido
    bool  aimToggle = false;
    bool  aimAutoFire = false;
    int   aimFireDelayMs = 50;

    // ---- TRIGGERBOT ----
    bool  triggerbot = false;
    int   triggerKey = VK_MENU;
    float triggerDelay = 30.f;
    float triggerRadius = 10.f;

    // ---- ARMA ----
    bool  noRecoil = false;
    bool  noSpread = false;
    bool  infAmmo = false;
    bool  noReload = false;
    bool  rapidFire = false;

    // ---- MOVIMENTO ----
    bool  speedHack = false;
    float speedValue = 8.f;
    bool  fly = false;
    float flySpeed = 6.f;
    bool  superJump = false;
    float jumpPower = 6.f;
    bool  teleport = false;

    // ---- MISC ----
    bool  godmode = false;
    bool  hitmarker = false;
    float camFov = 90.f;
    bool  crosshair = false;
    float crosshairSize = 6.f;
    float crosshairColor[4] = { 0.f, 1.f, 0.f, 1.f };
    bool  watermark = true;

    // ---- ANTICHEAT ----
    bool  antiCheat = true;
    bool  blockedNoZone = false;
    bool  blockedUnlock = false;
    int   stage = 0;

    // ---- UNLOCK ALL ----
    bool  unlockAll = true;
    bool  unlockWeapons = true;
    bool  unlockSkins = true;
    bool  unlockAttachments = true;
    bool  unlockCurrency = true;
    bool  fillLoadoutSlots = false;
    bool  applySlots = false;
    int   slotWeapon[3] = { -1, -1, -1 };
    int   currencyValue = 999999;
    float unlockInterval = 0.5f;
    int   unlockLevel = 999;

    // ---- NOMES ----
    bool  forceNames = false;
    char  weaponName[32] = "";
    char  skinName[32] = "";
    char  attachName[32] = "";

    // ---- SKIN CHANGER ----
    bool  skinChanger = false;
    int   skinIndex = 0;

    bool  aimKeyDown = false;
    float lastHitTime = 0.f;

    // ---- SELECAO DO LOADOUT / SKINS / PERSONAGENS ----
    bool  unlockChars = true;
    bool  applySelection = true;
    int   selWeapon = -1;
    int   selCamo = -1;
    int   selChar = -1;
    int   selAttach[3] = { -1, -1, -1 };
    int   loadoutSlot = 0;
    char  charName[32] = "";

    // ---- LOG / DUMP ----
    bool  probeLog = false;
    bool  beatLog = false;
    bool  verboseUnlock = true;
    int   watchMaxLines = 12;

    bool  dumpOnce = true;
    int   dumpIntervalMs = 5000;

    bool  gunCustomizeUnlock = true;
    bool  gunCustomizeAllSlots = true;

    bool  showAimTargetName = false;
    bool  showFpsGraph = false;
};

extern Config  g_cfg;

#ifdef C
#undef C
#endif
extern Config& C;

#define C(f) g_cfg.f

namespace ConfigIO {
    void Init();
    void Save();
    void Load();
}

#endif // BFI_CONFIG_H