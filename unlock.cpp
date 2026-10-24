// ============================================================================
//  unlock.cpp — bfi (v43.22)
//
//  >>> v43.22  (PERF DO FRAME + REVALIDACAO DO ARRAY + LIMPEZA)
//    * PERF: ApplyAll() rodava TUDO a cada frame. ApplyAvailable() varria as
//      ~65 armas (2 acessos com checagem de pagina por arma) todo frame, e
//      ApplyAttachments/ApplyPlayerInts reescreviam os mesmos valores sem
//      parar. Agora: available 1x/2s, anexos 1x/250ms, ints do player 1x/250ms.
//      A passada e o Build forcam o available na hora (g_availNext = 0).
//    * PERF: ApplyAvailable() le os elementos pelo ponteiro base do array em
//      vez de ArrElem() (que refazia a validacao a cada elemento).
//    * FIX: RevalidateArray() — o g_arr cacheado nunca era revalidado. Se o
//      jogo realocasse o array, o cheat escrevia em memoria morta. Revalida a
//      cada 5s contra o offset do player.
//    * PERF/DIAG: perfil QPC por sub-fase do Tick, logado a cada 2s.
//    * LIXO: removidos InvokeSafe() (morto desde o v43.21) e os offsets
//      g_offCamoType/g_offTracerType/g_offGcs (resolvidos e nunca lidos).
//
//  >>> v43.21  (EQUIP ARG ERRADO + LOG DE EXCECAO + KILL SWITCHES)
//    * TryEquipSmart: manda o TIPO da arma (ex. 61) em vez do INDICE do array
//      (ex. 3) para SetGunAsPrimary/SetGunAsSecondary/SetAsGadget. Se lancar,
//      repete com o indice. Evidencia: LoadoutManager::sel1 == currentGunType
//      e GadgetSlotOptions/ONODAGDHALL guardam TIPOS, nao posicoes.
//    * InvokeEquipLogged: le a excecao gerenciada pendente (il2cpp_runtime_invoke
//      com o 4o parametro) e loga classe + mensagem + currentGunType/sel1
//      antes->depois. O InvokeSafe antigo passava nullptr e nunca via o NRE.
//    * Kill switches p/ bisect do freeze: offEquip / offIdx / offFv / offPrefs /
//      offCand + SetOff*().
//
//  >>> v43.20
//    * ApplySelection NAO reinicia mais a cada 4s (g_selSlot/g_selDone/
//      g_selForce/g_selForceEquip + RequestApply()).
//    * PlayerPrefs.Save() so quando ha mudanca real (g_prefsDirty + PrefsFlush).
//    * PrefStrictKey(): nao escreve mais nome de arma em roomType /
//      allowedweapons / isHoldingMinigun.
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

#ifndef BFI_CONFIG_VER
#error "config.h ANTIGO sendo incluido (sem BFI_CONFIG_VER). Substitua o config.h."
#elif BFI_CONFIG_VER < 4315
#error "config.h desatualizado (BFI_CONFIG_VER < 4315). Substitua o config.h."
#endif

#define BFI_UNLOCK_VER "v43.22"
namespace Unlock {

    static void* StaticInstanceOf(Il2CppClass* k);

    static void* g_arr = nullptr;
    static int    g_arrLen = 0;
    static uint32_t g_offArr = 0;

    static uint32_t g_offName = 0, g_offAvailable = 0;
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

    static const uint32_t kLmCurType = 0x290;
    static const uint32_t kLmSel1 = 0x2BC;
    static const uint32_t kLmSel2 = 0x2C0;
    static const uint32_t kLmSel3 = 0x2C4;
    static const uint32_t kLmActive = 0x2D0;

    static const uint32_t kAmJson = 0x90;

    static bool   g_installed = false;
    static const char* kSlotNames[3] = { "Mira (sights)", "Acessorios", "Cano (barrels)" };

    static ULONGLONG g_nextInstallTry = 0;
    static bool      g_dupeEnsured = false;

    // >>> v43.22: portoes de escrita per-frame
    static ULONGLONG g_arrNext = 0;
    static ULONGLONG g_availNext = 0;
    static ULONGLONG g_attachNext = 0;
    static ULONGLONG g_intsNext = 0;

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
    static bool HasWord(const char* lowHay, const char* lowNeedle) {
        return (lowHay && lowNeedle && lowNeedle[0] && strstr(lowHay, lowNeedle) != nullptr);
    }

