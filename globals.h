#pragma once
// =====================================================================
//  globals.h
//  Declaracoes globais compartilhadas.
// =====================================================================

#include "deps.h"
#include <cstddef>

// ---------------------------------------------------------------------
//  Config
// ---------------------------------------------------------------------
struct Config;

extern Config  g_cfg;
extern Config& C;

extern HMODULE g_hModule;

// ---------------------------------------------------------------------
//  Logging
// ---------------------------------------------------------------------
#if defined(_MSC_VER)
#  include <sal.h>
#  define VN_PRINTF_FMT _Printf_format_string_
#else
#  define VN_PRINTF_FMT
#endif

void   LogInit();
void   Log(VN_PRINTF_FMT const char* fmt, ...);

// Ring buffer em memoria: o menu le o log daqui (sem tocar no arquivo).
// LogSnapshot escreve o texto das ultimas linhas em 'dst' (NUL-terminated)
// e devolve o numero de bytes escritos.
size_t LogSnapshot(char* dst, size_t cap);
void   LogClearRing();

