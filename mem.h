#pragma once
// ============================================================================
//  mem.h — ACESSO A MEMORIA. Dono unico. Nao sabe de modulo nem de pattern.
//
//  ===========================================================================
//  QUATRO CAMADAS. O RETORNO DIZ O QUE ACONTECEU — NAO EXISTE FALHA MUDA.
//  ===========================================================================
//  A) ReadAt / WriteAt          — CRUA, sem SEH. Mais barata.
//     Uso  : endereco ja' validado por voce.
//     Falha: nao ha'. addr nulo => T{} / no-op. addr ruim => CRASH.
//     Nao retorna status porque nao tem como falhar silenciosamente.
//
//  B) ReadOr / WriteOr          — PROTEGIDA, com FALLBACK ESCOLHIDO POR VOCE.
//     Uso  : quer um valor e nao quer saber do status.
//     Falha: explicita — o fallback e' parametro, nao um default escondido.
//     NUNCA crasha. Nao retorna status.
//
//  C) TryRead / TryWrite        — PROTEGIDA, com STATUS EXPLICITO.
//     Uso  : a distincao "falhou" x "leu zero" importa.
//     Falha: retorno bool. 'out' SO e' escrito quando true.
//
//  D) ReadMemEx / WriteMemEx    — PROTEGIDA, bloco, com CAUSA da falha.
//     Uso  : copiar N bytes. WriteMemEx ajusta e restaura protecao de pagina.
//     Falha: MemResult (Ok / BadArg / BadRange / ProtectFail / AccessFail /
//            RestoreFail). NUNCA crasha.
//
//  REGRA: ponteiro vindo de ponteiro => camada B/C/D. ReadAt so' para
//  endereco conhecido bom. Na duvida, TryRead — ela nunca mente.
//
//  T deve ser trivialmente copiavel (POD) nas camadas B e C: copia via memcpy.
//  Este header NAO valida que a faixa esta' mapeada antes de acessar — quem
//  faz isso e' o __try (camadas B/C/D) ou RangeReadable() (pre-validacao).
// ============================================================================

#include "common.h"

// ---------------------------------------------------------------------------
//  FAIXA DE ENDERECO
// ---------------------------------------------------------------------------
//  InUserRange: checagem BARATA de faixa numerica. NAO consulta o sistema de
//  memoria. Endereco dentro da faixa mas nao mapeado PASSA por aqui — quem
//  reprova e' o acesso.
//  IsBadPtr: alias legado. O nome promete mais do que entrega (nao e' um teste
//  de "ponteiro ruim", e' um teste de faixa). Prefira InUserRange em codigo novo.
// ---------------------------------------------------------------------------
inline bool InUserRange(uintptr_t a) {
    return a >= 0x10000u && a <= 0x7FFFFFFFFFFFULL;
}
inline bool IsBadPtr(uintptr_t a) { return !InUserRange(a); }

// ---------------------------------------------------------------------------
//  RangeOk: valida a FAIXA INTEIRA, nao so' o primeiro byte.
//    - rejeita n == 0
//    - rejeita addr fora da faixa de usuario
//    - detecta overflow em addr + n - 1
//    - rejeita se o ULTIMO byte cair fora da faixa
//  Continua sem garantir mapeamento.
// ---------------------------------------------------------------------------
inline bool RangeOk(uintptr_t addr, size_t n) {
    if (n == 0) return false;
    if (!InUserRange(addr)) return false;
    if (addr > UINTPTR_MAX - (n - 1)) return false;
    return InUserRange(addr + n - 1);
}

// ---------------------------------------------------------------------------
//  RangeReadable: pre-valida MAPEAMENTO, pagina a pagina via VirtualQuery.
//  Caro (uma chamada por regiao), mas e' a unica forma de saber ANTES de
//  acessar. Rejeita regiao nao commitada, PAGE_NOACCESS e PAGE_GUARD.
// ---------------------------------------------------------------------------
inline bool RangeReadable(uintptr_t addr, size_t n) {
    if (!RangeOk(addr, n)) return false;

    const uintptr_t end = addr + n;
    uintptr_t p = addr;
    while (p < end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<const void*>(p), &mbi, sizeof(mbi))) return false;
        if (mbi.State != MEM_COMMIT) return false;
        if (mbi.Protect & PAGE_NOACCESS) return false;
        if (mbi.Protect & PAGE_GUARD) return false;

        const uintptr_t next =
            reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (next <= p) return false;   // defensivo: VirtualQuery nao avancou
        p = next;
    }
    return true;
}

