// ============================================================================
//  anticheat.cpp — bypass em camadas (v6)
//
//  >>> FIX v6 — o log de runtime provou tres defeitos encadeados:
//
//   (1) CLASSE ERRADA. Install() escolhia por PONTUACAO de nomes de campo. O
//       jogo ofusca nomes (EFIPOFGCOKN, GDMAGEDMDBO...), entao PlayerAntiHack
//       pontuou 1 e a classe da livraria de terceiro (TimeCheatingDetector)
//       pontuou 7 e ganhou. Agora a escolha e' em TIERS: as classes DO JOGO tem
//       prioridade absoluta e a pontuacao so' desempata DENTRO do tier.
//
//   (2) INSTANCIA NUNCA ACHADA. O singleton mora num estatico com nome
//       OFUSCADO (<GDMAGEDMDBO>k__BackingField) e a busca por NOME
//       ({"instance","Instance","m_Instance",...}) nunca casava. Agora existe
//       busca por TIPO (varre os estaticos de g_cls e dos pais aceitando campo
//       estatico cujo tipo seja g_cls/derivado) + fallback FindObjectsOfType.
//       E o "desisti de procurar" permanente virou retry lento (8s).
//
//   (3) ESTRAGO. Com g_obj==NULL o laco de estaticos nao limpava nada e sobrava
//       o writeback em g_localPlayer (pb + off). Como os offsets eram de outra
//       classe, o codigo escrevia PlayerScript+0x38=false e PlayerScript+0x5C=0
//       TODO FRAME. O writeback agora e' explicito (kPlayerWriteback) e
//       desligado por padrao, com o que foi limpo logado 1x.
//
//  Mantido de v5: throttle 1x/frame no hk_acUpdate, template dos 16 getters,
//  IsStaticField/IsStaticMethod antes de static_get/runtime_invoke,
//  ScanFieldsFallback, __try em tudo, ScoreClass.
// ============================================================================
#include "features.h"
#include "sdk.h"
#include "hooks.h"
#include "common.h"

#include "MinHook.h"

#include <windows.h>
#include <atomic>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <vector>

void Log(const char* fmt, ...);

extern "C" int bfi_anticheat_obj = 0xACE;

namespace {

    typedef uint32_t(*t_field_get_flags)(FieldInfo*);
    typedef void        (*t_field_static_set_value)(FieldInfo*, void*);
    typedef void        (*t_field_static_get_value)(FieldInfo*, void*);
    typedef const char* (*t_class_get_name)(Il2CppClass*);
    typedef uint32_t(*t_method_get_flags)(const MethodInfo*, uint32_t*);

    static t_field_get_flags         g_fieldGetFlags = nullptr;
    static t_field_static_set_value  g_fieldStaticSet = nullptr;
    static t_field_static_get_value  g_fieldStaticGet = nullptr;
    static t_class_get_name          g_classGetName = nullptr;
    static t_method_get_flags        g_methodGetFlags = nullptr;

    static void ResolveAcApi() {
        static bool done = false;
        if (done) return;
        done = true;

        HMODULE h = GetModuleHandleA("GameAssembly.dll");
        if (!h) { Log("[ac] GameAssembly.dll nao carregada"); return; }

        g_fieldGetFlags = (t_field_get_flags)GetProcAddress(h, "il2cpp_field_get_flags");
        g_fieldStaticSet = (t_field_static_set_value)GetProcAddress(h, "il2cpp_field_static_set_value");
        g_fieldStaticGet = (t_field_static_get_value)GetProcAddress(h, "il2cpp_field_static_get_value");
        g_classGetName = (t_class_get_name)GetProcAddress(h, "il2cpp_class_get_name");
        g_methodGetFlags = (t_method_get_flags)GetProcAddress(h, "il2cpp_method_get_flags");

        Log("[ac] api extra: get_flags=%d static_set=%d static_get=%d class_name=%d method_flags=%d",
            (int)(g_fieldGetFlags != nullptr), (int)(g_fieldStaticSet != nullptr),
            (int)(g_fieldStaticGet != nullptr), (int)(g_classGetName != nullptr),
            (int)(g_methodGetFlags != nullptr));
    }

    static bool IsStaticField(FieldInfo* f) {
        return (g_fieldGetFlags && f) ? ((g_fieldGetFlags(f) & 0x10u) != 0u) : false;
    }
    static bool IsStaticMethod(const MethodInfo* m) {
        return (g_methodGetFlags && m) ? ((g_methodGetFlags(m, nullptr) & 0x10u) != 0u) : false;
    }
    static const char* ClassName(Il2CppClass* k) {
        return (g_classGetName && k) ? g_classGetName(k) : "?";
    }
    static void StaticSet(FieldInfo* f, void* v) {
        if (g_fieldStaticSet && f) g_fieldStaticSet(f, v);
    }

