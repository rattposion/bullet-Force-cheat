#pragma once
// =====================================================================
//  deps.h
//  Dependencias externas e de plataforma (Windows, ImGui, MinHook) e
//  contêineres/utilitarios da STL usados de forma transversal.
//
//  Inclua este header somente onde as dependencias pesadas forem
//  realmente necessarias. Para tipos matematicos use "types.h".
// =====================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <initializer_list>

#include "imgui.h"
#include "MinHook.h"

// Declaracao necessaria para o backend Win32 do ImGui.
// Depende de IMGUI_IMPL_API, por isso vem depois de "imgui.h".
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg,
    WPARAM wParam, LPARAM lParam);