    struct TypeNameCU {
        static const int SLOTS = 16384;
        const void* key[SLOTS];
        const char* val[SLOTS];
        TypeNameCU() { memset(key, 0, sizeof(key)); memset(val, 0, sizeof(val)); }
        const char* Get(const Il2CppType* t) {
            if (!t || !il2cpp_type_get_name) return "";
            size_t h = ((uintptr_t)t >> 4) & (SLOTS - 1);
            for (int i = 0; i < SLOTS; ++i) {
                size_t idx = (h + i) & (SLOTS - 1);
                if (key[idx] == (const void*)t) return val[idx];
                if (!key[idx]) {
                    const char* n = il2cpp_type_get_name(t);
                    key[idx] = (const void*)t;
                    val[idx] = n ? n : "";
                    return val[idx];
                }
            }
            return "";
        }
    };
    static TypeNameCU g_tn;
    static const char* TN(const Il2CppType* t) { return g_tn.Get(t); }

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
    static bool StaticGetRaw(FieldInfo* f, void* out, size_t n) {
        if (!f || !p_fieldStaticGet || !out) return false;
        unsigned char buf[256];
        memset(buf, 0, sizeof(buf));
        __try { p_fieldStaticGet(f, buf); }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        memcpy(out, buf, n > sizeof(buf) ? sizeof(buf) : n);
        return true;
    }
    static bool StaticGetInt(FieldInfo* f, int* out) {
        *out = 0;
        return StaticGetRaw(f, out, sizeof(int));
    }
    static void StaticSetRaw(FieldInfo* f, const void* in, size_t n) {
        if (!f || !p_fieldStaticSet || !in) return;
        unsigned char buf[256];
        memset(buf, 0, sizeof(buf));
        memcpy(buf, in, n > sizeof(buf) ? sizeof(buf) : n);
        __try { p_fieldStaticSet(f, buf); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
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
    //  >>> v43.22: perfil (QPC). Acha o custo real do Tick em vez de chutar.
    // =========================================================================
    static double QpcMs() {
        static LARGE_INTEGER f = {};
        if (!f.QuadPart) QueryPerformanceFrequency(&f);
        LARGE_INTEGER c; QueryPerformanceCounter(&c);
        return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
    }
    static double g_pfTick = 0, g_pfMoney = 0, g_pfInner = 0;
    static double g_pfAvail = 0, g_pfAtt = 0, g_pfInts = 0, g_pfSel = 0;
    static int    g_pfN = 0;
    static ULONGLONG g_pfNext = 0;

    static void PfLog() {
        const ULONGLONG now = GetTickCount64();
        if (now < g_pfNext) return;
        g_pfNext = now + 2000;
        if (!g_pfN) return;
        const double n = (double)g_pfN;
        Log("[prof] tick medio: total=%.2fms money=%.2f inner=%.2f | avail=%.2f attach=%.2f ints=%.2f sel=%.2f | n=%d",
            g_pfTick / n, g_pfMoney / n, g_pfInner / n,
            g_pfAvail / n, g_pfAtt / n, g_pfInts / n, g_pfSel / n, g_pfN);
        g_pfTick = g_pfMoney = g_pfInner = 0;
        g_pfAvail = g_pfAtt = g_pfInts = g_pfSel = 0;
        g_pfN = 0;
    }

    // =========================================================================
    //  CARTEIRA / ESPELHOS
    // =========================================================================
    struct MnyMirror {
        const char* cls;
        const char* fld;
        const char* fldKey;
        const char* hopFld;
        uint32_t    hopOff;
        const char* hopFld2;
        uint32_t    hopOff2;
        const char* hopFld3;
        uint32_t    hopOff3;
        const char* label;
        int         kind;
        bool        lock;
        bool        ok;
        bool        isStatic;
        bool        logged;
        void* base;
        FieldInfo* sf;
        uint32_t    off;
        const char* typeName;
    };

    static MnyMirror g_mny[] = {
        { "AccountManager",  "JBNJGEGJGAD", "money",  0,             0,    0,            0,    0,             0,    "Conta (acc.int34)",   0, false, false, false, false, 0, 0, 0 },
        { "AccountManager",  "FIOMIPONAAM", "money",  0,             0,    0,            0,    0,             0,    "Conta (acc.intA4)",   0, false, false, false, false, 0, 0, 0 },

        { "CreditsManager",  "LDOGMJMLIOP", "credit", 0,             0,    0,            0,    0,             0,    "Creditos (UI)",       1, true,  false, false, false, 0, 0, 0 },
        { "AccountManager",  "GEEFAPKBHJO", "gold",   0,             0,    0,            0,    0,             0,    "Conta (gold UI)",     1, true,  false, false, false, 0, 0, 0 },
        { "AccountManager",  "gold",        "gold",   "accountInfo", 0x58, "customInfo", 0x18, "bfAccountInfo", 0x10, "Conta (bf.gold)",     0, true,  false, false, false, 0, 0, 0 },
        { "AccountManager",  "money",       "money",  "accountInfo", 0x58, "customInfo", 0x18, "bfAccountInfo", 0x10, "Conta (bf.money)",    0, true,  false, false, false, 0, 0, 0 },
        { "AccountManager",  "xp",          "xp",     "accountInfo", 0x58, "customInfo", 0x18, "bfAccountInfo", 0x10, "Conta (bf.xp)",       0, false, false, false, false, 0, 0, 0 },
        { "AccountManager",  "cases",       "case",   "accountInfo", 0x58, "customInfo", 0x18, "bfAccountInfo", 0x10, "Conta (bf.cases)",    0, false, false, false, false, 0, 0, 0 },
        { "AccountManager",  "gold",        "gold",   "BMJJMJJIKPB", 0x98, "bfAccountInfo", 0x10, 0,             0,    "Conta (BM.gold)",     0, true,  false, false, false, 0, 0, 0 },
        { "AccountManager",  "money",       "money",  "BMJJMJJIKPB", 0x98, "bfAccountInfo", 0x10, 0,             0,    "Conta (BM.money)",    0, true,  false, false, false, 0, 0, 0 },
        { "CamoCaseManager", "goldCases",   "case",   0,             0,    0,            0,    0,             0,    "Caixas (gold)",       1, true,  false, false, false, 0, 0, 0 },
        { "LoadoutManager",  "goldAmount",  "gold",   0,             0,    0,            0,    0,             0,    "Loadout (gold)",      1, true,  false, false, false, 0, 0, 0 },
        { "LoadoutAndShop",  "goldAmount",  "gold",   "sidebar",     0x20, 0,            0,    0,             0,    "Loja (sidebar)",      0, true,  false, false, false, 0, 0, 0 },
    };
    static const int g_mnyN = (int)(sizeof(g_mny) / sizeof(g_mny[0]));

    static bool      g_mnyDone = false;
    static ULONGLONG g_mnyNext = 0;
    static int       g_mnyLastOk = -1;

    static bool MnyIn(int i) { return i >= 0 && i < g_mnyN; }

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

    static FieldInfo* MnyFindField(void* obj, const char* fld, const char* key, bool& isStaticOut) {
        isStaticOut = false;
        Il2CppClass* k = obj ? il2cpp_object_get_class((Il2CppObject*)obj) : nullptr;
        if (!k) return nullptr;

        for (Il2CppClass* c = k; c; c = il2cpp_class_get_parent(c)) {
            void* it = nullptr;
            for (;;) {
                FieldInfo* f = il2cpp_class_get_fields(c, &it);
                if (!f) break;
                if (IsStatic(f)) continue;
                const char* fn = il2cpp_field_get_name(f);
                if (!fn || _stricmp(fn, fld) != 0) continue;
                if (il2cpp_field_get_offset(f)) return f;
            }
        }
        for (Il2CppClass* c = k; c; c = il2cpp_class_get_parent(c)) {
            void* it = nullptr;
            for (;;) {
                FieldInfo* f = il2cpp_class_get_fields(c, &it);
                if (!f) break;
                if (!IsStatic(f)) continue;
                const char* fn = il2cpp_field_get_name(f);
                if (!fn || _stricmp(fn, fld) != 0) continue;
                isStaticOut = true;
                return f;
            }
        }
        if (key) {
            char kl[40]; Lower(kl, sizeof(kl), key);
            for (Il2CppClass* c = k; c; c = il2cpp_class_get_parent(c)) {
                void* it = nullptr;
                for (;;) {
                    FieldInfo* f = il2cpp_class_get_fields(c, &it);
                    if (!f) break;
                    const Il2CppType* t = il2cpp_field_get_type(f);
                    const char* tn = TN(t);
                    if (!tn) continue;
                    if (!strstr(tn, "Int32") && !strstr(tn, "ObscuredInt") && !strstr(tn, "Int64")) continue;
                    const char* fn = il2cpp_field_get_name(f);
                    if (!fn) continue;
                    char nl[80]; Lower(nl, sizeof(nl), fn);
                    if (!strstr(nl, kl)) continue;
                    if (!IsStatic(f) && !il2cpp_field_get_offset(f)) continue;
                    isStaticOut = IsStatic(f);
                    return f;
                }
            }
        }
        return nullptr;
    }

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
                if (n++ >= max) break;
                const Il2CppType* t = il2cpp_field_get_type(f);
                const char* tn = TN(t);
                Log("[mirror]   %s::%s %s @0x%X%s", il2cpp_class_get_name(c),
                    il2cpp_field_get_name(f), tn, (unsigned)il2cpp_field_get_offset(f),
                    IsStatic(f) ? " [static]" : "");
            }
        }
    }

    static void MnyResolve() {
        if (g_mnyDone) return;
        const ULONGLONG now = GetTickCount64();
        if (now < g_mnyNext) return;
        g_mnyNext = now + 2000;
        int ok = 0;
        for (int i = 0; i < g_mnyN; ++i) {
            MnyMirror& m = g_mny[i];
            m.ok = false; m.base = nullptr; m.off = 0; m.sf = nullptr;
            m.isStatic = false; m.typeName = nullptr;

            void* base = MnyStaticObj(m.cls, "singleton", "Instance");
            if (!base) base = MnyStaticObj(m.cls, "instance", "s_instance");
            if (!base) {
                if (!m.logged) { m.logged = true; Log("[mirror] %s: singleton ausente", m.label); }
                continue;
            }

            bool hopFail = false;
            for (int h = 0; h < 3 && !hopFail; ++h) {
                const char* hf = (h == 0) ? m.hopFld : (h == 1) ? m.hopFld2 : m.hopFld3;
                uint32_t hoff = (h == 0) ? m.hopOff : (h == 1) ? m.hopOff2 : m.hopOff3;
                if (!hf && !hoff) continue;
                uint32_t got = MnyOffExact(base, hf);
                if (!got) got = hoff;
                void* mid = got ? SafeRead<void*>((uintptr_t)base + got, nullptr) : nullptr;
                if (!LooksLikeObj(mid)) {
                    hopFail = true;
                    if (!m.logged) {
                        m.logged = true;
                        Log("[mirror] %s: salto%d '%s' falhou (off=0x%X, base=%p)",
                            m.label, h + 1, hf ? hf : "?", got, base);
                        MnyDumpObj(base, m.cls, 40);
                    }
                    break;
                }
                base = mid;
            }
            if (hopFail) continue;

            bool isStatic = false;
            FieldInfo* f = MnyFindField(base, m.fld, m.fldKey, isStatic);
            if (!f) {
                if (!m.logged) {
                    m.logged = true;
                    Log("[mirror] %s: campo '%s' nao achado", m.label, m.fld);
                    MnyDumpObj(base, m.label, 48);
                }
                continue;
            }

            const Il2CppType* ft = il2cpp_field_get_type(f);
            const char* ftn = ft ? TN(ft) : nullptr;
            m.typeName = (ftn && ftn[0]) ? ftn : "?";
            if (ftn) {
                if (strstr(ftn, "ObscuredInt"))      m.kind = 1;
                else if (strstr(ftn, "Int64"))       m.kind = 2;
                else if (strstr(ftn, "Int32"))       m.kind = 0;
                if (strstr(ftn, "String")) {
                    if (!m.logged) { m.logged = true; Log("[mirror] %s: campo e' String (%s)", m.label, ftn); }
                }
            }

            m.sf = f; m.isStatic = isStatic;
            if (!isStatic) { m.base = base; m.off = (uint32_t)il2cpp_field_get_offset(f); }
            m.ok = true;
            ++ok;
        }
        if (ok == g_mnyN) g_mnyDone = true;
        if (ok != g_mnyLastOk) {
            g_mnyLastOk = ok;
            Log("[mirror] %d/%d espelhos resolvidos", ok, g_mnyN);
            for (int i = 0; i < g_mnyN; ++i) {
                const MnyMirror& mm = g_mny[i];
                if (!mm.ok) continue;
                Log("[mirror]   [%d] %s -> %s::%s tipo=%s kind=%d%s",
                    i, mm.label, mm.cls, mm.fld, mm.typeName ? mm.typeName : "?",
                    mm.kind, mm.isStatic ? " [static]" : "");
            }
        }
    }

    static long long MnyRead(const MnyMirror& m) {
        if (!m.ok || !m.sf) return 0;
        if (m.kind == 2) {
            if (m.isStatic) { long long v = 0; StaticGetRaw(m.sf, &v, sizeof(v)); return v; }
            return SafeRead<long long>((uintptr_t)m.base + m.off, 0);
        }
        if (m.isStatic) {
            if (m.kind == 0) { int v = 0; StaticGetInt(m.sf, &v); return (long long)v; }
            unsigned char buf[64]; memset(buf, 0, sizeof(buf));
            if (!StaticGetRaw(m.sf, buf, sizeof(buf))) return 0;
            int key = 0, hid = 0;
            memcpy(&key, buf + 0, 4); memcpy(&hid, buf + 4, 4);
            return (long long)(int)(hid ^ key);
        }
        uintptr_t a = (uintptr_t)m.base + m.off;
        if (m.kind == 0) return (long long)SafeRead<int>(a, 0);
        const int key = SafeRead<int>(a + 0, 0);
        const int hid = SafeRead<int>(a + 4, 0);
        return (long long)(int)(hid ^ key);
    }

    static void MnyWriteOne(MnyMirror& m, long long v) {
        if (!m.ok || !m.sf) return;
        if (m.kind == 2) {
            if (m.isStatic) { StaticSetRaw(m.sf, &v, sizeof(v)); return; }
            SafeWrite<long long>((uintptr_t)m.base + m.off, v);
            return;
        }
        const int iv = (int)v;
        if (m.isStatic) {
            if (m.kind == 0) { StaticSetInt(m.sf, iv, 0); return; }
            unsigned char buf[64]; memset(buf, 0, sizeof(buf));
            StaticGetRaw(m.sf, buf, sizeof(buf));
            int key = 0; memcpy(&key, buf + 0, 4);
            int hid = iv ^ key; int one = 1;
            memcpy(buf + 4, &hid, 4); memcpy(buf + 8, &iv, 4); memcpy(buf + 0xC, &one, 4);
            StaticSetRaw(m.sf, buf, sizeof(buf));
            return;
        }
        uintptr_t a = (uintptr_t)m.base + m.off;
        if (m.kind == 0) { SafeWrite<int>(a, iv); return; }
        const int key = SafeRead<int>(a + 0, 0);
        SafeWrite<int>(a + 4, iv ^ key);
        SafeWrite<int>(a + 8, iv);
        SafeWrite<int>(a + 0xC, 1);
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


    // =========================================================================
    //  JSON da conta
    // =========================================================================
    static long long g_accMoney = -1, g_accGold = -1, g_accXp = -1;

    static bool AccountJson(char* out, size_t n) {
        if (!out || !n) return false;
        out[0] = 0;
        Il2CppClass* ak = SDK::FindClass("AccountManager");
        if (!ak) return false;
        void* am = StaticInstanceOf(ak);
        if (!LooksLikeObj(am)) return false;
        uint32_t off = MnyOffExact(am, "EIMOMMHLNPO");
        if (!off) off = kAmJson;
        void* s = SafeRead<void*>((uintptr_t)am + off, nullptr);
        if (!LooksLikeObj(s)) return false;
        CStr(s, out, n);
        return out[0] != 0;
    }

    static bool JsonNum(const char* js, const char* key, long long* out) {
        if (!js || !key || !out) return false;
        char pat[48];
        sprintf_s(pat, sizeof(pat), "\"%s\":", key);
        const char* p = strstr(js, pat);
        if (!p) return false;
        p += strlen(pat);
        while (*p == ' ' || *p == '\t') ++p;
        bool neg = false;
        if (*p == '-') { neg = true; ++p; }
        if (*p < '0' || *p > '9') return false;
        long long v = 0;
        while (*p >= '0' && *p <= '9') { v = v * 10 + (*p - '0'); ++p; }
        *out = neg ? -v : v;
        return true;
    }

    static void AccountJsonRefresh() {
        char js[4096]; js[0] = 0;
        if (!AccountJson(js, sizeof(js))) return;
        long long v = 0;
        if (JsonNum(js, "money", &v)) g_accMoney = v;
        if (JsonNum(js, "gold", &v)) g_accGold = v;
        if (JsonNum(js, "xp", &v)) g_accXp = v;
    }

    long long MoneyReal() {
        AccountJsonRefresh();
        return g_accMoney;
    }

    // =========================== API publica da carteira =====================
    int MoneyCount() { return g_mnyN; }
    const char* MoneyName(int idx) { return MnyIn(idx) ? g_mny[idx].label : ""; }
    bool MoneyLocked(int idx) { return MnyIn(idx) ? g_mny[idx].lock : false; }
    void MoneySetLock(int idx, bool on) { if (MnyIn(idx)) g_mny[idx].lock = on; }

    long long MoneyValue(int idx) {
        if (!MnyIn(idx)) return 0;
        MnyResolve();
        return MnyRead(g_mny[idx]);
    }

    void MoneySet(int idx, long long value) {
        if (!MnyIn(idx)) return;
        MnyResolve();
        MnyWriteOne(g_mny[idx], value);
        Log("[mirror] %s <- %lld (leitura: %lld)", g_mny[idx].label, value, MnyRead(g_mny[idx]));
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
            Log("[mirror]   %s = %lld | leitura=%lld", m.label, value, MnyRead(m));
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
        Log("[mirror] ForceMirrors(%lld, mode=%lld): %d/%d escritos", value, mode, n, g_mnyN);
        return n;
    }

    void MoneyUnlockAll() {
        const long long v = MnyCfgValue();
        for (int i = 0; i < g_mnyN; ++i) g_mny[i].lock = true;
        ForceMirrors(v, 0);
    }

    void ShopForceAll() {
        const long long gold = (C.accGold > 0) ? (long long)C.accGold : MnyCfgValue();
        const long long cases = (C.accCases > 0) ? (long long)C.accCases : gold;
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
                const char* tn = TN(t);
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
                const char* tn = TN(t);
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

    // >>> v43.22: o array cacheado nunca era revalidado.
    static void RevalidateArray() {
        const ULONGLONG now = GetTickCount64();
        if (now < g_arrNext) return;
        g_arrNext = now + 5000;
        if (!SDK::g_localPlayer || !g_offArr) return;
        void* a = SafeRead<void*>((uintptr_t)SDK::g_localPlayer + g_offArr, nullptr);
        if (!LooksLikeObj(a) || a == g_arr) return;
        int len = ArrLen((Il2CppArray*)a);
        if (len < 8 || len > 512) return;
        Log("[unlock] array de armas MUDOU %p -> %p (len %d -> %d)", g_arr, a, g_arrLen, len);
        g_arr = a; g_arrLen = len;
        g_availNext = 0;
    }

    static void ResolveOffsets() {
        if (!SDK::kGunStats) return;
        g_offName = FieldOffNamed(SDK::kGunStats, "weaponName");
        g_offAvailable = FieldOffNamed(SDK::kGunStats, "available");
        g_offSights = FieldOffNamed(SDK::kGunStats, "possibleSights");
        g_offAcc = FieldOffNamed(SDK::kGunStats, "possibleAccessories");
        g_offBarrels = FieldOffNamed(SDK::kGunStats, "possibleBarrels");
        if (!g_offName)       g_offName = 0x278;
        if (!g_offAvailable)  g_offAvailable = 0x270;
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
                const char* tn = TN(t);
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
        const char* tn = TN(t);
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
                    const char* tn = TN(t);
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
                    const char* tn = TN(t);
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
    // >>> v43.22: le os elementos pelo base do array (sem ArrElem por item) e
    //     so roda quando o portao deixa. Era o que varria as ~65 armas por frame.
    static void ApplyAvailable() {
        if (!g_arr || g_arrLen <= 0 || !g_offAvailable) return;
        if (!LooksLikeObj(g_arr)) return;
        const uintptr_t items = (uintptr_t)g_arr + 0x20;
        int n = 0;
        for (int i = 0; i < g_arrLen; ++i) {
            void* gun = *(void**)(items + (size_t)i * 8);
            if (!gun) continue;
            if (SafeRead<int>((uintptr_t)gun + g_offAvailable, 0) != 1) {
                SafeWrite<int>((uintptr_t)gun + g_offAvailable, 1);
                ++n;
            }
        }
        if (n) {
            static int lastN = -1;
            if (n != lastN) { lastN = n; Log("[unlock] available=1 em %d arma(s)", n); }
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
                const char* tn = TN(t);
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

    // =========================================================================
    //  EQUIP POR SLOT (LoadoutManager)
    // =========================================================================
    static MethodInfo* g_mEquipPrim = nullptr;
    static MethodInfo* g_mEquipSec = nullptr;
    static MethodInfo* g_mEquipGad = nullptr;
    static MethodInfo* g_mEquip = nullptr;
    static MethodInfo* g_mEquipForce = nullptr;
    static void* g_equipForceObj = nullptr;

    static bool        g_equipResolved = false;
    static bool        g_equipDumped = false;
    static bool        g_equipMgrWarned = false;
    static bool        g_equipSlotWarned = false;
    static int         g_lastEquip = -2;
    static int         g_lastEquipSlot = -2;

    static const char* kEquipForceClass = "";
    static const char* kEquipForceName = "";
    static const int   kEquipForceArgc = -1;

    typedef void* (*fn_invoke_ex)(MethodInfo*, void*, void**, void**);
    static fn_invoke_ex p_invokeEx = nullptr;

    static bool g_equipUseType = true;   // manda o TIPO primeiro, nao o indice
    static bool g_offEquip = false, g_offIdx = false, g_offFv = false;
    static bool g_offPrefs = false, g_offCand = false;

    void SetOffEquip(bool b) { g_offEquip = b; Log("[dbg] offEquip=%d", b); }
    void SetOffIdx(bool b) { g_offIdx = b;   Log("[dbg] offIdx=%d", b); }
    void SetOffFv(bool b) { g_offFv = b;    Log("[dbg] offFv=%d", b); }
    void SetOffPrefs(bool b) { g_offPrefs = b; Log("[dbg] offPrefs=%d", b); }
    void SetOffCand(bool b) { g_offCand = b;  Log("[dbg] offCand=%d", b); }
    void SetEquipUseType(bool b) { g_equipUseType = b; Log("[dbg] equipUseType=%d", b); }

    typedef uint32_t(*fn_epParamCount)(const MethodInfo*);
    typedef const Il2CppType* (*fn_epParam)(const MethodInfo*, uint32_t);
    typedef MethodInfo* (*fn_class_get_methods)(Il2CppClass*, void**);
    typedef const char* (*fn_method_get_name)(const MethodInfo*);
    typedef uint32_t(*fn_method_get_flags)(const MethodInfo*, uint32_t*);
    static fn_epParamCount p_epParamCount = nullptr;
    static fn_epParam      p_epParam = nullptr;
    static fn_class_get_methods p_classGetMethods = nullptr;
    static fn_method_get_name   p_methodGetName = nullptr;
    static fn_method_get_flags  p_methodGetFlags = nullptr;

    static void ResolveEquipProcs() {
        static bool done = false;
        if (done) return;
        done = true;
        HMODULE ga = GetModuleHandleA("GameAssembly.dll");
        if (!ga) return;
        p_epParamCount = (fn_epParamCount)GetProcAddress(ga, "il2cpp_method_get_param_count");
        p_epParam = (fn_epParam)GetProcAddress(ga, "il2cpp_method_get_param");
        p_classGetMethods = (fn_class_get_methods)GetProcAddress(ga, "il2cpp_class_get_methods");
        p_methodGetName = (fn_method_get_name)GetProcAddress(ga, "il2cpp_method_get_name");
        p_methodGetFlags = (fn_method_get_flags)GetProcAddress(ga, "il2cpp_method_get_flags");
        p_invokeEx = (fn_invoke_ex)GetProcAddress(ga, "il2cpp_runtime_invoke");
    }

    static bool MethodIsStatic(MethodInfo* m) {
        if (!m || !p_methodGetFlags) return false;
        uint32_t fl = 0;
        __try { fl = p_methodGetFlags(m, nullptr); }
        __except (EXCEPTION_EXECUTE_HANDLER) { fl = 0; }
        return (fl & 0x10) != 0;
    }

    static void* StaticInstanceOf(Il2CppClass* k) {
        if (!k) return nullptr;
        static const char* nm[] = { "Instance", "instance", "singleton", "Singleton", "s_instance" };
        for (Il2CppClass* c = k; c; c = il2cpp_class_get_parent(c)) {
            for (size_t i = 0; i < sizeof(nm) / sizeof(nm[0]); ++i) {
                FieldInfo* f = il2cpp_class_get_field_from_name(c, nm[i]);
                if (!f || !IsStatic(f)) continue;
                void* o = nullptr; StaticGetSafe(f, &o);
                if (LooksLikeObj(o)) return o;
            }
        }
        return nullptr;
    }

    static void* LoadoutMgr() {
        static void* cached = nullptr;
        if (LooksLikeObj(cached)) return cached;
        Il2CppClass* k = SDK::FindClass("LoadoutManager");
        if (!k) return nullptr;
        void* o = StaticInstanceOf(k);
        if (!o) {
            FieldInfo* f = il2cpp_class_get_field_from_name(k, "Instance");
            if (f && IsStatic(f)) StaticGetSafe(f, &o);
        }
        if (!LooksLikeObj(o)) {
            if (!g_equipMgrWarned) { g_equipMgrWarned = true; Log("[unlock] LoadoutMgr() AUSENTE (cls=%p)", (void*)k); }
            return nullptr;
        }
        cached = o;
        if (!g_equipMgrWarned) { g_equipMgrWarned = true; Log("[unlock] LoadoutMgr() = %p", o); }
        return cached;
    }

    static MethodInfo* FindMethodAnyArgc(Il2CppClass* k, const char* name,
        int argcMin, int argcMax) {
        if (!k || !name) return nullptr;
        for (Il2CppClass* c = k; c; c = il2cpp_class_get_parent(c)) {
            for (int a = argcMin; a <= argcMax; ++a) {
                MethodInfo* m = il2cpp_class_get_method_from_name(c, name, a);
                if (m) return m;
            }
        }
        return nullptr;
    }

    static bool EquipParamOk(const Il2CppType* t) {
        if (!t) return false;
        const char* tn = TN(t);
        if (!tn) return false;
        if (strstr(tn, "Int32") || strstr(tn, "Byte") || strstr(tn, "Int16") ||
            strstr(tn, "SByte") || strstr(tn, "UInt16") || strstr(tn, "UInt32"))
            return true;
        if (p_classFromType && p_isValue) {
            Il2CppClass* k = p_classFromType(t);
            if (k && p_isValue(k)) return true;
        }
        return false;
    }

    static bool EquipIsEnumParam(const Il2CppType* t) {
        if (!t || !p_classFromType || !p_isValue) return false;
        Il2CppClass* k = p_classFromType(t);
        return (k && p_isValue(k));
    }

    static bool EquipNameKnown(const char* n) {
        if (!n) return false;
        static const char* list[] = {
            "serverSetWeapon", "rpcSetWeapon", "setWeaponType", "setWeapon",
            "equipWeapon", "changeWeapon", "setCurrentWeapon", "cmdSetWeapon",
            "serverChangeWeapon", "selectWeapon", "setLoadoutWeapon", "setGun",
            "serverSetLoadout", "applyLoadout", "setClassWeapon", "setPrimary",
            "setSecondary", "setGadget", "setGunAsPrimary", "setGunAsSecondary",
            "setAsGadget", "equip", "setIndex", "setWeaponIndex"
        };
        char l[96]; Lower(l, sizeof(l), n);
        for (size_t i = 0; i < sizeof(list) / sizeof(list[0]); ++i)
            if (HasWord(l, list[i])) return true;
        return false;
    }

    static bool EquipNameBlacklisted(const char* n) {
        if (!n) return true;
        static const char* bad[] = {
            "finalize","tostring","gethashcode","equals","compareto","gettype",
            "startcoroutine","invoke","getcomponent","setactive","setenabled",
            "getenabled","awake","update","lateupdate","fixedupdate","start",
            "get_","set_",".ctor","gethit","sethit"
        };
        char l[96]; Lower(l, sizeof(l), n);
        for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i)
            if (HasWord(l, bad[i])) return true;
        return false;
    }

    static int EquipNameScore(const char* n) {
        if (!n) return -999;
        char l[96]; Lower(l, sizeof(l), n);
        int s = 0;
        static const char* good[] = {
            "setweapon","equipweapon","selectweapon","changeweapon","switchweapon",
            "applyweapon","setcurrentweapon","setloadout","setgun","setclassweapon",
            "setprimary","setsecondary","setgadget","useweapon","equip"
        };
        for (size_t i = 0; i < sizeof(good) / sizeof(good[0]); ++i)
            if (strstr(l, good[i])) s += 100;
        if (strstr(l, "weapon")) s += 20;
        if (strstr(l, "gun"))    s += 15;
        if (strstr(l, "slot"))   s += 10;
        if (strstr(l, "type"))   s += 5;
        if (strstr(l, "index"))  s += 5;
        if (!strcmp(l, "setgunasprimary"))   s += 220;
        if (!strcmp(l, "setgunassecondary")) s += 210;
        if (!strcmp(l, "setasgadget"))       s += 200;
        if (strstr(l, "available")) s -= 45;
        if (strstr(l, "sight"))     s -= 30;
        if (strstr(l, "barrel"))    s -= 30;
        if (strstr(l, "accessor"))  s -= 30;
        if (strstr(l, "tracer"))    s -= 25;
        if (strstr(l, "camo"))      s -= 25;
        if (strstr(l, "ammo"))      s -= 30;
        if (strstr(l, "object"))    s -= 20;
        if (strstr(l, "stats"))     s -= 25;
        if (!strncmp(l, "is", 2))  s -= 40;
        if (!strncmp(l, "get", 3)) s -= 35;
        if (!strncmp(l, "has", 3)) s -= 30;
        if (!strncmp(l, "can", 3)) s -= 30;
        if (!strncmp(l, "should", 6)) s -= 30;
        if (!strncmp(l, "need", 4)) s -= 25;
        return s;
    }

    struct EquipCand { MethodInfo* m; const char* name; int score; uint32_t np; };

    static int CollectEquipCands(Il2CppClass* k, EquipCand* out, int max,
        const char* wantType, bool allowPred) {
        if (!k || !p_classGetMethods || !p_epParamCount || !p_epParam) return 0;
        int n = 0;
        for (Il2CppClass* c = k; c; c = il2cpp_class_get_parent(c)) {
            void* it = nullptr;
            for (;;) {
                MethodInfo* m = p_classGetMethods(c, &it);
                if (!m) break;
                const uint32_t np = p_epParamCount(m);
                if (np < 1 || np > 3) continue;

                bool hasWant = false, allOk = true;
                for (uint32_t i = 0; i < np; ++i) {
                    const Il2CppType* pt = p_epParam(m, i);
                    if (!EquipParamOk(pt)) allOk = false;
                    if (wantType && pt && p_classFromType) {
                        Il2CppClass* pk = p_classFromType(pt);
                        if (pk && strcmp(il2cpp_class_get_name(pk), wantType) == 0) hasWant = true;
                    }
                }
                if (!hasWant || !allOk) continue;

                const char* mn = p_methodGetName ? p_methodGetName(m) : "?";
                const int sc = EquipNameScore(mn);
                if (!allowPred && sc < 0) continue;
                if (n < max) { out[n].m = m; out[n].name = mn; out[n].score = sc; out[n].np = np; }
                ++n;
            }
        }
        return n;
    }

    static void DumpEquipCandidates(Il2CppClass* k) {
        if (!k || !p_classGetMethods || !p_methodGetName || !p_epParamCount || !p_epParam) return;
        Log("[equip] ===== %s =====", il2cpp_class_get_name(k));
        int shown = 0;
        void* it = nullptr;
        for (;;) {
            MethodInfo* m = p_classGetMethods(k, &it);
            if (!m) break;
            const uint32_t np = p_epParamCount(m);
            if (np < 1 || np > 3) continue;

            bool hasEnum = false, allOk = true;
            char pb[192] = "";
            for (uint32_t i = 0; i < np; ++i) {
                const Il2CppType* pt = p_epParam(m, i);
                if (EquipIsEnumParam(pt)) hasEnum = true;
                if (!EquipParamOk(pt)) allOk = false;
                const char* pn = "?";
                if (pt && p_classFromType) {
                    Il2CppClass* pk = p_classFromType(pt);
                    if (pk) pn = il2cpp_class_get_name(pk);
                }
                strncat_s(pb, sizeof(pb), pn, _TRUNCATE);
                if (i + 1 < np) strncat_s(pb, sizeof(pb), ",", _TRUNCATE);
            }
            if (!hasEnum) continue;
            const char* mn = p_methodGetName(m);
            Log("[equip]   score=%4d %s argc=%u%s%s (%s)",
                EquipNameScore(mn), mn, np,
                allOk ? "" : " *", EquipNameKnown(mn) ? " [nome-ok]" : "", pb);
            if (++shown >= 48) break;
        }
        if (!shown) Log("[equip]   (nenhum metodo com enum/valuetype)");
    }

    static void DumpEquipAllClasses() {
        static const char* cls[] = {
            "PlayerScript", "LoadoutManager", "LoadoutGuns",
            "FirstPersonGunView", "GunCustomizeScript", "PlayerLoadoutContainer"
        };
        for (size_t i = 0; i < sizeof(cls) / sizeof(cls[0]); ++i) {
            Il2CppClass* k = SDK::FindClass(cls[i]);
            if (k) DumpEquipCandidates(k);
        }
    }

    static MethodInfo* FindEquipLoose(Il2CppClass* k) {
        if (!k || !p_classGetMethods || !p_epParam || !p_epParamCount) return nullptr;
        for (Il2CppClass* c = k; c; c = il2cpp_class_get_parent(c)) {
            void* it = nullptr;
            for (;;) {
                MethodInfo* m = p_classGetMethods(c, &it);
                if (!m) break;
                const uint32_t np = p_epParamCount(m);
                if (np < 1 || np > 3) continue;
                const char* mn = p_methodGetName ? p_methodGetName(m) : "";
                if (EquipNameBlacklisted(mn)) continue;
                bool hasEnum = false, allOk = true;
                for (uint32_t i = 0; i < np; ++i) {
                    const Il2CppType* pt = p_epParam(m, i);
                    if (EquipIsEnumParam(pt)) hasEnum = true;
                    if (!EquipParamOk(pt)) allOk = false;
                }
                if (hasEnum && allOk) return m;
            }
        }
        return nullptr;
    }

    static void ResolveSlotEquipNames(Il2CppClass* lm) {
        if (!lm) return;
        static const char* primNames[] = { "SetGunAsPrimary", "SetPrimary", "SetPrimaryGun" };
        static const char* secNames[] = { "SetGunAsSecondary", "SetSecondary", "SetSecondaryGun" };
        static const char* gadNames[] = { "SetAsGadget", "SetGadget", "SetGunAsGadget", "EquipGadget" };
        for (size_t i = 0; i < sizeof(primNames) / sizeof(primNames[0]) && !g_mEquipPrim; ++i)
            g_mEquipPrim = FindMethodAnyArgc(lm, primNames[i], 1, 2);
        for (size_t i = 0; i < sizeof(secNames) / sizeof(secNames[0]) && !g_mEquipSec; ++i)
            g_mEquipSec = FindMethodAnyArgc(lm, secNames[i], 1, 2);
        for (size_t i = 0; i < sizeof(gadNames) / sizeof(gadNames[0]) && !g_mEquipGad; ++i)
            g_mEquipGad = FindMethodAnyArgc(lm, gadNames[i], 1, 2);
    }

    static void ResolveEquipMethod() {
        if (g_equipResolved) return;
        g_equipResolved = true;
        if (!SDK::kPlayerScript) return;
        ResolveEquipProcs();

        if (kEquipForceName && kEquipForceName[0]) {
            Il2CppClass* fk = (kEquipForceClass && kEquipForceClass[0])
                ? SDK::FindClass(kEquipForceClass) : SDK::kPlayerScript;
            if (fk) {
                MethodInfo* m = nullptr;
                if (kEquipForceArgc >= 0)
                    m = il2cpp_class_get_method_from_name(fk, kEquipForceName, kEquipForceArgc);
                else
                    for (int a = 1; a <= 4 && !m; ++a)
                        m = il2cpp_class_get_method_from_name(fk, kEquipForceName, a);
                if (m) {
                    g_mEquipForce = m;
                    const bool st = MethodIsStatic(m);
                    g_equipForceObj = st ? nullptr : StaticInstanceOf(fk);
                    if (!st && !g_equipForceObj && fk == SDK::kPlayerScript)
                        g_equipForceObj = SDK::g_localPlayer;
                    Log("[unlock] equip: %s::%s argc=%u (FORCADO%s)",
                        il2cpp_class_get_name(fk), kEquipForceName,
                        p_epParamCount ? p_epParamCount(m) : 0, st ? ", static" : "");
                    return;
                }
            }
            Log("[unlock] equip: forca \"%s::%s\" NAO achou, seguindo automatico",
                kEquipForceClass, kEquipForceName);
        }

        Il2CppClass* lm = SDK::FindClass("LoadoutManager");
        ResolveSlotEquipNames(lm);
        if (g_mEquipPrim || g_mEquipSec || g_mEquipGad) {
            Log("[unlock] equip POR SLOT: primary=%s secondary=%s gadget=%s (LoadoutManager)",
                g_mEquipPrim ? "OK" : "-", g_mEquipSec ? "OK" : "-", g_mEquipGad ? "OK" : "-");
            for (int s = 0; s < 3; ++s) {
                MethodInfo* m = (s == 0) ? g_mEquipPrim : (s == 1) ? g_mEquipSec : g_mEquipGad;
                if (!m) continue;
                Log("[unlock]   slot%d: %s argc=%u %s", s,
                    p_methodGetName ? p_methodGetName(m) : "?",
                    p_epParamCount ? p_epParamCount(m) : 0,
                    MethodIsStatic(m) ? "[static]" : "[inst]");
            }
            if (!g_mEquip) g_mEquip = g_mEquipPrim ? g_mEquipPrim
                : (g_mEquipSec ? g_mEquipSec : g_mEquipGad);
            Log("[unlock] equip fallback = %s (%s)",
                g_mEquip ? (p_methodGetName ? p_methodGetName(g_mEquip) : "?") : "-",
                MethodIsStatic(g_mEquip) ? "static" : "inst");
            return;
        }

        static const char* names[] = {
            "ServerSetWeapon", "RpcSetWeapon", "SetWeaponType", "SetWeapon",
            "EquipWeapon", "ChangeWeapon", "SetCurrentWeapon", "CmdSetWeapon",
            "ServerChangeWeapon", "SelectWeapon", "SetLoadoutWeapon", "SetGun",
            "ServerSetLoadout", "ApplyLoadout", "SetClassWeapon", "SetPrimary",
            "SetSecondary", "SetGadget", "Equip", "SetIndex", "SetWeaponIndex"
        };

        for (size_t i = 0; i < sizeof(names) / sizeof(names[0]) && !g_mEquip; ++i) {
            for (Il2CppClass* c = SDK::kPlayerScript; c && !g_mEquip; c = il2cpp_class_get_parent(c)) {
                for (int argc = 1; argc <= 3 && !g_mEquip; ++argc) {
                    MethodInfo* m = il2cpp_class_get_method_from_name(c, names[i], argc);
                    if (!m) continue;
                    bool ok = true;
                    for (int a = 0; a < argc && ok; ++a) {
                        const Il2CppType* pt = p_epParam ? p_epParam(m, (uint32_t)a) : nullptr;
                        if (!EquipParamOk(pt)) ok = false;
                    }
                    if (ok) { g_mEquip = m; Log("[unlock] equip: %s argc=%d", names[i], argc); }
                }
            }
        }
        if (!g_mEquip) {
            for (size_t i = 0; i < sizeof(names) / sizeof(names[0]) && !g_mEquip; ++i) {
                for (Il2CppClass* c = SDK::kPlayerScript; c && !g_mEquip; c = il2cpp_class_get_parent(c)) {
                    for (int argc = 1; argc <= 2 && !g_mEquip; ++argc) {
                        MethodInfo* m = il2cpp_class_get_method_from_name(c, names[i], argc);
                        if (m) { g_mEquip = m; Log("[unlock] equip: %s argc=%d (fallback)", names[i], argc); }
                    }
                }
            }
        }

        if (!g_mEquip) {
            if (!g_equipDumped) { g_equipDumped = true; DumpEquipAllClasses(); }

            static const char* clsW[] = {
                "PlayerScript", "LoadoutManager", "LoadoutGuns", "FirstPersonGunView"
            };
            static const char* want[] = {
                "EFIPOFGCOKN", "WeaponType", "weaponType", "GunType", "EnumWeapon",
                "Weapons", "GunTypeEnum", "WeaponEnum"
            };
            static EquipCand cands[128];
            for (size_t ci = 0; ci < sizeof(clsW) / sizeof(clsW[0]) && !g_mEquip; ++ci) {
                Il2CppClass* k = SDK::FindClass(clsW[ci]);
                if (!k) continue;
                for (size_t w = 0; w < sizeof(want) / sizeof(want[0]) && !g_mEquip; ++w) {
                    int n = CollectEquipCands(k, cands, 128, want[w], false);
                    if (n <= 0) n = CollectEquipCands(k, cands, 128, want[w], true);
                    if (n <= 0) continue;
                    if (n > 128) n = 128;

                    int best = 0;
                    for (int i = 1; i < n; ++i) if (cands[i].score > cands[best].score) best = i;

                    g_mEquip = cands[best].m;
                    Log("[unlock] equip: %s::%s argc=%u (pass3 classe=%s tipo \"%s\" score=%d, %d cands)",
                        clsW[ci], cands[best].name, cands[best].np, clsW[ci], want[w],
                        cands[best].score, n);
                    for (int i = 0; i < n && i < 48; ++i)
                        Log("[equip]   cand score=%4d %s argc=%u", cands[i].score, cands[i].name, cands[i].np);
                }
            }
        }
        if (!g_mEquip) {
            MethodInfo* m = FindEquipLoose(SDK::kPlayerScript);
            if (m) {
                g_mEquip = m;
                Log("[unlock] equip: %s argc=%u (pass3b candidato solto)",
                    p_methodGetName ? p_methodGetName(m) : "?", p_epParamCount(m));
            }
        }
        if (!g_mEquip) Log("[unlock] equip: nenhuma assinatura valida encontrada");
    }

    static void EquipArgFill(MethodInfo* m, void** args, int* ibuf, void** refbuf,
        int slot, int value, int wtype) {
        const uint32_t np = p_epParamCount ? p_epParamCount(m) : 0;
        for (uint32_t i = 0; i < np; ++i) {
            const Il2CppType* t = p_epParam ? p_epParam(m, i) : nullptr;
            const char* tn = TN(t);
            if (tn && strstr(tn, "Boolean")) { ibuf[i] = 1;     args[i] = &ibuf[i]; }
            else if (EquipIsEnumParam(t)) { ibuf[i] = wtype; args[i] = &ibuf[i]; }
            else if (tn && (strstr(tn, "Int32") || strstr(tn, "Byte") || strstr(tn, "Int16") ||
                strstr(tn, "UInt16") || strstr(tn, "UInt32") || strstr(tn, "SByte"))) {
                ibuf[i] = (i == 0) ? value : slot;
                args[i] = &ibuf[i];
            }
            else { refbuf[i] = nullptr; args[i] = &refbuf[i]; }
        }
    }

    static bool InvokeEquipLogged(MethodInfo* m, void* obj, int slot, int value, int wtype,
        const char* tag, const char* kind) {
        if (!m) return true;
        const uint32_t np = p_epParamCount ? p_epParamCount(m) : 1;
        if (np == 0 || np > 8) return true;

        int   ibuf[8] = {};
        void* refbuf[8] = {};
        void* args[8] = {};
        EquipArgFill(m, args, ibuf, refbuf, slot, value, wtype);

        void* lm = LoadoutMgr();
        const int gb = lm ? SafeRead<int>((uintptr_t)lm + kLmCurType, -1) : -1;
        const int sb = lm ? SafeRead<int>((uintptr_t)lm + kLmSel1, -1) : -1;

        void* exc = nullptr;
        __try {
            if (p_invokeEx) p_invokeEx(m, obj, args, &exc);
            else            il2cpp_runtime_invoke(m, obj, args, nullptr);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("[unlock] equip %s: AV NATIVO (SEH) arg=%s(%d)", tag, kind, value);
            return false;
        }
        const int ga2 = lm ? SafeRead<int>((uintptr_t)lm + kLmCurType, -1) : -1;
        const int sa2 = lm ? SafeRead<int>((uintptr_t)lm + kLmSel1, -1) : -1;

        if (exc) {
            Il2CppClass* ec = nullptr;
            __try { ec = il2cpp_object_get_class((Il2CppObject*)exc); }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
            char msg[160]; msg[0] = 0;
            if (ec) {
                MethodInfo* gm = il2cpp_class_get_method_from_name(ec, "get_Message", 0);
                if (gm && p_invokeEx) {
                    void* r = nullptr;
                    __try { r = p_invokeEx(gm, exc, nullptr, nullptr); }
                    __except (EXCEPTION_EXECUTE_HANDLER) {}
                    if (r) CStr(r, msg, sizeof(msg));
                }
            }
            Log("[unlock] equip %s: EXCECAO %s \"%s\" arg=%s(%d) | currentGunType %d->%d sel1 %d->%d",
                tag, ec ? il2cpp_class_get_name(ec) : "?", msg, kind, value, gb, ga2, sb, sa2);
            return false;
        }
        Log("[unlock] equip %s OK arg=%s(%d) | currentGunType %d->%d sel1 %d->%d",
            tag, kind, value, gb, ga2, sb, sa2);
        return true;
    }

    static bool TryEquipSmart(MethodInfo* m, void* obj, int slot, int live, int wtype,
        const char* tag) {
        if (g_equipUseType) {
            if (InvokeEquipLogged(m, obj, slot, wtype, wtype, tag, "tipo")) return true;
            Log("[unlock] equip %s: repetindo com INDICE", tag);
            if (InvokeEquipLogged(m, obj, slot, live, wtype, tag, "idx")) return true;
        }
        else {
            if (InvokeEquipLogged(m, obj, slot, live, wtype, tag, "idx")) return true;
            Log("[unlock] equip %s: repetindo com TIPO", tag);
            if (InvokeEquipLogged(m, obj, slot, wtype, wtype, tag, "tipo")) return true;
        }
        return false;
    }

    static void CallEquip(int slot, int weaponIdx, int weaponType) {
        if (!SDK::g_localPlayer) return;
        if (slot < 0) slot = 0;
        if (slot > 2) slot = 2;

        if (g_offEquip) {
            Log("[unlock] equip PULADO (off) slot=%d idx=%d tipo=%d", slot, weaponIdx, weaponType);
            return;
        }

        if (g_mEquipForce) {
            TryEquipSmart(g_mEquipForce, g_equipForceObj, slot, weaponIdx, weaponType, "FORCADO");
            Log("[unlock] equip FORCADO slot=%d idx=%d tipo=%d obj=%p",
                slot, weaponIdx, weaponType, g_equipForceObj);
            return;
        }

        MethodInfo* m = (slot == 0) ? g_mEquipPrim : (slot == 1) ? g_mEquipSec : g_mEquipGad;
        if (m) {
            const bool st = MethodIsStatic(m);
            void* obj = st ? nullptr : LoadoutMgr();
            if (st || obj) {
                Log("[unlock] equip POR SLOT: slot=%d metodo=%s argc=%u %s obj=%p",
                    slot, p_methodGetName ? p_methodGetName(m) : "?",
                    p_epParamCount ? p_epParamCount(m) : 0, st ? "[static]" : "[inst]", obj);
                TryEquipSmart(m, obj, slot, weaponIdx, weaponType, "slot");
                return;
            }
            if (!g_equipSlotWarned) {
                g_equipSlotWarned = true;
                Log("[unlock] equip slot%d: %s sem instancia do LoadoutManager",
                    slot, p_methodGetName ? p_methodGetName(m) : "?");
            }
        }

        if (g_mEquip) {
            const bool st = MethodIsStatic(g_mEquip);
            void* o2 = nullptr;
            if (!st) { o2 = LoadoutMgr(); if (!o2) o2 = SDK::g_localPlayer; }
            TryEquipSmart(g_mEquip, o2, slot, weaponIdx, weaponType, "fallback");
            Log("[unlock] equip FALLBACK: %s argc=%u slot=%d idx=%d tipo=%d obj=%p%s",
                p_methodGetName ? p_methodGetName(g_mEquip) : "?",
                p_epParamCount ? p_epParamCount(g_mEquip) : 0, slot, weaponIdx, weaponType,
                o2, st ? " [static]" : "");
        }
    }

    // =====================================================================
    //  PlayerPrefs
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
    static bool         g_prefsDirty = false;

    static Il2CppClass* g_strCls = nullptr;

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
        g_ppOk = (g_ppSetInt && g_ppGetInt && g_ppSetStr);
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

    static bool PrefStrictKey(const char* low) {
        if (!low) return false;
        if (strstr(low, "allowed")) return false;
        if (strstr(low, "holding")) return false;
        if (strstr(low, "minigun")) return false;
        if (strstr(low, "room"))    return false;
        if (strstr(low, "type") && !strstr(low, "weapon") && !strstr(low, "gun")) return false;
        return strstr(low, "weapon") || strstr(low, "gun") || strstr(low, "loadout") ||
            strstr(low, "slot") || strstr(low, "primary") || strstr(low, "secondary") ||
            strstr(low, "gadget") || strstr(low, "class");
    }

    static bool PrefIsPrefix(const char* k) {
        if (!k) return true;
        const size_t n = strlen(k);
        if (!n) return true;
        if (k[0] == '_' || k[n - 1] == '_') return true;
        if (strchr(k, '/') || strchr(k, '.')) return true;
        return false;
    }

    static bool PrefClassRelevant(const char* name) {
        if (!name || !name[0]) return false;
        static const char* w[] = {
            "pref","setting","config","stat","loadout","camo","skin","manager","client",
            "deep","chat","popup","anim","page","weapon","gun","map","bot","game","script",
            "data","profile","account","store","shop","save","slot","class","view",
            "control","handler","service","util","table","menu","inventory","item",
            "player","bf_","equip","unlock"
        };
        char l[96]; Lower(l, sizeof(l), name);
        for (size_t i = 0; i < sizeof(w) / sizeof(w[0]); ++i)
            if (strstr(l, w[i])) return true;
        return false;
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
        int clsSeen = 0, clsKept = 0, fldSeen = 0, strSeen = 0;

        if (!g_strCls) g_strCls = SDK::FindClass("System.String");

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
                    ++clsSeen;
                    const char* kn = il2cpp_class_get_name(k);
                    if (!PrefClassRelevant(kn)) continue;
                    ++clsKept;

                    void* it = nullptr;
                    for (;;) {
                        FieldInfo* f = il2cpp_class_get_fields(k, &it);
                        if (!f) break;
                        ++fldSeen;
                        if (!IsStatic(f)) continue;
                        const Il2CppType* t = il2cpp_field_get_type(f);
                        if (!t) continue;

                        if (p_classFromType && g_strCls) {
                            if (p_classFromType(t) != g_strCls) continue;
                        }
                        else {
                            const char* tn = TN(t);
                            if (!tn || !strstr(tn, "String") || strstr(tn, "[]")) continue;
                        }
                        ++strSeen;

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
        Log("[p] chaves de prefs uteis: %d (scan %llu ms | cls %d/%d fld %d str %d)",
            g_ppKN, GetTickCount64() - t0, clsKept, clsSeen, fldSeen, strSeen);
    }

    static void PrefsFlush() {
        if (!g_prefsDirty) return;
        if (g_ppSave) { void* a[1] = { nullptr }; InvokeRet(g_ppSave, nullptr, a); Log("[p] Save()"); }
        g_prefsDirty = false;
    }

    static void PrefsApply(int weaponType, void* nameStr) {
        if (g_offPrefs) return;
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
            const char* kk = g_ppK[i].key;
            if (PrefIsPrefix(kk)) continue;

            char kl[96]; Lower(kl, sizeof(kl), kk);
            if (!PrefWord(kl)) continue;
            if (!PrefStrictKey(kl)) continue;

            int before = -777777;
            if (g_ppGetInt) { void* a[2] = { g_ppK[i].str, &before }; InvokeRet(g_ppGetInt, nullptr, a); }

            if (before != -777777) {
                if (before != weaponType) {
                    int v = weaponType; void* a[2] = { g_ppK[i].str, &v };
                    InvokeRet(g_ppSetInt, nullptr, a);
                    Log("[p] SETINT \"%s\" = %d (era %d)", kk, weaponType, before);
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
                        Log("[p] SETSTR \"%s\" = \"%s\" (era \"%s\")", kk, nb, cb);
                        changed = true;
                    }
                }
            }
        }
        if (changed) { g_prefsDirty = true; PrefsFlush(); }
    }

    static int g_mnyPrefLastN = -1;

    void MoneyWritePrefs() {
        if (!ResolvePlayerPrefs()) return;
        if (g_ppKN <= 0) CollectPrefsKeys(true);
        if (g_ppKN <= 0 || !g_ppSetInt) return;
        static const char* keys[] = { "gold", "money", "credit", "cash", "coin", "case", "caixa" };
        int n = 0;
        for (int i = 0; i < g_ppKN; ++i) {
            if (PrefIsPrefix(g_ppK[i].key)) continue;
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
        if (n) { g_prefsDirty = true; PrefsFlush(); }
        if (n != g_mnyPrefLastN) {
            g_mnyPrefLastN = n;
            Log("[mirror] prefs: %d chave(s) escritas", n);
        }
    }

    static ULONGLONG g_mnyTickNext = 0;

    static void MnyApplyLocks() {
        const long long cred = MnyCfgValue();
        const long long gold = (C.accGold > 0) ? (long long)C.accGold : cred;
        const long long cases = (C.accCases > 0) ? (long long)C.accCases : gold;

        for (int i = 0; i < g_mnyN; ++i) {
            MnyMirror& m = g_mny[i];
            if (!m.ok || !m.lock) continue;
            long long v = cred;
            if (strstr(m.cls, "CamoCase"))       v = cases;
            else if (strstr(m.label, "gold"))    v = gold;
            else if (strstr(m.label, "case"))    v = cases;
            else if (strstr(m.label, "xp")) { if (C.unlockLevel <= 0) continue; v = (long long)C.unlockLevel; }
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
    //  >>> v43.20: ApplySelection roda UMA vez por selecao, sem timer de 4s
    // =========================================================================
    static int       g_selTarget = -2;
    static int       g_selSlot = -2;
    static int       g_selStep = -1;
    static bool      g_selDone = true;
    static bool      g_selForce = false;
    static bool      g_selForceEquip = false;
    static ULONGLONG g_selNext = 0;

    void RequestApply() {
        g_selForce = true;
        g_selForceEquip = true;
        g_selDone = false;
        g_selStep = -1;
    }
    void RequestApplySlot(int slot) {
        C.loadoutSlot = slot;
        RequestApply();
    }

    static void LogLoadoutReadback() {
        if (!SDK::g_localPlayer) return;
        void* fv = FpsGunView();
        Log("[r] prim=%d sec=%d gadget=%d | fv38=%p fv48=%d",
            SafeRead<unsigned char>((uintptr_t)SDK::g_localPlayer + kLoPrim, 255),
            SafeRead<unsigned char>((uintptr_t)SDK::g_localPlayer + kLoSec, 255),
            SafeRead<int>((uintptr_t)SDK::g_localPlayer + kLoGad, -1),
            fv ? SafeRead<void*>((uintptr_t)fv + kFvHeld, nullptr) : nullptr,
            fv ? SafeRead<int>((uintptr_t)fv + kFvSlot, -1) : -1);
        void* lm = LoadoutMgr();
        if (lm) Log("[r] LM: currentGunType=%d sel1=%d sel2=%d sel3=%d activeLoadout=%d",
            SafeRead<int>((uintptr_t)lm + kLmCurType, -1),
            SafeRead<int>((uintptr_t)lm + kLmSel1, -1),
            SafeRead<int>((uintptr_t)lm + kLmSel2, -1),
            SafeRead<int>((uintptr_t)lm + kLmSel3, -1),
            SafeRead<int>((uintptr_t)lm + kLmActive, -1));
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
        if (!C.applySelection && !g_selForce) return;
        if (!SDK::g_localPlayer) return;
        if (!g_arr || g_arrLen <= 0) return;
        if (!g_dupeEnsured) { Dupe::EnsureLoaded(); g_dupeEnsured = true; }

        const int listIdx = C.selWeapon;
        const int wcount = Dupe::WeaponCount();
        if (listIdx < 0 || listIdx >= wcount) {
            static int lastN = -2;
            if (wcount != lastN) {
                lastN = wcount;
                Log("[unlock] applySelection: armas=%d (aperte Build) selecao=%d", wcount, listIdx);
            }
            return;
        }

        int slot = C.loadoutSlot;
        if (slot < 0) slot = 0;
        if (slot > 2) slot = 2;

        if (!g_loDone) ResolveLoadoutFields();

        const int total = g_loCN + 2;
        const ULONGLONG now = GetTickCount64();

        const bool mudou = (listIdx != g_selTarget || slot != g_selSlot);
        if (mudou || g_selForce) {
            if (g_selForce && !mudou && now < g_selNext) return;
            g_selNext = now + 150;
            if (mudou)
                Log("[unlock] INICIO: lista=%d nome=\"%s\" slot=%d passos=%d",
                    listIdx, Dupe::WeaponName(listIdx), slot, total);
            else
                Log("[unlock] FORCADO: lista=%d nome=\"%s\" slot=%d passos=%d",
                    listIdx, Dupe::WeaponName(listIdx), slot, total);
            g_selTarget = listIdx;
            g_selSlot = slot;
            g_selStep = 0;
            g_selDone = false;
            if (!mudou) g_selForceEquip = true;
            g_selForce = false;
        }
        if (g_selDone || g_selStep < 0) return;

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
            if (!g_offCand) LoWriteCand(c, slot, live, wtype, nameStr);
            ++g_selStep;
            return;
        }

        if (g_selStep == g_loCN) {
            Log("[w] passo %d/%d: PlayerScript offsets 0x144/0x145/0x10A4", g_selStep, total);
            if (!g_offIdx) {
                uintptr_t b = (uintptr_t)SDK::g_localPlayer;
                if (slot == 0)      SafeWrite<unsigned char>(b + kLoPrim, (unsigned char)(live & 0xFF));
                else if (slot == 1) SafeWrite<unsigned char>(b + kLoSec, (unsigned char)(live & 0xFF));
                else                SafeWrite<int>(b + kLoGad, live);
            }
            ++g_selStep;
            return;
        }

        if (g_selStep == g_loCN + 1) {
            Log("[w] passo %d/%d: fpsGunView+0x38=gun +0x48=%d", g_selStep, total, slot);
            void* fv = FpsGunView();
            if (!g_offFv && fv && LooksLikeObj(gun)) {
                SafeWrite<void*>((uintptr_t)fv + kFvHeld, gun);
                SafeWrite<int>((uintptr_t)fv + kFvSlot, slot);
            }
            ++g_selStep;
            return;
        }

        Log("[w] passo %d/%d: equip+prefs (fim)", g_selStep, total);
        if (g_selForceEquip || g_lastEquip != live || g_lastEquipSlot != slot) {
            g_selForceEquip = false;
            g_lastEquip = live;
            g_lastEquipSlot = slot;
            CallEquip(slot, live, wtype);
        }
        else {
            Log("[w] equip PULADO (mesma arma/slot idx=%d slot=%d)", live, slot);
        }
        PrefsApply(wtype, nameStr);
        LogLoadoutReadback();
        g_availNext = 0;                       // >>> v43.22: reaplica available ja'
        g_selStep = -1;
        g_selDone = true;
        Log("[unlock] FIM passada (sem restart em 4s)");
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

    // >>> v43.22: cada escritor tem portao. ApplyAvailable() varria ~65 armas
    //     POR FRAME; agora 1x/2s. Anexos e ints do player: 1x/250ms.
    static void ApplyAll() {
        if (!g_arr || g_arrLen <= 0) return;
        const ULONGLONG now = GetTickCount64();

        if (now >= g_availNext) {
            g_availNext = now + 2000;
            if (C.unlockAll || C.unlockWeapons) {
                const double t = QpcMs();
                ApplyAvailable();
                g_pfAvail += QpcMs() - t;
            }
        }
        if (now >= g_attachNext) {
            g_attachNext = now + 250;
            if (C.unlockAll || C.unlockSkins || C.unlockAttachments || C.skinChanger) {
                const double t = QpcMs();
                ApplyAttachments();
                g_pfAtt += QpcMs() - t;
            }
        }
        if (now >= g_intsNext) {
            g_intsNext = now + 250;
            if (C.unlockAll || C.unlockCurrency || C.unlockLevel) {
                const double t = QpcMs();
                ApplyPlayerInts();
                g_pfInts += QpcMs() - t;
            }
        }
        ApplyLoadout();
        if (C.applySelection || g_selForce) {
            const double t = QpcMs();
            ApplySelection();
            g_pfSel += QpcMs() - t;
        }
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
                    const char* tn = TN(t);
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
        case 0: Dupe::SetStage(1); break;
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
        CollectPrefsKeys(false);
        Dupe::EnsureLoaded();
        g_dupeEnsured = true;
        g_installed = true;
        g_availNext = 0;
        Log("[unlock] install %s: arr@0x%X len=%d | armas=%d camos=%d chars=%d",
            BFI_UNLOCK_VER, g_offArr, g_arrLen,
            (int)Dupe::g_weapons.size(), (int)Dupe::g_camos.size(), (int)Dupe::g_chars.size());
        if (g_arrLen > 0 && Dupe::WeaponCount() == 0) {
            Log("[unlock] auto-build: armas no jogo=%d, lista vazia -> coletando", g_arrLen);
            __try {
                CollectWeapons();
                CollectAttachs();
                CollectCamos();
                CollectChars();
                Dupe::Save();
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                Log("[unlock] auto-build: EXCECAO durante coleta");
            }
            Log("[unlock] auto-build: armas=%d camos=%d chars=%d",
                (int)Dupe::g_weapons.size(), (int)Dupe::g_camos.size(), (int)Dupe::g_chars.size());
        }
        if (C.accAuto && g_mnyN > 0) {
            MoneyUnlockAll();
            Log("[unlock] install: mirrors travados e forcados (%d espelhos)", g_mnyN);
        }
        return true;
    }

    void BuildAllNow() {
        ResolveExtra();
        Dupe::g_wantBuild.store(false);
        Dupe::SetStage(0);

        if (!SDK::g_localPlayer || !SDK::kPlayerScript || !SDK::kGunStats) {
            Dupe::SetStatus("sem player - entre numa partida");
            Log("[dupe] build direta: sem player");
            return;
        }
        if (!g_installed || !g_arr || g_arrLen <= 0) {
            if (!Install()) {
                Dupe::SetStatus("sem armas - entre numa partida");
                Log("[dupe] build direta: install falhou");
                return;
            }
        }

        __try {
            Log("[dupe] build direta: inicio (arr len=%d)", g_arrLen);
            CollectWeapons();
            CollectAttachs();
            CollectCamos();
            CollectChars();
            if (Dupe::Save())
                Dupe::SetStatus("salvo: %d armas, %d camos, %d chars",
                    (int)Dupe::g_weapons.size(), (int)Dupe::g_camos.size(), (int)Dupe::g_chars.size());
            else
                Dupe::SetStatus("falha ao salvar");
            Log("[dupe] build direta: armas=%d camos=%d chars=%d",
                (int)Dupe::g_weapons.size(), (int)Dupe::g_camos.size(), (int)Dupe::g_chars.size());
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            Dupe::SetStatus("erro na build direta");
            Log("[dupe] build direta: EXCECAO");
        }
        g_availNext = 0;
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
        RevalidateArray();
        ApplyAll();
    }

    // >>> v43.22: Tick separado em TickBody para o perfil medir o total real.
    static void TickBody() {
        const bool anyMoney = C.accAuto || C.moneyApplyNow || C.moneyRescan || C.moneyWritePrefs;
        const bool any = C.unlockAll || C.unlockWeapons || C.unlockSkins || C.unlockAttachments ||
            C.unlockCurrency || C.unlockLevel || C.unlockChars || C.applySelection ||
            C.skinChanger || C.applySlots || C.fillLoadoutSlots || anyMoney || g_selForce;
        if (!any && !Dupe::Wants()) return;

        {
            const double t = QpcMs();
            __try { MoneyTick(); }
            __except (EXCEPTION_EXECUTE_HANDLER) { Log("[unlock] AV contido no MoneyTick"); }
            g_pfMoney += QpcMs() - t;
        }

        if (!SDK::g_localPlayer) return;

        if (Dupe::Wants()) {
            if (!EnsureInstalled()) return;
            BuildStep();
            return;
        }
        const double t = QpcMs();
        __try { TickInner(); }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            static int n = 0;
            if (n++ < 10) Log("[unlock] AV contido no Tick");
        }
        g_pfInner += QpcMs() - t;
    }

    void Tick() {
        const double t0 = QpcMs();
        TickBody();
        g_pfTick += QpcMs() - t0;
        ++g_pfN;
        PfLog();
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
                const char* tn = TN(t);
                Log("[dump]   %s::%s %s @0x%X%s",
                    il2cpp_class_get_name(c), il2cpp_field_get_name(f), tn,
                    (unsigned)il2cpp_field_get_offset(f), IsStatic(f) ? " [static]" : "");
            }
        }
    }

    static void DumpObjFields(void* o, const char* tag, int maxFields) {
        if (!LooksLikeObj(o)) return;
        Il2CppClass* k = il2cpp_object_get_class((Il2CppObject*)o);
        Log("[loja] %s: obj %p classe=%s", tag, o, k ? il2cpp_class_get_name(k) : "?");
        int n = 0;
        for (Il2CppClass* c = k; c && n < maxFields; c = il2cpp_class_get_parent(c)) {
            void* it = nullptr;
            for (;;) {
                FieldInfo* f = il2cpp_class_get_fields(c, &it);
                if (!f) break;
                if (IsStatic(f)) continue;
                if (n++ >= maxFields) break;
                const Il2CppType* t = il2cpp_field_get_type(f);
                const char* tn = TN(t);
                uint32_t off = (uint32_t)il2cpp_field_get_offset(f);
                if (!off) continue;
                const char* fn = il2cpp_field_get_name(f);
                char tl[96]; Lower(tl, sizeof(tl), tn);
                if (strstr(tn, "String")) {
                    char s[256]; s[0] = 0;
                    CStr(SafeRead<void*>((uintptr_t)o + off, nullptr), s, sizeof(s));
                    Log("[loja]   %s.%s = \"%s\"", tag, fn, s);
                }
                else if (strstr(tn, "[]") || strstr(tn, "List<")) {
                    void* a = SafeRead<void*>((uintptr_t)o + off, nullptr);
                    Log("[loja]   %s.%s %s len=%d", tag, fn, tn, LooksLikeObj(a) ? ArrLen((Il2CppArray*)a) : -1);
                }
                else if (strstr(tl, "single") || strstr(tl, "double"))
                    Log("[loja]   %s.%s %s = %.4f", tag, fn, tn, SafeRead<float>((uintptr_t)o + off, -999.f));
                else if (strstr(tl, "boolean"))
                    Log("[loja]   %s.%s %s = %d", tag, fn, tn, SafeRead<unsigned char>((uintptr_t)o + off, 0));
                else if (strstr(tl, "int32") || strstr(tl, "byte"))
                    Log("[loja]   %s.%s %s = %d", tag, fn, tn, SafeRead<int>((uintptr_t)o + off, -999));
                else if (strstr(tn, "ObscuredInt")) {
                    const int key = SafeRead<int>((uintptr_t)o + off + 0, 0);
                    const int hid = SafeRead<int>((uintptr_t)o + off + 4, 0);
                    Log("[loja]   %s.%s %s = %d (obf)", tag, fn, tn, hid ^ key);
                }
                else {
                    void* p = SafeRead<void*>((uintptr_t)o + off, nullptr);
                    if (LooksLikeObj(p)) Log("[loja]   %s.%s %s = OBJ %p", tag, fn, tn, p);
                    else                 Log("[loja]   %s.%s %s = raw %d", tag, fn, tn, SafeRead<int>((uintptr_t)o + off, -999));
                }
            }
        }
    }

    static void DumpAccountChain() {
        Il2CppClass* ak = SDK::FindClass("AccountManager");
        if (!ak) { Log("[loja] AccountManager NAO achado"); return; }
        void* am = StaticInstanceOf(ak);
        if (!LooksLikeObj(am)) { Log("[loja] AccountManager::Instance ausente"); return; }

        char js[4096]; js[0] = 0;
        if (AccountJson(js, sizeof(js))) {
            Log("[loja] JSON conta (%d chars): %s", (int)strlen(js), js);
        }
        else Log("[loja] JSON conta: campo EIMOMMHLNPO vazio/nao achado");

        AccountJsonRefresh();
        Log("[loja] JSON -> money=%lld gold=%lld xp=%lld", g_accMoney, g_accGold, g_accXp);

        uint32_t off = MnyOffExact(am, "accountInfo");
        if (!off) off = 0x58;
        void* ai = off ? SafeRead<void*>((uintptr_t)am + off, nullptr) : nullptr;
        if (LooksLikeObj(ai)) {
            Log("[loja] accountInfo off=0x%X -> %p", off, ai);
            DumpObjFields(ai, "accountInfo", 32);
            uint32_t ci = MnyOffExact(ai, "customInfo");
            if (!ci) ci = 0x18;
            void* cst = ci ? SafeRead<void*>((uintptr_t)ai + ci, nullptr) : nullptr;
            if (LooksLikeObj(cst)) {
                Log("[loja] accountInfo.customInfo off=0x%X -> %p", ci, cst);
                DumpObjFields(cst, "customInfo", 64);
                uint32_t bo = MnyOffExact(cst, "bfAccountInfo");
                if (!bo) bo = 0x10;
                void* bf = bo ? SafeRead<void*>((uintptr_t)cst + bo, nullptr) : nullptr;
                if (LooksLikeObj(bf)) {
                    Log("[loja] customInfo.bfAccountInfo off=0x%X -> %p", bo, bf);
                    Log("[loja]   bfAccountInfo: money=%d gold=%d cases=%d xp=%d",
                        SafeRead<int>((uintptr_t)bf + 0x14, -1),
                        SafeRead<int>((uintptr_t)bf + 0x44, -1),
                        SafeRead<int>((uintptr_t)bf + 0x30, -1),
                        SafeRead<int>((uintptr_t)bf + 0x18, -1));
                    DumpObjFields(bf, "bfAccountInfo", 48);
                }
                else Log("[loja] customInfo.bfAccountInfo off=0x%X = %p (nao obj)", bo, bf);
            }
            else Log("[loja] accountInfo.customInfo off=0x%X = %p (nao obj)", ci, cst);
        }
        else Log("[loja] accountInfo off=0x%X = %p (nao obj)", off, ai);

        static const char* cf[] = { "accountCustomInfoJSON", "BMJJMJJIKPB" };
        for (int i = 0; i < 2; ++i) {
            uint32_t o2 = MnyOffExact(am, cf[i]);
            void* c2 = o2 ? SafeRead<void*>((uintptr_t)am + o2, nullptr) : nullptr;
            if (LooksLikeObj(c2)) {
                Log("[loja] %s off=0x%X -> %p", cf[i], o2, c2);
                DumpObjFields(c2, cf[i], 48);
            }
        }
        Il2CppClass* mk = SDK::FindClass("AS_MySQLField");
        if (mk) DumpClassFields("AS_MySQLField");
    }

    static void DumpMoneyObjects() {
        DumpAccountChain();

        Il2CppClass* ck = SDK::FindClass("CreditsManager");
        if (ck) {
            void* cm = StaticInstanceOf(ck);
            if (LooksLikeObj(cm)) DumpObjFields(cm, "CreditsManager", 24);
        }
        Il2CppClass* sk = SDK::FindClass("Sidebar");
        if (sk) {
            void* sb = StaticInstanceOf(sk);
            if (LooksLikeObj(sb)) DumpObjFields(sb, "Sidebar", 32);
        }
        Il2CppClass* bk = SDK::FindClass("BF_AccountInfo");
        if (bk) {
            int len = 0;
            void* arr = FindObjsOfType(bk, len);
            Log("[loja] BF_AccountInfo: %d instancia(s)", len);
            for (int e = 0; e < len && e < 2; ++e) {
                void* o = ArrElem((Il2CppArray*)arr, e);
                if (LooksLikeObj(o)) DumpObjFields(o, "BF_AccountInfo", 40);
            }
        }
    }

    static void* AccountBfObj() {
        Il2CppClass* ak = SDK::FindClass("AccountManager");
        if (!ak) return nullptr;
        void* am = StaticInstanceOf(ak);
        if (!LooksLikeObj(am)) return nullptr;
        uint32_t o = MnyOffExact(am, "accountInfo"); if (!o) o = 0x58;
        void* ai = SafeRead<void*>((uintptr_t)am + o, nullptr);
        if (!LooksLikeObj(ai)) return nullptr;
        o = MnyOffExact(ai, "customInfo"); if (!o) o = 0x18;
        void* ci = SafeRead<void*>((uintptr_t)ai + o, nullptr);
        if (!LooksLikeObj(ci)) return nullptr;
        o = MnyOffExact(ci, "bfAccountInfo"); if (!o) o = 0x10;
        void* bf = SafeRead<void*>((uintptr_t)ci + o, nullptr);
        return LooksLikeObj(bf) ? bf : nullptr;
    }

    void DumpMirrors() {
        ResolveExtra();
        AccountJsonRefresh();
        Log("=== ESPELHOS DA CARTEIRA (%d) ===", g_mnyN);
        MoneyRefresh();
        for (int i = 0; i < g_mnyN; ++i) {
            const MnyMirror& m = g_mny[i];
            if (!m.ok) { Log("[mirror]   [%d] %s = NAO RESOLVIDO", i, m.label); continue; }
            const long long v = MnyRead(m);
            Log("[mirror]   [%d] %s %s::%s tipo=%s kind=%d %s | valor=%lld",
                i, m.label, m.cls, m.fld, m.typeName ? m.typeName : "?", m.kind,
                m.isStatic ? "[static]" : "inst", v);
        }
        Log("[mirror]   REAL (json conta): money=%lld gold=%lld xp=%lld",
            g_accMoney, g_accGold, g_accXp);
        void* bf = AccountBfObj();
        if (bf) Log("[mirror]   REAL (bfAccountInfo): money=%d gold=%d cases=%d xp=%d",
            SafeRead<int>((uintptr_t)bf + 0x14, -1),
            SafeRead<int>((uintptr_t)bf + 0x44, -1),
            SafeRead<int>((uintptr_t)bf + 0x30, -1),
            SafeRead<int>((uintptr_t)bf + 0x18, -1));
        else    Log("[mirror]   REAL (bfAccountInfo): ausente");
        Log("=== FIM ESPELHOS ===");
    }

    void DumpShop() {
        ResolveExtra();
        static const char* cls[] = {
            "CreditsManager", "AccountManager", "CamoCaseManager",
            "LoadoutManager", "LoadoutAndShop", "Sidebar", "BF_AccountInfo",
            "AS_AccountInfo", "AS_CustomInfo", "AS_MySQLField"
        };
        for (size_t i = 0; i < sizeof(cls) / sizeof(cls[0]); ++i) DumpClassFields(cls[i]);
        DumpMoneyObjects();
        DumpMirrors();
    }

    static void DumpMySqlFields(void* list) {
        if (!LooksLikeObj(list)) return;
        void* items = ListItems(list);
        const int n = ListSize(list);
        Log("[credit] accountInfo.fields: %d entrada(s)", n);
        for (int i = 0; i < n && i < 24; ++i) {
            void* e = LooksLikeObj(items)
                ? SafeRead<void*>((uintptr_t)items + 0x20 + (size_t)i * 8, nullptr) : nullptr;
            if (!LooksLikeObj(e)) continue;
            DumpObjFields(e, "mysqlField", 12);
        }
    }

    void DumpCredits() {
        ResolveExtra();
        Log("=== DUMP CREDITOS %s ===", BFI_UNLOCK_VER);
        DumpClassFields("CreditsManager");
        DumpClassFields("AccountManager");
        DumpClassFields("AS_AccountInfo");
        DumpClassFields("AS_MySQLField");
        DumpClassFields("BF_AccountInfo");
        DumpClassFields("Sidebar");

        Il2CppClass* ak = SDK::FindClass("AccountManager");
        if (ak) {
            void* am = StaticInstanceOf(ak);
            if (LooksLikeObj(am)) {
                DumpObjFields(am, "AccountManager", 64);
                Log("[credit] JBNJGEGJGAD=%d FIOMIPONAAM=%d GEEFAPKBHJO(obf)=%d",
                    SafeRead<int>((uintptr_t)am + 0x34, -1),
                    SafeRead<int>((uintptr_t)am + 0xA4, -1),
                    (SafeRead<int>((uintptr_t)am + 0x20, 0) ^ SafeRead<int>((uintptr_t)am + 0x24, 0)));
                uint32_t o = MnyOffExact(am, "accountInfo"); if (!o) o = 0x58;
                void* ai = SafeRead<void*>((uintptr_t)am + o, nullptr);
                if (LooksLikeObj(ai)) {
                    uint32_t fo = MnyOffExact(ai, "fields");
                    if (fo) DumpMySqlFields(SafeRead<void*>((uintptr_t)ai + fo, nullptr));
                }
            }
        }

        Il2CppClass* ck = SDK::FindClass("CreditsManager");
        if (ck) {
            void* o = StaticInstanceOf(ck);
            if (LooksLikeObj(o)) DumpObjFields(o, "CreditsManager", 64);
            else Log("[credit] CreditsManager sem instancia estatica");
            for (Il2CppClass* c = ck; c; c = il2cpp_class_get_parent(c)) {
                void* it = nullptr;
                for (;;) {
                    FieldInfo* f = il2cpp_class_get_fields(c, &it);
                    if (!f) break;
                    if (!IsStatic(f)) continue;
                    const Il2CppType* t = il2cpp_field_get_type(f);
                    const char* tn = TN(t);
                    if (!tn) continue;
                    if (!strstr(tn, "Int32") && !strstr(tn, "ObscuredInt") && !strstr(tn, "Int64")) continue;
                    int v = 0; StaticGetRaw(f, &v, sizeof(int));
                    Log("[credit] STATIC %s::%s %s = %d",
                        il2cpp_class_get_name(c), il2cpp_field_get_name(f), tn, v);
                }
            }
        }

        Il2CppClass* sk = SDK::FindClass("Sidebar");
        if (sk) {
            void* sb = StaticInstanceOf(sk);
            if (LooksLikeObj(sb)) DumpObjFields(sb, "Sidebar", 48);
            for (Il2CppClass* c = sk; c; c = il2cpp_class_get_parent(c)) {
                void* it = nullptr;
                for (;;) {
                    FieldInfo* f = il2cpp_class_get_fields(c, &it);
                    if (!f) break;
                    if (!IsStatic(f)) continue;
                    const Il2CppType* t = il2cpp_field_get_type(f);
                    const char* tn = TN(t);
                    if (!tn) continue;
                    if (!strstr(tn, "Int32") && !strstr(tn, "ObscuredInt") && !strstr(tn, "Int64")) continue;
                    int v = 0; StaticGetRaw(f, &v, sizeof(int));
                    Log("[credit] STATIC %s::%s %s = %d",
                        il2cpp_class_get_name(c), il2cpp_field_get_name(f), tn, v);
                }
            }
        }

        DumpMirrors();
        Log("=== FIM DUMP CREDITOS ===");
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
        DumpMoneyObjects();
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