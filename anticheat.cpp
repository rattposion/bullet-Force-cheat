// ============================================================================
//  anticheat.cpp — bypass em camadas (v9)
//  v9: + NAO adota classe sem campos de flag (PlayerAntiHack e' componente vazio:
//        adotar ela zerava g_bools/g_ints e o bypass virava no-op)
//      + g_instCls: classe REAL do objeto da instancia, separada da classe cujos
//        campos limpamos. Sem isso o cache de offset se invalidava a cada Tick.
//      + caches de type->class e class->isAC + WARM no Install: o 1o scan travava
//        o thread do jogo por ~1.6s ("[ac] scan ... levou 1687 ms").
//        O warm roda no thread de init, fora do frame.
//      + __try unico no laco de campos (era 1 por campo)
// ============================================================================
#include "features.h"
#include "sdk.h"
#include "hooks.h"
#include "common.h"
#include "memory.h"

#include "MinHook.h"

#include <windows.h>
#include <atomic>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <vector>

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

    // ---------------------------------------------------------------- estado
    static Il2CppClass* g_cls = nullptr;       // classe cujos campos limpamos
    static Il2CppObject* g_obj = nullptr;      // instancia viva
    static Il2CppClass* g_instCls = nullptr;   // classe REAL do objeto da instancia

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
    static bool     g_findGiveUp = false;
    static uint32_t g_offAcOnPlayer = 0;
    static bool     g_adopted = false;
    static uint32_t g_ticks = 0;
    static uint32_t g_cleared = 0;

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
        Log("[ac] statics de flag: %d", n);
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

    // ---------------------------------------------------------------- getters
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
                                        "kick", "ban", "desync", "check" }))
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

    // classe para VALIDAR a instancia (a real do objeto), nao a dos campos
    static Il2CppClass* InstCls() { return g_instCls ? g_instCls : g_cls; }

    // >>> caches: matam o custo repetido do scan
    static Il2CppClass* ClassFromTypeCached(const Il2CppType* t) {
        if (!t) return nullptr;
        static const Il2CppType* kt[512] = {};
        static Il2CppClass* kv[512] = {};
        static int n = 0;
        for (int i = 0; i < n; ++i) if (kt[i] == t) return kv[i];
        Il2CppClass* r = nullptr;
        __try { r = il2cpp_class_from_type(t); }
        __except (EXCEPTION_EXECUTE_HANDLER) { r = nullptr; }
        if (n < 512) { kt[n] = t; kv[n] = r; ++n; }
        return r;
    }

    static bool ClassIsAcCached(Il2CppClass* k) {
        if (!k) return false;
        static Il2CppClass* kc[256] = {};
        static unsigned char vc[256] = {};
        static int n = 0;
        for (int i = 0; i < n; ++i) if (kc[i] == k) return vc[i] != 0;
        bool v = false;
        for (Il2CppClass* q = k; q && AcPtr(q); q = il2cpp_class_get_parent(q))
            if (ClassIsAc(q)) { v = true; break; }
        if (n < 256) { kc[n] = k; vc[n] = v ? 1 : 0; ++n; }
        return v;
    }

    // ------------------------------------------------------------------------
    //  >>> v9 — aquece type->class de TODOS os campos do player no thread de init.
    //  E' isso que tira o freeze de ~1.6s de dentro do frame.
    // ------------------------------------------------------------------------
    static void WarmPlayerFieldCache() {
        Il2CppClass* pk = SDK::kPlayerScript ? SDK::kPlayerScript : SDK::FindClass("Entity");
        if (!pk) { Log("[ac] warm: classe do player ainda nao resolvida"); return; }

        const uint64_t t0 = GetTickCount64();
        int n = 0;
        __try {
            void* it = nullptr;
            for (;;) {
                FieldInfo* f = il2cpp_class_get_fields(pk, &it);
                if (!f) break;
                if (IsStaticField(f)) continue;
                const Il2CppType* t = il2cpp_field_get_type(f);
                if (!t) continue;
                ClassFromTypeCached(t);
                ++n;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}

        Log("[ac] warm: %d campos de %s pre-resolvidos em %llu ms (fora do frame)",
            n, ClassName(pk), (unsigned long long)(GetTickCount64() - t0));
    }

    static Il2CppObject* InstanceOnPlayerFast() {
        if (!SDK::g_localPlayer || !g_cls || !AcPtr(SDK::g_localPlayer)) return nullptr;

        if (g_offAcOnPlayer && g_instCls) {
            Il2CppObject* o = *(Il2CppObject**)((uintptr_t)SDK::g_localPlayer + g_offAcOnPlayer);
            if (AcObjOk(o, g_instCls)) return o;
            g_offAcOnPlayer = 0;
            g_instCls = nullptr;
        }

        const uint64_t t0 = GetTickCount64();
        uint32_t foundOff = 0;
        Il2CppObject* foundObj = nullptr;
        Il2CppClass* foundCls = nullptr;

        __try {
            Il2CppClass* pk = il2cpp_object_get_class(SDK::g_localPlayer);
            if (!AcPtr(pk)) return nullptr;

            // il2cpp_class_get_fields ja' percorre a hierarquia: NAO repetir o walk
            void* it = nullptr;
            for (;;) {
                FieldInfo* f = il2cpp_class_get_fields(pk, &it);
                if (!f) break;
                if (IsStaticField(f)) continue;

                const Il2CppType* t = il2cpp_field_get_type(f);
                if (!t) continue;

                Il2CppClass* ft = ClassFromTypeCached(t);
                if (!ft) continue;
                if (!ClassIsAcCached(ft)) continue;

                const uint32_t off = (uint32_t)il2cpp_field_get_offset(f);
                if (!off) continue;

                Il2CppObject* o = *(Il2CppObject**)((uintptr_t)SDK::g_localPlayer + off);
                if (!AcObjOk(o, nullptr)) continue;

                foundOff = off;
                foundObj = o;
                foundCls = ft;
                Log("[ac] instancia no player: '%s' @0x%X (%p) cls=%s",
                    il2cpp_field_get_name(f), off, o, ClassName(ft));
                break;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}

        const uint64_t dt = GetTickCount64() - t0;
        if (dt >= 20) Log("[ac] scan da instancia no player levou %llu ms",
            (unsigned long long)dt);

        if (!foundObj) return nullptr;
        if (foundCls) g_instCls = foundCls;

        // >>> so' troca a lista de campos se a nova classe realmente tiver flags
        if (foundCls && foundCls != g_cls && !g_adopted) {
            g_adopted = true;
            Il2CppClass* old = g_cls;
            Log("[ac] classe do player = %s (escolhida era %s)",
                ClassName(foundCls), ClassName(old));

            ClearScans();
            g_cls = foundCls;
            ScanFields(g_cls, "campos (player)");
            ScanStatics(g_cls);

            if (g_bools.empty() && g_ints.empty() && g_statics.empty() && old) {
                Log("[ac] %s sem flags -> campos continuam de %s",
                    ClassName(g_cls), ClassName(old));
                g_cls = old;
                ClearScans();
                ScanFields(g_cls, "campos");
                ScanStatics(g_cls);
            }
            DumpClass(g_cls);
        }

        g_offAcOnPlayer = foundOff;
        return AcObjOk(foundObj, InstCls()) ? foundObj : nullptr;
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

        if (AcObjOk(o, InstCls())) {
            g_instCls = g_cls;
            Log("[ac] instancia via getter %s (%p)", il2cpp_method_get_name(g), o);
            return o;
        }
        return nullptr;
    }

    static void FindInstance() {
        if (g_findGiveUp) return;

        uint64_t now = GetTickCount64();
        uint64_t wait = (g_findFails < 3) ? 3000 : 8000;
        if (g_lastTry && now - g_lastTry < wait) return;
        g_lastTry = now;

        if (!g_triedCheap) {
            g_triedCheap = true;

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

                if (AcObjOk(o, InstCls())) {
                    g_obj = o;
                    g_instCls = g_cls;
                    Log("[ac] instancia via estatico '%s' (%p)", n, o);
                    return;
                }
            }

            g_obj = InstanceViaGetter();
            if (g_obj) return;
        }

        g_obj = InstanceOnPlayerFast();
        if (g_obj) { g_findFails = 0; return; }

        if (++g_findFails >= 6) {
            g_findGiveUp = true;
            Log("[ac] instancia nao achada em 6 tentativas — parando de procurar");
        }
    }

    // ---------------------------------------------------------------- limpeza
    static void ClearAll() {
        if (g_obj && AcObjOk(g_obj, InstCls())) {
            const uintptr_t base = (uintptr_t)g_obj;

            __try { for (auto& b : g_bools) WriteAt<bool>(base + b.off, false); }
            __except (EXCEPTION_EXECUTE_HANDLER) {}

            __try { for (auto& i : g_ints)  WriteAt<int>(base + i.off, 0); }
            __except (EXCEPTION_EXECUTE_HANDLER) {}

            if (SDK::g_localPlayer) {
                const Vec3 p = SDK::GetPos(SDK::g_localPlayer);
                __try { for (auto& v : g_vecs) WriteAt<Vec3>(base + v.off, p); }
                __except (EXCEPTION_EXECUTE_HANDLER) {}
            }
        }

        __try {
            for (auto& s : g_statics) {
                if (s.isBool) { bool b = false; StaticSet(s.f, &b); }
                else { int32_t v = 0; StaticSet(s.f, &v); }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}

        if (SDK::g_localPlayer && !g_bools.empty()) {
            const uintptr_t pb = (uintptr_t)SDK::g_localPlayer;
            __try { for (auto& b : g_bools) WriteAt<bool>(pb + b.off, false); }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }

        if (++g_cleared % 300 == 0)
            Log("[ac] limpo (obj=%p bools=%d ints=%d vecs=%d statics=%d)",
                g_obj, (int)g_bools.size(), (int)g_ints.size(),
                (int)g_vecs.size(), (int)g_statics.size());
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

    // ---------------------------------------------------------------- install
    bool Install() {
        ResolveAcApi();

        static const char* names[] = {
            "PlayerAntiHack", "AntiHack", "AntiCheatSystem",
            "CodeStage.AntiCheat.Detectors.SpeedHackDetector",
            "CodeStage.AntiCheat.Detectors.WallHackDetector",
            "CodeStage.AntiCheat.Detectors.ObscuredCheatingDetector",
            "CodeStage.AntiCheat.Detectors.InjectionDetector",
            "CodeStage.AntiCheat.Detectors.TimeCheatingDetector",
            "StoreAntiHack", "WallHackAntiCheatMap", "WallHackSector",
            "AntiCheat", "CheatDetector", "HackDetector"
        };

        Il2CppClass* best = nullptr;
        int bestScore = 0;
        for (const char* n : names) {
            Il2CppClass* k = SDK::FindClass(n);
            if (!k) continue;
            int s = ScoreClass(k);
            Log("[ac] candidata %s -> campos de flag=%d", n, s);
            if (!best || s > bestScore) { bestScore = s; best = k; }
        }
        g_cls = best ? best : SDK::kAntiHack;
        if (!g_cls) { Log("[ac] classe do anti-cheat NAO encontrada"); return false; }
        Log("[ac] classe escolhida = %s (campos de flag=%d)", ClassName(g_cls), bestScore);

        ClearScans();
        ScanFields(g_cls, "campos");
        ScanStatics(g_cls);
        DumpClass(g_cls);
        HookFlagGetters(g_cls);
        HookAcUpdates();

        WarmPlayerFieldCache();     // >>> v9: tira o custo do thread do jogo

        FindInstance();

        Log("[ac] Install ok cls=%p obj=%p bools=%d ints=%d statics=%d",
            g_cls, g_obj, (int)g_bools.size(), (int)g_ints.size(), (int)g_statics.size());
        return true;
    }

    void Tick() {
        PHASE("AntiCheat::Tick");
        if (!C.antiCheat) return;
        if (!g_cls) return;

        __try {
            if (g_obj && !AcObjOk(g_obj, InstCls())) {
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

    // ======================================================================
    //  ALERTA DE RISCO DE BAN / DESCONEXAO
    // ======================================================================
    static void AddReason(char* out, int n, const char* what)
    {
        if (!out || n <= 0) return;
        const size_t len = strlen(out);
        if (len + 2 >= (size_t)n) return;
        if (len) strcat_s(out, (size_t)n, ", ");
        strcat_s(out, (size_t)n, what);
    }

    void RiskReasons(char* out, int n)
    {
        if (!out || n <= 0) return;
        out[0] = 0;

        if (!C.antiCheat)   AddReason(out, n, "bypass OFF");
        if (C.speedHack)    AddReason(out, n, "speed hack");
        if (C.fly)          AddReason(out, n, "fly");
        if (C.teleport)     AddReason(out, n, "teleport");
        if (C.godmode)      AddReason(out, n, "god mode");
        if (C.rapidFire)    AddReason(out, n, "rapid fire");
        if (C.infAmmo)      AddReason(out, n, "municao inf");
        if (C.noReload)     AddReason(out, n, "sem reload");
        if (C.silentAim)    AddReason(out, n, "silent aim");
        if (C.magicBullets) AddReason(out, n, "magic bullets");
        if (C.noRecoil)     AddReason(out, n, "sem recoil");
        if (C.noSpread)     AddReason(out, n, "sem spread");
        if (C.bhop)         AddReason(out, n, "bunny hop");
        if (C.shootInAir)   AddReason(out, n, "tiro no ar");
        if (!out[0])        AddReason(out, n, "so ESP/aimbot");
    }

    int RiskScore()
    {
        int r = 0;

        if (!C.antiCheat) r += 4;
        else if (!g_cls || !g_obj) r += 1;

        if (C.speedHack)  r += (C.speedValue >= 2.0f) ? 4 : 1;
        if (C.fly)        r += 3;
        if (C.godmode)    r += 2;
        if (C.teleport)   r += 1;
        if (C.rapidFire)  r += 1;
        if (C.infAmmo)    r += 1;
        if (C.noReload)   r += 1;
        if (C.silentAim)  r += 1;
        if (C.magicBullets) r += 1;
        if (C.noRecoil)   r += 1;
        if (C.noSpread)   r += 1;
        if (C.bhop)       r += 1;
        if (C.shootInAir) r += 1;

        return r;
    }

    int RiskLevel()
    {
        const int r = RiskScore();
        if (r >= 12) return 3;
        if (r >= 7)  return 2;
        if (r >= 3)  return 1;
        return 0;
    }

    const char* RiskText()
    {
        static const char* k[] = { "BAIXO", "MEDIO", "ALTO", "CRITICO" };
        const int l = RiskLevel();
        return k[(l < 0 || l > 3) ? 0 : l];
    }

} // namespace AntiCheat
// ==== FIM anticheat.cpp ====