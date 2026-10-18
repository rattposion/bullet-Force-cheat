#pragma once
// ============================================================================
//  memory.h — UMBRELLA.
//
//  ESTE ARQUIVO NAO CONTEM DEFINICAO NENHUMA. So' inclui.
//  Se voce encontrar aqui IsBadPtr / SafeRead / SafeWrite / ReadMem / WriteMem,
//  ou ModuleBase / PatternScan / Scan, e' conteudo antigo que nao foi removido.
//
//  Donos:
//      mem.h   -> faixas de endereco, camadas de acesso, resultados
//      scan.h  -> modulos carregados, pattern scan, parser de assinatura
//
//  Regra: qualquer novo inline/template de memoria entra em mem.h ou scan.h,
//  nunca aqui.
// ============================================================================

#include "mem.h"
#include "scan.h"