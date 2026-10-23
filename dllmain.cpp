// ============================================================================
//  dllmain.cpp — bfi (v3)
//
//  Mudancas:
//   1) Teardown() remove os HANDLERS (VEH de primeira chance + filtro top-level)
//      ANTES do FreeLibrary — sem isso a DLL sai do mapa e o proximo AV chama
//      endereco liberado.
//   2) Modulos de feature sao INICIALIZADOS aqui (Combat/Weapons/Aimbot/
//      Triggerbot/AntiCheat/Recoil).
//   3) CrashFilter com modulo+offset, chain com o filtro anterior (Unity).
//   4) Log/LogInit vem do globals.h (sem re-declarar -> sem C28251).
// ============================================================================
#define BFI_LOG_FIRST_CHANCE 1

#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cstring>

#include "il2cpp.h"
#include "il2cpp_api.h"
#include "common.h"
#include "config.h"
#include "sdk.h"
#include "hooks.h"
#include "features.h"

HMODULE g_hModule = nullptr;
std::atomic<bool> g_unload{ false };

static std::atomic<bool> g_tornDown{ false };
static bool                      g_inFilter = false;
static LPTOP_LEVEL_EXCEPTION_FILTER g_prevFilter = nullptr;
static PVOID                     g_vehFirstChance = nullptr;
static int                       g_crashLogs = 0;

// ---------------------------------------------------------------------------
//  Modulo + RVA a partir de um endereco. Devolve "?" se nao pertencer a modulo.
// ---------------------------------------------------------------------------
static const char* ModuleOf(void* addr, uintptr_t* outRva)
{
    thread_local char name[MAX_PATH];
    strcpy_s(name, "?");
    if (outRva) *outRva = 0;

    HMODULE h = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        (LPCSTR)addr, &h) && h)
    {
        GetModuleFileNameA(h, name, MAX_PATH);
        char* s = strrchr(name, '\\');
        if (s) memmove(name, s + 1, strlen(s + 1) + 1);
        if (outRva) *outRva = (uintptr_t)addr - (uintptr_t)h;
    }
    return name;
}

// ---------------------------------------------------------------------------
//  Filtro de ultimo recurso. Loga e passa a bola pro handler anterior (Unity).
// ---------------------------------------------------------------------------
static LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep)
{
    if (g_inFilter) return EXCEPTION_CONTINUE_SEARCH;
    g_inFilter = true;

    EXCEPTION_RECORD* er = ep ? ep->ExceptionRecord : nullptr;
    if (er && g_crashLogs < 50) {
        ++g_crashLogs;

        uintptr_t rva = 0;
        const char* mod = ModuleOf(er->ExceptionAddress, &rva);

        if (er->ExceptionCode == 0xC0000005 && er->NumberParameters >= 2) {
            static const char* acc[3] = { "read", "write", "execute" };
            ULONG_PTR kind = er->ExceptionInformation[0];
            Log("!!!!!!!! EXCECAO AV %s em %s+0x%llX (alvo=%p) fase=%s",
                acc[kind < 3 ? (int)kind : 0], mod,
                (unsigned long long)rva,
                (void*)er->ExceptionInformation[1],
                g_phase ? g_phase : "?");
        }
        else {
            Log("!!!!!!!! EXCECAO code=0x%08X em %s+0x%llX fase=%s",
                er->ExceptionCode, mod, (unsigned long long)rva,
                g_phase ? g_phase : "?");
        }
    }

    LONG r = EXCEPTION_EXECUTE_HANDLER;
    if (g_prevFilter && ep) {
        __try { r = g_prevFilter(ep); }
        __except (EXCEPTION_EXECUTE_HANDLER) { r = EXCEPTION_EXECUTE_HANDLER; }
    }

    g_inFilter = false;
    return r;
}

