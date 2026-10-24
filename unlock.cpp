// ============================================================================
//  unlock.cpp — bfi (v43.1)
//
//  >>> v43.1
//    * ESPELHO DA LOJA: a busca de campo ficou CASE-INSENSITIVE
//      (il2cpp_class_get_field_from_name e' case-sensitive, 'GoldAmount' nao
//      casava com 'goldAmount'), ganhou fallback por palavra-chave e o salto
//      'sidebar' ganhou fallback por offset (0x20).
//    * Quando um espelho falha, MnyDumpObj joga no log a classe real e TODOS
//      os campos do objeto (nome/tipo/offset) para voce corrigir a tabela.
//    * O resumo "[mirror] N/M espelhos resolvidos" so' loga quando muda.
//
//  >>> v43
//    * Define a API da CARTEIRA que o Menu::TabConta consome (assinaturas
//      exatas: MoneyCount/MoneyName/MoneyLocked/MoneySetLock/MoneyValue/
//      MoneySet/MoneyRefresh/MoneyLockByName/ForceMirrors/MoneyUnlockAll).
//    * Install() e' bool (sem E0311).
//    * Os espelhos sao campos de INSTANCIA alcancados pelo singleton
//      ESTATICO da classe (bug da v40: procurava-se campo static).
//      kind 0 = int, 1 = ObscuredInt (hidden = v ^ key, +0xC inited=1).
//    * MoneyTick consome C.accAuto, C.moneyIntervalMs, C.moneyApplyNow,
//      C.moneyRescan, C.moneyWritePrefs, C.moneyOnlyWallet, C.accGold,
//      C.accCases.
//    * Tick(): o tick da carteira roda ANTES do check de g_localPlayer.
//
//  >>> v25 (mantido): retry de Install no maximo 2x/s.
//  >>> v24 (mantido): loadout de ALFA/BRAVO/CHARLIE vem dos PlayerPrefs.
// ============================================================================
#include "features.h"
#include "sdk.h"
#include "hooks.h"
#include "common.h"
#include "memory.h"
#include "il2cpp_api.h"
#include "unlock.h"
#include "dupe.h"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#define BFI_UNLOCK_VER "v43.1"

namespace Unlock {

    static void* g_arr = nullptr;
    static int    g_arrLen = 0;
    static uint32_t g_offArr = 0;

    static uint32_t g_offName = 0, g_offAvailable = 0, g_offCamoType = 0;
    static uint32_t g_offTracerType = 0, g_offGcs = 0;
    static uint32_t g_offSights = 0, g_offAcc = 0, g_offBarrels = 0;
    static uint32_t g_offFpsGunView = 0;

    static const uint32_t kFvHeld = 0x38;
    static const uint32_t kFvSlot = 0x48;
    static const uint32_t kFvSights = 0x4C;
    static const uint32_t kFvBarrels = 0x50;
    static const uint32_t kFvAcc = 0x54;

    static const uint32_t kLoPrim = 0x144;
    static const uint32_t kLoSec = 0x145;
    static const uint32_t kLoGad = 0x10A4;

    static bool   g_installed = false;
    static const char* kSlotNames[3] = { "Mira (sights)", "Acessorios", "Cano (barrels)" };

    static ULONGLONG g_nextInstallTry = 0;
    static bool      g_dupeEnsured = false;

    // ================================================================= helpers
    typedef Il2CppClass* (*fn_elem)(Il2CppClass*);
    typedef uint32_t(*fn_arrlen)(const Il2CppArray*);
    typedef uint32_t(*fn_field_flags)(FieldInfo*);
    typedef void     (*fn_field_static_get)(FieldInfo*, void*);
    typedef void     (*fn_field_static_set)(FieldInfo*, void*);
    typedef void* (*fn_class_get_image)(Il2CppClass*);
    typedef size_t(*fn_image_class_count)(const void*);
    typedef Il2CppClass* (*fn_image_get_class)(const void*, size_t);
    typedef bool     (*fn_class_is_valuetype)(Il2CppClass*);
    typedef Il2CppClass* (*fn_class_from_type)(const Il2CppType*);
    typedef Il2CppType* (*fn_class_get_type)(Il2CppClass*);
    typedef void* (*fn_type_get_object)(const Il2CppType*);

    static fn_elem p_elem = nullptr;
    static fn_arrlen p_arrlen = nullptr;
    static fn_field_flags p_fieldFlags = nullptr;
    static fn_field_static_get p_fieldStaticGet = nullptr;
    static fn_field_static_set p_fieldStaticSet = nullptr;
    static fn_class_get_image p_classImage = nullptr;
    static fn_image_class_count p_imgCount = nullptr;
    static fn_image_get_class p_imgClass = nullptr;
    static fn_class_is_valuetype p_isValue = nullptr;
    static fn_class_from_type p_classFromType = nullptr;
    static fn_class_get_type p_classGetType = nullptr;
    static fn_type_get_object p_typeGetObject = nullptr;

    static void ResolveExtra() {
        static bool done = false;
        if (done) return;
        done = true;
        HMODULE ga = GetModuleHandleA("GameAssembly.dll");
        if (!ga) return;
        p_elem = (fn_elem)GetProcAddress(ga, "il2cpp_class_get_element_class");
        p_arrlen = (fn_arrlen)GetProcAddress(ga, "il2cpp_array_length");
        p_fieldFlags = (fn_field_flags)GetProcAddress(ga, "il2cpp_field_get_flags");
        p_fieldStaticGet = (fn_field_static_get)GetProcAddress(ga, "il2cpp_field_static_get_value");
        p_fieldStaticSet = (fn_field_static_set)GetProcAddress(ga, "il2cpp_field_static_set_value");
        p_classImage = (fn_class_get_image)GetProcAddress(ga, "il2cpp_class_get_image");
        p_imgCount = (fn_image_class_count)GetProcAddress(ga, "il2cpp_image_get_class_count");
        p_imgClass = (fn_image_get_class)GetProcAddress(ga, "il2cpp_image_get_class");
        p_isValue = (fn_class_is_valuetype)GetProcAddress(ga, "il2cpp_class_is_valuetype");
        p_classFromType = (fn_class_from_type)GetProcAddress(ga, "il2cpp_class_from_type");
        p_classGetType = (fn_class_get_type)GetProcAddress(ga, "il2cpp_class_get_type");
        p_typeGetObject = (fn_type_get_object)GetProcAddress(ga, "il2cpp_type_get_object");
    }

    static void* TypeGetObjectSafe(Il2CppType* t) {
        if (!t || !p_typeGetObject) return nullptr;
        void* r = nullptr;
        __try { r = p_typeGetObject(t); }
        __except (EXCEPTION_EXECUTE_HANDLER) { r = nullptr; }
        return r;
    }