    static bool AcPtr(const void* p) {
        uintptr_t a = (uintptr_t)p;
        if (a < 0x10000 || a > 0x00007FFFFFFFFFFFULL) return false;
        if (a & 7) return false;
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(p, &mbi, sizeof(mbi))) return false;
        if (mbi.State != MEM_COMMIT) return false;
        if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
        return true;
    }

    static bool AcObjOk(Il2CppObject* o, Il2CppClass* want) {
        if (!AcPtr(o)) return false;
        __try {
            Il2CppClass* k = il2cpp_object_get_class(o);
            if (!AcPtr(k)) return false;
            if (!want) return true;
            for (Il2CppClass* c = k; c && AcPtr(c); c = il2cpp_class_get_parent(c))
                if (c == want) return true;
            return false;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
    }

    static bool NameIsUnityBase(const char* cn) {
        if (!cn) return false;
        return !strcmp(cn, "MonoBehaviour") || !strcmp(cn, "Behaviour") ||
            !strcmp(cn, "Component") || !strcmp(cn, "Object") ||
            !strcmp(cn, "ScriptableObject") || !strcmp(cn, "UnityEngine.Object");
    }

} // namespace

namespace AntiCheat {

    // ---------------------------------------------------------------- politica
    // (3) do topo: ligue aqui se quiser que os offsets dos campos do anti-cheat
    //     sejam escritos TAMBEM em g_localPlayer. Com o log provou-se que, com a
    //     classe errada, isso escrevia offsets alheios no PlayerScript todo
    //     frame. Padrao: OFF (limpa so' o objeto do anti-cheat).
    static const bool kPlayerWriteback = false;

    // Classes do JOGO. Tier com prioridade absoluta sobre a livraria CodeStage.
    static const char* kGameNames[] = {
        "PlayerAntiHack", "PlayerAntiCheat", "AntiCheatSystem",
        "StoreAntiHack", "AntiHack", "WallHackAntiCheatMap", "WallHackSector"
    };
    // Classes de terceiro / genericas. So' entram se o tier do jogo estiver vazio.
    static const char* kLibNames[] = {
        "CodeStage.AntiCheat.Detectors.SpeedHackDetector",
        "CodeStage.AntiCheat.Detectors.WallHackDetector",
        "CodeStage.AntiCheat.Detectors.ObscuredCheatingDetector",
        "CodeStage.AntiCheat.Detectors.InjectionDetector",
        "CodeStage.AntiCheat.Detectors.TimeCheatingDetector",
        "AntiCheat", "CheatDetector", "HackDetector"
    };

    // ---------------------------------------------------------------- estado
    static Il2CppClass* g_cls = nullptr;
    static Il2CppObject* g_obj = nullptr;

    static Il2CppClass* g_clsAlt[8] = {};
    static int          g_clsAltN = 0;

    struct FieldRef { uint32_t off; char name[48]; };
    static std::vector<FieldRef> g_bools;
    static std::vector<FieldRef> g_ints;
    static std::vector<FieldRef> g_vecs;

    struct StaticRef { FieldInfo* f; char name[48]; bool isBool; };
    static std::vector<StaticRef> g_statics;

    static void* o_acUpdate = nullptr;
    static uint64_t g_lastTry = 0;
    static int      g_findFails = 0;
    static bool     g_triedCheap = false;
    static uint32_t g_offAcOnPlayer = 0;
    static bool     g_adopted = false;
    static uint32_t g_ticks = 0;
    static uint32_t g_cleared = 0;
    static int      g_findAttempts = 0;

