#pragma once
// ============================================================================
//  scan.h — modulos carregados e busca de padrao. Nao faz acesso generico.
//
//  PARSER DE ASSINATURA x BUSCA sao coisas separadas de proposito:
//    ParseSignature() diz se o TEXTO e' valido e devolve bytes+mask.
//    PatternScan()    busca bytes+mask ja' prontos.
//  Assim "assinatura malformada" deixa de ser indistinguivel de "nao achou".
//
//  POLITICA DE ERRO: nenhuma funcao aqui lanca nem captura nada alem do
//  __try do PatternScan. Falha = retorno explicito (SigError / 0 / false).
// ============================================================================

#include "common.h"
#include "mem.h"
#include <psapi.h>

#if defined(_MSC_VER)
#pragma comment(lib, "psapi.lib")
#endif

// ---------------------------------------------------------------------------
//  Modulos
// ---------------------------------------------------------------------------
inline uintptr_t ModuleBase(const char* name) {
    return reinterpret_cast<uintptr_t>(GetModuleHandleA(name));
}

inline size_t ModuleSize(const char* name) {
    HMODULE h = GetModuleHandleA(name);
    if (!h) return 0;
    MODULEINFO mi{};
    if (!GetModuleInformation(GetCurrentProcess(), h, &mi, sizeof(mi))) return 0;
    return mi.SizeOfImage;
}

inline bool IsInModule(uintptr_t addr, const char* mod) {
    const uintptr_t base = ModuleBase(mod);
    const size_t    size = ModuleSize(mod);
    if (!base || !size) return false;
    if (base > UINTPTR_MAX - size) return false;   // overflow defensivo
    return addr >= base && addr < base + size;
}

// ---------------------------------------------------------------------------
//  Parser de assinatura
// ---------------------------------------------------------------------------
enum class SigError : int {
    Ok = 0,
    Null,        // sig == nullptr
    Empty,       // nenhum token
    BadArg,      // buffer de saida nulo ou capacidade zero
    BadToken,    // token com caractere que nao e' hex nem '?'
    TooLong,     // mais tokens que a capacidade
};

inline const char* SigErrorName(SigError e) {
    switch (e) {
    case SigError::Ok:       return "Ok";
    case SigError::Null:     return "Null";
    case SigError::Empty:    return "Empty";
    case SigError::BadArg:   return "BadArg";
    case SigError::BadToken: return "BadToken";
    case SigError::TooLong:  return "TooLong";
    }
    return "?";
}

inline int HexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// ---------------------------------------------------------------------------
//  ParseSignature
//
//  'cap' e' a QUANTIDADE MAXIMA de bytes. Os buffers precisam ter cap + 1:
//  o parser sempre escreve o terminador nulo em outMask[outLen].
//
//  mask por byte:
//      'x' -> byte exato
//      '?' -> byte livre
//      'h' -> compara so' o nibble ALTO  (ex.: "4?")
//      'l' -> compara so' o nibble BAIXO (ex.: "?4")
//
//  Token de 1 digito hex ("4") vale 0x04 — comportamento antigo mantido.
//  Retorno: SigError::Ok somente se o texto inteiro for valido. Em qualquer
//  outro caso outLen fica 0 e o conteudo dos buffers e' descartavel.
// ---------------------------------------------------------------------------
inline SigError ParseSignature(const char* sig,
    unsigned char* outBytes, char* outMask,
    size_t cap, size_t& outLen)
{
    outLen = 0;
    if (!sig) return SigError::Null;
    if (!outBytes || !outMask || cap == 0) return SigError::BadArg;

    const char* p = sig;
    size_t n = 0;

    while (*p) {
        while (*p == ' ') ++p;
        if (!*p) break;

        if (n >= cap) return SigError::TooLong;

        const char* tok = p;
        while (*p && *p != ' ') ++p;
        const size_t tlen = (size_t)(p - tok);

        if (tlen == 1) {
            const int v1 = HexNibble(tok[0]);
            if (v1 < 0) {
                if (tok[0] != '?') return SigError::BadToken;
                outMask[n] = '?'; outBytes[n] = 0;
            }
            else { outMask[n] = 'x'; outBytes[n] = (unsigned char)v1; }
        }
        else if (tlen == 2) {
            const int hi = HexNibble(tok[0]);
            const int lo = HexNibble(tok[1]);
            if (hi < 0 && lo < 0) {
                if (tok[0] != '?' || tok[1] != '?') return SigError::BadToken;
                outMask[n] = '?'; outBytes[n] = 0;
            }
            else if (hi < 0) {
                if (tok[0] != '?') return SigError::BadToken;
                outMask[n] = 'l'; outBytes[n] = (unsigned char)lo;
            }
            else if (lo < 0) {
                if (tok[1] != '?') return SigError::BadToken;
                outMask[n] = 'h'; outBytes[n] = (unsigned char)(hi << 4);
            }
            else { outMask[n] = 'x'; outBytes[n] = (unsigned char)((hi << 4) | lo); }
        }
        else {
            // token longo: aceita so' hex puro (ex.: "E9", "0000")
            for (size_t i = 0; i < tlen; ++i)
                if (HexNibble(tok[i]) < 0) return SigError::BadToken;
            outMask[n] = 'x';
            outBytes[n] = (unsigned char)strtoul(tok, nullptr, 16);
        }
        ++n;
    }

    if (!n) return SigError::Empty;

    outMask[n] = '\0';      // exige buffer de cap + 1
    outLen = n;
    return SigError::Ok;
}

