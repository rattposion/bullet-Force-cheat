#pragma once
// ============================================================================
//  unlock.h — API de unlock/catalogo + CARTEIRA, consumida pelo menu.
//  DONO UNICO do namespace Unlock.
//
//  ATENCAO: as assinaturas da secao CARTEIRA sao consumidas pelo
//  Menu::TabConta (menu.cpp). Nao mude nome/tipo/ordem — o linker casa por
//  nome mangled (LNK2019 se mudar).
//
//  Instalacao: Tick() e' o unico ponto de escrita. Dump()/MoneyRefresh() podem
//  ser chamados de qualquer thread/estado.
// ============================================================================

namespace Unlock {

    // ------------------------------------------------------------------------
    //  Install() — RETORNO bool (igual em todo o projeto: unlock.cpp, recoil.cpp)
    //  Idempotente: 2a chamada -> false.
    // ------------------------------------------------------------------------
    bool Install();

    void Tick();
    void Dump();

    bool Ready();       // catalogo publicado
    int  Generation();  // muda a cada reconstrucao

    // ------------------------------------------------------------------ armas
    int WeaponCount();
    const char* const* WeaponList();
    const char* WeaponName(int idx);

    // ----------------------------------------------------------------- anexos
    int AttachSlotCount();
    const char* AttachSlotName(int slot);
    int AttachCount(int slot);
    const char* const* AttachList(int slot);
    const char* AttachName(int slot, int idx);
    int AttachValue(int slot, int idx);

    // ------------------------------------------------------------------ skins
    int CamoCount();
    const char* const* CamoList();
    const char* CamoName(int idx);

    // ------------------------------------------------------------ personagens
    int CharCount();
    const char* const* CharList();
    const char* CharName(int idx);

    // ========================================================================
    //  CARTEIRA / ESPELHOS  (Menu::TabConta)
    //  idx sempre em [0, MoneyCount()); fora da faixa o comportamento e'
    //  definido (0 / false / ""), nunca UB.
    // ========================================================================
    int         MoneyCount();            // quantidade de espelhos da tabela
    const char* MoneyName(int idx);      // nome amigavel (nunca nullptr)
    bool        MoneyLocked(int idx);    // esse espelho esta travado?
    void        MoneySetLock(int idx, bool on);

    long long   MoneyValue(int idx);     // valor REAL lido do jogo agora
    void        MoneySet(int idx, long long value);

    void        MoneyRefresh();          // re-resolve os espelhos agora

    // casa por classe e/ou campo (aceita "Classe::Campo" no 1o parametro),
    // trava os que casarem e escreve value. Devolve quantos escreveu.
    int         MoneyLockByName(const char* cls, const char* field, long long value);

    // mode: <=0 = todos, 1 = so os travados, 2 = so os destravados.
    // Devolve quantos escreveu.
    int         ForceMirrors(long long value, long long mode);

    void        MoneyUnlockAll();        // trava todos e aplica o valor do config

    bool        MoneyReady();            // todos os espelhos resolvidos
    void        MoneyTick();             // chamado pelo Tick()
    void        MoneyWritePrefs();       // grava tambem nos PlayerPrefs
    void        ShopForceAll();          // so gold/caixas/loja

    void        DumpMirrors();
    void        DumpShop();
}