#if BFI_LOG_FIRST_CHANCE
// ---------------------------------------------------------------------------
//  VEH de primeira chance: roda ANTES de qualquer __try. So loga AV e devolve
//  CONTINUE_SEARCH, entao o SEH do projeto continua funcionando normalmente.
// ---------------------------------------------------------------------------
static LONG WINAPI FirstChanceVEH(EXCEPTION_POINTERS* ep)
{
    EXCEPTION_RECORD* er = ep ? ep->ExceptionRecord : nullptr;
    if (!er || er->ExceptionCode != 0xC0000005) return EXCEPTION_CONTINUE_SEARCH;

    static ULONGLONG last = 0;
    static int       n = 0;

    ULONGLONG now = GetTickCount64();
    if (n < 200 && now - last > 300) {
        last = now;
        ++n;

        uintptr_t rva = 0;
        const char* mod = ModuleOf(er->ExceptionAddress, &rva);
        static const char* acc[3] = { "read", "write", "execute" };
        ULONG_PTR kind = (er->NumberParameters >= 2) ? er->ExceptionInformation[0] : 0;

        Log("[veh] AV %s em %s+0x%llX (alvo=%p) fase=%s",
            acc[kind < 3 ? (int)kind : 0], mod,
            (unsigned long long)rva,
            (er->NumberParameters >= 2) ? (void*)er->ExceptionInformation[1] : nullptr,
            g_phase ? g_phase : "?");
    }

    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

// ---------------------------------------------------------------------------
static void Teardown()
{
    if (g_tornDown.exchange(true)) return;

    // handlers FORA do mapa antes de a DLL sair
    if (g_vehFirstChance) { RemoveVectoredExceptionHandler(g_vehFirstChance); g_vehFirstChance = nullptr; }
    if (g_prevFilter) { SetUnhandledExceptionFilter(g_prevFilter); g_prevFilter = nullptr; }

    Hooks::Shutdown();
    SDK::Shutdown();
}

// ---------------------------------------------------------------------------
//  Vigia do unload. Thread propria: FreeLibraryAndExitThread de dentro do
//  swap hook mataria a thread de render.
// ---------------------------------------------------------------------------
static DWORD WINAPI UnloadWatch(LPVOID)
{
    while (!g_unload.load()) Sleep(50);

    Log("[bfi] descarregando...");
    Sleep(200);

    Teardown();

    Log("[bfi] tchau");
    FreeLibraryAndExitThread(g_hModule, 0);
    return 0;
}

// ---------------------------------------------------------------------------
static DWORD WINAPI Main(LPVOID)
{
    LogInit();

    Log("================================================");
    Log("[bfi] injetado. aguardando o jogo...");
    Log("================================================");

    g_prevFilter = SetUnhandledExceptionFilter(CrashFilter);
#if BFI_LOG_FIRST_CHANCE
    g_vehFirstChance = AddVectoredExceptionHandler(1, FirstChanceVEH);
#endif

    while (!GetModuleHandleA("GameAssembly.dll")) {
        if (g_unload.load()) return 0;
        Sleep(100);
    }
    Log("[bfi] GameAssembly.dll = %p", GetModuleHandleA("GameAssembly.dll"));

    g_phase = "resolve";
    if (!IL2CPP_API::Resolve()) { Log("[bfi] Resolve FALHOU"); return 0; }

    g_phase = "resolve2";
    Log("[bfi] IL2CPP::Init = %d", (int)IL2CPP::Init());

    g_phase = "domain";
    Il2CppDomain* dom = il2cpp_domain_get ? il2cpp_domain_get() : nullptr;
    if (!dom) { Log("[bfi] domain NULO"); return 0; }

    g_phase = "attach";
    if (il2cpp_thread_attach) il2cpp_thread_attach(dom);

    g_phase = "sdk";
    if (!SDK::Init()) Log("[bfi] SDK::Init FALHOU");

    g_phase = "config";
    ConfigIO::Init();

    g_phase = "hooks";
    if (!Hooks::Init()) Log("[bfi] Hooks::Init FALHOU");

    // ---- modulos de feature ----
    g_phase = "features";
    Combat::Init();
    Weapons::Init();
    Aimbot::Install();
    Triggerbot::Install();
    AntiCheat::Install();
    Recoil::Install();
    Log("[bfi] modulos de feature inicializados");

    g_phase = "done";
    Log("[bfi] ===== pronto =====");

    CreateThread(nullptr, 0, UnloadWatch, nullptr, 0, nullptr);
    return 0;
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_hModule = h;
        DisableThreadLibraryCalls(h);
        CreateThread(nullptr, 0, Main, nullptr, 0, nullptr);
    }
    else if (reason == DLL_PROCESS_DETACH) {
        g_unload = true;
        Teardown();
    }
    return TRUE;
}