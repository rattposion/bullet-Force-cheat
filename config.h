#pragma once
#ifndef BFI_CONFIG_H
#define BFI_CONFIG_H

#include <windows.h>

// >>> v4322: marcador de versao. unlock.cpp/menu checam isto e dao #error
//     se um config.h antigo estiver no include path.
#define BFI_CONFIG_VER 4322

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
    float aimFov = 8.f;              // GRAUS (mundo). Ver aimSelect.
    float aimSmooth = 5.f;           // consumido pelo SDK::AimLock
    int   aimBone = 0;               // 0=head; fallback automatico nos outros
    int   aimMode = 0;
    bool  aimVisible = true;

    bool  aimVisCheck = true;
    bool  aimStrictVis = true;

    // >>> v4321 [H2]: fonte da mira. 0 = player (fresco, PADRAO) / 1 = camera.
    //     Origem E direcao saem do MESMO objeto (GetAimFrame no aimbot.cpp v21).
    //     O log mostrou o camPos ~1s atras do player: se a mira sair torta,
    //     troque pra 1 e compare pelo log "[aim] DIVERGENCIA".
    int   aimCamSrc = 0;

    bool  aimTeam = false;
    bool  aimRotate = true;          // se false, so' o silent aim mexe
    bool  showFovCircle = false;
    float aimMaxDist = 250.f;
    float aimColor[4] = { 0.f, 1.f, 0.f, 1.f };

    bool  silentAim = false;
    bool  magicBullets = false;

    bool  aimTeamCheck = false;
    int   aimSelect = 0;             // 0=angulo 1=dist 2=hibrido 3=tela
    bool  aimToggle = false;
    bool  aimAutoFire = false;
    int   aimFireDelayMs = 50;

    // ========================================================================
    //  >>> v4320 — mira forte (aimbot.cpp v20/v21)
    // ========================================================================
    bool  aimPredict = true;         // [G4] lead de movimento
    float aimPredictAmt = 1.0f;      // 1.0 = tempo total de voo
    float aimBulletSpeed = 400.f;    // m/s aproximado
    float aimDeadzone = 0.12f;       // [G5] graus: nao re-mira se ja esta em cima
    float aimInvisPenalty = 12.f;    // graus somados em alvo invisivel (nao-estrito)
    int   aimVisTopN = 8;            // [G6] max de raycasts de LOS por frame
    int   aimSwitchMs = 180;         // cooldown de troca de alvo (mira nao para)

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

    // ---- BUNNYHOP / TIRO NO AR ----
    bool  bhop = false;
    int   bhopKey = 0;
    bool  bhopKeepSpeed = false;
    bool  bhopSkipMenu = true;

    int   bhopMode = 1;
    int   bhopHoldMs = 25;
    int   bhopRePressMs = 70;
    float bhopJumpSpeed = 0.f;

    bool  shootInAir = false;
    int   airFireKey = VK_LBUTTON;
    int   airFireDelayMs = 70;

    // ---- MISC ----
    bool  godmode = false;
    float godmodeValue = 100.f;
    bool  hitmarker = false;
    float camFov = 90.f;
    bool  crosshair = false;
    float crosshairSize = 6.f;
    float crosshairColor[4] = { 0.f, 1.f, 0.f, 1.f };
    bool  watermark = true;

    // ---- ALERTA DE RISCO ----
    bool  showRiskAlert = true;
    bool  riskAlertBlink = true;

    // ---- ANTICHEAT ----
    bool  antiCheat = true;
    bool  blockedNoZone = false;
    bool  blockedUnlock = false;
    int   stage = 0;

    // >>> v4322 — BISECT DO FREEZE (UnityPlayer+0x36FD30, alvo=0, fase=present).
    //     Default = AC 100% INERTE (nenhuma escrita, nenhum hook). Ligue UM por
    //     run e veja qual faz o freeze voltar.
    //
    //     acClear        : escreve (ClearList/statics). false = so' leitura.
    //     acUpdHook      : hook em TimeCheatingDetector::Update (roda no meio do
    //                      frame do jogo; no v10 ele escrevia mesmo com o
    //                      anti-cheat desligado no menu).
    //     acHookGetters  : neutraliza getters bool (MH_CreateHook).
    //     acWritePlayer  : AC joga os offsets DELE na classe do player (chute).
    //     acSameClassOnly: so' escreve se a classe do OBJETO == classe dos
    //                      OFFSETS. O log v10 provou a incoerencia (objeto =
    //                      PlayerAntiCheat @0xE08, offsets = TimeCheatingDetector).
    //     acLogWrites    : loga as primeiras 60 escritas (offset, nome, antigo->0).
    bool  acClear = false;
    bool  acUpdHook = false;
    bool  acHookGetters = false;
    bool  acWritePlayer = false;
    bool  acSameClassOnly = true;
    bool  acLogWrites = true;

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

    // ---- SELECAO ----
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

    bool  showFpsGraph = false;


    // ========================================================================
    //  >>> v42 — carteira / conta / janela de log
    // ========================================================================
    bool  moneyOnlyWallet = true;
    bool  moneyWritePrefs = false;
    bool  moneyRescan = false;
    int   moneyMode = 0;
    int   moneyIntervalMs = 1000;
    bool  moneyApplyNow = false;

    int   accGold = 0;
    int   accCases = 0;
    bool  accAuto = true;

    bool  logAutoScroll = true;
    char  logFilter[64] = "";
};

extern Config  g_cfg;

#ifdef C
#undef C
#endif
extern Config& C;

namespace ConfigIO {
    void Init();
    void Save();
    void Load();
}

#endif // BFI_CONFIG_H