    // ---------------------------------------------------------------- helpers
    static void Lower(const char* s, char* out, size_t n) {
        size_t i = 0;
        for (; s && s[i] && i + 1 < n; ++i) {
            char c = s[i];
            out[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
        }
        out[i] = 0;
    }
    static bool NameHas(const char* low, std::initializer_list<const char*> keys) {
        for (const char* k : keys) if (strstr(low, k)) return true;
        return false;
    }

    static int TypeKind(FieldInfo* f) {
        if (!f) return 0;
        const Il2CppType* t = il2cpp_field_get_type(f);
        const char* tn = (t && il2cpp_type_get_name) ? il2cpp_type_get_name(t) : nullptr;
        if (!tn) return 0;
        if (strstr(tn, "Boolean")) return 1;
        if (strstr(tn, "Int32"))   return 2;
        if (strstr(tn, "Single"))  return 3;
        if (strstr(tn, "Vector3")) return 4;
        return 0;
    }

    static bool NameKeyAny(const char* low) {
        return NameHas(low, { "block", "speed", "fly", "noclip", "cheat", "hack", "detect",
                              "violation", "warn", "kick", "ban", "suspect", "teleport",
                              "desync", "lag", "afk", "idle", "freeze", "strike", "count",
                              "flag", "stage", "level", "time", "delta", "pos", "range" });
    }

    static int ScoreClass(Il2CppClass* k) {
        if (!k) return 0;
        int n = 0;
        for (Il2CppClass* c = k; c; c = il2cpp_class_get_parent(c)) {
            void* it = nullptr;
            for (;;) {
                FieldInfo* f = il2cpp_class_get_fields(c, &it);
                if (!f) break;
                const char* fn = il2cpp_field_get_name(f);
                if (!fn) continue;
                char low[96]; Lower(fn, low, sizeof(low));
                if (NameKeyAny(low)) ++n;
            }
        }
        return n;
    }

    static bool IsGameClassName(const char* n) {
        if (!n) return false;
        for (const char* s : kGameNames) if (strcmp(n, s) == 0) return true;
        return false;
    }

    // ---------------------------------------------------------------- getters
    static void ClearScans() {
        g_bools.clear();
        g_ints.clear();
        g_vecs.clear();
        g_statics.clear();
    }

    static void ScanFieldsFallback(Il2CppClass* k, int& nb, int& ni) {
        if (nb > 0 || ni > 0) return;
        for (Il2CppClass* c = k; c; c = il2cpp_class_get_parent(c)) {
            void* it = nullptr;
            for (;;) {
                FieldInfo* f = il2cpp_class_get_fields(c, &it);
                if (!f) break;
                if (IsStaticField(f)) continue;
                uint32_t off = (uint32_t)il2cpp_field_get_offset(f);
                if (!off) continue;
                int kind = TypeKind(f);
                if (kind != 1 && kind != 2) continue;
                FieldRef r{}; r.off = off;
                strncpy_s(r.name, il2cpp_field_get_name(f), _TRUNCATE);
                if (kind == 1) { g_bools.push_back(r); ++nb; }
                else { g_ints.push_back(r);  ++ni; }
            }
        }
        if (nb || ni) Log("[ac] FALLBACK (sem match de nome) -> bools=%d ints=%d", nb, ni);
    }

    static void ScanFields(Il2CppClass* k, const char* tag) {
        if (!k) return;
        int nb = 0, ni = 0, nv = 0;
        for (Il2CppClass* c = k; c; c = il2cpp_class_get_parent(c)) {
            void* it = nullptr;
            for (;;) {
                FieldInfo* f = il2cpp_class_get_fields(c, &it);
                if (!f) break;
                if (IsStaticField(f)) continue;
                uint32_t off = (uint32_t)il2cpp_field_get_offset(f);
                if (!off) continue;
                const char* fn = il2cpp_field_get_name(f);
                if (!fn) continue;

                char low[96]; Lower(fn, low, sizeof(low));
                int kind = TypeKind(f);

                if (kind == 1) {
                    if (NameHas(low, { "block", "speed", "fly", "noclip", "cheat",
                                       "hack", "detect", "violation", "warn",
                                       "kick", "ban", "suspect", "teleport",
                                       "desync", "lag", "afk", "idle", "freeze" })) {
                        FieldRef r{}; r.off = off;
                        strncpy_s(r.name, fn, _TRUNCATE);
                        g_bools.push_back(r); ++nb;
                    }
                }
                else if (kind == 2) {
                    if (NameHas(low, { "violation", "warn", "strike", "count", "cheat",
                                       "hack", "detect", "kick", "stage", "level",
                                       "block", "speed", "fly", "noclip" })) {
                        FieldRef r{}; r.off = off;
                        strncpy_s(r.name, fn, _TRUNCATE);
                        g_ints.push_back(r); ++ni;
                    }
                }
                else if (kind == 4) {
                    if (NameHas(low, { "last", "prev", "old", "start", "check", "base" })) {
                        FieldRef r{}; r.off = off;
                        strncpy_s(r.name, fn, _TRUNCATE);
                        g_vecs.push_back(r); ++nv;
                    }
                }
            }
        }
        ScanFieldsFallback(k, nb, ni);
        Log("[ac] %s: bools=%d ints=%d vecs=%d", tag, nb, ni, nv);
        // >>> v6: se nao casou NADA por nome, limpar o que casou e' limpar nada.
        //     Avisa o operador para ele saber que o bypass esta' inerte.
        if (nb == 0 && ni == 0 && nv == 0)
            Log("[ac] ATENCAO: nenhum campo casou por nome nesta classe — bypass inerte");
    }

    static void ScanStatics(Il2CppClass* k) {
        if (!k) return;
        int n = 0;
        for (Il2CppClass* c = k; c; c = il2cpp_class_get_parent(c)) {
            void* it = nullptr;
            for (;;) {
                FieldInfo* f = il2cpp_class_get_fields(c, &it);
                if (!f) break;
                if (!IsStaticField(f)) continue;
                int kind = TypeKind(f);
                if (kind != 1 && kind != 2) continue;
                const char* fn = il2cpp_field_get_name(f);
                if (!fn) continue;
                char low[96]; Lower(fn, low, sizeof(low));
                if (!NameHas(low, { "block", "speed", "fly", "noclip", "cheat", "hack",
                                    "detect", "violation", "warn", "kick", "ban", "suspect" }))
                    continue;
                StaticRef r{}; r.f = f; r.isBool = (kind == 1);
                strncpy_s(r.name, fn, _TRUNCATE);
                g_statics.push_back(r); ++n;
            }
        }
        Log("[ac] statics de flag em %s: %d", ClassName(k), n);
    }

    static void DumpClass(Il2CppClass* k) {
        if (!k) return;
        Log("[acdump] ==== classe %s ====", ClassName(k));
        for (Il2CppClass* c = k; c; c = il2cpp_class_get_parent(c)) {
            Log("[acdump] -- %s --", ClassName(c));
            void* it = nullptr;
            for (;;) {
                FieldInfo* f = il2cpp_class_get_fields(c, &it);
                if (!f) break;
                const Il2CppType* t = il2cpp_field_get_type(f);
                const char* tn = (t && il2cpp_type_get_name) ? il2cpp_type_get_name(t) : "?";
                Log("[acdump]   %-34s %-42s @0x%X%s",
                    il2cpp_field_get_name(f), tn ? tn : "?",
                    (unsigned)il2cpp_field_get_offset(f),
                    IsStaticField(f) ? " [static]" : "");
            }
        }
        Log("[acdump] ==== fim ====");
    }

    // ---------------------------------------------------------------- getters hook
    typedef bool(__fastcall* tGetterHook)(void*, void*);
    static void* g_tramp[16] = { nullptr };
    static int   g_slotUsed = 0;

    template<int N>
    static bool __fastcall hk_slot(void* self, void* method) {
        __try {
            if (g_tramp[N]) ((bool(__fastcall*)(void*, void*))g_tramp[N])(self, method);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        return false;
    }

    static const tGetterHook g_slotFn[16] = {
        &hk_slot<0>,  &hk_slot<1>,  &hk_slot<2>,  &hk_slot<3>,
        &hk_slot<4>,  &hk_slot<5>,  &hk_slot<6>,  &hk_slot<7>,
        &hk_slot<8>,  &hk_slot<9>,  &hk_slot<10>, &hk_slot<11>,
        &hk_slot<12>, &hk_slot<13>, &hk_slot<14>, &hk_slot<15>
    };

    static int HookGettersPass(Il2CppClass* k, bool filtrarNome) {
        int n = 0;
        for (Il2CppClass* c = k; c && g_slotUsed < 16; c = il2cpp_class_get_parent(c)) {
            const char* ccn = ClassName(c);
            if (!filtrarNome && NameIsUnityBase(ccn)) continue;

            void* it = nullptr;
            for (;;) {
                const MethodInfo* m = il2cpp_class_get_methods(c, &it);
                if (!m || g_slotUsed >= 16) break;
                if (il2cpp_method_get_param_count(m) != 0) continue;

                const char* rt = SDK::MethodReturnName((MethodInfo*)m);
                if (!rt || !strstr(rt, "Boolean")) continue;

                const char* mn = il2cpp_method_get_name(m);
                if (filtrarNome) {
                    if (!mn) continue;
                    char low[96]; Lower(mn, low, sizeof(low));
                    if (!NameHas(low, { "speed", "fly", "noclip", "no_clip", "cheat", "hack",
                                        "block", "detect", "teleport", "suspect", "violation",
                                        "kick", "ban", "desync", "check", "time", "valid",
                                        "enable", "active" }))
                        continue;
                }

                void* p = SDK::MethodPtr((MethodInfo*)m);
                if (!AcPtr(p)) continue;

                bool dup = false;
                for (int j = 0; j < g_slotUsed; ++j) if (g_tramp[j] == p) dup = true;
                if (dup) continue;

                if (MH_CreateHook(p, (void*)g_slotFn[g_slotUsed], &g_tramp[g_slotUsed]) == MH_OK) {
                    MH_EnableHook(p);
                    Log("[ac] getter neutralizado: %s::%s (%p)", ccn, mn ? mn : "?", p);
                    ++g_slotUsed; ++n;
                }
            }
        }
        return n;
    }

    static int HookFlagGetters(Il2CppClass* k) {
        int n = HookGettersPass(k, true);
        if (n == 0) {
            Log("[ac] nenhum getter casou por nome -> forcando TODOS os bool sem arg");
            n = HookGettersPass(k, false);
        }
        Log("[ac] getters forcados p/ false: %d", n);
        return n;
    }

    // ---------------------------------------------------------------- instancia
    static bool ClassIsAc(Il2CppClass* k) {
        if (!k) return false;
        if (k == g_cls) return true;
        const char* n = ClassName(k);
        if (!n) return false;
        return strstr(n, "AntiHack") || strstr(n, "AntiCheat");
    }

    // >>> v6: busca por TIPO. Acha singleton com nome de campo ofuscado
    //     (ex.: <GDMAGEDMDBO>k__BackingField), que a busca por nome perdia.
    static Il2CppObject* StaticInstanceOfClass() {
        if (!g_cls) return nullptr;
        Il2CppObject* found = nullptr;
        __try {
            for (Il2CppClass* c = g_cls; c && !found; c = il2cpp_class_get_parent(c)) {
                void* it = nullptr;
                for (;;) {
                    FieldInfo* f = il2cpp_class_get_fields(c, &it);
                    if (!f) break;
                    if (!IsStaticField(f)) continue;
                    const Il2CppType* t = il2cpp_field_get_type(f);
                    if (!t || !AcPtr(t)) continue;
                    Il2CppClass* ft = nullptr;
                    __try { ft = il2cpp_class_from_type(t); }
                    __except (EXCEPTION_EXECUTE_HANDLER) { ft = nullptr; }
                    if (!AcPtr(ft)) continue;
                    bool match = false;
                    for (Il2CppClass* q = ft; !match && q && AcPtr(q); q = il2cpp_class_get_parent(q))
                        if (ClassIsAc(q)) match = true;
                    if (!match) continue;
                    Il2CppObject* o = nullptr;
                    __try { if (g_fieldStaticGet) g_fieldStaticGet(f, &o); }
                    __except (EXCEPTION_EXECUTE_HANDLER) { o = nullptr; }
                    if (AcObjOk(o, g_cls)) {
                        Log("[ac] instancia via static TIPADO '%s' (%p)",
                            il2cpp_field_get_name(f), o);
                        found = o;
                        break;
                    }
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        return found;
    }

    static Il2CppObject* InstanceOnPlayerFast() {
        if (!SDK::g_localPlayer || !g_cls || !AcPtr(SDK::g_localPlayer)) return nullptr;

        if (g_offAcOnPlayer) {
            Il2CppObject* o = SafeRead<Il2CppObject*>(
                (uintptr_t)SDK::g_localPlayer + g_offAcOnPlayer, nullptr);
            if (AcObjOk(o, g_cls)) return o;
            g_offAcOnPlayer = 0;
        }

        uint64_t t0 = GetTickCount64();
        uint32_t foundOff = 0;
        Il2CppObject* foundObj = nullptr;
        Il2CppClass* foundCls = nullptr;

        __try {
            Il2CppClass* pk = il2cpp_object_get_class(SDK::g_localPlayer);
            if (!AcPtr(pk)) return nullptr;

            for (Il2CppClass* c = pk; c && AcPtr(c) && !foundObj; c = il2cpp_class_get_parent(c)) {
                void* it = nullptr;
                for (;;) {
                    FieldInfo* f = il2cpp_class_get_fields(c, &it);
                    if (!f) break;
                    const Il2CppType* t = il2cpp_field_get_type(f);
                    if (!t || !AcPtr(t)) continue;

                    Il2CppClass* ft = nullptr;
                    __try { ft = il2cpp_class_from_type(t); }
                    __except (EXCEPTION_EXECUTE_HANDLER) { ft = nullptr; }
                    if (!AcPtr(ft)) continue;

                    bool match = false;
                    for (Il2CppClass* q = ft; !match && q && AcPtr(q); q = il2cpp_class_get_parent(q))
                        if (ClassIsAc(q)) match = true;
                    if (!match) continue;

                    uint32_t off = (uint32_t)il2cpp_field_get_offset(f);
                    if (!off) continue;

                    Il2CppObject* o = SafeRead<Il2CppObject*>(
                        (uintptr_t)SDK::g_localPlayer + off, nullptr);
                    if (AcObjOk(o, nullptr)) {
                        foundOff = off;
                        foundObj = o;
                        foundCls = ft;
                        Log("[ac] instancia no player: '%s' @0x%X (%p) cls=%s",
                            il2cpp_field_get_name(f), off, o, ClassName(ft));
                        break;
                    }
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}

        uint64_t dt = GetTickCount64() - t0;
        if (dt >= 20) Log("[ac] scan da instancia no player levou %llu ms",
            (unsigned long long)dt);

        if (!foundObj) return nullptr;

        if (foundCls && foundCls != g_cls && !g_adopted) {
            g_adopted = true;
            Log("[ac] adotando classe %s (era %s)", ClassName(foundCls), ClassName(g_cls));
            g_cls = foundCls;
            ClearScans();
            ScanFields(g_cls, "campos (adotada)");
            ScanStatics(g_cls);
            DumpClass(g_cls);
        }

        g_offAcOnPlayer = foundOff;
        return AcObjOk(foundObj, g_cls) ? foundObj : nullptr;
    }

    static Il2CppObject* InstanceViaGetter() {
        MethodInfo* g = nullptr;
        for (Il2CppClass* c = g_cls; c && !g; c = il2cpp_class_get_parent(c)) {
            void* it = nullptr;
            for (;;) {
                const MethodInfo* m = il2cpp_class_get_methods(c, &it);
                if (!m) break;
                if (il2cpp_method_get_param_count(m) != 0) continue;
                if (!IsStaticMethod(m)) continue;
                const char* mn = il2cpp_method_get_name(m);
                if (!mn) continue;
                if (strcmp(mn, "get_Instance") == 0 || strcmp(mn, "get_instance") == 0 ||
                    strcmp(mn, "get_Current") == 0 || strcmp(mn, "get_Singleton") == 0) {
                    g = (MethodInfo*)m; break;
                }
            }
        }
        if (!g || !il2cpp_runtime_invoke) return nullptr;

        Il2CppObject* o = nullptr;
        __try {
            Il2CppException* ex = nullptr;
            o = il2cpp_runtime_invoke(g, nullptr, nullptr, &ex);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { o = nullptr; }

        if (AcObjOk(o, g_cls)) {
            Log("[ac] instancia via getter %s (%p)", il2cpp_method_get_name(g), o);
            return o;
        }
        return nullptr;
    }

    // >>> v6: ultimo recurso. Custa mais, entao so' nas primeiras tentativas.
    static Il2CppObject* InstanceViaFindObjects() {
        if (!g_cls) return nullptr;
        Il2CppClass* oc = nullptr;
        for (Il2CppClass* c = g_cls; c && AcPtr(c); c = il2cpp_class_get_parent(c)) {
            const char* n = ClassName(c);
            if (n && strcmp(n, "Object") == 0) { oc = c; break; }
        }
        if (!oc) return nullptr;
        MethodInfo* fot = nullptr;
        __try { fot = il2cpp_class_get_method_from_name(oc, "FindObjectsOfType", 1); }
        __except (EXCEPTION_EXECUTE_HANDLER) { fot = nullptr; }
        if (!fot) return nullptr;
        Il2CppType* t = nullptr;
        __try { t = il2cpp_class_get_type(g_cls); }
        __except (EXCEPTION_EXECUTE_HANDLER) { t = nullptr; }
        if (!t) return nullptr;
        Il2CppObject* to = nullptr;
        __try { to = il2cpp_type_get_object(t); }
        __except (EXCEPTION_EXECUTE_HANDLER) { to = nullptr; }
        if (!AcPtr(to)) return nullptr;
        void* args[1] = { to };
        Il2CppObject* arr = nullptr;
        __try { arr = il2cpp_runtime_invoke(fot, nullptr, args, nullptr); }
        __except (EXCEPTION_EXECUTE_HANDLER) { arr = nullptr; }
        if (!AcPtr(arr)) return nullptr;
        const uint32_t n = (uint32_t)SafeRead<uintptr_t>((uintptr_t)arr + 0x18, 0);
        for (uint32_t i = 0; i < n && i < 8; ++i) {
            Il2CppObject* o = SafeRead<Il2CppObject*>((uintptr_t)arr + 0x20 + (size_t)i * 8, nullptr);
            if (AcObjOk(o, g_cls)) {
                Log("[ac] instancia via FindObjectsOfType (%p)", o);
                return o;
            }
        }
        return nullptr;
    }

    static void FindInstance() {
        uint64_t now = GetTickCount64();
        uint64_t wait = (g_findFails < 3) ? 3000 : 8000;
        if (g_lastTry && now - g_lastTry < wait) return;
        g_lastTry = now;
        ++g_findAttempts;

        if (!g_triedCheap) {
            g_triedCheap = true;

            // 1) nome classico (funciona quando o campo NAO e' ofuscado)
            static const char* cands[] = { "instance", "Instance", "m_Instance", "_instance",
                                           "singleton", "m_Singleton", "current", "m_Current",
                                           "s_Instance", "g_Instance" };
            for (const char* n : cands) {
                FieldInfo* f = nullptr;
                __try { f = il2cpp_class_get_field_from_name(g_cls, n); }
                __except (EXCEPTION_EXECUTE_HANDLER) { f = nullptr; }
                if (!f) continue;
                if (!IsStaticField(f)) continue;

                Il2CppObject* o = nullptr;
                __try { if (g_fieldStaticGet) g_fieldStaticGet(f, &o); }
                __except (EXCEPTION_EXECUTE_HANDLER) { o = nullptr; }

                if (AcObjOk(o, g_cls)) {
                    g_obj = o;
                    Log("[ac] instancia via estatico '%s' (%p)", n, o);
                    return;
                }
            }
        }

        // 2) TIPO — acha nome de campo ofuscado. Roda em toda tentativa.
        g_obj = StaticInstanceOfClass();
        if (g_obj) { g_findFails = 0; return; }

        // 3) getter get_Instance/get_Current/...
        if (g_findAttempts <= 3) {
            g_obj = InstanceViaGetter();
            if (g_obj) { g_findFails = 0; return; }
        }

        // 4) campo no player
        g_obj = InstanceOnPlayerFast();
        if (g_obj) { g_findFails = 0; return; }

        // 5) FindObjectsOfType
        if (g_findAttempts <= 3) {
            g_obj = InstanceViaFindObjects();
            if (g_obj) { g_findFails = 0; return; }
        }

        ++g_findFails;
        // >>> v6: NAO desiste de vez. A instancia aparece e some com a partida.
        if (g_findFails == 6)
            Log("[ac] instancia: 6 tentativas sem achar — segue tentando a cada 8s");
    }

    // ---------------------------------------------------------------- limpeza
    static void ClearAll() {
        if (g_obj && AcObjOk(g_obj, g_cls)) {
            uintptr_t base = (uintptr_t)g_obj;
            for (auto& b : g_bools) WriteAt<bool>(base + b.off, false);
            for (auto& i : g_ints)  WriteAt<int>(base + i.off, 0);

            if (SDK::g_localPlayer) {
                Vec3 p = SDK::GetPos(SDK::g_localPlayer);
                for (auto& v : g_vecs) WriteAt<Vec3>(base + v.off, p);
            }
        }

        for (auto& s : g_statics) {
            __try {
                if (s.isBool) { bool b = false; StaticSet(s.f, &b); }
                else { int32_t v = 0; StaticSet(s.f, &v); }
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }

        // >>> v6: writeback explicito (kPlayerWriteback). Era o que escrevia
        //     PlayerScript+0x38 / +0x5C quando a classe escolhida era de outro
        //     tipo e o objeto nunca era achado.
        if (kPlayerWriteback && SDK::g_localPlayer && !g_bools.empty()) {
            uintptr_t pb = (uintptr_t)SDK::g_localPlayer;
            for (auto& b : g_bools) {
                __try { WriteAt<bool>(pb + b.off, false); }
                __except (EXCEPTION_EXECUTE_HANDLER) {}
            }
        }

        if (++g_cleared % 300 == 0)
            Log("[ac] limpo (obj=%p bools=%d ints=%d vecs=%d statics=%d wb=%d)",
                g_obj, (int)g_bools.size(), (int)g_ints.size(),
                (int)g_vecs.size(), (int)g_statics.size(), (int)kPlayerWriteback);
    }

    typedef void(__fastcall* tAcUpd)(void* self, void* method);
    static void __fastcall hk_acUpdate(void* self, void* method) {
        static uint64_t lastFrame = (uint64_t)-1;
        const uint64_t f = SDK::g_frameId;
        const bool doIt = (f != lastFrame);
        lastFrame = f;

        if (doIt) { __try { ClearAll(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
        if (o_acUpdate) ((tAcUpd)o_acUpdate)(self, method);
        if (doIt) { __try { ClearAll(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
    }

    static void HookAcUpdates() {
        static const char* ms[] = { "Update", "FixedUpdate", "LateUpdate" };
        for (const char* name : ms) {
            MethodInfo* m = nullptr;
            for (Il2CppClass* c = g_cls; c && !m; c = il2cpp_class_get_parent(c)) {
                void* it = nullptr;
                for (;;) {
                    const MethodInfo* f = il2cpp_class_get_methods(c, &it);
                    if (!f) break;
                    if (il2cpp_method_get_param_count(f) == 0 &&
                        strcmp(il2cpp_method_get_name(f), name) == 0) {
                        m = (MethodInfo*)f; break;
                    }
                }
            }
            if (!m) continue;
            void* p = SDK::MethodPtr(m);
            if (!AcPtr(p)) continue;
            if (MH_CreateHook(p, &hk_acUpdate, (void**)&o_acUpdate) == MH_OK) {
                MH_EnableHook(p);
                Log("[ac] hook %s OK (%p) — limpa antes/depois (1x/frame)", name, p);
                return;
            }
        }
        Log("[ac] nenhum Update do anti-cheat encontrado (fica so o Tick)");
    }

    // ---------------------------------------------------------------- selecao
    static Il2CppClass* PickClass() {
        Il2CppClass* bestGame = nullptr; int bestGameScore = -1;
        Il2CppClass* bestLib = nullptr;  int bestLibScore = -1;
        g_clsAltN = 0;

        for (const char* n : kGameNames) {
            Il2CppClass* k = nullptr;
            __try { k = SDK::FindClass(n); }
            __except (EXCEPTION_EXECUTE_HANDLER) { k = nullptr; }
            if (!k) continue;
            const char* kn = ClassName(k);
            if (!kn || strcmp(kn, n) != 0) continue;   // exige nome EXATO
            const int s = ScoreClass(k);
            Log("[ac] candidata JOGO  %s -> campos de flag=%d", n, s);
            if (g_clsAltN < 8 && !bestGame) g_clsAlt[g_clsAltN++] = k;
            if (!bestGame || s > bestGameScore) { bestGame = k; bestGameScore = s; }
        }

        for (const char* n : kLibNames) {
            Il2CppClass* k = nullptr;
            __try { k = SDK::FindClass(n); }
            __except (EXCEPTION_EXECUTE_HANDLER) { k = nullptr; }
            if (!k) continue;
            const int s = ScoreClass(k);
            Log("[ac] candidata LIVRARIA %s -> campos de flag=%d", n, s);
            if (!bestLib || s > bestLibScore) { bestLib = k; bestLibScore = s; }
        }

        // SDK::kAntiHack tem prioridade absoluta SE for uma classe do jogo.
        if (SDK::kAntiHack) {
            const char* kn = ClassName(SDK::kAntiHack);
            if (kn && IsGameClassName(kn)) {
                Log("[ac] SDK::kAntiHack = %s (jogo) -> prioridade", kn);
                return SDK::kAntiHack;
            }
        }
        if (bestGame) {
            Log("[ac] tier JOGO venceu (melhor=%s score=%d)", ClassName(bestGame), bestGameScore);
            return bestGame;
        }
        if (bestLib) {
            Log("[ac] ATENCAO: nenhuma classe do jogo — caindo na livraria (%s)",
                ClassName(bestLib));
            return bestLib;
        }
        return SDK::kAntiHack;
    }

    // ---------------------------------------------------------------- install
    bool Install() {
        ResolveAcApi();
        PHASE("ac.install");

        g_cls = PickClass();
        if (!g_cls) { Log("[ac] classe do anti-cheat NAO encontrada"); return false; }
        Log("[ac] classe escolhida = %s", ClassName(g_cls));

        ClearScans();
        ScanFields(g_cls, "campos");
        ScanStatics(g_cls);
        // >>> v6: estaticos tambem das outras classes do jogo (custo baixo, e o
        //     jogo costuma espalhar as flags entre PlayerAntiHack/StoreAntiHack).
        for (int i = 0; i < g_clsAltN; ++i) {
            if (g_clsAlt[i] == g_cls) continue;
            ScanStatics(g_clsAlt[i]);
        }
        DumpClass(g_cls);
        HookFlagGetters(g_cls);
        HookAcUpdates();
        FindInstance();

        Log("[ac] Install ok cls=%p obj=%p bools=%d ints=%d vecs=%d statics=%d wb=%d",
            g_cls, g_obj, (int)g_bools.size(), (int)g_ints.size(),
            (int)g_vecs.size(), (int)g_statics.size(), (int)kPlayerWriteback);
        // Log 1x dos offsets que serao zerados — para conferir contra o acdump.
        for (auto& b : g_bools) Log("[ac]   zerando bool @0x%X (%s)", b.off, b.name);
        for (auto& i : g_ints)  Log("[ac]   zerando int  @0x%X (%s)", i.off, i.name);
        return true;
    }

    void Tick() {
        PHASE("ac.tick");
        if (!C.antiCheat) return;
        if (!g_cls) return;

        __try {
            if (g_obj && !AcObjOk(g_obj, g_cls)) {
                Log("[ac] instancia invalida, refazendo");
                g_obj = nullptr;
            }
            if (!g_obj) FindInstance();

            if (g_obj || !g_statics.empty()) ClearAll();
            ++g_ticks;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            static int n = 0;
            if (n++ < 10) Log("[ac] AV contido no Tick");
        }
    }

} // namespace AntiCheat
// ==== FIM anticheat.cpp ====