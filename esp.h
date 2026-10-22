// ============================================================================
//  esp.h — stub. A definicao COMPLETA de Snap e as declaracoes canonicas vivem
//  no features.h. Este header existe so' pra quem quer o ESP sem arrastar o
//  features.h inteiro (basta o ponteiro const Snap*).
//
//  v2: adicionado Reset() (faltava — dava erro se chamado com so' este include).
// ============================================================================
#pragma once

namespace ESP {
    struct Snap;                 // definicao completa em features.h

    void        Tick();          // thread do jogo  (dono: esp.cpp)
    const Snap* Latest();        // thread de render (dono: esp.cpp)
    void        Draw();          // thread de render (dono: esp.cpp)
    int         Count();
    int         Invisiveis();
    void        Reset();
}