// ---------------------------------------------------------------------------
//  PatternScan (bytes + mask ja' prontos)
//
//  'mask' PRECISA estar terminada em nulo — o comprimento vem de strlen.
//
//  Valida base, size, pattern, mask, comprimento e overflow de base+size.
//  O __try envolve o laco inteiro: uma pagina nao mapeada no meio do modulo
//  aborta o scan em vez de derrubar o processo.
//  Retorno: 0 = nao achou OU varredura abortada. Distinguir os dois exigiria
//  um out-param; para isso use ScanEx e olhe o SigError (que cobre o parsing,
//  nao a varredura).
// ---------------------------------------------------------------------------
inline uintptr_t PatternScan(uintptr_t base, size_t size,
    const void* pattern, const char* mask)
{
    if (!base || !size || !pattern || !mask) return 0;
    if (base > UINTPTR_MAX - size) return 0;

    const size_t plen = strlen(mask);
    if (!plen || plen > size) return 0;

    const unsigned char* p = reinterpret_cast<const unsigned char*>(base);
    const unsigned char* v = reinterpret_cast<const unsigned char*>(pattern);
    const size_t last = size - plen;

    __try {
        for (size_t i = 0; i <= last; ++i) {
            size_t j = 0;
            for (; j < plen; ++j) {
                const char m = mask[j];
                if (m == '?') continue;
                const unsigned char b = p[i + j];
                if (m == 'x') { if (b != v[j]) break; }
                else if (m == 'h') { if ((b >> 4) != (v[j] >> 4)) break; }
                else { if ((b & 0x0F) != (v[j] & 0x0F)) break; }
            }
            if (j == plen) return base + i;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }

    return 0;
}

// Compat: mesma assinatura antiga (modulo por nome).
inline uintptr_t PatternScan(const char* mod, const char* pat, const char* mask) {
    return PatternScan(ModuleBase(mod), ModuleSize(mod), pat, mask);
}

// ---------------------------------------------------------------------------
//  Scan — distingue "malformado" de "nao achou"
//  'outErr' (opcional) recebe o resultado do PARSER:
//      Ok        -> a busca rodou; retorno 0 significa nao encontrado.
//      qualquer outro -> a assinatura estava malformada; a busca nem rodou.
// ---------------------------------------------------------------------------
inline uintptr_t ScanEx(const char* mod, const char* sig, SigError* outErr) {
    unsigned char bytes[256] = {};
    char          mask[256] = {};
    size_t        n = 0;

    const SigError e = ParseSignature(sig, bytes, mask, sizeof(mask) - 1, n);
    if (outErr) *outErr = e;
    if (e != SigError::Ok) return 0;

    return PatternScan(ModuleBase(mod), ModuleSize(mod), bytes, mask);
}

// Atalho: 0 tanto para malformado quanto para nao achou.
inline uintptr_t Scan(const char* mod, const char* sig) {
    return ScanEx(mod, sig, nullptr);
}