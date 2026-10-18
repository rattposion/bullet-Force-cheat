#pragma once
// ============================================================================
//  unlock.h — API de unlock/catalogo consumida pelo menu.
//  DONO UNICO do namespace Unlock.
//
//  DEPENDENCIAS: nenhuma. Este header nao inclui common.h — nao usa nada dele.
//  Se algum .cpp pegava common.h de graca por causa daqui, inclua la.
//
//  ===========================================================================
//  MODELO DE ESTADO
//  ===========================================================================
//  O catalogo tem exatamente dois estados:
//
//    (A) NAO PRONTO  — Ready() == false
//    (B) PRONTO      — Ready() == true
//
//  Em (A): todos os Count() == 0, todos os List() == nullptr, todos os
//  acessores por nome devolvem nullptr / -1. Nada e' valido antes de (B).
//
//  Regra invariante: List() != nullptr  <=>  Count() > 0.
//  Categoria vazia com catalogo pronto => Count() == 0 e List() == nullptr.
//  Nao existe lista valida com zero elementos: o chamador detecta "vazio"
//  com Count() == 0 e nunca precisa dereferenciar um ponteiro nulo.
//
//  ===========================================================================
//  VALIDADE DOS PONTEIROS (leia antes de guardar qualquer coisa)
//  ===========================================================================
//  As listas sao montadas em memoria propria do unlock e NAO sao realocadas
//  enquanto a geracao atual valer. Portanto:
//
//    - Os PONTEIROS do array sao estaveis ATE a proxima reconstrucao.
//    - As STRINGS apontadas tambem pertencem ao unlock (nao libere, nao
//      escreva — dai o 'const char*').
//    - Reconstrucao acontece quando o catalogo e' remontado (reload do
//      bfi_dupe.json, nova varredura DUPE). Depois disso, TODO ponteiro
//      guardado fica pendurado.
//
//  Para guardar com seguranca: guarde Generation() junto do ponteiro. Se
//  Generation() mudou, re-consulte. Sem isso, a estabilidade e' so' "ate o
//  proximo reload" — nao e' permanente.
//
//  Guardar o INDICE tambem vale, com a mesma ressalva: indices podem mudar
//  de significado entre geracoes. Sempre releia apos Generation() mudar.
//
//  ===========================================================================
//  ENTRADAS INVALIDAS (contrato explicito, nunca UB)
//  ===========================================================================
//  Faixas validas:
//    slot  : [0, AttachSlotCount())            -> hoje 0..2
//    idx   : [0, Count() da categoria)
//
//  Fora da faixa — INCLUSIVE NEGATIVO — o comportamento e' definido:
//
//    AttachSlotName(slot invalido) -> ""       (nunca nullptr)
//    AttachCount(slot invalido)    -> 0
//    AttachList(slot invalido)     -> nullptr
//    AttachValue(slot|idx invalido)-> -1
//    WeaponName/CamoName/CharName/AttachName(idx invalido) -> nullptr
//
//  Com Ready() == false, tudo cai nas mesmas respostas de "invalido".
//  Chamar com qualquer int, em qualquer estado, nunca estoura array.
//
//  ===========================================================================
//  THREADS
//  ===========================================================================
//  Tick() roda no thread do jogo e e' quem MONTA e PUBLICA as listas.
//  O menu LE no thread de render. A leitura so' e' segura depois que Ready()
//  vira true (o flag e' publicado com release/acquire).
//  Regra: escrever = so' no Tick(). Ler = qualquer thread, com Ready() true.
//  Dump() e' a unica que pode ser chamada de qualquer thread em qualquer
//  estado (ver contrato abaixo).
// ============================================================================

namespace Unlock {

    // ------------------------------------------------------------------------
    //  Install()
    //  Registra os hooks/patches do subsistema de unlock.
    //
    //  Pre-condicoes : chamado no thread de inicializacao, UMA vez, depois de
    //                  SDK::Init() e com o jogo ja' carregado o suficiente para
    //                  os alvos de hook existirem.
    //  Retorno       : true  -> hooks instalados nesta chamada.
    //                  false -> nada feito, por um destes motivos:
    //                           - ja' estava instalado (2a chamada em diante);
    //                           - um alvo de hook nao foi encontrado (SDK cedo
    //                             demais);
    //                           - um hook falhou ao instalar.
    //  Falha         : nao e' fatal. Em caso de false, chamar de novo mais tarde
    //                  e' valido e esperado (o alvo pode ja' existir).
    //  Efeito        : nao monta catalogo, nao toca em Ready()/Generation().
    //  Idempotencia  : sim — 2a chamada devolve false e nao reinstala.
    // ------------------------------------------------------------------------
    bool Install();

    // ------------------------------------------------------------------------
    //  Tick()
    //  Um passo por frame do jogo. E' o UNICO ponto de escrita do estado.
    //
    //  Pre-condicoes : thread do jogo. Install() ja' retornou true.
    //  Responsabilidades, nesta ordem:
    //                  1) se Ready() == false e ha' dados, montar as listas;
    //                  2) publicar Generation() novo e entao Ready() = true;
    //                  3) aplicar selecao pendente vinda do menu.
    //  Retorno       : void.
    //  Custo         : barato quando Ready() == true e nada mudou (early-out).
    //  Chamar 2x no mesmo frame: inofensivo, mas 1x e' o esperado.
    // ------------------------------------------------------------------------
    void Tick();

    // ------------------------------------------------------------------------
    //  Dump()
    //  Escreve o catalogo atual no log (via Log()), legivel, para diagnostico.
    //  Nao exporta arquivo, nao altera estado, nao toca no jogo.
    //
    //  Pre-condicoes : nenhuma. Pode ser chamada de qualquer thread, em
    //                  qualquer estado — inclusive do thread de render, para o
    //                  botao "Dump no log" do menu.
    //  Com !Ready()  : escreve "catalogo vazio" e sai. Nao e' erro.
    //  Retorno       : void.
    // ------------------------------------------------------------------------
    void Dump();

    // ------------------------------------------------------------------------
    //  Estado
    // ------------------------------------------------------------------------
    bool Ready();      // true somente apos a publicacao no Tick()
    int  Generation(); // muda a cada reconstrucao; compare antes de reusar ponteiro

    // ------------------------------------------------------------------------
    //  Armas  — List() != nullptr <=> Count() > 0
    // ------------------------------------------------------------------------
    int WeaponCount();                       // quantidade de armas do loadout
    const char* const* WeaponList();         // "00 - AK47", ... ; nullptr se vazio
    const char* WeaponName(int idx);         // nullptr se idx fora da faixa

    // ------------------------------------------------------------------------
    //  Anexos  — slot em [0, AttachSlotCount())
    // ------------------------------------------------------------------------
    int AttachSlotCount();                   // 3 (sights / accessories / barrels)
    const char* AttachSlotName(int slot);    // nunca nullptr; "" se slot invalido
    int AttachCount(int slot);               // 0 se slot invalido
    const char* const* AttachList(int slot); // nullptr se slot invalido ou vazio
    const char* AttachName(int slot, int idx); // nullptr se slot ou idx invalido
    int AttachValue(int slot, int idx);      // valor real do enum; -1 se invalido

    // ------------------------------------------------------------------------
    //  Skins
    // ------------------------------------------------------------------------
    int CamoCount();
    const char* const* CamoList();
    const char* CamoName(int idx);

    // ------------------------------------------------------------------------
    //  Personagens
    // ------------------------------------------------------------------------
    int CharCount();
    const char* const* CharList();
    const char* CharName(int idx);
}