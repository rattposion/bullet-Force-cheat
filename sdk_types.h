#pragma once
// ============================================================================
//  sdk_types.h — tipos de dados do SDK. Sem estado, sem funcoes, sem Hooks.
//  Quem so' precisa de 'Target' inclui ESTE, nao o sdk.h.
// ============================================================================

#include "common.h"
#include "il2cpp.h"

// Capacidade do buffer de nome. Se mudar, mude junto com quem escreve.
inline constexpr int kTargetNameLen = 40;

// Teto duro de alvos por frame. g_targetCount nunca passa disto.
#ifndef MAX_TARGETS
#define MAX_TARGETS 128
#endif

// ============================================================================
//  Target
// ============================================================================
//  CONTRATO DE TEMPO DE VIDA
//    - Os ponteiros (obj, player, headBone, footBone) sao EMPRESTADOS: o
//      Target nao e' dono e nao mantem nada vivo. Valem enquanto o objeto
//      il2cpp existir. Depois de um respawn/troca de cena, podem estar
//      pendurados — revalide com Plausible() antes de dereferenciar.
//    - Copiar um Target copia os PONTEIROS (raso). A copia nao estende a vida
//      do objeto apontado.
//
//  CONTRATO DE ATUALIZACAO
//    - Um Target e' um RETRATO: os campos numericos ficam congelados na copia.
//      Um RefreshTargets() posterior NAO atualiza a copia.
//    - g_targets[] e' reescrito in-place pelo thread do jogo. Para usar fora
//      do lock, copie (Snapshot()).
//
//  >>> onScreen
//    Quem escreve e' UpdateTargets(), e ele precisa SEMPRE rodar — inclusive
//    quando o local player nao existe naquele frame. RefreshTargets() zera
//    'onScreen' de todo mundo; se UpdateTargets() saisse antes de recomputar
//    (era o caso), o ESP ficava vazio com os alvos perfeitamente validos.
//    O esp.cpp trata 'onScreen == false' como "nao desenha este alvo".
// ============================================================================
struct Target {
    Il2CppObject* obj = nullptr;        // emprestado
    Il2CppObject* player = nullptr;     // emprestado
    Il2CppObject* headBone = nullptr;   // emprestado
    Il2CppObject* footBone = nullptr;   // emprestado

    Vec3   pos{ 0.f, 0.f, 0.f };
    Vec3   feet{ 0.f, 0.f, 0.f };
    Vec3   head{ 0.f, 0.f, 0.f };
    Vec2   scrHead{ 0.f, 0.f };
    Vec2   scrFeet{ 0.f, 0.f };

    bool   onScreen = false;
    bool   alive = false;
    bool   npc = false;
    bool   visible = false;

    int    team = 0;
    int    weapon = 0;
    float  health = 100.f;
    float  dist = 0.f;

    uint32_t slot = 0;                  // estavel enquanto o player existir
    uint64_t lastSeen = 0;              // ms; monotonico

    char   name[kTargetNameLen]{};

    // Zera tudo. Use antes de preencher um Target reutilizado, para nao herdar
    // lixo da rodada anterior.
    void Reset() { *this = Target{}; }

    // Unica forma segura de escrever 'name': respeita a capacidade e garante
    // terminacao nula. strncpy puro nao termina quando a origem e' maior.
    void SetName(const char* s) {
        if (!s) { name[0] = '\0'; return; }
        strncpy_s(name, kTargetNameLen, s, _TRUNCATE);
    }

    bool HasName() const { return name[0] != '\0'; }
};