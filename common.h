#pragma once
// =====================================================================
//  common.h - UMBRELLA
//
//  Mantem a compatibilidade: quem faz #include "common.h" continua
//  recebendo tudo o que recebia antes. A modularizacao vive nos
//  headers abaixo, que podem ser incluidos individualmente por quem
//  nao precisar do conjunto completo.
//
//      types.h    -> Vec2, Vec3, Quat (sem Windows/ImGui/MinHook)
//      deps.h     -> Windows, ImGui, MinHook, STL
//      globals.h  -> g_cfg, C, macro C(...), g_hModule, Log
//
//  Ordem importa: globals.h define a macro C(...) no final, portanto
//  "deps.h" (e o imgui.h dentro dele) precisa vir antes.
// =====================================================================

#include "deps.h"
#include "types.h"
#include "globals.h"