    static bool Plausible(const void* p) {
        uintptr_t a = (uintptr_t)p;
        if (a < 0x10000 || a > 0x00007FFFFFFFFFFFULL) return false;
        if (a & 7) return false;
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(p, &mbi, sizeof(mbi))) return false;
        if (mbi.State != MEM_COMMIT) return false;
        if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
        return true;
    }
    static bool LooksLikeObj(const void* p) {
        if (!Plausible(p)) return false;
        return Plausible(*(Il2CppClass**)p);
    }
    static bool IsStatic(FieldInfo* f) {
        return (p_fieldFlags && f) ? ((p_fieldFlags(f) & 0x10) != 0) : false;
    }

    static void CStr(void* s, char* out, size_t n) {
        if (!out || !n) return;
        out[0] = 0;
        if (!LooksLikeObj(s)) return;
        int len = SafeRead<int>((uintptr_t)s + 0x10, 0);
        if (len <= 0) return;
        if (len > (int)n - 1) len = (int)n - 1;
        const unsigned short* w = (const unsigned short*)((uintptr_t)s + 0x14);
        for (int i = 0; i < len; ++i) out[i] = (char)(w[i] & 0xFF);
        out[len] = 0;
    }
    static void Lower(char* dst, size_t n, const char* src) {
        size_t i = 0;
        for (; src && src[i] && i + 1 < n; ++i) {
            char c = src[i];
            dst[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
        }
        dst[i] = 0;
    }

    static int ArrLen(Il2CppArray* a) {
        if (!a) return 0;
        if (p_arrlen) return (int)p_arrlen(a);
        return SafeRead<int>((uintptr_t)a + 0x18, 0);
    }
    static void* ArrElem(Il2CppArray* a, int i) {
        if (!a || i < 0) return nullptr;
        return SafeRead<void*>((uintptr_t)a + 0x20 + (size_t)i * 8, nullptr);
    }
    static void ArrSetPtr(Il2CppArray* a, int i, void* v) {
        if (!a || i < 0) return;
        SafeWrite<void*>((uintptr_t)a + 0x20 + (size_t)i * 8, v);
    }
    static void ArrSetI32(Il2CppArray* a, int i, int v) {
        if (!a || i < 0) return;
        SafeWrite<int>((uintptr_t)a + 0x20 + (size_t)i * 4, v);
    }
    static void ArrSetI8(Il2CppArray* a, int i, int v) {
        if (!a || i < 0) return;
        SafeWrite<unsigned char>((uintptr_t)a + 0x20 + (size_t)i, (unsigned char)v);
    }

    static void* ListItems(void* list) {
        if (!LooksLikeObj(list)) return nullptr;
        return SafeRead<void*>((uintptr_t)list + 0x10, nullptr);
    }
    static int ListSize(void* list) {
        if (!LooksLikeObj(list)) return 0;
        return SafeRead<int>((uintptr_t)list + 0x18, 0);
    }
    static int ListElemInt(void* list, int i) {
        void* items = ListItems(list);
        if (!LooksLikeObj(items)) return -1;
        int n = ListSize(list);
        if (i < 0 || i >= n) return -1;
        return SafeRead<int>((uintptr_t)items + 0x20 + (size_t)i * 4, -1);
    }

    static void StaticGetSafe(FieldInfo* f, void** out) {
        if (out) *out = nullptr;
        if (!f || !p_fieldStaticGet || !out) return;
        unsigned char buf[256];
        __try { p_fieldStaticGet(f, buf); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return; }
        memcpy(out, buf, sizeof(void*));
    }
    static void StaticSetInt(FieldInfo* f, int value, int isByte) {
        if (!f || !p_fieldStaticSet) return;
        unsigned char buf[64];
        memset(buf, 0, sizeof(buf));
        if (isByte) buf[0] = (unsigned char)(value & 0xFF);
        else        memcpy(buf, &value, sizeof(int));
        __try { p_fieldStaticSet(f, buf); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    static void InvokeSafe(MethodInfo* m, void* obj, void** args) {
        if (!m) return;
        __try { il2cpp_runtime_invoke(m, obj, args, nullptr); }
        __except (EXCEPTION_EXECUTE_HANDLER) { Log("[unlock] AV no invoke"); }
    }

    static Dupe::Item MkItem(int v, const std::string& n, int aux = 0) {
        Dupe::Item it; it.value = v; it.name = n; it.aux = aux; return it;
    }

    struct DictEnt { int key; void* val; };
    static int ReadDict(void* dict, DictEnt* out, int max) {
        if (!LooksLikeObj(dict) || !out || max <= 0) return 0;
        void* entries = SafeRead<void*>((uintptr_t)dict + 0x18, nullptr);
        int count = SafeRead<int>((uintptr_t)dict + 0x20, 0);
        if (!LooksLikeObj(entries) || count <= 0) return 0;
        if (count > max) count = max;
        int n = 0;
        for (int i = 0; i < count; ++i) {
            uintptr_t e = (uintptr_t)entries + 0x20 + (size_t)i * 24;
            if (SafeRead<int>(e + 0, 0) < 0) continue;
            void* val = SafeRead<void*>(e + 16, nullptr);
            if (!LooksLikeObj(val)) continue;
            out[n].key = SafeRead<int>(e + 8, 0);
            out[n].val = val;
            ++n;
        }
        return n;
    }

    // =========================================================================
    //  >>> v43.1 — MODULO CARTEIRA / ESPELHOS
    //  kind: 0 = int, 1 = ObscuredInt.
    //  A busca de campo e' CASE-INSENSITIVE e tem fallback por palavra-chave.
    //  hopFld/hopOff = salto intermediario (nome com fallback por offset).
    // =========================================================================
    struct MnyMirror {
        const char* cls;      // classe dona do campo
        const char* fld;      // campo do espelho (INSTANCIA)
        const char* fldKey;   // palavra-chave alternativa (substring, CI)
        const char* hopFld;   // campo intermediario (0 = nenhum)
        uint32_t    hopOff;   // fallback do salto por offset (0 = nenhum)
        const char* label;    // nome amigavel (MoneyName)
        int         kind;     // 0 = int, 1 = ObscuredInt
        bool        lock;
        bool        ok;
        void* base;
        uint32_t    off;
    };

    static MnyMirror g_mny[] = {
        { "CreditsManager",  "LDOGMJMLIOP", "credit", 0,         0,    "Creditos (UI)",  1, true, false, 0, 0 },
        { "AccountManager",  "GEEFAPKBHJO", "gold",   0,         0,    "Conta (gold)",   1, true, false, 0, 0 },
        { "CamoCaseManager", "goldCases",   "case",   0,         0,    "Caixas (gold)",  0, true, false, 0, 0 },
        { "LoadoutManager",  "goldAmount",  "gold",   0,         0,    "Loadout (gold)", 0, true, false, 0, 0 },
        { "LoadoutAndShop",  "goldAmount",  "gold",   "sidebar", 0x20, "Loja (sidebar)", 0, true, false, 0, 0 },
    };
    static const int g_mnyN = (int)(sizeof(g_mny) / sizeof(g_mny[0]));

    static bool      g_mnyDone = false;
    static ULONGLONG g_mnyNext = 0;
    static int       g_mnyLastOk = -1;

    static bool MnyIn(int i) { return i >= 0 && i < g_mnyN; }

    // singleton: por nome (varias grafias) e, se falhar, varredura CI
    static void* MnyStaticObj(const char* cls, const char* f1, const char* f2) {
        Il2CppClass* k = cls ? SDK::FindClass(cls) : nullptr;
        if (!k) return nullptr;
        const char* cands[4] = { f1, f2, "singleton", "Instance" };
        for (Il2CppClass* c = k; c; c = il2cpp_class_get_parent(c)) {
            for (int i = 0; i < 4; ++i) {
                if (!cands[i]) continue;
                FieldInfo* f = il2cpp_class_get_field_from_name(c, cands[i]);
                if (!f || !IsStatic(f)) continue;
                void* o = nullptr; StaticGetSafe(f, &o);
                if (LooksLikeObj(o)) return o;
            }
        }
        for (Il2CppClass* c = k; c; c = il2cpp_class_get_parent(c)) {
            void* it = nullptr;
            for (;;) {
                FieldInfo* f = il2cpp_class_get_fields(c, &it);
                if (!f) break;
                if (!IsStatic(f)) continue;
                const char* fn = il2cpp_field_get_name(f);
                if (!fn) continue;
                if (_stricmp(fn, "singleton") != 0 && _stricmp(fn, "instance") != 0) continue;
                void* o = nullptr; StaticGetSafe(f, &o);
                if (LooksLikeObj(o)) return o;
            }
        }
        return nullptr;
    }

    // campo por nome EXATO, case-insensitive
    // (il2cpp_class_get_field_from_name e' case-sensitive: 'GoldAmount' != 'goldAmount')
    static uint32_t MnyOffExact(void* obj, const char* fld) {
        if (!LooksLikeObj(obj) || !fld) return 0;
        Il2CppClass* k = il2cpp_object_get_class((Il2CppObject*)obj);
        for (Il2CppClass* c = k; c; c = il2cpp_class_get_parent(c)) {
            void* it = nullptr;
            for (;;) {
                FieldInfo* f = il2cpp_class_get_fields(c, &it);
                if (!f) break;
                if (IsStatic(f)) continue;
                const char* fn = il2cpp_field_get_name(f);
                if (!fn || _stricmp(fn, fld) != 0) continue;
                uint32_t off = (uint32_t)il2cpp_field_get_offset(f);
                if (off) return off;
            }
        }
        return 0;
    }

    // fallback: primeiro campo inteiro cujo nome contenha a palavra-chave
    static uint32_t MnyOffKey(void* obj, const char* key) {
        if (!LooksLikeObj(obj) || !key) return 0;
        char kl[40]; Lower(kl, sizeof(kl), key);
        Il2CppClass* k = il2cpp_object_get_class((Il2CppObject*)obj);
        for (Il2CppClass* c = k; c; c = il2cpp_class_get_parent(c)) {
            void* it = nullptr;
            for (;;) {
                FieldInfo* f = il2cpp_class_get_fields(c, &it);
                if (!f) break;
                if (IsStatic(f)) continue;
                const Il2CppType* t = il2cpp_field_get_type(f);
                const char* tn = (t && il2cpp_type_get_name) ? il2cpp_type_get_name(t) : nullptr;
                if (!tn) continue;
                if (!strstr(tn, "Int32") && !strstr(tn, "ObscuredInt") && !strstr(tn, "Int64")) continue;
                const char* fn = il2cpp_field_get_name(f);
                if (!fn) continue;
                char nl[80]; Lower(nl, sizeof(nl), fn);
                if (!strstr(nl, kl)) continue;
                uint32_t off = (uint32_t)il2cpp_field_get_offset(f);
                if (off) return off;
            }
        }
        return 0;
    }

    // diagnostico: joga no log a classe e os campos do objeto que falhou
    static void MnyDumpObj(void* o, const char* tag, int max) {
        if (!LooksLikeObj(o)) return;
        Il2CppClass* k = il2cpp_object_get_class((Il2CppObject*)o);
        Log("[mirror] %s: objeto classe=%s %p", tag, k ? il2cpp_class_get_name(k) : "?", o);
        int n = 0;
        for (Il2CppClass* c = k; c && n < max; c = il2cpp_class_get_parent(c)) {
            void* it = nullptr;
            for (;;) {
                FieldInfo* f = il2cpp_class_get_fields(c, &it);
                if (!f) break;
                if (IsStatic(f)) continue;
                if (n++ >= max) break;
                const Il2CppType* t = il2cpp_field_get_type(f);
                const char* tn = (t && il2cpp_type_get_name) ? il2cpp_type_get_name(t) : "?";
                Log("[mirror]   %s::%s %s @0x%X", il2cpp_class_get_name(c),
                    il2cpp_field_get_name(f), tn, (unsigned)il2cpp_field_get_offset(f));
            }
        }
    }

    static void MnyResolve() {
        if (g_mnyDone) return;
        const ULONGLONG now = GetTickCount64();
        if (now < g_mnyNext) return;
        g_mnyNext = now + 2000;                 // retry no maximo 1x/2s
        int ok = 0;
        for (int i = 0; i < g_mnyN; ++i) {
            MnyMirror& m = g_mny[i];
            m.ok = false; m.base = nullptr; m.off = 0;

            void* base = MnyStaticObj(m.cls, "singleton", "Instance");
            if (!base) base = MnyStaticObj(m.cls, "instance", "s_instance");
            if (!base) { Log("[mirror] %s: singleton ausente", m.label); continue; }

            if (m.hopFld || m.hopOff) {
                uint32_t ho = MnyOffExact(base, m.hopFld);
                if (!ho) ho = m.hopOff;         // fallback por offset
                void* mid = ho ? SafeRead<void*>((uintptr_t)base + ho, nullptr) : nullptr;
                if (!LooksLikeObj(mid)) {
                    Log("[mirror] %s: salto falhou (off=0x%X)", m.label, ho);
                    continue;
                }
                base = mid;
            }

            uint32_t off = MnyOffExact(base, m.fld);
            if (!off && m.fldKey) off = MnyOffKey(base, m.fldKey);
            if (!off) {
                Log("[mirror] %s: campo '%s' nao achado", m.label, m.fld);
                MnyDumpObj(base, m.label, 48);
                continue;
            }
            m.base = base; m.off = off; m.ok = true;
            ++ok;
        }
        if (ok == g_mnyN) g_mnyDone = true;
        if (ok != g_mnyLastOk) {                // nao spamma o log a cada 2s
            g_mnyLastOk = ok;
            Log("[mirror] %d/%d espelhos resolvidos", ok, g_mnyN);
        }
    }

    static long long MnyRead(const MnyMirror& m) {
        if (!m.ok || !m.base || !m.off) return 0;
        uintptr_t a = (uintptr_t)m.base + m.off;
        if (m.kind == 0) return (long long)SafeRead<int>(a, 0);
        const int key = SafeRead<int>(a + 0, 0);
        const int hid = SafeRead<int>(a + 4, 0);
        return (long long)(int)(hid ^ key);
    }

    static void MnyWriteOne(MnyMirror& m, long long v) {
        if (!m.ok || !m.base || !m.off) return;
        uintptr_t a = (uintptr_t)m.base + m.off;
        const int iv = (int)v;
        if (m.kind == 0) { SafeWrite<int>(a, iv); return; }
        const int key = SafeRead<int>(a + 0, 0);
        SafeWrite<int>(a + 4, iv ^ key);   // ObscuredInt: +0 key, +4 hidden
        SafeWrite<int>(a + 8, iv);         //               +8 fake
        SafeWrite<int>(a + 0xC, 1);        //               +0xC inited
    }

    static bool MnyMatch(const MnyMirror& m, const char* cls, const char* fld) {
        if (cls && cls[0]) {
            const char* sep = strstr(cls, "::");
            if (sep) {
                const size_t n = (size_t)(sep - cls);
                if (n && !(m.cls && _strnicmp(m.cls, cls, n) == 0)) return false;
                const char* f = sep + 2;
                if (f[0] && !(m.fld && _stricmp(m.fld, f) == 0)) return false;
            }
            else {
                if (!(m.cls && _strnicmp(m.cls, cls, strlen(cls)) == 0)) return false;
            }
        }
        if (fld && fld[0]) {
            if (!(m.fld && _stricmp(m.fld, fld) == 0)) return false;
        }
        return true;
    }

    static long long MnyCfgValue() {
        return (C.currencyValue > 0) ? (long long)C.currencyValue : 999999LL;
    }

    // =========================== API publica da carteira =====================
    int MoneyCount() { return g_mnyN; }

    const char* MoneyName(int idx) {
        return MnyIn(idx) ? g_mny[idx].label : "";
    }

    bool MoneyLocked(int idx) {
        return MnyIn(idx) ? g_mny[idx].lock : false;
    }

    void MoneySetLock(int idx, bool on) {
        if (MnyIn(idx)) g_mny[idx].lock = on;
    }

    long long MoneyValue(int idx) {
        if (!MnyIn(idx)) return 0;
        MnyResolve();
        return MnyRead(g_mny[idx]);
    }

    void MoneySet(int idx, long long value) {
        if (!MnyIn(idx)) return;
        MnyResolve();
        MnyWriteOne(g_mny[idx], value);
        Log("[mirror] %s <- %lld", g_mny[idx].label, value);
    }

    void MoneyRefresh() {
        g_mnyDone = false;
        g_mnyNext = 0;
        g_mnyLastOk = -1;
        MnyResolve();
    }

    bool MoneyReady() {
        MnyResolve();
        for (int i = 0; i < g_mnyN; ++i) if (!g_mny[i].ok) return false;
        return true;
    }

    int MoneyLockByName(const char* cls, const char* field, long long value) {
        MnyResolve();
        int n = 0;
        for (int i = 0; i < g_mnyN; ++i) {
            MnyMirror& m = g_mny[i];
            if (!m.ok) continue;
            if (!MnyMatch(m, cls, field)) continue;
            m.lock = true;
            MnyWriteOne(m, value);
            ++n;
        }
        Log("[mirror] LockByName(\"%s\", \"%s\", %lld): %d espelho(s)",
            cls ? cls : "", field ? field : "", value, n);
        return n;
    }

    int ForceMirrors(long long value, long long mode) {
        MnyResolve();
        int n = 0;
        for (int i = 0; i < g_mnyN; ++i) {
            MnyMirror& m = g_mny[i];
            if (!m.ok) continue;
            if (mode == 1 && !m.lock) continue;
            if (mode == 2 && m.lock) continue;
            MnyWriteOne(m, value);
            ++n;
        }
        Log("[mirror] ForceMirrors(%lld, mode=%lld): %d/%d escritos",
            value, mode, n, g_mnyN);
        return n;
    }

    void MoneyUnlockAll() {
        const long long v = MnyCfgValue();
        for (int i = 0; i < g_mnyN; ++i) g_mny[i].lock = true;
        ForceMirrors(v, 0);
    }

    void ShopForceAll() {
        const long long gold = (C.accGold > 0) ? (long long)C.accGold : MnyCfgValue();
        const long long cases = (C.accCases > 0) ? (long long)C.accCases : 500LL;
        MoneyLockByName("AccountManager", "GEEFAPKBHJO", gold);
        MoneyLockByName("CamoCaseManager", "goldCases", cases);
        MoneyLockByName("LoadoutManager", "goldAmount", gold);
        MoneyLockByName("LoadoutAndShop", "goldAmount", gold);
    }

    // =============================================================== descoberta
    static uint32_t FieldOffNamed(Il2CppClass* k, const char* name) {
        if (!k) return 0;
        for (Il2CppClass* c = k; c; c = il2cpp_class_get_parent(c)) {
            FieldInfo* f = il2cpp_class_get_field_from_name(c, name);
            if (!f || IsStatic(f)) continue;
            uint32_t off = (uint32_t)il2cpp_field_get_offset(f);
            if (off) return off;
        }
        return 0;
    }
    static uint32_t FieldOffByType(Il2CppClass* k, const char* typeKey) {
        if (!k) return 0;
        for (Il2CppClass* c = k; c; c = il2cpp_class_get_parent(c)) {
            void* it = nullptr;
            for (;;) {
                FieldInfo* f = il2cpp_class_get_fields(c, &it);
                if (!f) break;
                if (IsStatic(f)) continue;
                const Il2CppType* t = il2cpp_field_get_type(f);
                const char* tn = (t && il2cpp_type_get_name) ? il2cpp_type_get_name(t) : nullptr;
                if (!tn || !strstr(tn, typeKey)) continue;
                uint32_t off = (uint32_t)il2cpp_field_get_offset(f);
                if (off) return off;
            }
        }
        return 0;
    }

    static bool FindWeaponArray(void* p, uint32_t& outOff, void*& outArr, int& outLen) {
        outOff = 0; outArr = nullptr; outLen = 0;
        if (!p || !SDK::kPlayerScript || !SDK::kGunStats) return false;
        uint32_t bestOff = 0; void* bestArr = nullptr; int bestLen = 0;
        for (Il2CppClass* c = SDK::kPlayerScript; c; c = il2cpp_class_get_parent(c)) {
            void* it = nullptr;
            for (;;) {
                FieldInfo* f = il2cpp_class_get_fields(c, &it);
                if (!f) break;
                if (IsStatic(f)) continue;
                const Il2CppType* t = il2cpp_field_get_type(f);
                const char* tn = (t && il2cpp_type_get_name) ? il2cpp_type_get_name(t) : nullptr;
                if (!tn || !strstr(tn, "[]")) continue;
                uint32_t off = (uint32_t)il2cpp_field_get_offset(f);
                if (!off) continue;
                void* a = SafeRead<void*>((uintptr_t)p + off, nullptr);
                if (!LooksLikeObj(a)) continue;
                int len = ArrLen((Il2CppArray*)a);
                if (len < 8 || len > 512) continue;
                void* e0 = ArrElem((Il2CppArray*)a, 0);
                if (!LooksLikeObj(e0)) continue;
                Il2CppClass* k = il2cpp_object_get_class((Il2CppObject*)e0);
                bool ok = (k == SDK::kGunStats);
                for (Il2CppClass* q = k; !ok && q; q = il2cpp_class_get_parent(q))
                    if (q == SDK::kGunStats) ok = true;
                if (!ok) continue;
                if (len > bestLen) { bestOff = off; bestArr = a; bestLen = len; }
            }
        }
        if (!bestArr) return false;
        outOff = bestOff; outArr = bestArr; outLen = bestLen;
        return true;
    }

    static void ResolveOffsets() {
        if (!SDK::kGunStats) return;
        g_offName = FieldOffNamed(SDK::kGunStats, "weaponName");
        g_offAvailable = FieldOffNamed(SDK::kGunStats, "available");
        g_offCamoType = FieldOffNamed(SDK::kGunStats, "camoType");
        g_offTracerType = FieldOffNamed(SDK::kGunStats, "tracerType");
        g_offGcs = FieldOffNamed(SDK::kGunStats, "gunCustomizeScript");
        g_offSights = FieldOffNamed(SDK::kGunStats, "possibleSights");
        g_offAcc = FieldOffNamed(SDK::kGunStats, "possibleAccessories");
        g_offBarrels = FieldOffNamed(SDK::kGunStats, "possibleBarrels");
        if (!g_offName)       g_offName = 0x278;
        if (!g_offAvailable)  g_offAvailable = 0x270;
        if (!g_offCamoType)   g_offCamoType = 0x2C0;
        if (!g_offTracerType) g_offTracerType = 0x2C4;
        if (!g_offGcs)        g_offGcs = 0x2B0;
        if (!g_offSights)     g_offSights = 0x118;
        if (!g_offAcc)        g_offAcc = 0x120;
        if (!g_offBarrels)    g_offBarrels = 0x128;
        if (SDK::kPlayerScript) {
            g_offFpsGunView = FieldOffNamed(SDK::kPlayerScript, "fpsGunView");
            if (!g_offFpsGunView) g_offFpsGunView = FieldOffByType(SDK::kPlayerScript, "FirstPersonGunView");
            if (!g_offFpsGunView) g_offFpsGunView = 0x8F0;
        }
    }

    static void* FpsGunView() {
        if (!SDK::g_localPlayer || !g_offFpsGunView) return nullptr;
        void* v = SafeRead<void*>((uintptr_t)SDK::g_localPlayer + g_offFpsGunView, nullptr);
        return LooksLikeObj(v) ? v : nullptr;
    }

    // =========================================================================
    struct LoCand {
        int kind; FieldInfo* baseF; void* basePtr; uint32_t off; FieldInfo* f;
        int type; int useType; int slot; char cls[40]; char name[48];
    };
    static LoCand g_loC[160];
    static int    g_loCN = 0;
    static bool   g_loDone = false;

    static int SlotFromName(const char* nl) {
        if (strstr(nl, "primary") || strstr(nl, "primaria"))  return 0;
        if (strstr(nl, "secondary") || strstr(nl, "secundaria")) return 1;
        if (strstr(nl, "gadget"))                             return 2;
        if (strstr(nl, "weapon1") || strstr(nl, "gun1"))      return 0;
        if (strstr(nl, "weapon2") || strstr(nl, "gun2"))      return 1;
        if (strstr(nl, "weapon3") || strstr(nl, "gun3"))      return 2;
        return -1;
    }
    static bool NameReject(const char* nl) {
        static const char* rej[] = { "ammo","reserve","count","timer","cool","reload",
            "fire","burst","mag","clip","last","temp","scope","zoom","time",
            "speed","angle","offset","pos","rot","size","rate","dmg","damage","camo","skin",
            "text","label","name","prefs","url","icon","color","prefab" };
        for (size_t i = 0; i < sizeof(rej) / sizeof(rej[0]); ++i)
            if (strstr(nl, rej[i])) return true;
        return false;
    }
    static bool NameAcceptArray(const char* nl) {
        return strstr(nl, "loadout") || strstr(nl, "weapon") || strstr(nl, "gun") ||
            strstr(nl, "slot") || strstr(nl, "primary") || strstr(nl, "secondary") ||
            strstr(nl, "gadget") || strstr(nl, "class") || strstr(nl, "default");
    }

    static void PushCand(int kind, void* basePtr, FieldInfo* baseF, FieldInfo* f,
        int type, int useType, int slot, const char* cls) {
        if (g_loCN >= 160 || !f) return;
        LoCand& c = g_loC[g_loCN++];
        c.kind = kind; c.baseF = baseF; c.basePtr = basePtr;
        c.off = (uint32_t)il2cpp_field_get_offset(f);
        c.f = f; c.type = type; c.useType = useType; c.slot = slot;
        strncpy_s(c.cls, cls ? cls : "?", _TRUNCATE);
        strncpy_s(c.name, il2cpp_field_get_name(f), _TRUNCATE);
    }

    static bool FindElemSlotField(void* elemObj, int slot, int& offOut, int& typeOut, int& useTypeOut) {
        offOut = 0; typeOut = 0; useTypeOut = 0;
        if (!LooksLikeObj(elemObj)) return false;
        Il2CppClass* k = il2cpp_object_get_class((Il2CppObject*)elemObj);
        if (!k) return false;
        for (Il2CppClass* c = k; c; c = il2cpp_class_get_parent(c)) {
            void* it = nullptr;
            for (;;) {
                FieldInfo* f = il2cpp_class_get_fields(c, &it);
                if (!f) break;
                if (IsStatic(f)) continue;
                const Il2CppType* t = il2cpp_field_get_type(f);
                const char* tn = (t && il2cpp_type_get_name) ? il2cpp_type_get_name(t) : nullptr;
                if (!tn) continue;
                char tl[64]; Lower(tl, sizeof(tl), tn);
                if (strstr(tl, "[]")) continue;
                int ty = 0, ut = 0;
                if (strstr(tl, "int32")) { ty = 2; ut = 0; }
                else if (strstr(tl, "byte")) { ty = 1; ut = 0; }
                else if (p_classFromType && p_isValue) {
                    Il2CppClass* ek = p_classFromType(t);
                    if (!ek || !p_isValue(ek)) continue;
                    if (strstr(tl, "single") || strstr(tl, "double") ||
                        strstr(tl, "boolean") || strstr(tl, "string")) continue;
                    ty = 2; ut = 1;
                }
                else continue;
                char nl[64]; Lower(nl, sizeof(nl), il2cpp_field_get_name(f));
                if (NameReject(nl)) continue;
                if (SlotFromName(nl) != slot) continue;
                uint32_t off = (uint32_t)il2cpp_field_get_offset(f);
                if (!off) continue;
                offOut = (int)off; typeOut = ty; useTypeOut = ut;
                return true;
            }
        }
        return false;
    }

    static bool ArrayElemInfo(const Il2CppType* t, const char* tl, int& type, int& useType) {
        type = 0; useType = 0;
        if (!t || !tl) return false;
        if (strstr(tl, "int32")) { type = 3; useType = 0; return true; }
        if (strstr(tl, "byte")) { type = 6; useType = 0; return true; }
        if (strstr(tl, "gunstats")) { type = 4; useType = 0; return true; }
        if (!p_classFromType || !p_isValue || !p_elem) return false;
        Il2CppClass* arrCls = p_classFromType(t);
        if (!arrCls) return false;
        Il2CppClass* elemCls = p_elem(arrCls);
        if (!elemCls) return false;
        const char* en = il2cpp_class_get_name(elemCls);
        char el[64]; Lower(el, sizeof(el), en ? en : "");
        if (p_isValue(elemCls)) {
            if (strstr(el, "vector") || strstr(el, "color") || strstr(el, "quaternion") ||
                strstr(el, "rect") || strstr(el, "matrix") || strstr(el, "single") ||
                strstr(el, "double") || strstr(el, "boolean")) return false;
            type = 3; useType = 1;
            return true;
        }
        if (strstr(el, "gunstats")) { type = 4; return true; }
        if (strstr(el, "string")) { type = 7; return true; }
        if (strstr(el, "loadout") || strstr(el, "class") || strstr(el, "profile")) { type = 5; return true; }
        return false;
    }

    static void AddCandField(int kind, void* instBase, void* basePtr, FieldInfo* baseF,
        FieldInfo* f, const char* cls, bool isStaticScope) {
        const Il2CppType* t = il2cpp_field_get_type(f);
        const char* tn = (t && il2cpp_type_get_name) ? il2cpp_type_get_name(t) : nullptr;
        const char* fn = il2cpp_field_get_name(f);
        if (!tn || !fn) return;
        char tl[96]; Lower(tl, sizeof(tl), tn);
        char nl[64]; Lower(nl, sizeof(nl), fn);
        if (NameReject(nl)) return;

        bool isArr = (strstr(tl, "[]") != nullptr);
        int type = 0, useType = 0;
        if (isArr) {
            if (!ArrayElemInfo(t, tl, type, useType)) return;
            const bool enumArr = (type == 3 && useType == 1);
            if (!enumArr && !NameAcceptArray(nl)) return;
        }
        else if (strstr(tl, "int32")) { type = 2; useType = 0; }
        else if (strstr(tl, "byte")) { type = 1; useType = 0; }
        else if (p_classFromType && p_isValue) {
            Il2CppClass* ek = p_classFromType(t);
            if (!ek || !p_isValue(ek)) return;
            if (strstr(tl, "single") || strstr(tl, "double") ||
                strstr(tl, "boolean") || strstr(tl, "string")) return;
            type = 2; useType = 1;
        }
        else return;

        int slot = -1;
        if (type == 1 || type == 2) {
            slot = SlotFromName(nl);
            if (slot < 0) return;
        }
        else {
            uint32_t off = (uint32_t)il2cpp_field_get_offset(f);
            void* a = nullptr;
            if (isStaticScope) StaticGetSafe(f, &a);
            else {
                if (!instBase || !off) return;
                a = SafeRead<void*>((uintptr_t)instBase + off, nullptr);
            }
            if (!LooksLikeObj(a)) return;
            int len = ArrLen((Il2CppArray*)a);
            if (len < 3 || len > 8) return;
        }
        PushCand(kind, basePtr, baseF, f, type, useType, slot, cls);
    }

    static void ScanInstanceFields(int kind, void* instBase, void* basePtr, FieldInfo* baseF,
        const char* cls) {
        if (!instBase) return;
        Il2CppClass* k = il2cpp_object_get_class((Il2CppObject*)instBase);
        if (!k) return;
        for (Il2CppClass* c = k; c; c = il2cpp_class_get_parent(c)) {
            void* it = nullptr;
            for (;;) {
                FieldInfo* f = il2cpp_class_get_fields(c, &it);
                if (!f) break;
                if (IsStatic(f)) continue;
                AddCandField(kind, instBase, basePtr, baseF, f, cls, false);
            }
        }
    }

    static void DumpObjFields(void* o, const char* tag, int maxFields);

    static void ScanLoadoutSingletons() {
        if (!p_classImage || !p_imgCount || !p_imgClass || !SDK::kPlayerScript) return;
        void* img = p_classImage(SDK::kPlayerScript);
        if (!img) return;
        size_t n = p_imgCount(img);
        if (n > 60000) n = 60000;
        int found = 0;
        for (size_t i = 0; i < n && found < 40; ++i) {
            Il2CppClass* k = p_imgClass(img, i);
            if (!k) continue;
            const char* cn = il2cpp_class_get_name(k);
            if (!cn || !strstr(cn, "Loadout")) continue;
            ++found;
            for (Il2CppClass* c = k; c; c = il2cpp_class_get_parent(c)) {
                void* it = nullptr;
                for (;;) {
                    FieldInfo* f = il2cpp_class_get_fields(c, &it);
                    if (!f) break;
                    if (!IsStatic(f)) continue;
                    const Il2CppType* t = il2cpp_field_get_type(f);
                    const char* tn = (t && il2cpp_type_get_name) ? il2cpp_type_get_name(t) : nullptr;
                    if (!tn) continue;
                    if (strstr(tn, "[]") || strstr(tn, "List<")) continue;
                    if (!strstr(tn, cn)) continue;
                    void* obj = nullptr;
                    StaticGetSafe(f, &obj);
                    if (!LooksLikeObj(obj)) continue;
                    Log("[lod] STATIC OBJ %s::%s %s = %p", cn, il2cpp_field_get_name(f), tn, obj);
                    if (found <= 2) DumpObjFields(obj, cn, 24);
                    ScanInstanceFields(3, obj, nullptr, f, cn);
                }
            }
        }
    }

    static void* FindObjsOfType(Il2CppClass* k, int& outLen) {
        outLen = 0;
        if (!k || !p_classGetType || !p_typeGetObject) return nullptr;
        Il2CppClass* oc = nullptr;
        for (Il2CppClass* c = k; c; c = il2cpp_class_get_parent(c))
            if (strcmp(il2cpp_class_get_name(c), "Object") == 0) { oc = c; break; }
        if (!oc) return nullptr;
        MethodInfo* fot = il2cpp_class_get_method_from_name(oc, "FindObjectsOfType", 1);
        if (!fot) return nullptr;
        Il2CppType* t = p_classGetType(k);
        if (!t) return nullptr;
        void* typeObj = TypeGetObjectSafe(t);
        if (!typeObj) return nullptr;
        void* args[1] = { typeObj };
        void* arr = nullptr;
        __try { arr = il2cpp_runtime_invoke(fot, nullptr, args, nullptr); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
        if (!LooksLikeObj(arr)) return nullptr;
        outLen = ArrLen((Il2CppArray*)arr);
        return arr;
    }

    static void ScanLoadoutContainers() {
        static const char* names[] = { "PlayerLoadoutContainer", "DefaultLoadoutLoader" };
        for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
            Il2CppClass* k = SDK::FindClass(names[i]);
            if (!k) continue;
            const char* kn = il2cpp_class_get_name(k);
            if (!kn || strcmp(kn, names[i]) != 0) continue;
            int len = 0;
            void* arr = FindObjsOfType(k, len);
            for (int e = 0; e < len && e < 4; ++e) {
                void* o = ArrElem((Il2CppArray*)arr, e);
                if (!LooksLikeObj(o)) continue;
                ScanInstanceFields(4, o, o, nullptr, names[i]);
            }
        }
    }

    static void ScanLoadoutGlobals() {
        if (!p_classImage || !p_imgCount || !p_imgClass || !SDK::kPlayerScript) return;
        void* img = p_classImage(SDK::kPlayerScript);
        if (!img) return;
        size_t n = p_imgCount(img);
        if (n > 60000) n = 60000;
        for (size_t i = 0; i < n; ++i) {
            Il2CppClass* k = p_imgClass(img, i);
            if (!k) continue;
            const char* cn = il2cpp_class_get_name(k);
            if (!cn) continue;
            const bool nameHit = (strstr(cn, "Loadout") || strstr(cn, "Class") ||
                strstr(cn, "Slot") || strstr(cn, "Profile") ||
                strstr(cn, "PlayerData") || strstr(cn, "WeaponData"));
            for (Il2CppClass* c = k; c; c = il2cpp_class_get_parent(c)) {
                void* it = nullptr;
                for (;;) {
                    FieldInfo* f = il2cpp_class_get_fields(c, &it);
                    if (!f) break;
                    if (!IsStatic(f)) continue;
                    const Il2CppType* t = il2cpp_field_get_type(f);
                    const char* tn = (t && il2cpp_type_get_name) ? il2cpp_type_get_name(t) : nullptr;
                    if (!tn) continue;
                    const bool isArr = (strstr(tn, "[]") != nullptr);
                    if (!isArr && !nameHit) continue;
                    void* v = nullptr;
                    StaticGetSafe(f, &v);
                    if (!LooksLikeObj(v)) continue;
                    if (isArr) {
                        int len = ArrLen((Il2CppArray*)v);
                        if (len < 3 || len > 8) continue;
                        char tl[96]; Lower(tl, sizeof(tl), tn);
                        int ty = 0, ut = 0;
                        if (!ArrayElemInfo(t, tl, ty, ut)) continue;
                        AddCandField(1, nullptr, nullptr, nullptr, f, cn, true);
                    }
                }
            }
        }
    }

    static void ResolveLoadoutFields() {
        if (g_loDone) return;
        if (!SDK::kPlayerScript || !SDK::g_localPlayer) return;
        g_loDone = true;
        g_loCN = 0;
        ScanInstanceFields(0, SDK::g_localPlayer, nullptr, nullptr, "PlayerScript");
        void* fv = FpsGunView();
        if (fv) ScanInstanceFields(2, fv, nullptr, nullptr, "FirstPersonGunView");
        ScanLoadoutGlobals();
        ScanLoadoutSingletons();
        ScanLoadoutContainers();
        Log("[unlock] loadout: %d candidatos", g_loCN);
        for (int i = 0; i < g_loCN; ++i)
            Log("[unlock]   cand[%02d] %s::%s kind=%d type=%d useType=%d slot=%d off=0x%X",
                i, g_loC[i].cls, g_loC[i].name, g_loC[i].kind, g_loC[i].type,
                g_loC[i].useType, g_loC[i].slot, g_loC[i].off);
    }

    static void* CandBase(const LoCand& c) {
        if (c.kind == 0) return SDK::g_localPlayer;
        if (c.kind == 2) return FpsGunView();
        if (c.kind == 4) return LooksLikeObj(c.basePtr) ? c.basePtr : nullptr;
        if (c.kind == 3) {
            if (!c.baseF) return nullptr;
            void* o = nullptr;
            StaticGetSafe(c.baseF, &o);
            return LooksLikeObj(o) ? o : nullptr;
        }
        return nullptr;
    }

    static void LoWriteArray(void* a, const LoCand& c, int slot, int idx, int typeVal, void* nameStr) {
        if (!LooksLikeObj(a)) return;
        const int v = c.useType ? typeVal : idx;
        if (c.type == 3) { ArrSetI32((Il2CppArray*)a, slot, v); return; }
        if (c.type == 6) { ArrSetI8((Il2CppArray*)a, slot, v); return; }
        if (c.type == 7) { if (LooksLikeObj(nameStr)) ArrSetPtr((Il2CppArray*)a, slot, nameStr); return; }
        if (c.type == 4) {
            void* g = (g_arr && idx < g_arrLen) ? ArrElem((Il2CppArray*)g_arr, idx) : nullptr;
            if (LooksLikeObj(g)) ArrSetPtr((Il2CppArray*)a, slot, g);
            return;
        }
        int len = ArrLen((Il2CppArray*)a);
        for (int e = 0; e < len; ++e) {
            void* el = ArrElem((Il2CppArray*)a, e);
            int off = 0, ty = 0, ut = 0;
            if (!FindElemSlotField(el, slot, off, ty, ut)) continue;
            int vv = ut ? typeVal : idx;
            if (ty == 1) SafeWrite<unsigned char>((uintptr_t)el + off, (unsigned char)(vv & 0xFF));
            else         SafeWrite<int>((uintptr_t)el + off, vv);
        }
    }

    static void LoWriteCand(const LoCand& c, int slot, int idx, int typeVal, void* nameStr) {
        if (c.slot >= 0 && c.slot != slot) return;
        if (c.kind == 1) {
            if (!p_fieldStaticSet || !c.f) return;
            if (c.type == 1) { StaticSetInt(c.f, idx, 1); return; }
            if (c.type == 2) { StaticSetInt(c.f, c.useType ? typeVal : idx, 0); return; }
            void* a = nullptr; StaticGetSafe(c.f, &a);
            LoWriteArray(a, c, slot, idx, typeVal, nameStr);
            return;
        }
        uintptr_t b = (uintptr_t)CandBase(c);
        if (!b || !c.off) return;
        if (c.type == 1) { SafeWrite<unsigned char>(b + c.off, (unsigned char)(idx & 0xFF)); return; }
        if (c.type == 2) { SafeWrite<int>(b + c.off, c.useType ? typeVal : idx); return; }
        void* a = SafeRead<void*>(b + c.off, nullptr);
        LoWriteArray(a, c, slot, idx, typeVal, nameStr);
    }

    static void LoadoutWrite(int slot, int weaponIdx, int weaponType) {
        if (slot < 0 || slot > 2 || weaponIdx < 0) return;
        if (!SDK::g_localPlayer) return;
        if (!g_loDone) ResolveLoadoutFields();
        void* nameStr = nullptr;
        if (g_arr && weaponIdx < g_arrLen) {
            void* g = ArrElem((Il2CppArray*)g_arr, weaponIdx);
            if (LooksLikeObj(g)) nameStr = SafeRead<void*>((uintptr_t)g + g_offName, nullptr);
        }
        for (int i = 0; i < g_loCN; ++i) LoWriteCand(g_loC[i], slot, weaponIdx, weaponType, nameStr);
    }

    // =========================================================================
    static int      g_liveIdx = -1;
    static int      g_liveFor = -2;
    static uint32_t g_liveEpoch = 0;

    static int LiveWeaponIndex(int listIdx) {
        if (!g_arr || g_arrLen <= 0 || listIdx < 0) return listIdx;
        if (g_liveFor == listIdx && g_liveEpoch == Dupe::g_epoch.load()) return g_liveIdx;
        int guess = Dupe::WeaponValue(listIdx);
        if (guess < 0 || guess >= g_arrLen) guess = listIdx;
        const char* want = Dupe::WeaponName(listIdx);
        void* gun = ArrElem((Il2CppArray*)g_arr, guess);
        if (gun && want && want[0]) {
            char nm[128]; nm[0] = 0;
            CStr(SafeRead<void*>((uintptr_t)gun + g_offName, nullptr), nm, sizeof(nm));
            if (nm[0] && _stricmp(nm, want) == 0) {
                g_liveFor = listIdx; g_liveEpoch = Dupe::g_epoch.load(); g_liveIdx = guess;
                return guess;
            }
        }
        if (want && want[0]) {
            for (int i = 0; i < g_arrLen; ++i) {
                void* g2 = ArrElem((Il2CppArray*)g_arr, i);
                if (!g2) continue;
                char nm[128]; nm[0] = 0;
                CStr(SafeRead<void*>((uintptr_t)g2 + g_offName, nullptr), nm, sizeof(nm));
                if (nm[0] && _stricmp(nm, want) == 0) {
                    g_liveFor = listIdx; g_liveEpoch = Dupe::g_epoch.load(); g_liveIdx = i;
                    return i;
                }
            }
        }
        g_liveFor = listIdx; g_liveEpoch = Dupe::g_epoch.load(); g_liveIdx = guess;
        return guess;
    }

    static int WeaponTypeOfList(int listIdx) {
        if (!g_dupeEnsured) { Dupe::EnsureLoaded(); g_dupeEnsured = true; }
        if (listIdx < 0 || listIdx >= (int)Dupe::g_weapons.size()) return 0;
        return Dupe::g_weapons[listIdx].aux;
    }

    // ================================================================ aplicar
    static void ApplyAvailable() {
        if (!g_arr || g_arrLen <= 0 || !g_offAvailable) return;
        for (int i = 0; i < g_arrLen; ++i) {
            void* gun = ArrElem((Il2CppArray*)g_arr, i);
            if (!gun) continue;
            if (SafeRead<int>((uintptr_t)gun + g_offAvailable, 0) != 1)
                SafeWrite<int>((uintptr_t)gun + g_offAvailable, 1);
        }
    }

    static void ApplyAttachments() {
        void* fv = FpsGunView();
        if (!fv) return;
        const uint32_t slotOff[3] = { kFvSights, kFvAcc, kFvBarrels };
        for (int s = 0; s < 3; ++s) {
            int idx = C.selAttach[s];
            if (idx < 0) continue;
            int val = Dupe::AttachValue(s, idx);
            if (val < 0) continue;
            SafeWrite<int>((uintptr_t)fv + slotOff[s], val);
        }
    }

    static int  g_moneyOff[4] = { 0, 0, 0, 0 }; static int g_moneyN = 0;
    static int  g_rankOff[4] = { 0, 0, 0, 0 }; static int g_rankN = 0;
    static bool g_playerIntsDone = false;

    static void ResolvePlayerInts() {
        if (g_playerIntsDone) return;
        g_playerIntsDone = true;
        if (!SDK::kPlayerScript || !SDK::g_localPlayer) return;
        static const char* moneyKeys[] = { "money", "cash", "coins", "credits" };
        static const char* rankKeys[] = { "rank", "level", "xp", "experience" };
        auto addOff = [](int* arr, int& n, uint32_t off) {
            for (int i = 0; i < n; ++i) if (arr[i] == (int)off) return;
            if (n < 4) arr[n++] = (int)off;
            };
        for (Il2CppClass* c = SDK::kPlayerScript; c; c = il2cpp_class_get_parent(c)) {
            void* it = nullptr;
            for (;;) {
                FieldInfo* f = il2cpp_class_get_fields(c, &it);
                if (!f) break;
                if (IsStatic(f)) continue;
                const Il2CppType* t = il2cpp_field_get_type(f);
                const char* tn = (t && il2cpp_type_get_name) ? il2cpp_type_get_name(t) : nullptr;
                if (!tn || (!strstr(tn, "Int32") && !strstr(tn, "Int64"))) continue;
                const char* fn = il2cpp_field_get_name(f);
                if (!fn) continue;
                uint32_t off = (uint32_t)il2cpp_field_get_offset(f);
                if (!off) continue;
                for (int q = 0; q < 4; ++q) if (_stricmp(fn, moneyKeys[q]) == 0) addOff(g_moneyOff, g_moneyN, off);
                for (int q = 0; q < 4; ++q) if (_stricmp(fn, rankKeys[q]) == 0) addOff(g_rankOff, g_rankN, off);
            }
        }
    }

    static void ApplyPlayerInts() {
        if (!SDK::g_localPlayer) return;
        if (!g_playerIntsDone) ResolvePlayerInts();
        uintptr_t p = (uintptr_t)SDK::g_localPlayer;
        if (C.unlockCurrency)
            for (int i = 0; i < g_moneyN; ++i) SafeWrite<int>(p + g_moneyOff[i], C.currencyValue);
        if (C.unlockLevel)
            for (int i = 0; i < g_rankN; ++i) SafeWrite<int>(p + g_rankOff[i], C.unlockLevel);
    }

    static MethodInfo* g_mEquip = nullptr;
    static bool        g_equipResolved = false;
    static int         g_lastEquip = -2;

    static void ResolveEquipMethod() {
        if (g_equipResolved) return;
        g_equipResolved = true;
        if (!SDK::kPlayerScript) return;
        static const char* names[] = {
            "ServerSetWeapon", "RpcSetWeapon", "SetWeaponType", "SetWeapon",
            "EquipWeapon", "ChangeWeapon", "SetCurrentWeapon", "CmdSetWeapon",
            "ServerChangeWeapon", "SelectWeapon", "SetLoadoutWeapon", "SetGun",
            "ServerSetLoadout", "ApplyLoadout", "SetClassWeapon", "SetPrimary",
            "SetSecondary", "SetGadget", "Equip", "SetIndex", "SetWeaponIndex"
        };
        for (size_t i = 0; i < sizeof(names) / sizeof(names[0]) && !g_mEquip; ++i) {
            for (Il2CppClass* c = SDK::kPlayerScript; c && !g_mEquip; c = il2cpp_class_get_parent(c)) {
                for (int argc = 1; argc <= 2 && !g_mEquip; ++argc) {
                    MethodInfo* m = il2cpp_class_get_method_from_name(c, names[i], argc);
                    if (m) { g_mEquip = m; Log("[unlock] equip: %s argc=%d", names[i], argc); }
                }
            }
        }
    }

    static void CallEquip(int weaponIdx) {
        if (!g_mEquip || !SDK::g_localPlayer) return;
        union { int i; long long l; } v;
        v.l = 0; v.i = weaponIdx;
        void* args[2] = { &v, nullptr };
        InvokeSafe(g_mEquip, SDK::g_localPlayer, args);
    }

    // =====================================================================
    //  v24 — PlayerPrefs
    // =====================================================================
    typedef void* (*fn_dom_get)();
    typedef const void** (*fn_dom_asms)(void*, size_t*);
    typedef void* (*fn_asm_image)(const void*);
    typedef size_t(*fn_img_cnt2)(const void*);
    typedef Il2CppClass* (*fn_img_cls2)(const void*, size_t);
    typedef void* (*fn_str_new)(const char*);

    static fn_dom_get   p_domGet = nullptr;
    static fn_dom_asms  p_domAsms = nullptr;
    static fn_asm_image p_asmImage = nullptr;
    static fn_img_cnt2  p_imgCnt = nullptr;
    static fn_img_cls2  p_imgCls = nullptr;
    static fn_str_new   p_strNew = nullptr;

    static Il2CppClass* g_ppCls = nullptr;
    static MethodInfo* g_ppSetInt = nullptr;
    static MethodInfo* g_ppGetInt = nullptr;
    static MethodInfo* g_ppSetStr = nullptr;
    static MethodInfo* g_ppGetStr = nullptr;
    static MethodInfo* g_ppHasKey = nullptr;
    static MethodInfo* g_ppSave = nullptr;
    static bool         g_ppTried = false;
    static bool         g_ppOk = false;
    static bool         g_ppProbed = false;
    static int          g_ppRetry = 0;

    static void* InvokeRet(MethodInfo* m, void* obj, void** args) {
        void* r = nullptr;
        if (!m) return nullptr;
        __try { r = il2cpp_runtime_invoke(m, obj, args, nullptr); }
        __except (EXCEPTION_EXECUTE_HANDLER) { r = nullptr; }
        return r;
    }

    static void ResolvePrefsProcs() {
        static bool done = false;
        if (done) return;
        done = true;
        HMODULE ga = GetModuleHandleA("GameAssembly.dll");
        if (!ga) return;
        p_domGet = (fn_dom_get)GetProcAddress(ga, "il2cpp_domain_get");
        p_domAsms = (fn_dom_asms)GetProcAddress(ga, "il2cpp_domain_get_assemblies");
        p_asmImage = (fn_asm_image)GetProcAddress(ga, "il2cpp_assembly_get_image");
        p_imgCnt = (fn_img_cnt2)GetProcAddress(ga, "il2cpp_image_get_class_count");
        p_imgCls = (fn_img_cls2)GetProcAddress(ga, "il2cpp_image_get_class");
        p_strNew = (fn_str_new)GetProcAddress(ga, "il2cpp_string_new");
    }

    static bool ResolvePlayerPrefs() {
        if (g_ppTried) return g_ppOk;
        g_ppTried = true;
        ResolvePrefsProcs();
        if (!p_domGet || !p_domAsms || !p_asmImage || !p_imgCnt || !p_imgCls) {
            Log("[p] procs de dominio ausentes");
            return false;
        }
        void* dom = p_domGet();
        if (!dom) return false;
        size_t n = 0;
        const void** asms = p_domAsms(dom, &n);
        if (!asms) return false;
        for (size_t i = 0; i < n && !g_ppCls; ++i) {
            void* img = asms[i] ? p_asmImage(asms[i]) : nullptr;
            if (!img) continue;
            size_t c = p_imgCnt(img);
            if (c > 80000) c = 80000;
            for (size_t j = 0; j < c; ++j) {
                Il2CppClass* k = p_imgCls(img, j);
                if (!k) continue;
                const char* cn = il2cpp_class_get_name(k);
                if (cn && strcmp(cn, "PlayerPrefs") == 0) { g_ppCls = k; break; }
            }
        }
        if (!g_ppCls) { Log("[p] PlayerPrefs: NAO achado"); return false; }
        g_ppSetInt = il2cpp_class_get_method_from_name(g_ppCls, "SetInt", 2);
        g_ppGetInt = il2cpp_class_get_method_from_name(g_ppCls, "GetInt", 2);
        g_ppSetStr = il2cpp_class_get_method_from_name(g_ppCls, "SetString", 2);
        g_ppGetStr = il2cpp_class_get_method_from_name(g_ppCls, "GetString", 2);
        g_ppHasKey = il2cpp_class_get_method_from_name(g_ppCls, "HasKey", 1);
        g_ppSave = il2cpp_class_get_method_from_name(g_ppCls, "Save", 0);
        g_ppOk = (g_ppSetInt && g_ppGetInt && g_ppGetStr);
        Log("[p] PlayerPrefs=%p setInt=%p getInt=%p setStr=%p getStr=%p hasKey=%p save=%p -> %s",
            (void*)g_ppCls, (void*)g_ppSetInt, (void*)g_ppGetInt, (void*)g_ppSetStr,
            (void*)g_ppGetStr, (void*)g_ppHasKey, (void*)g_ppSave, g_ppOk ? "OK" : "FALHOU");
        return g_ppOk;
    }

    struct PpKey { char field[72]; char key[96]; void* str; };
    static PpKey g_ppK[48];
    static int   g_ppKN = 0;
    static bool  g_ppKeysDone = false;

    static bool PrefWord(const char* low) {
        return strstr(low, "weapon") || strstr(low, "loadout") || strstr(low, "slot") ||
            strstr(low, "primary") || strstr(low, "secondary") || strstr(low, "gadget") ||
            strstr(low, "gun") || strstr(low, "index") || strstr(low, "camo") ||
            strstr(low, "skin") || strstr(low, "class") || strstr(low, "type");
    }

    static void PpAdd(void* s, const char* field, const char* val) {
        if (g_ppKN >= 48 || !val || !val[0]) return;
        for (int q = 0; q < g_ppKN; ++q) if (!strcmp(g_ppK[q].key, val)) return;
        strncpy_s(g_ppK[g_ppKN].field, field ? field : "?", _TRUNCATE);
        strncpy_s(g_ppK[g_ppKN].key, val, _TRUNCATE);
        g_ppK[g_ppKN].str = s;
        ++g_ppKN;
    }

    static void CollectPrefsKeys(bool force) {
        if (g_ppKeysDone && !force) return;
        g_ppKeysDone = true;
        g_ppKN = 0;
        if (!ResolvePlayerPrefs()) return;

        const ULONGLONG t0 = GetTickCount64();
        int logged = 0;
        if (p_domGet && p_domAsms && p_asmImage && p_imgCnt && p_imgCls) {
            void* dom = p_domGet();
            size_t nAsm = 0;
            const void** asms = dom ? p_domAsms(dom, &nAsm) : nullptr;
            for (size_t a = 0; asms && a < nAsm; ++a) {
                void* img = asms[a] ? p_asmImage(asms[a]) : nullptr;
                if (!img) continue;
                size_t nc = p_imgCnt(img);
                if (nc > 80000) nc = 80000;
                for (size_t i = 0; i < nc; ++i) {
                    Il2CppClass* k = p_imgCls(img, i);
                    if (!k) continue;
                    for (Il2CppClass* c = k; c; c = il2cpp_class_get_parent(c)) {
                        void* it = nullptr;
                        for (;;) {
                            FieldInfo* f = il2cpp_class_get_fields(c, &it);
                            if (!f) break;
                            if (!IsStatic(f)) continue;
                            const Il2CppType* t = il2cpp_field_get_type(f);
                            if (!t) continue;
                            const char* tn = il2cpp_type_get_name ? il2cpp_type_get_name(t) : nullptr;
                            if (!tn || !strstr(tn, "String")) continue;
                            const char* fn = il2cpp_field_get_name(f);
                            if (!fn) continue;
                            char fl[80]; Lower(fl, sizeof(fl), fn);
                            void* s = nullptr; StaticGetSafe(f, &s);
                            if (!LooksLikeObj(s)) continue;
                            char v[96]; v[0] = 0; CStr(s, v, sizeof(v));
                            if (!v[0] || strlen(v) > 80) continue;
                            char vl[96]; Lower(vl, sizeof(vl), v);
                            const bool nameHit = strstr(fl, "pref") || strstr(fl, "key");
                            if (!nameHit && !PrefWord(vl)) continue;
                            if (logged < 60) {
                                Log("[p] chave %s::%s = \"%s\"", il2cpp_class_get_name(k), fn, v);
                                ++logged;
                            }
                            PpAdd(s, fn, v);
                        }
                    }
                }
            }
        }

        static const char* lit[] = {
            "PrefsKey_WeaponToShowOnLoad", "WeaponToShowOnLoad", "Loadout",
            "Loadout0", "Loadout1", "Loadout2", "LoadoutALFA", "LoadoutBRAVO",
            "LoadoutCHARLIE", "PrimaryWeapon", "SecondaryWeapon", "GadgetWeapon",
            "SelectedWeapon", "WeaponIndex"
        };
        if (p_strNew && g_ppHasKey) {
            for (size_t i = 0; i < sizeof(lit) / sizeof(lit[0]); ++i) {
                void* ks = p_strNew(lit[i]);
                if (!ks) continue;
                void* ar[1] = { ks };
                void* r = InvokeRet(g_ppHasKey, nullptr, ar);
                if (r) { Log("[p] LITERAL existe: \"%s\"", lit[i]); PpAdd(ks, lit[i], lit[i]); }
            }
        }
        Log("[p] chaves de prefs uteis: %d (scan %llu ms)", g_ppKN, GetTickCount64() - t0);
    }

    static void PrefsApply(int weaponType, void* nameStr) {
        if (!ResolvePlayerPrefs()) return;
        if (g_ppKN <= 0 && g_ppRetry < 3) { ++g_ppRetry; CollectPrefsKeys(true); }
        if (g_ppKN <= 0) return;

        if (!g_ppProbed) {
            g_ppProbed = true;
            for (int i = 0; i < g_ppKN; ++i) {
                int iv = -777777; void* a1[2] = { g_ppK[i].str, &iv };
                if (g_ppGetInt) InvokeRet(g_ppGetInt, nullptr, a1);
                void* d = nullptr; void* a2[2] = { g_ppK[i].str, &d };
                void* sv = g_ppGetStr ? InvokeRet(g_ppGetStr, nullptr, a2) : nullptr;
                char sb[80]; sb[0] = 0; if (sv) CStr(sv, sb, sizeof(sb));
                Log("[p] PROBE \"%s\": int=%s str=\"%s\"", g_ppK[i].key,
                    (iv == -777777) ? "-" : "SIM", sb);
            }
        }

        bool changed = false;
        for (int i = 0; i < g_ppKN; ++i) {
            char kl[96]; Lower(kl, sizeof(kl), g_ppK[i].key);
            if (!PrefWord(kl)) continue;

            int before = -777777;
            if (g_ppGetInt) { void* a[2] = { g_ppK[i].str, &before }; InvokeRet(g_ppGetInt, nullptr, a); }

            if (before != -777777) {
                if (before != weaponType) {
                    int v = weaponType; void* a[2] = { g_ppK[i].str, &v };
                    InvokeRet(g_ppSetInt, nullptr, a);
                    Log("[p] SETINT \"%s\" = %d (era %d)", g_ppK[i].key, weaponType, before);
                    changed = true;
                }
                continue;
            }

            if (nameStr && g_ppSetStr) {
                void* d = nullptr; void* a[2] = { g_ppK[i].str, &d };
                void* cur = g_ppGetStr ? InvokeRet(g_ppGetStr, nullptr, a) : nullptr;
                char cb[80]; cb[0] = 0; if (cur) CStr(cur, cb, sizeof(cb));
                const bool simples = (!cb[0]) || (strlen(cb) < 24 && !strpbrk(cb, ";|,"));
                if (simples) {
                    char nb[64]; nb[0] = 0; CStr(nameStr, nb, sizeof(nb));
                    if (nb[0] && _stricmp(cb, nb) != 0) {
                        void* a2[2] = { g_ppK[i].str, nameStr };
                        InvokeRet(g_ppSetStr, nullptr, a2);
                        Log("[p] SETSTR \"%s\" = \"%s\" (era \"%s\")", g_ppK[i].key, nb, cb);
                        changed = true;
                    }
                }
            }
        }
        if (changed && g_ppSave) { void* a[1] = { nullptr }; InvokeRet(g_ppSave, nullptr, a); Log("[p] Save()"); }
    }

    // =========================================================================
    //  >>> v43 — prefs de dinheiro + tick da carteira
    // =========================================================================
    void MoneyWritePrefs() {
        if (!ResolvePlayerPrefs()) return;
        if (g_ppKN <= 0) CollectPrefsKeys(true);
        if (g_ppKN <= 0 || !g_ppSetInt) return;
        static const char* keys[] = { "gold", "money", "credit", "cash", "coin", "case", "caixa" };
        int n = 0;
        for (int i = 0; i < g_ppKN; ++i) {
            char kl[96]; Lower(kl, sizeof(kl), g_ppK[i].key);
            bool hit = false;
            for (size_t q = 0; q < sizeof(keys) / sizeof(keys[0]) && !hit; ++q)
                if (strstr(kl, keys[q])) hit = true;
            if (!hit) continue;
            int v = (int)MnyCfgValue();
            void* a[2] = { g_ppK[i].str, &v };
            InvokeRet(g_ppSetInt, nullptr, a);
            ++n;
        }
        if (n && g_ppSave) { void* a[1] = { nullptr }; InvokeRet(g_ppSave, nullptr, a); }
        Log("[mirror] prefs: %d chave(s) escritas", n);
    }

    static ULONGLONG g_mnyTickNext = 0;

    static void MnyApplyLocks() {
        const long long cred = MnyCfgValue();
        const long long gold = (C.accGold > 0) ? (long long)C.accGold : cred;
        const long long cases = (C.accCases > 0) ? (long long)C.accCases : 500LL;

        for (int i = 0; i < g_mnyN; ++i) {
            MnyMirror& m = g_mny[i];
            if (!m.ok || !m.lock) continue;
            long long v = cred;
            if (strstr(m.cls, "CamoCase")) v = cases;
            else if (m.kind == 0 && strstr(m.label, "gold")) v = gold;
            MnyWriteOne(m, v);
        }
        if (!C.moneyOnlyWallet) {
            MoneyLockByName("CamoCaseManager", "goldCases", cases);
            MoneyLockByName("LoadoutAndShop", "goldAmount", gold);
        }
    }

    void MoneyTick() {
        if (C.moneyRescan) { C.moneyRescan = false; MoneyRefresh(); }
        if (C.moneyApplyNow) {
            C.moneyApplyNow = false;
            ForceMirrors(MnyCfgValue(), 0);
            if (C.moneyWritePrefs) MoneyWritePrefs();
        }
        if (!C.accAuto) return;
        const ULONGLONG now = GetTickCount64();
        if (now < g_mnyTickNext) return;
        g_mnyTickNext = now + (ULONGLONG)((C.moneyIntervalMs > 0) ? C.moneyIntervalMs : 1000);
        MnyApplyLocks();
        if (C.moneyWritePrefs) MoneyWritePrefs();
    }

    // =========================================================================
    static int       g_selTarget = -2;
    static int       g_selStep = -1;
    static ULONGLONG g_selNext = 0;

    static void LogLoadoutReadback() {
        if (!SDK::g_localPlayer) return;
        void* fv = FpsGunView();
        Log("[r] prim=%d sec=%d gadget=%d | fv38=%p fv48=%d",
            SafeRead<unsigned char>((uintptr_t)SDK::g_localPlayer + kLoPrim, 255),
            SafeRead<unsigned char>((uintptr_t)SDK::g_localPlayer + kLoSec, 255),
            SafeRead<int>((uintptr_t)SDK::g_localPlayer + kLoGad, -1),
            fv ? SafeRead<void*>((uintptr_t)fv + kFvHeld, nullptr) : nullptr,
            fv ? SafeRead<int>((uintptr_t)fv + kFvSlot, -1) : -1);
        for (int i = 0; i < g_loCN; ++i) {
            const LoCand& c = g_loC[i];
            if (c.kind != 1 || c.type != 3) continue;
            void* a = nullptr;
            StaticGetSafe(c.f, &a);
            if (!LooksLikeObj(a)) continue;
            const int n = ArrLen((Il2CppArray*)a);
            if (n < 1) continue;
            Log("[r] %s::%s len=%d = [%d, %d, %d]", c.cls, c.name, n,
                SafeRead<int>((uintptr_t)a + 0x20, -1),
                n > 1 ? SafeRead<int>((uintptr_t)a + 0x24, -1) : -1,
                n > 2 ? SafeRead<int>((uintptr_t)a + 0x28, -1) : -1);
        }
    }

    static void ApplySelection() {
        if (!C.applySelection || !SDK::g_localPlayer) return;
        if (!g_arr || g_arrLen <= 0) return;
        if (!g_dupeEnsured) { Dupe::EnsureLoaded(); g_dupeEnsured = true; }

        const int listIdx = C.selWeapon;
        if (listIdx < 0 || listIdx >= Dupe::WeaponCount()) return;

        int slot = C.loadoutSlot;
        if (slot < 0) slot = 0;
        if (slot > 2) slot = 2;

        if (!g_loDone) ResolveLoadoutFields();

        const int total = g_loCN + 2;
        const ULONGLONG now = GetTickCount64();
        if (listIdx != g_selTarget || now >= g_selNext) {
            if (listIdx != g_selTarget)
                Log("[unlock] INICIO: lista=%d nome=\"%s\" slot=%d passos=%d",
                    listIdx, Dupe::WeaponName(listIdx), slot, total);
            g_selTarget = listIdx;
            g_selStep = 0;
            g_selNext = now + 4000;
        }
        if (g_selStep < 0) return;

        const int live = LiveWeaponIndex(listIdx);
        const int wtype = WeaponTypeOfList(listIdx);
        if (live < 0 || live >= g_arrLen) return;

        void* nameStr = nullptr;
        void* gun = ArrElem((Il2CppArray*)g_arr, live);
        if (LooksLikeObj(gun)) nameStr = SafeRead<void*>((uintptr_t)gun + g_offName, nullptr);

        if (g_selStep < g_loCN) {
            const LoCand& c = g_loC[g_selStep];
            Log("[w] passo %d/%d: %s::%s (kind=%d type=%d useType=%d slot=%d off=0x%X)",
                g_selStep, total, c.cls, c.name, c.kind, c.type, c.useType, c.slot, c.off);
            LoWriteCand(c, slot, live, wtype, nameStr);
            ++g_selStep;
            return;
        }

        if (g_selStep == g_loCN) {
            Log("[w] passo %d/%d: PlayerScript offsets 0x144/0x145/0x10A4", g_selStep, total);
            uintptr_t b = (uintptr_t)SDK::g_localPlayer;
            if (slot == 0)      SafeWrite<unsigned char>(b + kLoPrim, (unsigned char)(live & 0xFF));
            else if (slot == 1) SafeWrite<unsigned char>(b + kLoSec, (unsigned char)(live & 0xFF));
            else                SafeWrite<int>(b + kLoGad, live);
            ++g_selStep;
            return;
        }

        if (g_selStep == g_loCN + 1) {
            Log("[w] passo %d/%d: fpsGunView+0x38=gun +0x48=%d", g_selStep, total, slot);
            void* fv = FpsGunView();
            if (fv && LooksLikeObj(gun)) {
                SafeWrite<void*>((uintptr_t)fv + kFvHeld, gun);
                SafeWrite<int>((uintptr_t)fv + kFvSlot, slot);
            }
            ++g_selStep;
            return;
        }

        Log("[w] passo %d/%d: equip+prefs (fim)", g_selStep, total);
        if (g_lastEquip != live) { g_lastEquip = live; CallEquip(live); }
        PrefsApply(wtype, nameStr);
        LogLoadoutReadback();
        g_selStep = -1;
    }

    static void ApplyLoadout() {
        if (!C.fillLoadoutSlots && !C.applySlots) return;
        if (!SDK::g_localPlayer || !g_arr || g_arrLen <= 0) return;
        if (!g_dupeEnsured) { Dupe::EnsureLoaded(); g_dupeEnsured = true; }
        for (int s = 0; s < 3; ++s) {
            int idx = C.slotWeapon[s];
            if (idx < 0 || idx >= Dupe::WeaponCount()) continue;
            LoadoutWrite(s, LiveWeaponIndex(idx), WeaponTypeOfList(idx));
        }
    }

    static void ApplyAll() {
        if (!g_arr || g_arrLen <= 0) return;
        if (C.unlockAll || C.unlockWeapons) ApplyAvailable();
        if (C.unlockAll || C.unlockSkins || C.unlockAttachments || C.skinChanger) ApplyAttachments();
        if (C.unlockAll || C.unlockCurrency || C.unlockLevel) ApplyPlayerInts();
        ApplyLoadout();
        if (C.applySelection) ApplySelection();
    }

    // ================================================================ coleta
    static void CollectWeapons() {
        std::vector<Dupe::Item> tmp;
        if (g_arr && g_arrLen > 0) {
            tmp.reserve((size_t)g_arrLen);
            for (int i = 0; i < g_arrLen; ++i) {
                void* gun = ArrElem((Il2CppArray*)g_arr, i);
                char nm[128]; nm[0] = 0;
                int wt = 0;
                if (gun) {
                    CStr(SafeRead<void*>((uintptr_t)gun + g_offName, nullptr), nm, sizeof(nm));
                    wt = SafeRead<int>((uintptr_t)gun + 0x1C, 0);
                }
                tmp.push_back(MkItem(i, nm[0] ? std::string(nm) : ("Arma " + std::to_string(i)), wt));
            }
        }
        if (tmp.empty()) { Dupe::SetStatus("sem armas - entre numa partida"); return; }
        Log("[dupe] armas: %d", (int)tmp.size());
        Dupe::SwapIn(tmp, Dupe::g_weapons, Dupe::g_wpnStr, Dupe::g_wpnPtr);
    }

    static void CollectAttachs() {
        void* gun0 = (g_arr && g_arrLen > 0) ? ArrElem((Il2CppArray*)g_arr, 0) : nullptr;
        const uint32_t offs[3] = { g_offSights, g_offAcc, g_offBarrels };
        for (int s = 0; s < 3; ++s) {
            std::vector<Dupe::Item> tmp;
            void* list = (gun0 && offs[s]) ? SafeRead<void*>((uintptr_t)gun0 + offs[s], nullptr) : nullptr;
            int cnt = LooksLikeObj(list) ? ListSize(list) : 0;
            for (int i = 0; i < cnt && i < 128; ++i) {
                int val = ListElemInt(list, i);
                if (val < 0) continue;
                char nm[96]; sprintf_s(nm, sizeof(nm), "%s %d", kSlotNames[s], i);
                tmp.push_back(MkItem(val, nm));
            }
            const int n = (int)tmp.size();
            Dupe::SwapIn(tmp, Dupe::g_attach[s], Dupe::g_atStr[s], Dupe::g_atPtr[s]);
            Log("[dupe] anexos %s: %d", kSlotNames[s], n);
        }
    }

    static void CollectAllCamosDict(const char* field, const char* prefix,
        std::vector<Dupe::Item>& out) {
        Il2CppClass* k = SDK::FindClass("AllCamos");
        if (!k) return;
        FieldInfo* f = il2cpp_class_get_field_from_name(k, field);
        if (!f) return;
        void* dict = nullptr;
        StaticGetSafe(f, &dict);
        if (!LooksLikeObj(dict)) return;
        static DictEnt ents[512];
        int n = ReadDict(dict, ents, 512);
        Log("[dupe] AllCamos.%s: %d entradas", field, n);
        for (int i = 0; i < n; ++i) {
            void* camo = ents[i].val;
            char nm[128]; nm[0] = 0;
            if (LooksLikeObj(camo))
                CStr(SafeRead<void*>((uintptr_t)camo + 0x58, nullptr), nm, sizeof(nm));
            char line[160];
            if (nm[0]) sprintf_s(line, sizeof(line), "%s", nm);
            else       sprintf_s(line, sizeof(line), "%s %d", prefix, ents[i].key);
            out.push_back(MkItem(ents[i].key, line));
        }
    }

    static void CollectStringsFromClasses(const char* key,
        std::vector<std::string>& out, const char* tag) {
        if (!p_classImage || !p_imgCount || !p_imgClass || !SDK::kPlayerScript) return;
        void* img = p_classImage(SDK::kPlayerScript);
        if (!img) return;
        size_t n = p_imgCount(img);
        if (n > 60000) n = 60000;
        for (size_t i = 0; i < n; ++i) {
            Il2CppClass* k = p_imgClass(img, i);
            if (!k) continue;
            const char* cn = il2cpp_class_get_name(k);
            if (!cn || !strstr(cn, key)) continue;
            for (Il2CppClass* c = k; c; c = il2cpp_class_get_parent(c)) {
                void* it = nullptr;
                for (;;) {
                    FieldInfo* f = il2cpp_class_get_fields(c, &it);
                    if (!f) break;
                    if (!IsStatic(f)) continue;
                    const Il2CppType* t = il2cpp_field_get_type(f);
                    const char* tn = (t && il2cpp_type_get_name) ? il2cpp_type_get_name(t) : nullptr;
                    if (!tn || !strstr(tn, "String")) continue;
                    if (!strstr(tn, "[]") && !strstr(tn, "List<")) continue;
                    void* v = nullptr;
                    StaticGetSafe(f, &v);
                    if (!LooksLikeObj(v)) continue;
                    bool isArr = (strstr(tn, "[]") != nullptr);
                    int cnt = isArr ? ArrLen((Il2CppArray*)v) : ListSize(v);
                    for (int j = 0; j < cnt && j < 512; ++j) {
                        void* e = isArr ? ArrElem((Il2CppArray*)v, j)
                            : ArrElem((Il2CppArray*)ListItems(v), j);
                        char s[128]; s[0] = 0;
                        CStr(e, s, sizeof(s));
                        if (!s[0]) continue;
                        bool dup = false;
                        for (size_t q = 0; q < out.size(); ++q) if (out[q] == s) { dup = true; break; }
                        if (!dup) out.push_back(s);
                    }
                }
            }
        }
        Log("[dupe] %s: %d strings unicas", tag, (int)out.size());
    }

    static void CollectCamos() {
        std::vector<Dupe::Item> tmp;
        CollectAllCamosDict("weaponCamos", "Camo", tmp);
        if (tmp.empty()) CollectAllCamosDict("allCamos", "Camo", tmp);
        if (tmp.empty()) {
            std::vector<std::string> ss;
            CollectStringsFromClasses("CamoDataScriptableObject", ss, "camos");
            for (size_t i = 0; i < ss.size(); ++i) tmp.push_back(MkItem((int)i, ss[i]));
        }
        if (tmp.empty()) for (int i = 0; i < 32; ++i) tmp.push_back(MkItem(i, "Camo " + std::to_string(i)));
        const int n = (int)tmp.size();
        Dupe::SwapIn(tmp, Dupe::g_camos, Dupe::g_camoStr, Dupe::g_camoPtr);
        Log("[dupe] camos: %d", n);
    }

    static void CollectChars() {
        std::vector<Dupe::Item> tmp;
        CollectAllCamosDict("characterCamos", "Personagem", tmp);
        if (tmp.empty()) CollectAllCamosDict("charCamos", "Personagem", tmp);
        if (tmp.empty()) {
            for (int i = 0; i < 8; ++i) {
                char nm[64]; sprintf_s(nm, sizeof(nm), "Personagem %d", i);
                tmp.push_back(MkItem(i, nm));
            }
        }
        const int n = (int)tmp.size();
        Dupe::SwapIn(tmp, Dupe::g_chars, Dupe::g_charStr, Dupe::g_charPtr);
        Log("[dupe] personagens: %d", n);
    }

    static void BuildStepInner() {
        const int st = Dupe::Stage();
        Log("[dupe] etapa %d", st);
        switch (st) {
        case 1: CollectWeapons(); Dupe::SetStage(2); break;
        case 2: CollectAttachs(); Dupe::SetStage(3); break;
        case 3: CollectCamos();   Dupe::SetStage(4); break;
        case 4: CollectChars();   Dupe::SetStage(5); break;
        case 5:
            if (Dupe::Save()) Dupe::SetStatus("salvo: %d armas, %d camos, %d chars",
                (int)Dupe::g_weapons.size(), (int)Dupe::g_camos.size(), (int)Dupe::g_chars.size());
            Dupe::SetStage(0);
            Dupe::g_wantBuild.store(false);
            break;
        default:
            Dupe::SetStage(0);
            Dupe::g_wantBuild.store(false);
            break;
        }
    }

    void BuildStep() {
        if (!Dupe::Wants()) return;
        if (Dupe::Building()) return;
        Dupe::g_building.store(true);
        __try { BuildStepInner(); }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            Dupe::SetStatus("erro na build");
            Dupe::SetStage(0);
            Dupe::g_wantBuild.store(false);
        }
        Dupe::g_building.store(false);
    }

    bool Install() {
        ResolveExtra();
        if (!SDK::g_localPlayer || !SDK::kPlayerScript || !SDK::kGunStats) return false;
        uint32_t off = 0; void* a = nullptr; int len = 0;
        if (!FindWeaponArray(SDK::g_localPlayer, off, a, len)) return false;
        g_offArr = off; g_arr = a; g_arrLen = len;
        ResolveOffsets();
        g_loDone = false;
        ResolveLoadoutFields();
        ResolveEquipMethod();
        ResolvePlayerPrefs();
        Dupe::EnsureLoaded();
        g_dupeEnsured = true;
        g_installed = true;
        Log("[unlock] install %s: arr@0x%X len=%d | armas=%d camos=%d chars=%d",
            BFI_UNLOCK_VER, g_offArr, g_arrLen,
            (int)Dupe::g_weapons.size(), (int)Dupe::g_camos.size(), (int)Dupe::g_chars.size());
        return true;
    }

    static bool EnsureInstalled() {
        if (g_installed && g_arr && g_arrLen > 0) return true;
        const ULONGLONG now = GetTickCount64();
        if (now < g_nextInstallTry) return false;
        g_nextInstallTry = now + 500;
        return Install();
    }

    static void TickInner() {
        if (!EnsureInstalled()) return;
        ApplyAll();
    }

    void Tick() {
        const bool anyMoney = C.accAuto || C.moneyApplyNow || C.moneyRescan || C.moneyWritePrefs;
        const bool any = C.unlockAll || C.unlockWeapons || C.unlockSkins || C.unlockAttachments ||
            C.unlockCurrency || C.unlockLevel || C.unlockChars || C.applySelection ||
            C.skinChanger || C.applySlots || C.fillLoadoutSlots || anyMoney;
        if (!any && !Dupe::Wants()) return;

        // carteira: vale ate' no menu, nao depende do array de armas
        __try { MoneyTick(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { Log("[unlock] AV contido no MoneyTick"); }

        if (!SDK::g_localPlayer) return;

        if (Dupe::Wants()) {
            if (!EnsureInstalled()) return;
            BuildStep();
            return;
        }
        __try { TickInner(); }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            static int n = 0;
            if (n++ < 10) Log("[unlock] AV contido no Tick");
        }
    }

    // =================================================================== dump
    static void DumpClassFields(const char* clsName) {
        Il2CppClass* k = SDK::FindClass(clsName);
        if (!k) { Log("[dump] classe '%s' NAO encontrada", clsName); return; }
        Log("[dump] ==== %s ====", clsName);
        for (Il2CppClass* c = k; c; c = il2cpp_class_get_parent(c)) {
            void* it = nullptr;
            for (;;) {
                FieldInfo* f = il2cpp_class_get_fields(c, &it);
                if (!f) break;
                const Il2CppType* t = il2cpp_field_get_type(f);
                const char* tn = (t && il2cpp_type_get_name) ? il2cpp_type_get_name(t) : "?";
                Log("[dump]   %s::%s %s @0x%X%s",
                    il2cpp_class_get_name(c), il2cpp_field_get_name(f), tn,
                    (unsigned)il2cpp_field_get_offset(f), IsStatic(f) ? " [static]" : "");
            }
        }
    }

    static void DumpObjFields(void* o, const char* tag, int maxFields) {
        if (!LooksLikeObj(o)) return;
        Il2CppClass* k = il2cpp_object_get_class((Il2CppObject*)o);
        int n = 0;
        for (Il2CppClass* c = k; c && n < maxFields; c = il2cpp_class_get_parent(c)) {
            void* it = nullptr;
            for (;;) {
                FieldInfo* f = il2cpp_class_get_fields(c, &it);
                if (!f) break;
                if (IsStatic(f)) continue;
                if (n++ >= maxFields) break;
                const Il2CppType* t = il2cpp_field_get_type(f);
                const char* tn = (t && il2cpp_type_get_name) ? il2cpp_type_get_name(t) : "?";
                uint32_t off = (uint32_t)il2cpp_field_get_offset(f);
                if (!off) continue;
                const char* fn = il2cpp_field_get_name(f);
                char tl[96]; Lower(tl, sizeof(tl), tn);
                if (strstr(tn, "String")) {
                    char s[64]; s[0] = 0;
                    CStr(SafeRead<void*>((uintptr_t)o + off, nullptr), s, sizeof(s));
                    Log("[lod]   %s.%s = \"%s\"", tag, fn, s);
                }
                else if (strstr(tn, "[]") || strstr(tn, "List<")) {
                    void* a = SafeRead<void*>((uintptr_t)o + off, nullptr);
                    Log("[lod]   %s.%s %s len=%d", tag, fn, tn, LooksLikeObj(a) ? ArrLen((Il2CppArray*)a) : -1);
                }
                else if (strstr(tl, "single") || strstr(tl, "double"))
                    Log("[lod]   %s.%s %s = %.4f", tag, fn, tn, SafeRead<float>((uintptr_t)o + off, -999.f));
                else if (strstr(tl, "boolean"))
                    Log("[lod]   %s.%s %s = %d", tag, fn, tn, SafeRead<unsigned char>((uintptr_t)o + off, 0));
                else if (strstr(tl, "int32") || strstr(tl, "byte"))
                    Log("[lod]   %s.%s %s = %d", tag, fn, tn, SafeRead<int>((uintptr_t)o + off, -999));
                else {
                    void* p = SafeRead<void*>((uintptr_t)o + off, nullptr);
                    if (LooksLikeObj(p)) Log("[lod]   %s.%s %s = OBJ %p", tag, fn, tn, p);
                    else                 Log("[lod]   %s.%s %s = raw %d", tag, fn, tn, SafeRead<int>((uintptr_t)o + off, -999));
                }
            }
        }
    }

    void DumpMirrors() {
        ResolveExtra();
        Log("=== ESPELHOS DA CARTEIRA (%d) ===", g_mnyN);
        MoneyRefresh();
        for (int i = 0; i < g_mnyN; ++i) {
            const MnyMirror& m = g_mny[i];
            if (!m.ok) { Log("[mirror]   [%d] %s = NAO RESOLVIDO", i, m.label); continue; }
            uintptr_t a = (uintptr_t)m.base + m.off;
            const int key = SafeRead<int>(a + 0, 0);
            const int hid = SafeRead<int>(a + 4, 0);
            Log("[mirror]   [%d] %s %s::%s kind=%d base=%p off=0x%X | int=%d obscured=%d inited=%d",
                i, m.label, m.cls, m.fld, m.kind, m.base, m.off,
                key, hid ^ key, SafeRead<int>(a + 0xC, -1));
        }
        Log("=== FIM ESPELHOS ===");
    }

    void DumpShop() {
        ResolveExtra();
        static const char* cls[] = {
            "CreditsManager", "AccountManager", "CamoCaseManager",
            "LoadoutManager", "LoadoutAndShop", "Sidebar", "BF_AccountInfo"
        };
        for (size_t i = 0; i < sizeof(cls) / sizeof(cls[0]); ++i) DumpClassFields(cls[i]);
        DumpMirrors();
    }

    void Dump() {
        Log("=== DUMP UNLOCK %s ===", BFI_UNLOCK_VER);
        ResolveExtra();
        DumpClassFields("PlayerScript");
        ScanLoadoutGlobals();
        ScanLoadoutSingletons();
        ScanLoadoutContainers();
        ResolvePlayerPrefs();
        CollectPrefsKeys(false);
        DumpMirrors();
        Log("=== FIM DUMP UNLOCK ===");
    }

    // ================================================================= API
    bool Ready() { return Dupe::WeaponCount() > 0; }
    int  Generation() { return (int)Dupe::g_epoch.load(); }

    int WeaponCount() { return Dupe::WeaponCount(); }
    const char* const* WeaponList() { return Dupe::WeaponList(); }
    const char* WeaponName(int idx) { return Dupe::WeaponName(idx); }

    int AttachSlotCount() { return 3; }
    const char* AttachSlotName(int slot) { return (slot >= 0 && slot < 3) ? kSlotNames[slot] : "?"; }
    int AttachCount(int slot) { return Dupe::AttachCount(slot); }
    const char* const* AttachList(int slot) { return Dupe::AttachList(slot); }
    int AttachValue(int slot, int idx) { return Dupe::AttachValue(slot, idx); }
    const char* AttachName(int slot, int idx) {
        const char* const* l = Dupe::AttachList(slot);
        if (!l || idx < 0 || idx >= Dupe::AttachCount(slot)) return nullptr;
        return l[idx];
    }

    int CamoCount() { return Dupe::CamoCount(); }
    const char* const* CamoList() { return Dupe::CamoList(); }
    const char* CamoName(int idx) {
        const char* const* l = Dupe::CamoList();
        if (!l || idx < 0 || idx >= Dupe::CamoCount()) return nullptr;
        return l[idx];
    }

    int CharCount() { return Dupe::CharCount(); }
    const char* const* CharList() { return Dupe::CharList(); }
    const char* CharName(int idx) {
        const char* const* l = Dupe::CharList();
        if (!l || idx < 0 || idx >= Dupe::CharCount()) return nullptr;
        return l[idx];
    }

} // namespace Unlock
// ==== FIM unlock.cpp ====