#pragma once
// =====================================================================
//  globals.h
//  Declaracoes globais compartilhadas. DONO UNICO da macro C.
//
//  >>> FIX: config.h TAMBEM definia 'C' (com corpo diferente) e redeclarava
//      g_cfg/C. Duas definicoes concorrentes da MESMA macro, com o vencedor
//      decidido pela ordem de include — funcionava por acidente e quebrava
//      silenciosamente se um include mudasse de lugar. Agora so' este arquivo
//      define a macro. config.h cuida apenas da ESTRUTURA Config.
//
//  Como este header e' alcancado por TODOS os TUs (via common.h), todo mundo
//  que usa C.<campo> tem a macro disponivel, sem depender de ordem.
// =====================================================================

#include "deps.h"

// ---------------------------------------------------------------------
//  Config
//  A definicao completa de Config vive em config.h. Aqui apenas a
//  declaracao antecipada, suficiente para os simbolos abaixo.
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

void LogInit();
void Log(VN_PRINTF_FMT const char* fmt, ...);

// =====================================================================
//  Macro C - ACESSO DUPLO AO MESMO OBJETO
// =====================================================================
//
//  Existem DUAS formas de tocar os mesmos campos de g_cfg:
//
//      C.esp        -> usa a REFERENCIA global "Config& C"
//      C(esp)       -> usa esta MACRO, que expande para g_cfg.esp
//
//  Como a macro e "function-like", ela so expande quando "C" e seguido
//  de "(". Portanto "C.esp" NAO e tocado pela macro e resolve para a
//  referencia. As duas formas apontam para o MESMO objeto (g_cfg).
//
//  Regras praticas:
//    - Qualquer sintaxe que precise do valor/referencia do objeto usa
//      "C" (sem parenteses): &C, sizeof(C), f(C), C.esp = 1.f;
//    - A forma curta de campo e C(campo).
//    - "C" seguido de "(" SEMPRE expande. Nao existe uso de C como
//      algo chamavel, entao nao ha ambiguidade.
//
//  ATENCAO: a macro e definida aqui, DEPOIS de todos os includes de
//  terceiros. Assim "imgui.h"/"MinHook.h" nao enxergam "C" como macro.
//  Se algum header posterior definir "C", ele sobrescreveria esta macro —
//  e' por isso que config.h nao define mais nada.
// =====================================================================
#ifdef C
#undef C
#endif
#define C(...) g_cfg.__VA_ARGS__