// ---------------------------------------------------------------------------
//  Resultado explicito
// ---------------------------------------------------------------------------
enum class MemResult : int {
    Ok = 0,
    BadArg,        // ponteiro nulo / n == 0
    BadRange,      // fora da faixa de usuario ou overflow
    ProtectFail,   // VirtualProtect nao conseguiu aplicar
    AccessFail,    // o __try pegou excecao (pagina nao mapeada)
    RestoreFail,   // escreveu, mas NAO conseguiu restaurar a protecao
};

inline const char* MemResultName(MemResult r) {
    switch (r) {
    case MemResult::Ok:          return "Ok";
    case MemResult::BadArg:      return "BadArg";
    case MemResult::BadRange:    return "BadRange";
    case MemResult::ProtectFail: return "ProtectFail";
    case MemResult::AccessFail:  return "AccessFail";
    case MemResult::RestoreFail: return "RestoreFail";
    }
    return "?";
}

// ---------------------------------------------------------------------------
//  Helpers com locais POD — concentram TODO o __try do arquivo.
//  Evita C2712 (MSVC proibe __try em funcao com unwinding de objetos).
// ---------------------------------------------------------------------------
inline bool MemReadRaw(const void* src, void* dst, size_t n) {
    __try { memcpy(dst, src, n); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}

inline bool MemWriteRaw(void* dst, const void* src, size_t n) {
    __try { memcpy(dst, src, n); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    return true;
}

// ===========================================================================
//  CAMADA A — crua
// ===========================================================================
template <typename T> inline T ReadAt(void* addr) {
    return addr ? *reinterpret_cast<const T*>(addr) : T{};
}
template <typename T> inline T ReadAt(uintptr_t addr) {
    return *reinterpret_cast<const T*>(addr);
}
template <typename T> inline T ReadAt(void* base, uint32_t off) {
    return base ? *reinterpret_cast<const T*>(reinterpret_cast<const uint8_t*>(base) + off) : T{};
}
template <typename T> inline T ReadAt(uintptr_t base, uint32_t off) {
    return *reinterpret_cast<const T*>(base + off);
}

template <typename T> inline void WriteAt(void* addr, T v) {
    if (addr) *reinterpret_cast<T*>(addr) = v;
}
template <typename T> inline void WriteAt(uintptr_t addr, T v) {
    *reinterpret_cast<T*>(addr) = v;
}
template <typename T> inline void WriteAt(void* base, uint32_t off, T v) {
    if (base) *reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(base) + off) = v;
}
template <typename T> inline void WriteAt(uintptr_t base, uint32_t off, T v) {
    *reinterpret_cast<T*>(base + off) = v;
}

// ===========================================================================
//  CAMADA B — protegida, fallback explicito
// ===========================================================================
template <typename T>
inline T ReadOr(uintptr_t addr, T fallback) {
    if (!InUserRange(addr)) return fallback;
    T out{};
    if (!MemReadRaw(reinterpret_cast<const void*>(addr), &out, sizeof(T))) return fallback;
    return out;
}

// Compat: SafeRead = ReadOr. O problema do nome antigo era o overload sem
// fallback esconder o default; aqui ele e' so' um atalho documentado.
template <typename T>
inline T SafeRead(uintptr_t addr, T fallback) { return ReadOr<T>(addr, fallback); }

template <typename T>
inline T SafeRead(uintptr_t addr) { return ReadOr<T>(addr, T{}); }

template <typename T>
inline void WriteOr(uintptr_t addr, const T& val) {
    if (!InUserRange(addr)) return;
    MemWriteRaw(reinterpret_cast<void*>(addr), &val, sizeof(T));
}
template <typename T>
inline void SafeWrite(uintptr_t addr, const T& val) { WriteOr<T>(addr, val); }
template <typename T>
inline void SafeWrite(void* addr, const T& val) {
    WriteOr<T>(reinterpret_cast<uintptr_t>(addr), val);
}
inline void SafeWrite(uintptr_t addr, const void* src, size_t n) {
    if (!src) return;
    MemWriteRaw(reinterpret_cast<void*>(addr), src, n);
}

// ===========================================================================
//  CAMADA C — protegida, status explicito
// ===========================================================================
template <typename T>
inline bool TryRead(void* addr, T& out) {
    if (!addr) return false;
    return MemReadRaw(addr, &out, sizeof(T));
}
template <typename T>
inline bool TryRead(uintptr_t addr, T& out) {
    if (!InUserRange(addr)) return false;
    return MemReadRaw(reinterpret_cast<const void*>(addr), &out, sizeof(T));
}

template <typename T>
inline bool TryWrite(void* addr, const T& v) {
    if (!addr) return false;
    return MemWriteRaw(addr, &v, sizeof(T));
}
template <typename T>
inline bool TryWrite(uintptr_t addr, const T& v) {
    if (!InUserRange(addr)) return false;
    return MemWriteRaw(reinterpret_cast<void*>(addr), &v, sizeof(T));
}

// ===========================================================================
//  CAMADA D — bloco, com causa da falha
// ===========================================================================
inline MemResult ReadMemEx(uintptr_t addr, void* out, size_t n) {
    if (!out) return MemResult::BadArg;
    if (!RangeOk(addr, n)) return MemResult::BadRange;
    return MemReadRaw(reinterpret_cast<const void*>(addr), out, n)
        ? MemResult::Ok : MemResult::AccessFail;
}

inline bool ReadMem(uintptr_t addr, void* out, size_t n) {
    return ReadMemEx(addr, out, n) == MemResult::Ok;
}

// Precedencia quando mais de uma coisa falha:
//   AccessFail > RestoreFail  (se a escrita nao aconteceu, e' isso que importa;
//   o estado da protecao e' reportado por WriteMemEx separadamente quando Ok).
// Se a restauracao falhar DEPOIS de escrever com sucesso => RestoreFail.
const int kMaxProtectPages = 64;

inline MemResult WriteMemEx(uintptr_t addr, const void* in, size_t n) {
    if (!in) return MemResult::BadArg;
    if (!RangeOk(addr, n)) return MemResult::BadRange;

    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    const size_t page = si.dwPageSize ? si.dwPageSize : 4096;

    const uintptr_t first = addr & ~(uintptr_t)(page - 1);
    const uintptr_t lastPage = (addr + n - 1) & ~(uintptr_t)(page - 1);
    const size_t    pages = (size_t)((lastPage - first) / page) + 1;

    // Range grande: uma protecao unica. 'old' traz so' a protecao da PRIMEIRA
    // pagina — restaurar com ela aplica uma protecao unica ao range inteiro,
    // o que e' impreciso se as paginas tinham protecoes diferentes.
    if (pages > (size_t)kMaxProtectPages) {
        DWORD old = 0;
        if (!VirtualProtect(reinterpret_cast<void*>(addr), n,
            PAGE_EXECUTE_READWRITE, &old))
            return MemResult::ProtectFail;

        const bool wrote = MemWriteRaw(reinterpret_cast<void*>(addr), in, n);

        DWORD tmp = 0;
        const bool restored =
            VirtualProtect(reinterpret_cast<void*>(addr), n, old, &tmp) != FALSE;

        if (!wrote) return MemResult::AccessFail;
        return restored ? MemResult::Ok : MemResult::RestoreFail;
    }

    DWORD  old[kMaxProtectPages] = {};
    size_t done = 0;

    for (; done < pages; ++done) {
        void* p = reinterpret_cast<void*>(first + done * page);
        if (!VirtualProtect(p, page, PAGE_EXECUTE_READWRITE, &old[done])) {
            for (size_t k = 0; k < done; ++k) {
                DWORD tmp = 0;
                VirtualProtect(reinterpret_cast<void*>(first + k * page), page, old[k], &tmp);
            }
            return MemResult::ProtectFail;
        }
    }

    const bool wrote = MemWriteRaw(reinterpret_cast<void*>(addr), in, n);

    // Restaura pagina a pagina, e VERIFICA cada restauracao.
    bool restored = true;
    for (size_t i = 0; i < pages; ++i) {
        DWORD tmp = 0;
        if (!VirtualProtect(reinterpret_cast<void*>(first + i * page), page, old[i], &tmp))
            restored = false;
    }

    if (!wrote) return MemResult::AccessFail;
    return restored ? MemResult::Ok : MemResult::RestoreFail;
}

inline bool WriteMem(uintptr_t addr, const void* in, size_t n) {
    return WriteMemEx(addr, in, n) == MemResult::Ok;
}