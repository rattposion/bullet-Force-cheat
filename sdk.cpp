// ============================================================================
//  sdk.cpp — UTF-16->UTF-8, SyncVar, bones, LOS via Physics.Raycast
//  v6.8 — FIX LOS:
//    [FIXL1] OwnerKindUncached: walk de parent até 15 níveis, checando
//            localTf e targetTf em CADA nó + fallback por distância.
//    [FIXL2] HasLineOfSightExRaw: loop avança pelo hit REAL (hitDist+0.05),
//            não por 0.30 fixo. Sai do próprio corpo do player.
//    [FIXL3] kind==0 com remaining<1.5m -> considera VIS (evita falso bloqueio).
//    Mantidos todos os FIXT1..FIXT5 e FIX1..FIX5 anteriores.
// ============================================================================
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>
#include <unordered_map>
#include <mutex>
#include <atomic>

#include "il2cpp.h"
#include "il2cpp_api.h"
#include "common.h"
#include "memory.h"
#include "config.h"
#include "sdk.h"
#include "hooks.h"

extern HMODULE g_hModule;
const char* g_phase = "boot";

extern "C" {
    Il2CppDomain* (*il2cpp_domain_get)() = nullptr;
    const Il2CppAssembly** (*il2cpp_domain_get_assemblies)(const Il2CppDomain*, size_t*) = nullptr;
    Il2CppImage* (*il2cpp_assembly_get_image)(const Il2CppAssembly*) = nullptr;
    size_t(*il2cpp_image_get_class_count)(const Il2CppImage*) = nullptr;
    Il2CppClass* (*il2cpp_image_get_class)(const Il2CppImage*, size_t) = nullptr;
    const char* (*il2cpp_class_get_name)(const Il2CppClass*) = nullptr;
    const char* (*il2cpp_class_get_namespace)(const Il2CppClass*) = nullptr;
    Il2CppClass* (*il2cpp_class_get_parent)(const Il2CppClass*) = nullptr;
    Il2CppImage* (*il2cpp_class_get_image)(const Il2CppClass*) = nullptr;
    Il2CppType* (*il2cpp_class_get_type)(const Il2CppClass*) = nullptr;
    Il2CppClass* (*il2cpp_class_from_type)(const Il2CppType*) = nullptr;
    FieldInfo* (*il2cpp_class_get_field_from_name)(const Il2CppClass*, const char*) = nullptr;
    FieldInfo* (*il2cpp_class_get_fields)(const Il2CppClass*, void**) = nullptr;
    MethodInfo* (*il2cpp_class_get_method_from_name)(const Il2CppClass*, const char*, int) = nullptr;
    const MethodInfo* (*il2cpp_class_get_methods)(const Il2CppClass*, void**) = nullptr;
    size_t(*il2cpp_field_get_offset)(const FieldInfo*) = nullptr;
    const char* (*il2cpp_field_get_name)(const FieldInfo*) = nullptr;
    Il2CppType* (*il2cpp_field_get_type)(const FieldInfo*) = nullptr;
    void (*il2cpp_field_static_get_value)(const FieldInfo*, void*) = nullptr;
    const char* (*il2cpp_method_get_name)(const MethodInfo*) = nullptr;
    uint32_t(*il2cpp_method_get_param_count)(const MethodInfo*) = nullptr;
    const Il2CppType* (*il2cpp_method_get_param)(const MethodInfo*, uint32_t) = nullptr;
    Il2CppClass* (*il2cpp_object_get_class)(const Il2CppObject*) = nullptr;
    Il2CppObject* (*il2cpp_object_new)(const Il2CppClass*) = nullptr;
    void* (*il2cpp_object_unbox)(const Il2CppObject*) = nullptr;
    Il2CppObject* (*il2cpp_runtime_invoke)(const MethodInfo*, void*, void**, Il2CppException**) = nullptr;
    Il2CppString* (*il2cpp_string_new)(const char*) = nullptr;
    const char* (*il2cpp_string_chars)(const Il2CppString*) = nullptr;
    uint32_t(*il2cpp_array_length)(const Il2CppArray*) = nullptr;
    Il2CppArray* (*il2cpp_array_new)(const Il2CppClass*, size_t) = nullptr;
    Il2CppDomain* (*il2cpp_thread_attach)(const Il2CppDomain*) = nullptr;
    Il2CppObject* (*il2cpp_type_get_object)(const Il2CppType*) = nullptr;
    char* (*il2cpp_type_get_name)(const Il2CppType*) = nullptr;

    Il2CppType* (*il2cpp_method_get_return_type)(const MethodInfo*) = nullptr;
}

namespace IL2CPP_API {
    static int g_resolved = 0, g_missing = 0;
    bool Resolve() {
        HMODULE ga = GetModuleHandleA("GameAssembly.dll");
        if (!ga) { Log("[il2cpp] GameAssembly.dll nao carregada"); return false; }
        struct Entry { const char* name; void** slot; };
        static const Entry tbl[] = {
            {"il2cpp_domain_get", (void**)&il2cpp_domain_get},
            {"il2cpp_domain_get_assemblies", (void**)&il2cpp_domain_get_assemblies},
            {"il2cpp_assembly_get_image", (void**)&il2cpp_assembly_get_image},
            {"il2cpp_image_get_class_count", (void**)&il2cpp_image_get_class_count},
            {"il2cpp_image_get_class", (void**)&il2cpp_image_get_class},
            {"il2cpp_class_get_name", (void**)&il2cpp_class_get_name},
            {"il2cpp_class_get_namespace", (void**)&il2cpp_class_get_namespace},
            {"il2cpp_class_get_parent", (void**)&il2cpp_class_get_parent},
            {"il2cpp_class_get_image", (void**)&il2cpp_class_get_image},
            {"il2cpp_class_get_type", (void**)&il2cpp_class_get_type},
            {"il2cpp_class_from_type", (void**)&il2cpp_class_from_type},
            {"il2cpp_class_get_field_from_name", (void**)&il2cpp_class_get_field_from_name},
            {"il2cpp_class_get_fields", (void**)&il2cpp_class_get_fields},
            {"il2cpp_class_get_method_from_name", (void**)&il2cpp_class_get_method_from_name},
            {"il2cpp_class_get_methods", (void**)&il2cpp_class_get_methods},
            {"il2cpp_field_get_offset", (void**)&il2cpp_field_get_offset},
            {"il2cpp_field_get_name", (void**)&il2cpp_field_get_name},
            {"il2cpp_field_get_type", (void**)&il2cpp_field_get_type},
            {"il2cpp_field_static_get_value", (void**)&il2cpp_field_static_get_value},
            {"il2cpp_method_get_name", (void**)&il2cpp_method_get_name},
            {"il2cpp_method_get_param_count", (void**)&il2cpp_method_get_param_count},
            {"il2cpp_method_get_param", (void**)&il2cpp_method_get_param},
            {"il2cpp_method_get_return_type", (void**)&il2cpp_method_get_return_type},
            {"il2cpp_object_get_class", (void**)&il2cpp_object_get_class},
            {"il2cpp_object_new", (void**)&il2cpp_object_new},
            {"il2cpp_object_unbox", (void**)&il2cpp_object_unbox},
            {"il2cpp_runtime_invoke", (void**)&il2cpp_runtime_invoke},
            {"il2cpp_string_new", (void**)&il2cpp_string_new},
            {"il2cpp_string_chars", (void**)&il2cpp_string_chars},
            {"il2cpp_array_length", (void**)&il2cpp_array_length},
            {"il2cpp_array_new", (void**)&il2cpp_array_new},
            {"il2cpp_thread_attach", (void**)&il2cpp_thread_attach},
            {"il2cpp_type_get_object", (void**)&il2cpp_type_get_object},
            {"il2cpp_type_get_name", (void**)&il2cpp_type_get_name}
        };
        g_resolved = g_missing = 0;
        for (size_t i = 0; i < sizeof(tbl) / sizeof(tbl[0]); ++i) {
            FARPROC p = GetProcAddress(ga, tbl[i].name);
            if (p) { *tbl[i].slot = (void*)p; ++g_resolved; }
            else { *tbl[i].slot = nullptr; ++g_missing; Log("[il2cpp] faltando: %s", tbl[i].name); }
        }

        if (!il2cpp_class_from_type) {
            FARPROC p = GetProcAddress(ga, "il2cpp_class_from_il2cpp_type");
            if (p) {
                il2cpp_class_from_type = (decltype(il2cpp_class_from_type))p;
                Log("[il2cpp] class_from_type resolvido pelo nome longo");
            }
        }

        Log("[il2cpp] resolvidas %d, faltando %d", g_resolved, g_missing);
        return g_resolved > 0;
    }
}

#define SE_BEGIN __try
#define SE_END   __except (EXCEPTION_EXECUTE_HANDLER)

static void StrToUtf8(Il2CppString* s, char* out, int outSize) {
    if (!out || outSize < 2) return;
    out[0] = 0;
    if (!s) return;
    SE_BEGIN{
        int len = *(int*)((uintptr_t)s + 0x10);
        if (len <= 0) { out[0] = 0; }
        else {
            int maxc = outSize - 1;
            if (len > maxc) len = maxc;
            const wchar_t* src = (const wchar_t*)((uintptr_t)s + 0x14);
            int n = WideCharToMultiByte(CP_UTF8, 0, src, len, out, maxc, nullptr, nullptr);
            out[(n > 0 && n < outSize) ? n : 0] = 0;
        }
    }
    SE_END{ out[0] = 0; }
}

static Il2CppDomain* se_domain_get() {
    Il2CppDomain* r = nullptr;
    SE_BEGIN{ if (il2cpp_domain_get) r = il2cpp_domain_get(); } SE_END{ r = nullptr; }
    return r;
}
static const Il2CppAssembly** se_domain_get_assemblies(Il2CppDomain* d, size_t* n) {
    const Il2CppAssembly** r = nullptr;
    SE_BEGIN{ if (il2cpp_domain_get_assemblies) r = il2cpp_domain_get_assemblies(d, n); } SE_END{ r = nullptr; }
    return r;
}
static Il2CppImage* se_assembly_get_image(const Il2CppAssembly* a) {
    Il2CppImage* r = nullptr;
    SE_BEGIN{ if (il2cpp_assembly_get_image) r = il2cpp_assembly_get_image(a); } SE_END{ r = nullptr; }
    return r;
}
static size_t se_image_class_count(Il2CppImage* img) {
    size_t r = 0;
    SE_BEGIN{ if (il2cpp_image_get_class_count) r = il2cpp_image_get_class_count(img); } SE_END{ r = 0; }
    return r;
}
static Il2CppClass* se_image_get_class(Il2CppImage* img, size_t i) {
    Il2CppClass* r = nullptr;
    SE_BEGIN{ if (il2cpp_image_get_class) r = il2cpp_image_get_class(img, i); } SE_END{ r = nullptr; }
    return r;
}
static const char* se_class_name(Il2CppClass* k) {
    const char* r = nullptr;
    SE_BEGIN{ if (il2cpp_class_get_name) r = il2cpp_class_get_name(k); } SE_END{ r = nullptr; }
    return r;
}
static const char* se_class_namespace(Il2CppClass* k) {
    const char* r = nullptr;
    SE_BEGIN{ if (il2cpp_class_get_namespace) r = il2cpp_class_get_namespace(k); } SE_END{ r = nullptr; }
    return r;
}
static Il2CppClass* se_class_parent(Il2CppClass* k) {
    Il2CppClass* r = nullptr;
    SE_BEGIN{ if (il2cpp_class_get_parent) r = il2cpp_class_get_parent(k); } SE_END{ r = nullptr; }
    return r;
}
static MethodInfo* se_class_get_method_from_name(Il2CppClass* k, const char* n, int a) {
    MethodInfo* r = nullptr;
    SE_BEGIN{ if (il2cpp_class_get_method_from_name) r = il2cpp_class_get_method_from_name(k, n, a); } SE_END{ r = nullptr; }
    return r;
}
static FieldInfo* se_class_get_field_from_name(Il2CppClass* k, const char* n) {
    FieldInfo* r = nullptr;
    SE_BEGIN{ if (il2cpp_class_get_field_from_name) r = il2cpp_class_get_field_from_name(k, n); } SE_END{ r = nullptr; }
    return r;
}
static FieldInfo* se_class_get_fields(Il2CppClass* k, void** iter) {
    FieldInfo* r = nullptr;
    SE_BEGIN{ if (il2cpp_class_get_fields) r = il2cpp_class_get_fields(k, iter); } SE_END{ r = nullptr; }
    return r;
}
static const char* se_field_name(FieldInfo* f) {
    const char* r = nullptr;
    SE_BEGIN{ if (il2cpp_field_get_name) r = il2cpp_field_get_name(f); } SE_END{ r = nullptr; }
    return r;
}
static size_t se_field_offset(FieldInfo* f) {
    size_t r = 0;
    SE_BEGIN{ if (il2cpp_field_get_offset) r = il2cpp_field_get_offset(f); } SE_END{ r = 0; }
    return r;
}
static void se_field_static_get_value(FieldInfo* f, void* out) {
    SE_BEGIN{ if (il2cpp_field_static_get_value) il2cpp_field_static_get_value(f, out); } SE_END{}
}
static Il2CppClass* se_object_get_class(Il2CppObject* o) {
    if (!o) return nullptr;
    Il2CppClass* r = nullptr;
    SE_BEGIN{
        auto fn = il2cpp_object_get_class;
        if (fn) r = fn(o);
    } SE_END{ r = nullptr; }
    return r;
}
static Il2CppObject* se_object_new(Il2CppClass* k) {
    Il2CppObject* r = nullptr;
    SE_BEGIN{ if (il2cpp_object_new) r = il2cpp_object_new(k); } SE_END{ r = nullptr; }
    return r;
}
static void* se_object_unbox(Il2CppObject* o) {
    void* r = nullptr;
    SE_BEGIN{ if (il2cpp_object_unbox) r = il2cpp_object_unbox(o); } SE_END{ r = nullptr; }
    return r;
}

static thread_local bool t_il2cppAttached = false;
static void EnsureThreadAttached() {
    if (t_il2cppAttached) return;
    if (!il2cpp_thread_attach || !il2cpp_domain_get) return;

    Il2CppDomain* d = nullptr;
    SE_BEGIN{ d = il2cpp_domain_get(); } SE_END{ d = nullptr; }
    if (!d) return;

    Il2CppDomain* attached = nullptr;
    SE_BEGIN{ attached = il2cpp_thread_attach(d); } SE_END{ attached = nullptr; }
    t_il2cppAttached = (attached != nullptr);
}

static Il2CppObject* se_runtime_invoke(const MethodInfo* m, void* o, void** args) {
    if (!m) return nullptr;
    EnsureThreadAttached();
    Il2CppObject* r = nullptr;
    SE_BEGIN{ if (il2cpp_runtime_invoke) r = il2cpp_runtime_invoke(m, o, args, nullptr); } SE_END{ r = nullptr; }
    return r;
}
static Il2CppDomain* se_thread_attach(Il2CppDomain* d) {
    Il2CppDomain* r = nullptr;
    SE_BEGIN{ if (il2cpp_thread_attach) r = il2cpp_thread_attach(d); } SE_END{ r = nullptr; }
    return r;
}
static Il2CppObject* se_type_get_object(Il2CppType* t) {
    Il2CppObject* r = nullptr;
    SE_BEGIN{ if (il2cpp_type_get_object) r = il2cpp_type_get_object(t); } SE_END{ r = nullptr; }
    return r;
}
static Il2CppType* se_class_get_type(Il2CppClass* k) {
    Il2CppType* r = nullptr;
    SE_BEGIN{ if (il2cpp_class_get_type) r = il2cpp_class_get_type(k); } SE_END{ r = nullptr; }
    return r;
}
static bool se_read_bytes(void* src, void* dst, size_t len) {
    SE_BEGIN{ memcpy(dst, src, len); } SE_END{ return false; }
    return true;
}
static bool se_is_exec(void* p) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!p || VirtualQuery(p, &mbi, sizeof(mbi)) == 0) return false;
    return (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
        PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}
static void* se_array_at(Il2CppArray* a, uint32_t i) {
    void* r = nullptr;
    SE_BEGIN{ r = *(void**)((uintptr_t)a + 0x20 + (size_t)i * sizeof(void*)); } SE_END{ r = nullptr; }
    return r;
}
static bool se_read_ptr(void* base, size_t off, void** out) {
    *out = nullptr;
    SE_BEGIN{ *out = *(void**)((uintptr_t)base + off); } SE_END{ *out = nullptr; return false; }
    return *out != nullptr;
}
static bool se_read_int(void* base, size_t off, int* out) {
    *out = 0;
    SE_BEGIN{ *out = *(int*)((uintptr_t)base + off); } SE_END{ *out = 0; return false; }
    return true;
}
static bool se_read_bool(void* base, size_t off) {
    unsigned char b = 0;
    SE_BEGIN{ b = *(unsigned char*)((uintptr_t)base + off); } SE_END{ b = 0; }
    return b != 0;
}
static float se_read_float(void* base, size_t off) {
    float r = 0.f;
    SE_BEGIN{ r = *(float*)((uintptr_t)base + off); } SE_END{ r = 0.f; }
    return r;
}
static bool se_read_vec3(void* base, size_t off, Vec3* out) {
    SE_BEGIN{ memcpy(out, (void*)((uintptr_t)base + off), sizeof(Vec3)); } SE_END{ return false; }
    return true;
}

// ===========================================================================
//  caches
// ===========================================================================
static std::mutex g_methodMx;
static std::mutex g_typeMx;

struct MethodCacheEnt { Il2CppClass* k; const char* n; int a; MethodInfo* m; };
static const int  kMethodCap = 256;
static MethodCacheEnt g_methodCache[kMethodCap];
static int        g_methodCacheN = 0;
static uint32_t   g_methodEvict = 0;

static MethodInfo* MethodCached(Il2CppClass* k, const char* name, int argc) {
    if (!k || !name) return nullptr;
    std::lock_guard<std::mutex> lk(g_methodMx);
    for (int i = 0; i < g_methodCacheN; ++i)
        if (g_methodCache[i].k == k && g_methodCache[i].a == argc && g_methodCache[i].n == name)
            return g_methodCache[i].m;

    MethodInfo* m = se_class_get_method_from_name(k, name, argc);

    int slot;
    if (g_methodCacheN < kMethodCap) slot = g_methodCacheN++;
    else { slot = (int)(g_methodEvict++ % (uint32_t)kMethodCap); }

    g_methodCache[slot].k = k;
    g_methodCache[slot].n = name;
    g_methodCache[slot].a = argc;
    g_methodCache[slot].m = m;
    return m;
}

struct TypeCacheEnt { const Il2CppType* t; const char* n; };
static const size_t kTypeCap = 512;
static TypeCacheEnt g_typeCache[kTypeCap];
static size_t        g_typeCacheN = 0;
static size_t        g_typeEvict = 0;

static const char* TypeNameCached(const Il2CppType* t) {
    if (!t || !il2cpp_type_get_name) return "?";
    std::lock_guard<std::mutex> lock(g_typeMx);
    for (size_t i = 0; i < g_typeCacheN; ++i)
        if (g_typeCache[i].t == t) return g_typeCache[i].n;

    const char* nm = il2cpp_type_get_name(t);
    if (!nm) nm = "?";

    size_t slot;
    if (g_typeCacheN < kTypeCap) slot = g_typeCacheN++;
    else { slot = g_typeEvict++ % kTypeCap; }

    g_typeCache[slot].t = t;
    g_typeCache[slot].n = nm;
    return nm;
}

// ===========================================================================
//  validacao de ponteiro
// ===========================================================================
static uint64_t g_plausStamp = 0;
static inline void PlausibleBeginFrame(uint64_t f) { g_plausStamp = f; }

static bool PlausibleObjRaw(const void* p) {
    uintptr_t v = (uintptr_t)p;
    if (v < 0x10000ull) return false;
    if (v > 0x00007FFFFFFFFFFFull) return false;
    if (v & 7) return false;
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery((void*)v, &mbi, sizeof(mbi)) == 0) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return true;
}

static bool PlausibleObj(const void* p) {
    uintptr_t v = (uintptr_t)p;
    if (v < 0x10000ull) return false;
    if (v > 0x00007FFFFFFFFFFFull) return false;
    if (v & 7) return false;

    struct Ent { const void* p; bool ok; uint64_t st; };
    static Ent memo[256];
    const uint64_t st = g_plausStamp;
    Ent& e = memo[(v >> 4) & 255];
    if (e.p == p && e.st == st) return e.ok;

    bool ok = PlausibleObjRaw(p);
    e.p = p; e.ok = ok; e.st = st;
    return ok;
}

bool ValidInstance_(const void* o) {
    uintptr_t p = (uintptr_t)o;
    if (p == 0) return false;
    if (p == (uintptr_t)-1) return false;
    if (p < 0x10000) return false;
    if (p > 0x00007FFFFFFFFFFF) return false;
    if ((p & 7) != 0) return false;

    if (!PlausibleObj(o)) return false;

    __try {
        void* k = nullptr;
        if (!se_read_ptr((void*)o, 0, &k) || !k) return false;
        if (!PlausibleObj(k)) return false;
        return se_class_name((Il2CppClass*)k) != nullptr;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

Il2CppClass* SafeClass_(Il2CppObject* o) {
    if (!o) return nullptr;
    if (!PlausibleObj(o)) return nullptr;
    Il2CppClass* k = se_object_get_class(o);
    if (!k || !PlausibleObj(k)) return nullptr;
    if (!se_class_name(k)) return nullptr;
    return k;
}

static Il2CppObject* SafeObjField(void* base, uint32_t off) {
    if (!base || !off) return nullptr;
    void* p = nullptr;
    if (!se_read_ptr(base, (size_t)off, &p) || !p) return nullptr;
    if (!PlausibleObj(p)) return nullptr;
    return (Il2CppObject*)p;
}

namespace SDK {

    std::unordered_map<std::string, Il2CppClass*> g_classes;
    std::mutex g_classesMx;

    Il2CppObject* g_camera = nullptr;
    Il2CppObject* g_local = nullptr;
    Il2CppObject* g_localPlayer = nullptr;
    Il2CppObject* g_aimTarget = nullptr;

    Target g_targets[MAX_TARGETS];
    int    g_targetCount = 0;
    int    g_localTeam = -1;
    int    dbg_drawn = 0;
    Vec3   g_localPos{ 0.f, 0.f, 0.f };
    CRITICAL_SECTION g_targetCs;
    uint64_t g_frameId = 0;

    std::atomic<int>  g_fireDirSlot{ -1 };
    std::atomic<bool> g_recoilHookActive{ false };
    bool g_rateIsRpm = false;

    bool  g_viewProjOk = false;
    float g_viewProj[16] = { 0 };
    static volatile LONG g_vpSeq = 0;

    uint32_t O_health = 0, O_alive = 0, O_team = 0, O_isLocal = 0, O_name = 0, O_weapon = 0;
    uint32_t O_vel = 0, O_speed = 0, O_rot = 0, O_rotation = 0, O_cam = 0, O_camTf = 0;
    uint32_t O_mCamera = 0, O_humanMeshAnimator = 0, O_humanMeshReferences = 0;
    uint32_t O_stats = 0, O_gunView = 0, O_gunStats = 0, O_gunStatsOnView = 0;
    uint32_t O_curAmmo = 0, O_maxAmmo = 0, O_reloading = 0, O_spread = 0, O_fireRate = 0, O_dmg = 0;
    static uint32_t O_localPlayerScript = 0;
    bool     O_rotationIsQuat = true;

    static int g_teamKind = 0;

    static bool g_rotIsSyncVar = false;
    static bool g_healthIsSync = false;
    static bool g_teamIsSync = false;
    static Quat g_savedPlayerRot{ 0.f, 0.f, 0.f, 1.f };

    static uint32_t O_gsMagSize = 0, O_gsAmmo = 0, O_gsGunObj = 0;
    static uint32_t O_gunStatsArray = 0;
    static Il2CppClass* g_gsCls = nullptr;

    Il2CppClass* kEntity = nullptr;
    Il2CppClass* kPlayerScript = nullptr;
    Il2CppClass* kPlayer = nullptr;
    Il2CppClass* kNPC = nullptr;
    Il2CppClass* kTransform = nullptr;
    Il2CppClass* kCamera = nullptr;
    Il2CppClass* kGunStats = nullptr;
    Il2CppClass* kVector3 = nullptr;
    Il2CppClass* kQuaternion = nullptr;
    Il2CppClass* kAntiHack = nullptr;

    MethodInfo* M_shoot = nullptr;
    MethodInfo* M_recoil = nullptr;
    MethodInfo* M_fireBullet = nullptr;
    MethodInfo* M_FireBullet = nullptr;
    MethodInfo* M_getTransform = nullptr;
    MethodInfo* M_w2s = nullptr;
    MethodInfo* M_lookRot = nullptr;
    MethodInfo* M_set_position = nullptr;
    MethodInfo* M_set_forward = nullptr;
    MethodInfo* M_get_fov = nullptr;
    MethodInfo* M_set_fov = nullptr;

    static MethodInfo* M_get_name = nullptr;
    static MethodInfo* M_get_childCount = nullptr;
    static MethodInfo* M_GetChild = nullptr;
    static MethodInfo* M_GetComponent = nullptr;
    static MethodInfo* M_camW2C = nullptr;
    static MethodInfo* M_camProj = nullptr;
    static MethodInfo* M_findFromId = nullptr;
    static MethodInfo* M_camMain = nullptr;
    static Il2CppClass* kObject = nullptr;

    static Il2CppClass* kPhysics = nullptr;
    static Il2CppClass* kRaycastHit = nullptr;
    static MethodInfo* M_raycast = nullptr;
    static int          g_rayKind = 0;
    static bool         g_rayUsesRay = false;
    static uint32_t     O_hitDist = 0;
    static uint32_t     O_hitCollider = 0;
    static bool         g_hitColIsId = true;
    static bool         g_losLogged = false;
    static uint8_t      g_losState[MAX_TARGETS];
    static uint64_t     g_losFrame = (uint64_t)-1;

    static const char* kRayNames[] = {
        "off", "Raycast4", "Raycast5", "Linecast3", "Linecast4", "RaycastRay3", "RaycastRay4"
    };

    static std::mutex g_tfMx;
    static std::unordered_map<Il2CppObject*, Il2CppObject*> g_tfCache;

    static std::vector<uint32_t> g_strOffs;
    static uint32_t g_pickedNameOff = 0;

    static const int kMaxTargets = MAX_TARGETS;

    static Il2CppObject* g_slotOwner[MAX_TARGETS] = {};
    static uint32_t      g_slotSeen[MAX_TARGETS] = {};
    static uint32_t      g_slotGen = 1;

    struct BoneCacheEnt { Il2CppObject* player; Il2CppObject* head; Il2CppObject* foot; };
    static BoneCacheEnt g_boneCache[64] = {};
    static int          g_boneEvict = 0;

    static Il2CppClass* sObjCls = nullptr;
    static MethodInfo* sFindObjs = nullptr;
    static int          sOnlyEntity = -1;
    static bool         g_hierDumped = false;
    static bool         g_dumpTarget = false;
    static bool         g_inited = false;

    static void ResolveOffsets();
    static void ResolveMethods();
    bool Init();

    bool         ValidInstance(Il2CppObject* o) { return ValidInstance_((const void*)o); }
    Il2CppClass* SafeClass(Il2CppObject* o) { return SafeClass_(o); }

    // ======================= pick de campo =======================
    static uint32_t FindTypedField(Il2CppClass* k, std::initializer_list<const char*> names,
        const char* typeSub, const char** outName = nullptr) {
        if (!k) return 0;
        for (Il2CppClass* c = k; c; c = se_class_parent(c)) {
            for (const char* n : names) {
                FieldInfo* f = se_class_get_field_from_name(c, n);
                if (!f) continue;
                if (typeSub) {
                    const Il2CppType* t = il2cpp_field_get_type ? il2cpp_field_get_type(f) : nullptr;
                    const char* tn = TypeNameCached(t);
                    if (!strstr(tn, typeSub)) continue;
                }
                if (outName) *outName = n;
                uint32_t o = (uint32_t)se_field_offset(f);
                if (o) return o;
            }
        }
        return 0;
    }

    uint32_t PickFieldOff(Il2CppClass* k, std::initializer_list<const char*> names,
        const char* typeSub) {
        return FindTypedField(k, names, typeSub);
    }

    static const char* FieldTypeOf(Il2CppClass* k, const char* name) {
        FieldInfo* f = k ? se_class_get_field_from_name(k, name) : nullptr;
        if (!f) return "?";
        const Il2CppType* t = il2cpp_field_get_type ? il2cpp_field_get_type(f) : nullptr;
        return TypeNameCached(t);
    }

    const char* MethodReturnName(MethodInfo* m) {
        if (!m || !il2cpp_method_get_return_type) return "?";
        const Il2CppType* t = il2cpp_method_get_return_type(m);
        return TypeNameCached(t);
    }

    // ======================= SyncVar (FishNet) =======================
    struct SyncCacheEntry { Il2CppClass* cls; uint32_t off; };
    static SyncCacheEntry g_syncCache[16];
    static int g_syncCacheN = 0;
    static bool g_syncDumped = false;

    static void DumpSyncVarFields(Il2CppClass* k) {
        Log("[sync] ---- fields de %s ----", se_class_name(k));
        void* it = nullptr;
        int n = 0;
        for (;;) {
            FieldInfo* f = se_class_get_fields(k, &it);
            if (!f) break;
            const char* fn = se_field_name(f);
            const Il2CppType* t = il2cpp_field_get_type ? il2cpp_field_get_type(f) : nullptr;
            Log("[sync]   %-22s : %s @0x%X", fn ? fn : "?", TypeNameCached(t),
                (unsigned)se_field_offset(f));
            if (++n > 40) break;
        }
        Log("[sync] ---- fim ----");
    }

    static uintptr_t SyncVarValueAddr(Il2CppObject* holder, uint32_t off, const char* typeSub) {
        if (!holder || !off) return 0;

        void* p = nullptr;
        if (!se_read_ptr(holder, (size_t)off, &p) || !p) return 0;
        if (!PlausibleObj(p)) return 0;

        Il2CppObject* obj = (Il2CppObject*)p;
        Il2CppClass* k = SafeClass(obj);
        if (!k) return 0;

        const char* kn = se_class_name(k);
        if (!kn || !strstr(kn, "SyncVar")) return 0;

        uint32_t foff = 0;
        for (int i = 0; i < g_syncCacheN; ++i)
            if (g_syncCache[i].cls == k) { foff = g_syncCache[i].off; break; }

        if (!foff) {
            const char* fname = "?";
            foff = FindTypedField(k, { "_value", "value", "Value", "<Value>k__BackingField",
                                       "m_Value", "_currentValue", "currentValue" }, typeSub, &fname);
            if (!foff)
                foff = FindTypedField(k, { "_value", "value", "Value", "<Value>k__BackingField" }, nullptr, &fname);

            if (g_syncCacheN < 16) {
                g_syncCache[g_syncCacheN].cls = k; g_syncCache[g_syncCacheN].off = foff; ++g_syncCacheN;
            }
            Log("[sync] %s -> %s @0x%X (%s)", kn, fname, foff, typeSub);
            if (!g_syncDumped) { g_syncDumped = true; DumpSyncVarFields(k); }
        }
        if (!foff) return 0;
        return (uintptr_t)obj + foff;
    }

    static bool ReadHealthEx(Il2CppObject* e, float& out) {
        if (!e || !O_health) return false;

        float hp;
        uintptr_t a = SyncVarValueAddr(e, O_health, "Single");
        if (a) {
            hp = SafeRead<float>(a, 100.f);
        }
        else if (g_healthIsSync) {
            return false;
        }
        else {
            hp = se_read_float(e, O_health);
        }

        if (!(hp >= 0.f && hp <= 100000.f)) return false;
        if (hp > 0.f && hp < 1.5f) hp *= 100.f;
        out = hp;
        return true;
    }

    static float ReadHealth(Il2CppObject* e) {
        float v = 0.f;
        return ReadHealthEx(e, v) ? v : 100.f;
    }

    static bool ReadTeamEx(Il2CppObject* e, int& out) {
        out = -1;
        if (!e || !O_team) return false;

        uintptr_t a = SyncVarValueAddr(e, O_team, "Byte");
        if (!a) a = SyncVarValueAddr(e, O_team, "Int32");
        if (a) {
            unsigned char b = SafeRead<unsigned char>(a, 0xFF);
            if (b != 0xFF) { out = (int)b; return true; }
            int i = SafeRead<int>(a, -1);
            if (i >= 0) { out = i & 0xFF; return true; }
        }

        unsigned char bb = 0;
        SE_BEGIN{ bb = *(unsigned char*)((uintptr_t)e + O_team); } SE_END{ bb = 0; return false; }
        if (bb <= 63) { out = (int)bb; return true; }

        int ii = 0;
        SE_BEGIN{ ii = *(int*)((uintptr_t)e + O_team); } SE_END{ ii = -1; return false; }
        if (ii >= 0 && ii <= 63) { out = ii; return true; }

        out = (int)bb;
        return true;
    }

    static int ReadTeam(Il2CppObject* e) {
        int v = -1;
        return ReadTeamEx(e, v) ? v : -1;
    }

    static bool ReadStringField(Il2CppObject* o, uint32_t off, char* out, int n) {
        if (!o || !off || !out || n <= 0) return false;
        out[0] = 0;

        void* p = nullptr;
        if (!se_read_ptr(o, (size_t)off, &p) || !p) return false;

        uintptr_t a = SyncVarValueAddr(o, off, "String");
        Il2CppString* str = nullptr;
        if (a) {
            void* sp = nullptr;
            if (!se_read_ptr((void*)a, 0, &sp) || !sp) return false;
            str = (Il2CppString*)sp;
        }
        else {
            str = (Il2CppString*)p;
        }
        StrToUtf8(str, out, n);
        return out[0] != 0;
    }

    // ======================= cache de classes =======================
    static void RegisterClass(const char* ns, const char* nm, Il2CppClass* k) {
        if (!k || !nm || !nm[0]) return;
        std::string key;
        if (ns && ns[0]) { key.reserve(strlen(ns) + strlen(nm) + 2); key = ns; key += "."; key += nm; }
        else key = nm;
        std::lock_guard<std::mutex> lk(g_classesMx);
        g_classes.emplace(key, k);
    }

    static void CacheImage(Il2CppImage* img) {
        size_t count = se_image_class_count(img);
        for (size_t i = 0; i < count; ++i) {
            Il2CppClass* k = se_image_get_class(img, i);
            if (!k) continue;
            RegisterClass(se_class_namespace(k), se_class_name(k), k);
        }
    }

    void BuildCache() {
        { std::lock_guard<std::mutex> lk(g_classesMx); g_classes.clear(); }
        Il2CppDomain* dom = se_domain_get();
        if (!dom) { Log("[sdk] cache: domain nulo"); return; }
        size_t n = 0;
        const Il2CppAssembly** asms = se_domain_get_assemblies(dom, &n);
        if (!asms || n == 0) { Log("[sdk] cache: nenhum assembly"); return; }
        Log("[sdk] cache: %d assemblies", (int)n);
        for (size_t a = 0; a < n; ++a) CacheImage(se_assembly_get_image(asms[a]));
        std::lock_guard<std::mutex> lk(g_classesMx);
        Log("[sdk] cache: %d classes", (int)g_classes.size());
    }

    Il2CppClass* FindClass(const char* name, const char* ns) {
        if (!name || !*name) return nullptr;
        std::lock_guard<std::mutex> lk(g_classesMx);
        auto it = g_classes.find(name);
        if (it != g_classes.end()) return it->second;
        if (ns && *ns) {
            std::string key = ns; key += "."; key += name;
            it = g_classes.find(key);
            if (it != g_classes.end()) return it->second;
        }
        const size_t ln = strlen(name);
        Il2CppClass* best = nullptr;
        size_t bestLen = (size_t)-1;
        for (it = g_classes.begin(); it != g_classes.end(); ++it) {
            const std::string& k = it->first;
            if (k.size() < ln) continue;
            if (k.compare(k.size() - ln, ln, name) != 0) continue;
            if (k.size() < bestLen) { bestLen = k.size(); best = it->second; }
        }
        return best;
    }

    static Il2CppClass* FindClassExact(const char* name) {
        if (!name) return nullptr;
        std::lock_guard<std::mutex> lk(g_classesMx);
        auto it = g_classes.find(name);
        return it == g_classes.end() ? nullptr : it->second;
    }

    void ListClassesContaining(const char* sub) {
        if (!sub) return;
        std::lock_guard<std::mutex> lk(g_classesMx);
        int n = 0;
        for (auto& kv : g_classes) {
            if (kv.first.find(sub) == std::string::npos) continue;
            Log("[cls] %s", kv.first.c_str());
            if (++n > 80) break;
        }
        Log("[cls] ---- %d classes contendo \"%s\" ----", n, sub);
    }

    int ScreenW() { return Hooks::g_viewW; }
    int ScreenH() { return Hooks::g_viewH; }

    Il2CppObject* SafeReadObjPub(void* base, unsigned int off) {
        return SafeObjField(base, off);
    }

    Il2CppClass* AntiHackClass() { return kAntiHack; }

    Il2CppClass* FindAntiHackClass() {
        static const char* names[] = { "AntiHack", "AntiCheat", "AntiHackSystem", "AntiHackManager" };
        for (const char* n : names) {
            Il2CppClass* k = FindClassExact(n);
            if (k) return k;
        }
        return nullptr;
    }

    Il2CppObject* AntiHackInstance() {
        if (!kAntiHack) return nullptr;
        static const char* names[] = { "instance", "Instance", "_instance", "singleton", "Singleton" };
        for (const char* n : names) {
            FieldInfo* f = se_class_get_field_from_name(kAntiHack, n);
            if (!f) continue;
            Il2CppObject* v = nullptr;
            se_field_static_get_value(f, &v);
            if (v && PlausibleObj(v)) return v;
        }
        return nullptr;
    }

    // ======================= transform / posicao =======================
    static Il2CppObject* ResolveGetTransform(Il2CppObject* obj) {
        if (!obj) return nullptr;
        Il2CppClass* k = SafeClass(obj);
        if (!k) return nullptr;
        MethodInfo* m = MethodCached(k, "get_transform", 0);
        if (!m) return nullptr;
        Il2CppObject* r = se_runtime_invoke(m, obj, nullptr);
        return (r && PlausibleObj(r)) ? r : nullptr;
    }

    Il2CppObject* GetTransform(void* obj) { return ResolveGetTransform((Il2CppObject*)obj); }

    static Il2CppObject* CachedTransform(Il2CppObject* e) {
        if (!e) return nullptr;
        std::lock_guard<std::mutex> lk(g_tfMx);
        auto it = g_tfCache.find(e);
        if (it != g_tfCache.end()) return it->second;
        Il2CppObject* tr = ResolveGetTransform(e);
        g_tfCache[e] = tr;
        return tr;
    }

    void ClearTfCache() {
        std::lock_guard<std::mutex> lk(g_tfMx);
        g_tfCache.clear();
    }

    void PruneTfCache() {
        std::lock_guard<std::mutex> lk(g_tfMx);
        for (auto it = g_tfCache.begin(); it != g_tfCache.end(); ) {
            Il2CppObject* o = it->first;
            if (!o || !PlausibleObj(o)) it = g_tfCache.erase(it);
            else ++it;
        }
    }

    void ClearBoneCache() {
        memset(g_boneCache, 0, sizeof(g_boneCache));
        g_boneEvict = 0;
    }

    static bool IsTransformClass(Il2CppClass* k) {
        if (!k) return false;
        for (Il2CppClass* c = k; c; c = se_class_parent(c)) {
            const char* n = se_class_name(c);
            if (n && strcmp(n, "Transform") == 0) return true;
        }
        return false;
    }

    Vec3 GetPos(Il2CppObject* obj) {
        Vec3 out{ 0.f, 0.f, 0.f };
        if (!obj || !PlausibleObj(obj)) return out;

        Il2CppClass* k = SafeClass(obj);
        if (!k) return out;

        Il2CppObject* node = obj;
        if (!IsTransformClass(k)) {
            node = CachedTransform(obj);
            if (!node) return out;
            k = SafeClass(node);
            if (!k) return out;
        }

        MethodInfo* m = MethodCached(k, "get_position", 0);
        if (!m) return out;

        Il2CppObject* boxed = se_runtime_invoke(m, node, nullptr);
        if (!boxed) return out;
        void* p = se_object_unbox(boxed);
        if (p) memcpy(&out, p, sizeof(Vec3));
        return out;
    }

    void SetPos(Il2CppObject* obj, Vec3 v) {
        if (!obj || !PlausibleObj(obj)) return;
        Il2CppObject* node = obj;
        Il2CppClass* k = SafeClass(node);
        if (!k) return;
        if (!IsTransformClass(k)) {
            node = CachedTransform(obj);
            if (!node) return;
            k = SafeClass(node);
            if (!k) return;
        }
        MethodInfo* sp = MethodCached(k, "set_position", 1);
        if (!sp) return;

        Vec3 vv = v;
        void* args[1]; args[0] = &vv;
        se_runtime_invoke(sp, node, args);
    }

    // ======================= rotacao =======================
    Il2CppObject* CamTfObj() {
        if (!g_localPlayer || !O_camTf) return nullptr;
        Il2CppObject* tf = SafeObjField(g_localPlayer, O_camTf);
        if (!tf) return nullptr;
        if (kTransform && !IsA(tf, kTransform)) return nullptr;
        return tf;
    }

    Quat LookRot(Vec3 dir) {
        Quat out{ 0.f, 0.f, 0.f, 1.f };
        if (!kQuaternion) kQuaternion = FindClass("Quaternion", "UnityEngine");
        if (!kQuaternion) return out;
        if (!M_lookRot) M_lookRot = se_class_get_method_from_name(kQuaternion, "LookRotation", 1);
        if (!M_lookRot) return out;

        Vec3 d = dir;
        void* a[1]; a[0] = &d;
        Il2CppObject* r = se_runtime_invoke(M_lookRot, nullptr, a);
        if (!r) return out;
        void* p = se_object_unbox(r);
        if (p) memcpy(&out, p, sizeof(Quat));
        return out;
    }

    bool GetRot(Il2CppObject* tf, Quat& out) {
        if (!tf) return false;
        Il2CppClass* k = SafeClass(tf);
        if (!k) return false;
        MethodInfo* gr = MethodCached(k, "get_rotation", 0);
        if (!gr) return false;
        Il2CppObject* b = se_runtime_invoke(gr, tf, nullptr);
        if (!b) return false;
        void* p = se_object_unbox(b);
        if (!p) return false;
        memcpy(&out, p, sizeof(Quat));
        return true;
    }

    bool SetRot(Il2CppObject* tf, Quat q) {
        if (!tf) return false;
        Il2CppClass* k = SafeClass(tf);
        if (!k) return false;
        MethodInfo* sr = MethodCached(k, "set_rotation", 1);
        if (!sr) return false;
        void* a[1]; a[0] = &q;
        se_runtime_invoke(sr, tf, a);
        return true;
    }

    void SetPlayerRot(Il2CppObject* p, const Quat& q) {
        if (!p || !O_rotation) return;
        if (g_rotIsSyncVar) {
            uintptr_t a = SyncVarValueAddr(p, O_rotation, "Quaternion");
            if (a) { SafeWrite(a, (const void*)&q, sizeof(Quat)); return; }
        }
        SafeWrite((uintptr_t)p + O_rotation, (const void*)&q, sizeof(Quat));
    }

    Quat GetPlayerRot(Il2CppObject* p) {
        Quat q{ 0.f, 0.f, 0.f, 1.f };
        if (!p || !O_rotation) return q;
        if (g_rotIsSyncVar) {
            uintptr_t a = SyncVarValueAddr(p, O_rotation, "Quaternion");
            if (a) { se_read_bytes((void*)a, &q, sizeof(Quat)); return q; }
        }
        se_read_bytes((void*)((uintptr_t)p + O_rotation), &q, sizeof(Quat));
        return q;
    }

    Vec3 GetVelocity(Il2CppObject* o) {
        Vec3 v{ 0.f, 0.f, 0.f };
        if (!o || !O_vel) return v;
        se_read_vec3(o, O_vel, &v);
        return v;
    }

    Vec3 GetBonePos(void* bone) { return GetPos((Il2CppObject*)bone); }

    Vec3 MuzzlePos() {
        Il2CppObject* camTf = CamTfObj();
        if (camTf) {
            Vec3 p = GetPos(camTf);
            if (!p.zero()) return p;
        }
        if (g_localPlayer) {
            Vec3 p = GetPos(g_localPlayer);
            return Vec3(p.x, p.y + 1.5f, p.z);
        }
        return Vec3();
    }

    bool IsA(Il2CppObject* o, Il2CppClass* c) {
        if (!o || !c) return false;
        for (Il2CppClass* k = SafeClass(o); k; k = se_class_parent(k))
            if (k == c) return true;
        return false;
    }

    float GetHealth(Il2CppObject* o) { return ReadHealth(o); }

    bool IsEnemy(Il2CppObject* o) {
        if (!o) return false;
        if (!O_team) return true;

        int t = -1;
        if (!ReadTeamEx(o, t)) return true;
        if (t < 0) return true;
        if (g_localTeam < 0) return true;
        return t != g_localTeam;
    }

    // ======================= nome =======================
    static void BuildStringOffsets() {
        g_strOffs.clear();
        if (!kPlayerScript) return;
        void* it = nullptr;
        for (;;) {
            FieldInfo* f = se_class_get_fields(kPlayerScript, &it);
            if (!f) break;
            const Il2CppType* t = il2cpp_field_get_type ? il2cpp_field_get_type(f) : nullptr;
            if (!strstr(TypeNameCached(t), "String")) continue;
            uint32_t o = (uint32_t)se_field_offset(f);
            if (o) g_strOffs.push_back(o);
        }
        Log("[sdk] campos String em PlayerScript: %d", (int)g_strOffs.size());
    }

    static bool PlausibleName(const char* s) {
        if (!s || !s[0]) return false;
        size_t len = strlen(s);
        if (len < 2 || len > 24) return false;
        bool allDigits = true;
        for (const char* p = s; *p; ++p) {
            if (*p < 32 || *p > 126) return false;
            if (*p < '0' || *p > '9') allDigits = false;
        }
        return !allDigits;
    }

    void GetNameBuf(Il2CppObject* o, char* out, int n) {
        if (!out || n <= 0) return;
        out[0] = 0;
        if (!PtrOk_(o)) return;
        if (!ValidInstance_(o)) return;

        __try {
            char buf[64];

            if (O_name && ReadStringField(o, O_name, buf, sizeof(buf)) && PlausibleName(buf)) {
                strncpy_s(out, (size_t)n, buf, _TRUNCATE);
                return;
            }
            for (uint32_t off : g_strOffs) {
                if (!ReadStringField(o, off, buf, sizeof(buf))) continue;
                if (!PlausibleName(buf)) continue;
                if (!g_pickedNameOff) { g_pickedNameOff = off; Log("[sdk] campo de nome = 0x%X (\"%s\")", off, buf); }
                strncpy_s(out, (size_t)n, buf, _TRUNCATE);
                return;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            out[0] = 0;
        }
    }

    std::string GetName(Il2CppObject* o) {
        char buf[64]{};
        GetNameBuf(o, buf, sizeof(buf));
        return std::string(buf);
    }

    Il2CppObject* GetGunStats(Il2CppObject* p) { return (p && O_gunStats) ? SafeObjField(p, O_gunStats) : nullptr; }
    Il2CppObject* GetWeapon(Il2CppObject* p) { return (p && O_weapon) ? SafeObjField(p, O_weapon) : nullptr; }

    // ======================= diag =======================
    static void DumpStringFields(Il2CppObject* inst) {
        if (!inst) return;
        Il2CppClass* k = SafeClass(inst);
        if (!k) return;
        Log("[str] ---- campos String de %s ----", se_class_name(k));
        void* iter = nullptr;
        int n = 0;
        char buf[96];
        for (;;) {
            FieldInfo* f = se_class_get_fields(k, &iter);
            if (!f) break;
            const char* fn = se_field_name(f);
            if (!fn) continue;
            const Il2CppType* t = il2cpp_field_get_type ? il2cpp_field_get_type(f) : nullptr;
            if (!strstr(TypeNameCached(t), "String")) continue;
            uint32_t off = (uint32_t)se_field_offset(f);
            if (!off) continue;
            if (ReadStringField(inst, off, buf, sizeof(buf)))
                Log("[str]   %s @0x%X = \"%s\"", fn, off, buf);
            else
                Log("[str]   %s @0x%X = (vazio)", fn, off);
            if (++n > 25) break;
        }
        Log("[str] ---- fim ----");
    }

    static void LogHierarchy(Il2CppObject* root, int maxNodes) {
        if (!root) { Log("[hier] raiz nula"); return; }
        static Il2CppObject* stk[1024];
        int top = 0;
        stk[top++] = root;
        int visited = 0;

        while (top > 0 && visited < maxNodes) {
            Il2CppObject* cur = stk[--top];
            if (!cur) continue;
            Il2CppClass* k = SafeClass(cur);
            if (!k) continue;

            char nm[128] = "?";
            MethodInfo* gn = MethodCached(k, "get_name", 0);
            if (gn) {
                Il2CppObject* s = se_runtime_invoke(gn, cur, nullptr);
                if (s) StrToUtf8((Il2CppString*)s, nm, sizeof(nm));
            }

            int cc = 0;
            MethodInfo* gc = MethodCached(k, "get_childCount", 0);
            if (gc) {
                Il2CppObject* b = se_runtime_invoke(gc, cur, nullptr);
                if (b) { void* p = se_object_unbox(b); if (p) cc = *(int*)p; }
            }

            Log("[hier] %-40s filhos=%d", nm, cc);

            MethodInfo* gch = MethodCached(k, "GetChild", 1);
            if (!gch) continue;

            for (int i = 0; i < cc && top < 1024; ++i) {
                int idx = i;
                void* args[1]; args[0] = &idx;
                Il2CppObject* c = se_runtime_invoke(gch, cur, args);
                if (c) stk[top++] = c;
            }
            ++visited;
        }
    }

    // ======================= bones =======================
    static const char* kHeads[] = { "Head", "head", "HEAD", "Bip01 Head",
                                    "mixamorig:Head", "neck", "Bone_Head" };
    static const char* kFeet[] = { "Foot", "foot", "Foot_L", "Bip01 L Foot",
                                   "mixamorig:LeftFoot", "toe", "Bone_Foot" };

    Il2CppObject* FindBoneContains(Il2CppObject* root, const char* sub, int budget) {
        if (!root || !sub) return nullptr;

        Il2CppClass* rk = SafeClass(root);
        if (!rk) return nullptr;
        if (!IsTransformClass(rk)) {
            root = CachedTransform(root);
            if (!root) return nullptr;
        }

        static Il2CppObject* stk[2048];
        int top = 0;
        stk[top++] = root;

        while (top > 0 && budget-- > 0) {
            Il2CppObject* cur = stk[--top];
            if (!cur) continue;
            Il2CppClass* k = SafeClass(cur);
            if (!k) continue;

            if (!M_get_name) M_get_name = MethodCached(k, "get_name", 0);
            if (M_get_name) {
                Il2CppObject* s = se_runtime_invoke(M_get_name, cur, nullptr);
                if (s) {
                    char nm[128];
                    StrToUtf8((Il2CppString*)s, nm, sizeof(nm));
                    if (nm[0] && strstr(nm, sub)) return cur;
                }
            }

            if (!M_get_childCount) M_get_childCount = MethodCached(k, "get_childCount", 0);
            if (!M_GetChild) M_GetChild = MethodCached(k, "GetChild", 1);
            if (!M_get_childCount || !M_GetChild) continue;

            int cc = 0;
            Il2CppObject* b = se_runtime_invoke(M_get_childCount, cur, nullptr);
            if (b) { void* p = se_object_unbox(b); if (p) cc = *(int*)p; }

            for (int i = 0; i < cc && top < 2048; ++i) {
                int idx = i;
                void* args[1]; args[0] = &idx;
                Il2CppObject* c = se_runtime_invoke(M_GetChild, cur, args);
                if (c) stk[top++] = c;
            }
        }
        return nullptr;
    }

    static Il2CppObject* FindBoneAny(Il2CppObject* root, const char** names, int count) {
        if (!root) return nullptr;
        for (int i = 0; i < count; ++i) {
            Il2CppObject* b = FindBoneContains(root, names[i], 4096);
            if (b) return b;
        }
        return nullptr;
    }

    static BoneCacheEnt* BoneCacheFind(Il2CppObject* p) {
        for (auto& b : g_boneCache) if (b.player == p) return &b;
        return nullptr;
    }

    static BoneCacheEnt* BoneCacheInsert(Il2CppObject* p) {
        for (auto& b : g_boneCache) if (!b.player) { b.player = p; b.head = b.foot = nullptr; return &b; }
        BoneCacheEnt* b = &g_boneCache[g_boneEvict++ % 64];
        b->player = p; b->head = b->foot = nullptr;
        return b;
    }

    static void ResolveBones(Target& t, Il2CppObject* e) {
        BoneCacheEnt* bc = BoneCacheFind(e);
        if (!bc) {
            bc = BoneCacheInsert(e);
            if (bc) {
                Il2CppObject* playerTf = CachedTransform(e);
                Il2CppObject* animTf = nullptr;
                if (O_humanMeshAnimator) {
                    Il2CppObject* anim = SafeObjField(e, O_humanMeshAnimator);
                    if (anim) animTf = ResolveGetTransform(anim);
                }

                if (!g_hierDumped) {
                    g_hierDumped = true;
                    Log("[hier] ---- player transform ----");
                    LogHierarchy(playerTf, 80);
                    if (animTf && animTf != playerTf) {
                        Log("[hier] ---- animator transform ----");
                        LogHierarchy(animTf, 80);
                    }
                    Log("[hier] ---- fim ----");
                }

                bc->head = FindBoneAny(playerTf, kHeads, 7);
                bc->foot = FindBoneAny(playerTf, kFeet, 7);
                if ((!bc->head || !bc->foot) && animTf && animTf != playerTf) {
                    if (!bc->head) bc->head = FindBoneAny(animTf, kHeads, 7);
                    if (!bc->foot) bc->foot = FindBoneAny(animTf, kFeet, 7);
                }
                Log("[sdk] bones %p head=%p foot=%p", e, bc->head, bc->foot);
            }
        }
        if (bc) { t.headBone = bc->head; t.footBone = bc->foot; }
    }

    // ======================= camera / view =======================
    Il2CppObject* CameraObj() {
        if (!kCamera) kCamera = FindClass("Camera", "UnityEngine");
        if (!kCamera) return nullptr;

        if (!M_camMain) M_camMain = se_class_get_method_from_name(kCamera, "get_main", 0);
        if (M_camMain) {
            Il2CppObject* c = se_runtime_invoke(M_camMain, nullptr, nullptr);
            if (c && PlausibleObj(c)) return c;
        }
        if (g_localPlayer && O_mCamera) {
            Il2CppObject* c = SafeObjField(g_localPlayer, O_mCamera);
            if (c && IsA(c, kCamera)) return c;
        }
        if (g_localPlayer && O_camTf) {
            Il2CppObject* tf = SafeObjField(g_localPlayer, O_camTf);
            if (tf) {
                Il2CppClass* tk = SafeClass(tf);
                if (tk) {
                    if (!M_GetComponent) M_GetComponent = MethodCached(tk, "GetComponent", 1);
                    if (M_GetComponent) {
                        Il2CppType* t = se_class_get_type(kCamera);
                        if (t) {
                            Il2CppObject* to = se_type_get_object(t);
                            if (to) {
                                void* a[1]; a[0] = to;
                                Il2CppObject* c = se_runtime_invoke(M_GetComponent, tf, a);
                                if (c && PlausibleObj(c)) return c;
                            }
                        }
                    }
                }
            }
        }
        if (MethodInfo* m = se_class_get_method_from_name(kCamera, "get_current", 0)) {
            Il2CppObject* c = se_runtime_invoke(m, nullptr, nullptr);
            if (c && PlausibleObj(c)) return c;
        }
        if (MethodInfo* m = se_class_get_method_from_name(kCamera, "get_allCameras", 0)) {
            Il2CppArray* arr = (Il2CppArray*)se_runtime_invoke(m, nullptr, nullptr);
            if (arr && il2cpp_array_length && il2cpp_array_length(arr) > 0)
                return (Il2CppObject*)se_array_at(arr, 0);
        }
        return nullptr;
    }

    Il2CppObject* GetCamera() { return g_camera; }

    static bool ReadMatrix(Il2CppObject* obj, MethodInfo* m, float out[16]) {
        if (!obj || !m) return false;
        Il2CppObject* boxed = se_runtime_invoke(m, obj, nullptr);
        if (!boxed) return false;
        void* p = se_object_unbox(boxed);
        if (!p) return false;
        return se_read_bytes(p, out, 16 * sizeof(float));
    }

    void UpdateView() {
        g_camera = CameraObj();

        bool ok = false;
        float vp[16] = {};

        if (g_camera) {
            Il2CppClass* ck = SafeClass(g_camera);
            if (ck) {
                if (!M_camW2C)  M_camW2C = MethodCached(ck, "get_worldToCameraMatrix", 0);
                if (!M_camProj) M_camProj = MethodCached(ck, "get_projectionMatrix", 0);

                float V[16] = {}, P[16] = {};
                if (ReadMatrix(g_camera, M_camW2C, V) && ReadMatrix(g_camera, M_camProj, P)) {
                    for (int c = 0; c < 4; ++c)
                        for (int r = 0; r < 4; ++r) {
                            float s = 0.f;
                            for (int k = 0; k < 4; ++k) s += P[k * 4 + r] * V[c * 4 + k];
                            vp[c * 4 + r] = s;
                        }
                    ok = true;
                }
            }
        }

        InterlockedIncrement(&g_vpSeq);
        if (ok) memcpy(g_viewProj, vp, sizeof(vp));
        g_viewProjOk = ok;
        InterlockedIncrement(&g_vpSeq);
    }

    bool WorldToScreen(Vec3 w, Vec2& out) {
        float m[16] = {};
        bool ok = false;
        for (int guard = 0; guard < 64; ++guard) {
            LONG s1 = g_vpSeq;
            if (s1 & 1) { YieldProcessor(); continue; }
            memcpy(m, g_viewProj, sizeof(m));
            ok = g_viewProjOk;
            if (g_vpSeq == s1) break;
        }
        if (!ok) return false;

        float x = m[0] * w.x + m[4] * w.y + m[8] * w.z + m[12];
        float y = m[1] * w.x + m[5] * w.y + m[9] * w.z + m[13];
        float ww = m[3] * w.x + m[7] * w.y + m[11] * w.z + m[15];
        if (ww <= 0.0001f) return false;

        float inv = 1.f / ww;
        out.x = ((x * inv) * 0.5f + 0.5f) * (float)Hooks::g_viewW;
        out.y = (1.f - ((y * inv) * 0.5f + 0.5f)) * (float)Hooks::g_viewH;
        return out.x == out.x && out.y == out.y;
    }

    bool VisibleLine(Vec3 a, Vec3 b) {
        Vec2 sb;
        if (!WorldToScreen(b, sb)) return false;

        Vec3 a2 = a + (b - a) * 0.01f;
        Vec2 sa;
        if (!WorldToScreen(a2, sa)) {
            sa.x = (float)Hooks::g_viewW * 0.5f;
            sa.y = (float)Hooks::g_viewH * 0.5f;
        }

        const float W = (float)Hooks::g_viewW, H = (float)Hooks::g_viewH;
        auto inside = [&](const Vec2& p) {
            return p.x > -W * 0.25f && p.x < W * 1.25f && p.y > -H * 0.25f && p.y < H * 1.25f;
            };
        return inside(sa) && inside(sb);
    }

    // ======================= LINE OF SIGHT =======================
    static MethodInfo* FindPhysicsRay(const char* name, int nParams, int kind,
        const char* t0, const char* t1, const char* t2) {
        if (!kPhysics) kPhysics = FindClass("Physics", "UnityEngine");
        if (!kPhysics) return nullptr;

        void* it = nullptr;
        for (;;) {
            const MethodInfo* m = il2cpp_class_get_methods(kPhysics, &it);
            if (!m) break;
            if (strcmp(il2cpp_method_get_name(m), name) != 0) continue;
            if ((int)il2cpp_method_get_param_count(m) != nParams) continue;

            const Il2CppType* p0 = il2cpp_method_get_param(m, 0);
            if (!p0) continue;
            if (!strstr(TypeNameCached(p0), t0)) continue;

            if (t1) {
                const Il2CppType* p1 = il2cpp_method_get_param(m, 1);
                if (!p1 || !strstr(TypeNameCached(p1), t1)) continue;
            }
            if (t2) {
                const Il2CppType* p2 = il2cpp_method_get_param(m, 2);
                if (!p2 || !strstr(TypeNameCached(p2), t2)) continue;
            }
            g_rayKind = kind;
            return (MethodInfo*)m;
        }
        return nullptr;
    }

    static uint32_t HitDistOffset() {
        if (O_hitDist) return O_hitDist;
        if (!kRaycastHit) kRaycastHit = FindClass("RaycastHit", "UnityEngine");
        if (kRaycastHit) {
            FieldInfo* f = se_class_get_field_from_name(kRaycastHit, "m_Distance");
            if (!f) f = se_class_get_field_from_name(kRaycastHit, "distance");
            if (f) O_hitDist = (uint32_t)se_field_offset(f);

            FieldInfo* fc = se_class_get_field_from_name(kRaycastHit, "m_Collider");
            if (!fc) fc = se_class_get_field_from_name(kRaycastHit, "collider");
            if (fc) {
                const Il2CppType* t = il2cpp_field_get_type ? il2cpp_field_get_type(fc) : nullptr;
                const char* tn = TypeNameCached(t);
                g_hitColIsId = (strstr(tn, "Int32") != nullptr) || (strstr(tn, "Int") != nullptr);
                O_hitCollider = (uint32_t)se_field_offset(fc);
            }
        }
        if (!O_hitDist) O_hitDist = 0x1C;
        return O_hitDist;
    }

    static void ResolveRaycast() {
        if (M_raycast) return;
        g_rayKind = 0;
        g_rayUsesRay = false;

        M_raycast = FindPhysicsRay("Raycast", 4, 1, "Vector3", "Vector3", "RaycastHit");
        if (!M_raycast) M_raycast = FindPhysicsRay("Raycast", 5, 2, "Vector3", "Vector3", "RaycastHit");
        if (!M_raycast) M_raycast = FindPhysicsRay("Linecast", 3, 3, "Vector3", "Vector3", "RaycastHit");
        if (!M_raycast) M_raycast = FindPhysicsRay("Linecast", 4, 4, "Vector3", "Vector3", "RaycastHit");
        if (!M_raycast) M_raycast = FindPhysicsRay("Raycast", 3, 5, "Ray", nullptr, "RaycastHit");
        if (!M_raycast) M_raycast = FindPhysicsRay("Raycast", 4, 6, "Ray", nullptr, "RaycastHit");

        if (M_raycast) {
            g_rayUsesRay = (g_rayKind >= 5);
            HitDistOffset();
        }
        if (!g_losLogged) {
            g_losLogged = true;
            Log("[los] Physics=%p metodo=%p backend=%s hitOff=0x%X colOff=0x%X colIsId=%d",
                kPhysics, M_raycast, kRayNames[g_rayKind], O_hitDist, O_hitCollider, (int)g_hitColIsId);
        }
    }

    static MethodInfo* FindRaycast4() { ResolveRaycast(); return M_raycast; }

    bool LosReady() { if (!M_raycast) ResolveRaycast(); return M_raycast != nullptr; }

    const char* LosBackend() {
        int k = g_rayKind;
        if (k < 0 || k > 6) k = 0;
        return kRayNames[k];
    }

    struct RayS { Vec3 origin; Vec3 dir; };

    static int ShootRayEx(Vec3 from, Vec3 dir, float len, unsigned char* hitBuf) {
        if (g_rayKind == 0) return 0;

        Vec3 sv = from;
        Vec3 dv = dir;
        Vec3 ev = from + dir * len;
        float lv = len;
        int mask = -1;
        RayS ray{ from, dir };
        void* args[5] = { nullptr, nullptr, nullptr, nullptr, nullptr };

        switch (g_rayKind) {
        case 1: args[0] = &sv;  args[1] = &dv; args[2] = hitBuf; args[3] = &lv; break;
        case 2: args[0] = &sv;  args[1] = &dv; args[2] = hitBuf; args[3] = &lv; args[4] = &mask; break;
        case 3: args[0] = &sv;  args[1] = &ev; args[2] = hitBuf; break;
        case 4: args[0] = &sv;  args[1] = &ev; args[2] = hitBuf; args[3] = &mask; break;
        case 5: args[0] = &ray; args[1] = hitBuf; args[2] = &lv; break;
        case 6: args[0] = &ray; args[1] = hitBuf; args[2] = &lv; args[3] = &mask; break;
        default: return 0;
        }

        Il2CppObject* r = se_runtime_invoke(M_raycast, nullptr, args);
        if (!r) return 0;
        void* pb = se_object_unbox(r);
        if (!pb) return 0;
        return (*(unsigned char*)pb != 0) ? 2 : 1;
    }

    static bool ShootRay(Vec3 from, Vec3 dir, float len, unsigned char* hitBuf) {
        return ShootRayEx(from, dir, len, hitBuf) == 2;
    }

    static uint32_t g_losOk = 0, g_losSemHit = 0, g_losFalhou = 0, g_losBloqueado = 0;

    static void LosLog(const char* tag, Vec3 sv, float hit, float dist, int kind) {
        static ULONGLONG last = 0;
        ULONGLONG now = GetTickCount64();
        if (now - last < 120) return;
        last = now;
        Log("[los] %s: sv=(%.1f,%.1f,%.1f) hit=%.2f dist=%.2f kind=%d backend=%s "
            "| ok=%u semHit=%u falhou=%u bloqueado=%u",
            tag, sv.x, sv.y, sv.z, hit, dist, kind, LosBackend(),
            g_losOk, g_losSemHit, g_losFalhou, g_losBloqueado);
    }

    static Il2CppObject* ObjectFromInstanceID(int id) {
        if (id <= 0) return nullptr;
        if (!kObject) kObject = FindClass("Object", "UnityEngine");
        if (!kObject) return nullptr;
        if (!M_findFromId) M_findFromId = se_class_get_method_from_name(kObject, "FindObjectFromInstanceID", 1);
        if (!M_findFromId) return nullptr;
        void* a[1]; a[0] = &id;
        Il2CppObject* r = se_runtime_invoke(M_findFromId, nullptr, a);
        return (r && PlausibleObj(r)) ? r : nullptr;
    }

    static MethodInfo* M_get_parent = nullptr;

    static Il2CppObject* ParentOf(Il2CppObject* tf) {
        if (!tf) return nullptr;
        Il2CppClass* k = SafeClass(tf);
        if (!k) return nullptr;
        if (!M_get_parent) M_get_parent = MethodCached(k, "get_parent", 0);
        if (!M_get_parent) return nullptr;
        return se_runtime_invoke(M_get_parent, tf, nullptr);
    }

    namespace {
        struct IdEnt { int id = 0; Il2CppObject* obj = nullptr; uint64_t stamp = 0; };
        static IdEnt    g_idCache[64];
        static uint32_t g_idRing = 0;
    }

    static Il2CppObject* ColliderFromHit(unsigned char* hitBuf) {
        if (!O_hitCollider) return nullptr;
        if (g_hitColIsId) {
            int id = 0;
            if (!se_read_int(hitBuf, O_hitCollider, &id) || id <= 0) return nullptr;

            for (int i = 0; i < 64; ++i)
                if (g_idCache[i].stamp == g_frameId && g_idCache[i].id == id)
                    return g_idCache[i].obj;

            Il2CppObject* o = ObjectFromInstanceID(id);
            if (o) {
                IdEnt* e = &g_idCache[g_idRing];
                g_idRing = (g_idRing + 1) % 64;
                e->id = id; e->obj = o; e->stamp = g_frameId;
            }
            return o;
        }
        void* p = nullptr;
        if (!se_read_ptr(hitBuf, O_hitCollider, &p) || !p) return nullptr;
        return PlausibleObj(p) ? (Il2CppObject*)p : nullptr;
    }

    namespace {

        struct LosEnt { Vec3 aim{}; bool res = false; bool used = false; uint64_t stamp = 0; uint32_t slot = 0; };
        static LosEnt   g_losCache[64];
        static uint32_t g_losRing = 0;
        static uint64_t g_losStamp = (uint64_t)-1;
        static int      g_losBudget = 0;

        static const int   kLosBudget = 4;
        static const float kLosEps2 = 0.25f;

        static LosEnt* LosFind(Vec3 aim) {
            for (int i = 0; i < 64; ++i) {
                if (!g_losCache[i].used) continue;
                Vec3 d{ g_losCache[i].aim.x - aim.x,
                        g_losCache[i].aim.y - aim.y,
                        g_losCache[i].aim.z - aim.z };
                if (d.x * d.x + d.y * d.y + d.z * d.z < kLosEps2) return &g_losCache[i];
            }
            return nullptr;
        }

        static LosEnt* LosInsert(Vec3 aim, bool res) {
            LosEnt* e = &g_losCache[g_losRing];
            e->slot = g_losRing;
            g_losRing = (g_losRing + 1) % 64;
            e->aim = aim; e->res = res; e->used = true; e->stamp = g_losStamp;
            return e;
        }

        struct OkEnt { Il2CppObject* col = nullptr; Il2CppObject* ttf = nullptr; int kind = 0; uint64_t stamp = 0; };
        static OkEnt   g_okCache[64];
        static uint32_t g_okRing = 0;

        static bool OkFind(Il2CppObject* col, Il2CppObject* ttf, int* out) {
            for (int i = 0; i < 64; ++i) {
                if (g_okCache[i].stamp != g_losStamp) continue;
                if (g_okCache[i].col == col && g_okCache[i].ttf == ttf) {
                    *out = g_okCache[i].kind;
                    return true;
                }
            }
            return false;
        }

        static void OkInsert(Il2CppObject* col, Il2CppObject* ttf, int kind) {
            OkEnt* e = &g_okCache[g_okRing];
            g_okRing = (g_okRing + 1) % 64;
            e->col = col; e->ttf = ttf; e->kind = kind; e->stamp = g_losStamp;
        }

    } // namespace

    // =========================================================================
    //  [FIXL1] OwnerKindUncached — walk de parent até 15 níveis
    // =========================================================================
    static int OwnerKindUncached(Il2CppObject* col, Il2CppObject* localTf,
        Il2CppObject* targetTf, Vec3 aim) {
        if (!col) return 0;

        Il2CppObject* tf = ResolveGetTransform(col);

        // sobe a hierarquia comparando com localTf E targetTf em CADA nó
        Il2CppObject* walk = tf;
        for (int d = 0; walk && d < 15; ++d) {
            if (localTf && walk == localTf) return 1;
            if (targetTf && walk == targetTf) return 2;
            walk = ParentOf(walk);
        }

        // fallback por distancia
        if (tf) {
            Vec3 op = GetPos(tf);
            if (!op.zero()) {
                if (targetTf && (op - aim).len() < 3.0f) return 2;
                if (localTf && (op - g_localPos).len() < 3.0f) return 1;
            }
        }

        // último recurso: bateu perto de MIM -> é meu corpo
        if (!localTf && (aim - g_localPos).len() < 3.0f) return 1;

        return 0;
    }

    static int OwnerKind(Il2CppObject* col, Il2CppObject* localTf,
        Il2CppObject* targetTf, Vec3 aim) {
        if (!col) return 0;

        int cached = 0;
        if (OkFind(col, targetTf, &cached)) return cached;

        int kind = OwnerKindUncached(col, localTf, targetTf, aim);
        OkInsert(col, targetTf, kind);
        return kind;
    }

    // =========================================================================
    //  [FIXL2/FIXL3] HasLineOfSightExRaw — avança pelo hit REAL, não por 0.30
    // =========================================================================
    bool HasLineOfSightExRaw(Vec3 eye, Vec3 aim, Il2CppObject* localTf, Il2CppObject* targetTf) {
        if (!M_raycast) ResolveRaycast();
        if (!M_raycast) { ++g_losFalhou; return true; }
        if (!O_hitDist) HitDistOffset();

        if (!localTf && g_localPlayer) localTf = CachedTransform(g_localPlayer);

        Vec3 d = aim - eye;
        float dist = d.len();
        if (dist < 2.0f) { ++g_losOk; return true; }
        Vec3 dir{ d.x / dist, d.y / dist, d.z / dist };

        float adv = 0.45f;
        if (adv > dist * 0.4f) adv = dist * 0.4f;
        eye = eye + dir * adv;
        dist -= adv;
        if (dist < 0.2f) { ++g_losOk; return true; }

        alignas(16) unsigned char hitBuf[160];
        float travelled = 0.f;

        for (int iter = 0; iter < 10; ++iter) {
            float len = dist - travelled;
            if (len < 0.1f) { ++g_losOk; return true; }

            memset(hitBuf, 0, sizeof(hitBuf));
            Vec3 sv = eye + dir * travelled;
            int r = ShootRayEx(sv, dir, len, hitBuf);

            if (r == 0) { ++g_losFalhou; if (C.probeLog) LosLog("FALHOU", sv, -1.f, dist, -1); return true; }
            if (r == 1) { ++g_losSemHit; if (C.probeLog) LosLog("sem hit", sv, -1.f, dist, -1); return true; }

            Vec3 hitPoint{ 0.f, 0.f, 0.f };
            se_read_bytes(hitBuf, &hitPoint, sizeof(Vec3));
            float hitDist = (hitPoint - sv).len();
            if (hitDist < 0.01f)
                se_read_bytes(hitBuf + O_hitDist, &hitDist, sizeof(float));

            Il2CppObject* col = ColliderFromHit(hitBuf);
            int kind = col ? OwnerKind(col, localTf, targetTf, aim) : 0;

            if (kind == 2) { ++g_losOk; if (C.probeLog) LosLog("BATEU NO ALVO", sv, hitDist, dist, 2); return true; }

            if (kind == 1) {
                // bateu em mim: avança até o fim do hit e continua
                float step = hitDist + 0.05f;
                if (step < 0.2f) step = 0.2f;
                travelled += step;
                if (travelled >= dist) { ++g_losOk; return true; }
                continue;
            }

            // kind == 0: bateu em algo desconhecido
            // se o que falta até o alvo é pouco, considera visível
            float remaining = dist - (travelled + hitDist);
            if (remaining < 1.5f) { ++g_losOk; if (C.probeLog) LosLog("remaining<1.5 VIS", sv, travelled + hitDist, dist, 0); return true; }

            ++g_losBloqueado;
            if (C.probeLog) LosLog("BLOQUEADO", sv, travelled + hitDist, dist, 0);
            return false;
        }
        ++g_losOk;
        return true;
    }

    bool HasLineOfSightEx(Vec3 eye, Vec3 aim, Il2CppObject* lt, Il2CppObject* tt)
    {
        if (g_frameId != g_losStamp) { g_losStamp = g_frameId; g_losBudget = 0; }

        LosEnt* e = LosFind(aim);
        if (e && e->stamp == g_losStamp) return e->res;

        if (kLosBudget > 0 && e) {
            if (((e->slot + (uint32_t)g_losStamp) % (uint32_t)kLosBudget) != 0) return e->res;
            if (g_losBudget >= kLosBudget) return e->res;
        }

        ++g_losBudget;
        bool res = HasLineOfSightExRaw(eye, aim, lt, tt);

        if (e) { e->res = res; e->stamp = g_losStamp; }
        else { LosInsert(aim, res); }
        return res;
    }

    bool HasLineOfSight(Vec3 eye, Vec3 aim) {
        return HasLineOfSightEx(eye, aim, nullptr, nullptr);
    }

    void LosInvalidate() {
        memset(g_losCache, 0, sizeof(g_losCache));
        memset(g_okCache, 0, sizeof(g_okCache));
        g_losRing = 0;
        g_okRing = 0;
        g_losStamp = (uint64_t)-1;
        g_losBudget = 0;
        g_losFrame = (uint64_t)-1;
        memset(g_losState, 0xFF, sizeof(g_losState));
        memset(g_idCache, 0, sizeof(g_idCache));
        g_idRing = 0;

        ClearTfCache();
        ClearBoneCache();
    }

    bool TargetVisible(int idx, Vec3 eye, Vec3 aim) {
        if (idx < 0 || idx >= MAX_TARGETS) return HasLineOfSight(eye, aim);

        if (g_losFrame != g_frameId) {
            g_losFrame = g_frameId;
            memset(g_losState, 0xFF, sizeof(g_losState));
        }
        uint8_t st = g_losState[idx];
        if (st != 0xFF) return st != 0;

        bool vis = HasLineOfSight(eye, aim);
        g_losState[idx] = vis ? 1 : 0;
        return vis;
    }

    bool RaycastTest(Vec3 from, Vec3 to, float& outHitDist) {
        outHitDist = -1.f;
        if (!M_raycast) ResolveRaycast();
        if (!M_raycast) return false;
        if (!O_hitDist) HitDistOffset();

        Vec3 d = to - from;
        float len = d.len();
        if (len < 0.01f) return false;
        Vec3 dir{ d.x / len, d.y / len, d.z / len };

        alignas(16) unsigned char hitBuf[160];
        memset(hitBuf, 0, sizeof(hitBuf));
        if (!ShootRay(from, dir, len, hitBuf)) return false;

        Vec3 hitPoint{ 0.f, 0.f, 0.f };
        se_read_bytes(hitBuf, &hitPoint, sizeof(Vec3));
        outHitDist = (hitPoint - from).len();
        if (outHitDist < 0.01f)
            se_read_bytes(hitBuf + O_hitDist, &outHitDist, sizeof(float));
        return true;
    }

    float CameraCotHalfFovY() {
        if (!g_camera) return 1.732f;
        Il2CppClass* ck = SafeClass(g_camera);
        if (!ck) return 1.732f;
        if (!M_camProj) M_camProj = MethodCached(ck, "get_projectionMatrix", 0);
        float P[16] = {};
        if (!ReadMatrix(g_camera, M_camProj, P)) return 1.732f;
        if (P[5] <= 0.0001f) return 1.732f;
        return P[5];
    }

    float FovRadiusPx(float deg) {
        float cot = CameraCotHalfFovY();
        float h = (float)ScreenH();
        if (cot <= 0.f || h <= 0.f) return 99999.f;
        return (h * 0.5f) * tanf(deg * 0.01745329252f) * cot;
    }

    Vec3 BonePoint(Vec3 head, Vec3 feet, int boneMode) {
        const float dy = head.y - feet.y;
        switch (boneMode) {
        case 2: return Vec3(head.x, feet.y + dy * 0.85f, head.z);
        case 3: return Vec3(head.x, feet.y + dy * 0.65f, head.z);
        case 4: return Vec3(head.x, feet.y + dy * 0.30f, head.z);
        case 5: {
            static int pick = 0;
            static uint64_t last = 0;
            const uint64_t now = GetTickCount64();
            if (now - last > 250) { last = now; pick = (int)(now % 3); }
            const float t = (pick == 0) ? 0.85f : (pick == 1 ? 0.65f : 0.30f);
            return Vec3(head.x, feet.y + dy * t, head.z);
        }
        default: return head;
        }
    }

    Vec3 TargetAimPos(Il2CppObject* p, int boneMode) {
        if (!p) return Vec3();
        EnterCriticalSection(&g_targetCs);
        for (int i = 0; i < g_targetCount; ++i) {
            if (g_targets[i].player != p) continue;
            Target& t = g_targets[i];
            Vec3 h = t.headBone ? GetPos(t.headBone) : t.head;
            Vec3 f = t.footBone ? GetPos(t.footBone) : t.feet;
            LeaveCriticalSection(&g_targetCs);
            return BonePoint(h, f, boneMode);
        }
        LeaveCriticalSection(&g_targetCs);
        return Vec3();
    }

    void LockTargets() {
        if (g_targets[0].player == nullptr && g_targetCount == 0) return;
        __try {
            EnterCriticalSection(&g_targetCs);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            InitializeCriticalSection(&g_targetCs);
            EnterCriticalSection(&g_targetCs);
        }
    }

    void UnlockTargets() {
        __try {
            LeaveCriticalSection(&g_targetCs);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }

    void* MethodPtr(MethodInfo* m) {
        if (!m) return nullptr;
        void* p0 = nullptr; void* p8 = nullptr;
        se_read_ptr(m, 0, &p0);
        se_read_ptr(m, 8, &p8);
        if (p0 && se_is_exec(p0)) return p0;
        if (p8 && se_is_exec(p8)) return p8;
        return nullptr;
    }

    void DumpFields(Il2CppClass* k, const char* tag) {
        if (!k) { Log("[dump] classe %s nula", tag ? tag : "?"); return; }
        Log("[dump] ---- fields de %s ----", tag ? tag : "?");
        void* iter = nullptr;
        for (;;) {
            FieldInfo* f = se_class_get_fields(k, &iter);
            if (!f) break;
            const char* n = se_field_name(f);
            if (n) Log("[dump]   %-34s @ 0x%04X", n, (unsigned)se_field_offset(f));
        }
        Log("[dump] ---- fim ----");
    }

    void DumpFields(const char* className) { DumpFields(FindClass(className), className); }

    // ======================= weapon stats =======================
    static void EnsureGunOffsets(Il2CppClass* k) {
        if (!k || k == g_gsCls) return;
        const char* kn = se_class_name(k);
        if (!kn || (!strstr(kn, "gunStats") && !strstr(kn, "GunStats"))) return;

        g_gsCls = k;
        O_gsMagSize = PickFieldOff(k, { "magSize", "magazineSize", "maxAmmo" }, "Int32");
        O_gsAmmo = PickFieldOff(k, { "ammoInMagazine", "currentAmmo", "curAmmo" }, "Int32");
        O_gsGunObj = PickFieldOff(k, { "gunObject", "gunObj" });
        if (!O_gsMagSize) O_gsMagSize = 0xFC;
        if (!O_gsAmmo)    O_gsAmmo = 0x90;
        if (!O_gsGunObj)  O_gsGunObj = 0x148;

        Log("[sdk] gunStats offsets (cls=%s): mag=0x%X ammo=0x%X gunObj=0x%X",
            kn, O_gsMagSize, O_gsAmmo, O_gsGunObj);
    }

    static bool GunStatsLooksLive(Il2CppObject* s) {
        if (!s) return false;
        Il2CppClass* k = SafeClass(s);
        if (!k) return false;
        EnsureGunOffsets(k);

        if (O_gsGunObj && SafeObjField(s, O_gsGunObj)) return true;
        if (O_gsMagSize && SafeRead<int>((uintptr_t)s + O_gsMagSize, 0) > 0) return true;
        if (O_gsAmmo && SafeRead<int>((uintptr_t)s + O_gsAmmo, 0) > 0) return true;
        return false;
    }

    static bool GunStatsEquipped(Il2CppObject* s) {
        if (!s || !O_gsGunObj) return false;
        Il2CppObject* go = SafeObjField(s, O_gsGunObj);
        if (!go) return false;
        Il2CppClass* gk = SafeClass(go);
        if (!gk) return false;
        MethodInfo* m = MethodCached(gk, "get_activeInHierarchy", 0);
        if (!m) return false;
        Il2CppObject* r = se_runtime_invoke(m, go, nullptr);
        if (!r) return false;
        void* p = se_object_unbox(r);
        return p && *(unsigned char*)p != 0;
    }

    static Il2CppObject* GunStatsFromView() {
        if (!g_localPlayer || !O_gunView) return nullptr;
        Il2CppObject* v = SafeObjField(g_localPlayer, O_gunView);
        if (!v) return nullptr;

        Il2CppObject* held = SafeObjField(v, 0x38);
        if (GunStatsLooksLive(held)) return held;

        int idx = SafeRead<int>((uintptr_t)v + 0x48, -1);
        Il2CppObject* arr = SafeObjField(v, 0x30);
        if (arr && idx >= 0) {
            int len = 0;
            if (il2cpp_array_length) len = (int)il2cpp_array_length((const Il2CppArray*)arr);
            if (len <= 0) len = SafeRead<int>((uintptr_t)arr + 0x18, 0);
            if (idx < len) {
                void* e = nullptr;
                if (se_read_ptr(arr, 0x20 + (size_t)idx * 8, &e) && e && PlausibleObj(e) &&
                    GunStatsLooksLive((Il2CppObject*)e))
                    return (Il2CppObject*)e;
            }
        }
        return nullptr;
    }

    Il2CppObject* FindGunStatsObj() {
        if (!g_localPlayer) return nullptr;

        if (Il2CppObject* g = GunStatsFromView()) return g;

        Il2CppObject* cand = O_gunStats ? SafeObjField(g_localPlayer, O_gunStats) : nullptr;
        if (GunStatsLooksLive(cand)) return cand;

        if (O_gunStatsArray) {
            Il2CppObject* arr = SafeObjField(g_localPlayer, O_gunStatsArray);
            if (arr) {
                int len = 0;
                if (il2cpp_array_length) len = (int)il2cpp_array_length((const Il2CppArray*)arr);
                if (len <= 0) len = SafeRead<int>((uintptr_t)arr + 0x18, 0);

                if (len > 0 && len <= 128) {
                    Il2CppObject* firstLive = nullptr;
                    int firstLiveIdx = -1;

                    for (int i = 0; i < len; ++i) {
                        void* e = nullptr;
                        if (!se_read_ptr(arr, 0x20 + (size_t)i * 8, &e) || !e) continue;
                        if (!PlausibleObj(e)) continue;
                        Il2CppObject* g = (Il2CppObject*)e;
                        if (!GunStatsLooksLive(g)) continue;

                        if (!firstLive) { firstLive = g; firstLiveIdx = i; }
                        if (!GunStatsEquipped(g)) continue;

                        static int lastEq = -1;
                        if (lastEq != i) {
                            lastEq = i;
                            Log("[sdk] gunStats EQUIPADO = loadout[%d] (%p)", i, g);
                        }
                        return g;
                    }
                    if (firstLive) {
                        static int lastIdx = -1;
                        if (lastIdx != firstLiveIdx) {
                            lastIdx = firstLiveIdx;
                            Log("[sdk] gunStats vivo = loadout[%d] (%p) [nenhum ativo]",
                                firstLiveIdx, firstLive);
                        }
                        return firstLive;
                    }
                }
            }
        }

        if (cand) return cand;

        Il2CppClass* k = SafeClass(g_localPlayer);
        if (k) {
            void* it = nullptr;
            for (;;) {
                FieldInfo* f = se_class_get_fields(k, &it);
                if (!f) break;
                const Il2CppType* t = il2cpp_field_get_type ? il2cpp_field_get_type(f) : nullptr;
                if (!strstr(TypeNameCached(t), "Gun") && !strstr(TypeNameCached(t), "Weapon")) continue;
                uint32_t off = (uint32_t)se_field_offset(f);
                if (!off) continue;
                Il2CppObject* s = SafeObjField(g_localPlayer, off);
                if (GunStatsLooksLive(s)) return s;
            }
        }
        return nullptr;
    }

    Il2CppObject* GetGunStatsLive() { return FindGunStatsObj(); }

    int ResolveWeaponFields(Il2CppObject* stats,
        uint32_t& cur, uint32_t& max, uint32_t& reserve,
        uint32_t& reload, uint32_t& spread, uint32_t& rate, uint32_t& dmg) {
        cur = max = reserve = reload = spread = rate = dmg = 0;
        g_rateIsRpm = false;
        if (!stats) return 0;
        Il2CppClass* k = SafeClass(stats);
        if (!k) return 0;

        cur = PickFieldOff(k, { "ammoInMagazine", "currentAmmo", "curAmmo",
                                "mCurrentAmmo", "magazineAmmo" }, "Int32");
        max = PickFieldOff(k, { "magSize", "maxAmmo", "ammoMax",
                                "mMaxAmmo", "magazineSize", "clipSize" }, "Int32");
        reserve = PickFieldOff(k, { "totalAmmo", "reserveAmmo", "ammoReserve",
                                    "spareAmmo", "totalMags" }, "Int32");

        reload = PickFieldOff(k, { "reloading", "isReloading", "mReloading",
                                   "hasShellReloading", "requiresClearChamber" }, "Boolean");
        if (!reload) reload = PickFieldOff(k, { "reloadTimer", "reloadingTimer",
                                                "reloadAnimationTime" }, "Single");

        spread = PickFieldOff(k, { "hipMinSpread", "spread", "currentSpread", "mSpread" }, "Single");
        if (!spread) spread = PickFieldOff(k, { "adsMinSpread" }, "Single");

        rate = PickFieldOff(k, { "shotDelay", "fireDelay", "chamberTime", "cooldown" }, "Single");
        if (!rate) {
            rate = PickFieldOff(k, { "RPM" }, "Single");
            g_rateIsRpm = (rate != 0);
        }

        dmg = PickFieldOff(k, { "damage", "damagePerShot", "bulletDamage",
                                "hitDamage", "mDamage", "dmg" });

        int n = 0;
        if (cur) ++n; if (max) ++n; if (reserve) ++n; if (reload) ++n;
        if (spread) ++n; if (rate) ++n; if (dmg) ++n;

        Log("[weapons] stats cls=%s cur=0x%X max=0x%X reserve=0x%X reload=0x%X spread=0x%X rate=0x%X(%s) dmg=0x%X",
            se_class_name(k), cur, max, reserve, reload, spread, rate,
            g_rateIsRpm ? "RPM" : "delay", dmg);
        return n;
    }

    void DumpFieldsWithValues(Il2CppObject* o) {
        if (!o) { Log("[dumpv] obj nulo"); return; }
        Il2CppClass* k = SafeClass(o);
        if (!k) return;
        Log("[dumpv] ---- %s @ %p ----", se_class_name(k), o);

        void* it = nullptr;
        int n = 0;
        for (;;) {
            FieldInfo* f = se_class_get_fields(k, &it);
            if (!f) break;
            const char* fn = se_field_name(f);
            if (!fn) continue;
            const Il2CppType* t = il2cpp_field_get_type ? il2cpp_field_get_type(f) : nullptr;
            const char* tn = TypeNameCached(t);
            uint32_t off = (uint32_t)se_field_offset(f);
            if (!off) continue;

            if (strstr(tn, "Int32") || strstr(tn, "UInt32") || strstr(tn, "Int16") || strstr(tn, "Byte")) {
                int v = 0; se_read_int(o, off, &v);
                Log("[dumpv]   %-28s : %-14s @0x%X = %d", fn, tn, off, v);
            }
            else if (strstr(tn, "Single")) {
                Log("[dumpv]   %-28s : %-14s @0x%X = %.3f", fn, tn, off, se_read_float(o, off));
            }
            else if (strstr(tn, "Boolean")) {
                Log("[dumpv]   %-28s : %-14s @0x%X = %d", fn, tn, off, (int)se_read_bool(o, off));
            }
            else {
                void* p = nullptr;
                if (se_read_ptr(o, off, &p))
                    Log("[dumpv]   %-28s : %-14s @0x%X = %p", fn, tn, off, p);
                else
                    Log("[dumpv]   %-28s : %-14s @0x%X = -", fn, tn, off);
            }
            if (++n > 120) break;
        }
        Log("[dumpv] ---- fim ----");
    }

    // ======================= local player =======================
    static Il2CppObject* FindLocalPlayer() {
        static const char* classes[] = { "PlayerScript", "Entity", "GameManager", "Game", "HUD" };
        static const char* fields[] = { "localPlayer", "LocalPlayer", "localEntity", "myPlayer",
                                        "myEntity", "local", "player" };
        for (size_t c = 0; c < sizeof(classes) / sizeof(classes[0]); ++c) {
            Il2CppClass* k = FindClass(classes[c]); if (!k) continue;
            for (size_t f = 0; f < sizeof(fields) / sizeof(fields[0]); ++f) {
                FieldInfo* fi = se_class_get_field_from_name(k, fields[f]); if (!fi) continue;
                Il2CppObject* v = nullptr;
                se_field_static_get_value(fi, &v);
                if (v && PlausibleObj(v)) { Log("[sdk] localPlayer = %s.%s", classes[c], fields[f]); return v; }
            }
            MethodInfo* m = se_class_get_method_from_name(k, "get_localPlayer", 0);
            if (!m) m = se_class_get_method_from_name(k, "get_LocalPlayer", 0);
            if (m) {
                Il2CppObject* v = se_runtime_invoke(m, nullptr, nullptr);
                if (v && PlausibleObj(v)) { Log("[sdk] localPlayer = %s::get_localPlayer", classes[c]); return v; }
            }
        }
        return nullptr;
    }

    Il2CppObject* GetLocalPlayer() {
        if (!g_localPlayer) { g_localPlayer = FindLocalPlayer(); g_local = g_localPlayer; }
        return g_localPlayer;
    }

    // ======================= alvos =======================
    static void SyncAliases(Target& t) {
        t.obj = t.player;
        t.hp = t.health;
        t.enemy = (O_team == 0) ? true : (g_localTeam < 0 || t.team < 0 || t.team != g_localTeam);
        t.lastSeen = GetTickCount64();
    }

    static void ResolveHeadFeet(Target& t) {
        const float H = 1.75f;

        bool haveHead = (t.headBone != nullptr);
        bool haveFeet = (t.footBone != nullptr);

        if (haveHead) t.head = GetPos(t.headBone);
        if (haveFeet) t.feet = GetPos(t.footBone);

        if (haveHead && haveFeet) {
            float dy = t.head.y - t.feet.y;
            if (dy < 0.6f || dy > 2.6f)
                t.feet = Vec3(t.head.x, t.head.y - H, t.head.z);
        }
        else if (haveHead) {
            t.feet = Vec3(t.head.x, t.head.y - H, t.head.z);
        }
        else if (haveFeet) {
            t.head = Vec3(t.feet.x, t.feet.y + H, t.feet.z);
        }
        else {
            t.head = t.pos;
            t.feet = Vec3(t.pos.x, t.pos.y - H, t.pos.z);
        }

        if (t.head.y < t.feet.y) {
            float mid = (t.head.y + t.feet.y) * 0.5f;
            t.head.y = mid + H * 0.5f;
            t.feet.y = mid - H * 0.5f;
        }
    }

    static bool IsLocalEntity(Il2CppObject* e) {
        if (!e) return true;
        if (e == g_localPlayer || e == g_local) return true;
        if (O_isLocal && se_read_bool(e, O_isLocal)) return true;
        if (O_localPlayerScript && kPlayerScript && IsA(e, kPlayerScript)) {
            Il2CppObject* lp = SafeObjField(e, O_localPlayerScript);
            if (lp == e) return true;
        }
        return false;
    }

    static void AdoptLocal(Il2CppObject* e) {
        if (!e) return;
        const bool mudou = (e != g_localPlayer);
        g_localPlayer = e;
        g_local = e;
        g_localTeam = ReadTeam(e);
        if (mudou)
            Log("[sdk] localPlayer adotado (%p) team=%d", e, g_localTeam);
    }

    struct FoundEnt { int slot; Il2CppObject* e; };

    void RefreshTargets() {
        if (!g_localPlayer) { g_localPlayer = FindLocalPlayer(); g_local = g_localPlayer; }
        if (g_localPlayer) g_localTeam = ReadTeam(g_localPlayer);

        if (!sObjCls) {
            sObjCls = FindClass("Object", "UnityEngine");
            if (!sObjCls) return;
            sFindObjs = se_class_get_method_from_name(sObjCls, "FindObjectsOfType", 1);
            if (!sFindObjs) return;
        }

        if (sOnlyEntity < 0) {
            sOnlyEntity = 0;
            if (kPlayerScript && kEntity) {
                for (Il2CppClass* c = kPlayerScript; c; c = se_class_parent(c))
                    if (c == kEntity) { sOnlyEntity = 1; break; }
            }
            Log("[sdk] scan: %s", sOnlyEntity ? "so Entity (PlayerScript deriva)" : "PlayerScript + Entity");
        }

        Il2CppClass* scans[2];
        int nScan = 0;
        if (sOnlyEntity) {
            Il2CppClass* e = kEntity ? kEntity : FindClass("Entity");
            if (e) scans[nScan++] = e;
        }
        else {
            Il2CppClass* p = kPlayerScript ? kPlayerScript : FindClass("PlayerScript");
            Il2CppClass* e = kEntity ? kEntity : FindClass("Entity");
            if (p) scans[nScan++] = p;
            if (e && e != p) scans[nScan++] = e;
        }
        if (nScan == 0) return;

        MethodInfo* fot = sFindObjs;

        ++g_slotGen;

        std::vector<FoundEnt> found;
        found.reserve(MAX_TARGETS);
        int totalObjs = 0;

        for (int s = 0; s < nScan; ++s) {
            Il2CppClass* sc = scans[s];
            if (!sc) continue;

            Il2CppType* t = se_class_get_type(sc);
            if (!t) continue;
            Il2CppObject* typeObj = se_type_get_object(t);
            if (!typeObj) continue;

            void* args[1]; args[0] = typeObj;
            Il2CppArray* arr = (Il2CppArray*)se_runtime_invoke(fot, nullptr, args);
            if (!arr) continue;

            uint32_t n = il2cpp_array_length ? il2cpp_array_length(arr) : 0;
            totalObjs += (int)n;

            for (uint32_t i = 0; i < n; ++i) {
                Il2CppObject* e = (Il2CppObject*)se_array_at(arr, i);
                if (!e || !PlausibleObj(e)) continue;
                if (IsLocalEntity(e)) { AdoptLocal(e); continue; }

                bool dup = false;
                for (auto& f : found) if (f.e == e) { dup = true; break; }
                if (!dup) found.push_back({ -1, e });
            }
        }

        for (auto& f : found)
            for (int i = 0; i < MAX_TARGETS; ++i)
                if (g_slotOwner[i] == f.e) { f.slot = i; g_slotSeen[i] = g_slotGen; break; }

        for (auto& f : found) {
            if (f.slot >= 0) continue;
            for (int i = 0; i < MAX_TARGETS; ++i) {
                if (!g_slotOwner[i] || g_slotSeen[i] != g_slotGen) {
                    g_slotOwner[i] = f.e; g_slotSeen[i] = g_slotGen; f.slot = i; break;
                }
            }
        }
        for (int i = 0; i < MAX_TARGETS; ++i)
            if (g_slotSeen[i] != g_slotGen) g_slotOwner[i] = nullptr;

        std::sort(found.begin(), found.end(),
            [](const FoundEnt& a, const FoundEnt& b) { return a.slot < b.slot; });

        EnterCriticalSection(&g_targetCs);
        int c = 0;
        for (auto& f : found) {
            if (c >= kMaxTargets || f.slot < 0) break;

            Target& tgt = g_targets[c];
            if (tgt.player != f.e) {
                memset(&tgt, 0, sizeof(Target));
                tgt.player = f.e;
                tgt.obj = f.e;
                tgt.slot = (uint32_t)f.slot;
                tgt.team = ReadTeam(f.e);
                tgt.pos = GetPos(f.e);
                GetNameBuf(f.e, tgt.name, sizeof(tgt.name));
                if (O_weapon) se_read_int(f.e, O_weapon, &tgt.weapon);

                Il2CppClass* ek = SafeClass(f.e);
                const char* en = ek ? se_class_name(ek) : nullptr;
                tgt.npc = (en && strstr(en, "NPC") != nullptr);

                ResolveBones(tgt, f.e);
            }

            tgt.health = ReadHealth(f.e);
            tgt.alive = (tgt.health > 0.5f);
            ResolveHeadFeet(tgt);
            tgt.onScreen = false;
            SyncAliases(tgt);
            ++c;
        }
        g_targetCount = c;
        LeaveCriticalSection(&g_targetCs);

        static ULONGLONG lastTeamLog = 0;
        ULONGLONG nowt = GetTickCount64();
        if (nowt - lastTeamLog >= 1000) {
            lastTeamLog = nowt;
            Log("[team] localTeam=%d O_team=0x%X kind=%d alvos=%d",
                g_localTeam, O_team, g_teamKind, g_targetCount);
            for (int i = 0; i < g_targetCount && i < 4; ++i) {
                Target& t = g_targets[i];
                int tt = ReadTeam(t.player);
                Log("[team]   alvo[%d] \"%s\" team=%d enemy=%d",
                    i, t.name, tt, (int)IsEnemy(t.player));
            }
        }

        static ULONGLONG lastScan2 = 0;
        ULONGLONG nows = GetTickCount64();
        if (nows - lastScan2 >= 2000) {
            lastScan2 = nows;
            Log("[sdk] scan2s: objetos=%d alvos=%d local=%p localVivo=%d",
                totalObjs, g_targetCount, g_localPlayer,
                (int)(g_localPlayer && SafeClass(g_localPlayer) != nullptr));
        }

        static int lastLog = -1;
        if (g_targetCount != lastLog) {
            lastLog = g_targetCount;
            Log("[sdk] scan: objetos=%d alvos=%d local=%p team=%d",
                totalObjs, g_targetCount, g_localPlayer, g_localTeam);
        }

        if (!g_dumpTarget && g_targetCount > 0) {
            g_dumpTarget = true;
            Target& t = g_targets[0];
            Log("[diag] alvo=%p pos=(%.2f,%.2f,%.2f) head=(%.2f,%.2f,%.2f) feet=(%.2f,%.2f,%.2f) hb=%p fb=%p hp=%.1f team=%d nome=\"%s\"",
                t.player, t.pos.x, t.pos.y, t.pos.z, t.head.x, t.head.y, t.head.z,
                t.feet.x, t.feet.y, t.feet.z, t.headBone, t.footBone, t.health, t.team, t.name);
            DumpStringFields(t.player);
        }
    }

    void UpdateTargets() {
        static ULONGLONG lastRefresh = 0;
        static ULONGLONG lastTfPrune = 0;
        ULONGLONG now = GetTickCount64();
        if (now - lastRefresh > 200) { lastRefresh = now; RefreshTargets(); }
        if (now - lastTfPrune > 2000) { lastTfPrune = now; PruneTfCache(); }

        if (!g_localPlayer) { g_localPlayer = FindLocalPlayer(); g_local = g_localPlayer; }
        if (!g_localPlayer) return;

        if (!SafeClass(g_localPlayer)) {
            Log("[sdk] localPlayer morto (%p), re-resolvendo", g_localPlayer);
            g_localPlayer = FindLocalPlayer();
            g_local = g_localPlayer;
            if (!g_localPlayer) return;
        }

        ++g_frameId;
        PlausibleBeginFrame(g_frameId);
        g_localPos = GetPos(g_localPlayer);
        g_localTeam = ReadTeam(g_localPlayer);

        Il2CppObject* camTf = CamTfObj();
        Vec3 eye = camTf ? GetPos(camTf) : g_localPos;
        if (eye.zero()) eye = Vec3(g_localPos.x, g_localPos.y + 1.6f, g_localPos.z);

        EnterCriticalSection(&g_targetCs);
        for (int i = 0; i < g_targetCount; ++i) {
            Target& t = g_targets[i];

            if (!PtrOk_(t.player)) { t.player = nullptr; t.obj = nullptr; continue; }
            if (!ValidInstance_(t.player)) { t.player = nullptr; t.obj = nullptr; continue; }

            t.health = ReadHealth(t.player);
            t.alive = (t.health > 0.5f);
            t.pos = GetPos(t.player);
            ResolveHeadFeet(t);

            float dx = t.head.x - g_localPos.x, dy = t.head.y - g_localPos.y, dz = t.head.z - g_localPos.z;
            t.dist = sqrtf(dx * dx + dy * dy + dz * dz);

            bool okH = WorldToScreen(t.head, t.scrHead);
            bool okF = WorldToScreen(t.feet, t.scrFeet);
            float h = t.scrFeet.y - t.scrHead.y;
            t.onScreen = okH && okF && (h > 1.f)
                && t.scrFeet.y > -64.f
                && t.scrHead.y < (float)ScreenH() + 64.f;

            SyncAliases(t);
        }
        LeaveCriticalSection(&g_targetCs);

        if ((C.espVisCheck || C.aimVisCheck) && LosReady()) {
            float cap = C.espMaxDist > C.aimMaxDist ? C.espMaxDist : C.aimMaxDist;
            if (cap < 50.f) cap = 300.f;
            cap += 50.f;

            Il2CppObject* localTf = g_localPlayer ? CachedTransform(g_localPlayer) : nullptr;

            EnterCriticalSection(&g_targetCs);
            const int n = g_targetCount;
            int budget = C.losPerFrame > 0 ? C.losPerFrame : 8;
            if (budget > n) budget = n;

            static int losCursor = 0;
            if (n > 0) { losCursor %= n; if (losCursor < 0) losCursor = 0; }

            int done = 0;
            for (int k = 0; k < n && done < budget; ++k) {
                int i = losCursor + k;
                if (i >= n) i -= n;

                Target& t = g_targets[i];
                if (!t.player || !t.alive) { t.visible = false; ++done; continue; }
                if (!t.onScreen) continue;
                if (t.dist > cap) { t.visible = false; ++done; continue; }

                Il2CppObject* ttf = CachedTransform(t.player);
                t.visible = HasLineOfSightEx(eye, t.head, localTf, ttf);
                ++done;
            }
            if (n > 0) losCursor = (losCursor + budget) % n;
            LeaveCriticalSection(&g_targetCs);
        }
    }

    void SnapshotInto(std::vector<Target>& out) {
        EnterCriticalSection(&g_targetCs);
        const int count = (g_targetCount < 0) ? 0 :
            ((g_targetCount > MAX_TARGETS) ? MAX_TARGETS : g_targetCount);
        out.resize((size_t)count);
        if (count > 0) memcpy(out.data(), g_targets, (size_t)count * sizeof(Target));
        LeaveCriticalSection(&g_targetCs);
    }

    std::vector<Target> Snapshot() {
        std::vector<Target> out;
        SnapshotInto(out);
        return out;
    }

    // ======================= aim =======================
    bool AimLock(Il2CppObject* target, Quat& savedOriginal) {
        Il2CppObject* camTf = CamTfObj();
        if (!target || !camTf) return false;

        if (!GetRot(camTf, savedOriginal)) return false;
        g_savedPlayerRot = GetPlayerRot(g_localPlayer);

        Vec3 me = GetPos(camTf);
        Vec3 tp = TargetAimPos(target, C.aimBone);
        if (tp.zero()) tp = GetPos(target);
        Vec3 dir{ tp.x - me.x, tp.y - me.y, tp.z - me.z };
        float L = dir.len();
        if (L < 0.0001f) return false;
        dir = dir * (1.f / L);

        Quat q = LookRot(dir);
        if (!SetRot(camTf, q)) return false;
        if (g_localPlayer) SetPlayerRot(g_localPlayer, q);
        return true;
    }

    void RestoreAim(Il2CppObject*, const Quat& q) {
        Il2CppObject* camTf = CamTfObj();
        if (camTf) SetRot(camTf, q);
        if (g_localPlayer) SetPlayerRot(g_localPlayer, g_savedPlayerRot);
    }

    bool FireBulletReady() {
        if (!M_FireBullet) return false;
        uint32_t np = il2cpp_method_get_param_count ? il2cpp_method_get_param_count(M_FireBullet) : 0;
        if (np == 0) return false;
        for (uint32_t i = 0; i < np; ++i) {
            const Il2CppType* t = il2cpp_method_get_param ? il2cpp_method_get_param(M_FireBullet, i) : nullptr;
            const char* tn = TypeNameCached(t);
            if (strstr(tn, "Vector3") || strstr(tn, "Single") || strstr(tn, "Int32") ||
                strstr(tn, "Boolean") || strstr(tn, "Double") || strstr(tn, "Int64")) continue;
            return false;
        }
        return true;
    }

    void FireBulletAt(Vec3 pos) {
        if (!M_FireBullet || !g_localPlayer) return;

        Vec3 me = GetPos(g_localPlayer);
        Vec3 dir{ pos.x - me.x, pos.y - me.y, pos.z - me.z };
        float L = dir.len();
        if (L > 0.0001f) dir = dir * (1.f / L);

        const int dirSlot = g_fireDirSlot.load();
        const Vec3 muzzle = MuzzlePos();

        uint32_t np = il2cpp_method_get_param_count ? il2cpp_method_get_param_count(M_FireBullet) : 0;
        if (np == 0) np = 1;
        if (np > 16) np = 16;

        int vCount = 0;
        for (uint32_t i = 0; i < np; ++i) {
            const Il2CppType* t = il2cpp_method_get_param ? il2cpp_method_get_param(M_FireBullet, i) : nullptr;
            if (strstr(TypeNameCached(t), "Vector3")) ++vCount;
        }
        const int dirVIndex = (dirSlot >= 0) ? dirSlot : (vCount > 0 ? vCount - 1 : 0);

        Vec3          vbuf[16]{};
        float         fbuf[16]{};
        int32_t       ibuf[16]{};
        double        dbuf[16]{};
        unsigned char bbuf[16]{};
        void* args[16]{};

        int vIndex = 0;
        for (uint32_t i = 0; i < np; ++i) {
            const Il2CppType* t = il2cpp_method_get_param ? il2cpp_method_get_param(M_FireBullet, i) : nullptr;
            const char* tn = TypeNameCached(t);
            if (strstr(tn, "Vector3")) {
                vbuf[i] = (vIndex == dirVIndex) ? dir : muzzle;
                args[i] = &vbuf[i];
                ++vIndex;
            }
            else if (strstr(tn, "Single"))  args[i] = &fbuf[i];
            else if (strstr(tn, "Int32"))   args[i] = &ibuf[i];
            else if (strstr(tn, "Double"))  args[i] = &dbuf[i];
            else if (strstr(tn, "Boolean")) args[i] = &bbuf[i];
            else args[i] = nullptr;
        }
        se_runtime_invoke(M_FireBullet, g_localPlayer, args);
    }

    // ======================= invalidacao de runtime =======================
    static void ClearRuntimeCaches() {
        { std::lock_guard<std::mutex> lk(g_methodMx); g_methodCacheN = 0; g_methodEvict = 0; }
        { std::lock_guard<std::mutex> lk(g_typeMx);   g_typeCacheN = 0;   g_typeEvict = 0; }
        { std::lock_guard<std::mutex> lk(g_tfMx);     g_tfCache.clear(); }
        memset(g_boneCache, 0, sizeof(g_boneCache));
        g_boneEvict = 0;
        memset(g_syncCache, 0, sizeof(g_syncCache));
        g_syncCacheN = 0;
        g_syncDumped = false;
        g_strOffs.clear();
        g_pickedNameOff = 0;
        memset(g_slotOwner, 0, sizeof(g_slotOwner));
        memset(g_slotSeen, 0, sizeof(g_slotSeen));

        M_raycast = nullptr; g_rayKind = 0; O_hitDist = 0; O_hitCollider = 0;
        g_losLogged = false;
        M_lookRot = nullptr; M_get_parent = nullptr; M_findFromId = nullptr;
        M_camW2C = nullptr; M_camProj = nullptr; M_camMain = nullptr;
        M_get_name = nullptr; M_get_childCount = nullptr; M_GetChild = nullptr; M_GetComponent = nullptr;
        M_shoot = M_fireBullet = M_FireBullet = nullptr;
        M_set_position = M_set_forward = nullptr;
        M_get_fov = M_set_fov = nullptr;
        g_gsCls = nullptr;
        sObjCls = nullptr; sFindObjs = nullptr; sOnlyEntity = -1;
        g_hierDumped = false; g_dumpTarget = false;

        O_health = O_alive = O_team = O_isLocal = O_name = O_weapon = 0;
        O_vel = O_speed = O_rot = O_rotation = O_cam = O_camTf = 0;
        O_mCamera = O_humanMeshAnimator = O_humanMeshReferences = 0;
        O_stats = O_gunView = O_gunStats = O_gunStatsOnView = 0;
        O_curAmmo = O_maxAmmo = O_reloading = O_spread = O_fireRate = O_dmg = 0;
        O_gsMagSize = O_gsAmmo = O_gsGunObj = 0;
        O_gunStatsArray = 0;
        O_localPlayerScript = 0;
        g_teamKind = 0;

        g_localPlayer = nullptr; g_local = nullptr; g_camera = nullptr; g_aimTarget = nullptr;
        g_targetCount = 0;
        g_viewProjOk = false;
        g_localTeam = -1;

        g_plausStamp = 0;

        LosInvalidate();
    }

    void InvalidateRuntime() { ClearRuntimeCaches(); }

    bool OnDomainReload() {
        g_phase = "sdk.reload";
        if (!g_inited) return Init();
        ClearRuntimeCaches();
        BuildCache();
        ResolveOffsets();
        ResolveMethods();
        BuildStringOffsets();
        ResolveRaycast();
        g_phase = "sdk.done";
        Log("[sdk] OnDomainReload: metadata re-resolvida");
        return true;
    }

    // ======================= init / shutdown =======================
    void Shutdown() {
        g_phase = "sdk.shutdown";
        ClearRuntimeCaches();
        { std::lock_guard<std::mutex> lk(g_classesMx); g_classes.clear(); }
        if (g_inited) { DeleteCriticalSection(&g_targetCs); g_inited = false; }
    }

    static void ResolveOffsets() {
        if (kEntity) {
            O_health = PickFieldOff(kEntity, { "_health", "health", "hp" });
            O_alive = PickFieldOff(kEntity, { "shouldBeAlive", "alive", "isAlive" });
            O_isLocal = PickFieldOff(kEntity, { "isMine", "isLocalPlayer", "isOwner" }, "Boolean");
            O_rotation = PickFieldOff(kEntity, { "rotation" });
            O_camTf = PickFieldOff(kEntity, { "cameraTransform", "camTransform" }, "Transform");
            if (!O_camTf) O_camTf = PickFieldOff(kEntity, { "cameraTransform", "camTransform" });
            O_vel = PickFieldOff(kEntity, { "movingVelocity", "velocity" });
            O_name = PickFieldOff(kEntity, { "username", "playerName", "name" });
        }
        if (kPlayerScript) {
            if (!O_health) O_health = PickFieldOff(kPlayerScript, { "_health", "health", "hp" });
            if (!O_alive)  O_alive = PickFieldOff(kPlayerScript, { "shouldBeAlive", "alive", "isAlive" });
            if (!O_isLocal) O_isLocal = PickFieldOff(kPlayerScript, { "isMine", "isLocalPlayer" }, "Boolean");
            if (!O_rotation) O_rotation = PickFieldOff(kPlayerScript, { "rotation" });
            if (!O_camTf) O_camTf = PickFieldOff(kPlayerScript, { "cameraTransform", "camTransform", "mCameraTransform" }, "Transform");
            if (!O_camTf) O_camTf = PickFieldOff(kPlayerScript, { "cameraTransform", "camTransform", "mCameraTransform" });
            if (!O_name) O_name = PickFieldOff(kPlayerScript, { "username", "playerName", "name" });

            O_localPlayerScript = PickFieldOff(kPlayerScript, { "localPlayerScript" });
            O_gunStats = PickFieldOff(kPlayerScript, { "mpCurrentGunStats", "gunStats" });
            O_gunView = PickFieldOff(kPlayerScript, { "fpsGunView", "gunView" });
            O_gunStatsOnView = PickFieldOff(kPlayerScript, { "gunStatsOnView", "statsOnView" });
            O_weapon = PickFieldOff(kPlayerScript, { "weapon", "currentWeapon" });
            O_speed = PickFieldOff(kPlayerScript, { "movementSpeedMultiplier", "lastMoveVelocity" });
            O_mCamera = PickFieldOff(kPlayerScript, { "mCamera", "camera" });
            O_humanMeshAnimator = PickFieldOff(kPlayerScript, { "humanMeshAnimator", "meshAnimator", "animator" });
            O_humanMeshReferences = PickFieldOff(kPlayerScript, { "humanMeshReferences", "meshReferences" });
        }

        g_teamKind = 0;
        if (kPlayerScript) {
            O_team = PickFieldOff(kPlayerScript, { "mpTeamNumber" }, "Byte");
            if (O_team) g_teamKind = 1;
            if (!O_team) { O_team = PickFieldOff(kPlayerScript, { "mpTeamNumber" }, "Int32"); if (O_team) g_teamKind = 2; }
            if (!O_team) { O_team = PickFieldOff(kPlayerScript, { "mpTeamNumber" }); if (O_team) g_teamKind = 3; }
            if (!O_team) { O_team = PickFieldOff(kPlayerScript, { "team", "teamNumber" }, "Byte"); if (O_team) g_teamKind = 1; }
            if (!O_team) { O_team = PickFieldOff(kPlayerScript, { "team", "teamNumber" }, "Int32"); if (O_team) g_teamKind = 2; }
            if (!O_team) { O_team = PickFieldOff(kPlayerScript, { "team", "teamNumber" }); if (O_team) g_teamKind = 3; }
        }
        if (!O_team && kEntity) {
            O_team = PickFieldOff(kEntity, { "team", "teamNumber", "mpTeamNumber" }, "Byte");
            if (O_team) g_teamKind = 1;
            if (!O_team) { O_team = PickFieldOff(kEntity, { "team", "teamNumber", "mpTeamNumber" }, "Int32"); if (O_team) g_teamKind = 2; }
            if (!O_team) { O_team = PickFieldOff(kEntity, { "team", "teamNumber", "mpTeamNumber" }); if (O_team) g_teamKind = 3; }
        }

        if (!O_rotation) O_rotation = 0x9B8;
        if (!O_camTf)    O_camTf = 0x828;
        if (!O_alive)    O_alive = 0x63C;
        if (!O_health)   O_health = 0x108;
        if (!O_gunStats) O_gunStats = 0x570;
        if (!O_name)     O_name = 0xA68;
        if (!O_weapon)   O_weapon = 0x1A8;
        if (!O_mCamera)  O_mCamera = 0xAC8;
        if (!O_humanMeshAnimator) O_humanMeshAnimator = 0xBC0;
        if (!O_humanMeshReferences) O_humanMeshReferences = 0xA40;

        O_rot = O_rotation;
        O_cam = O_camTf;

        {
            const char* rtP = FieldTypeOf(kPlayerScript, "rotation");
            const char* rtE = FieldTypeOf(kEntity, "rotation");
            g_rotIsSyncVar = (rtP && strstr(rtP, "SyncVar")) || (rtE && strstr(rtE, "SyncVar"));

            const char* rotT = (rtP && rtP[0] != '?') ? rtP : rtE;
            O_rotationIsQuat = (!rotT) || rotT[0] == '?' || (strstr(rotT, "Quaternion") != nullptr);

            const char* hpT = FieldTypeOf(kPlayerScript, "_health");
            if (!hpT || hpT[0] == '?') hpT = FieldTypeOf(kEntity, "_health");
            g_healthIsSync = hpT && strstr(hpT, "SyncVar") != nullptr;

            const char* tmT = FieldTypeOf(kPlayerScript, "mpTeamNumber");
            if (!tmT || tmT[0] == '?') tmT = FieldTypeOf(kEntity, "team");
            g_teamIsSync = tmT && strstr(tmT, "SyncVar") != nullptr;
        }

        if (!kGunStats) kGunStats = FindClass("gunStats");
        if (!kGunStats) kGunStats = FindClass("GunStats");
        if (kGunStats) {
            O_gsMagSize = PickFieldOff(kGunStats, { "magSize", "magazineSize", "maxAmmo" }, "Int32");
            O_gsAmmo = PickFieldOff(kGunStats, { "ammoInMagazine", "currentAmmo" }, "Int32");
            O_gsGunObj = PickFieldOff(kGunStats, { "gunObject" });
        }
        if (!O_gsMagSize) O_gsMagSize = 0xFC;
        if (!O_gsAmmo)    O_gsAmmo = 0x90;
        if (!O_gsGunObj)  O_gsGunObj = 0x148;

        if (kPlayerScript && !O_gunStatsArray) {
            void* it = nullptr;
            for (;;) {
                FieldInfo* f = se_class_get_fields(kPlayerScript, &it);
                if (!f) break;
                const Il2CppType* t = il2cpp_field_get_type ? il2cpp_field_get_type(f) : nullptr;
                if (strstr(TypeNameCached(t), "gunStats[]")) {
                    O_gunStatsArray = (uint32_t)se_field_offset(f);
                    Log("[sdk] array de loadout = 0x%X (%s)", O_gunStatsArray, se_field_name(f));
                    break;
                }
            }
        }

        Log("[sdk] offsets: hp=0x%X(%s) name=0x%X(%s) team=0x%X(%s) kind=%d rot=0x%X(%s) camTf=0x%X(%s) anim=0x%X syncRot=%d",
            O_health, FieldTypeOf(kPlayerScript, "_health"),
            O_name, FieldTypeOf(kPlayerScript, "username"),
            O_team, FieldTypeOf(kPlayerScript, "mpTeamNumber"), g_teamKind,
            O_rotation, FieldTypeOf(kPlayerScript, "rotation"),
            O_camTf, FieldTypeOf(kPlayerScript, "cameraTransform"),
            O_humanMeshAnimator, (int)g_rotIsSyncVar);
        Log("[sdk] gunStats: mag=0x%X ammo=0x%X gunObj=0x%X array=0x%X",
            O_gsMagSize, O_gsAmmo, O_gsGunObj, O_gunStatsArray);
    }

    static MethodInfo* FindMethodByName(Il2CppClass* k, const char* name) {
        if (!k || !il2cpp_class_get_methods) return nullptr;
        void* it = nullptr;
        for (;;) {
            const MethodInfo* m = il2cpp_class_get_methods(k, &it);
            if (!m) break;
            if (strcmp(il2cpp_method_get_name(m), name) == 0) return (MethodInfo*)m;
        }
        return nullptr;
    }

    static void ResolveMethods() {
        if (kPlayerScript) {
            M_shoot = se_class_get_method_from_name(kPlayerScript, "SemiAutoShoot", 0);
            if (!M_shoot) M_shoot = se_class_get_method_from_name(kPlayerScript, "Shoot", 0);
            M_FireBullet = se_class_get_method_from_name(kPlayerScript, "FireBullet", 7);
            if (!M_FireBullet) M_FireBullet = FindMethodByName(kPlayerScript, "FireBullet");
            M_fireBullet = M_FireBullet;
        }
        if (kTransform) {
            M_set_position = se_class_get_method_from_name(kTransform, "set_position", 1);
            M_set_forward = se_class_get_method_from_name(kTransform, "set_forward", 1);
        }
        if (kCamera) {
            M_get_fov = se_class_get_method_from_name(kCamera, "get_fieldOfView", 0);
            M_set_fov = se_class_get_method_from_name(kCamera, "set_fieldOfView", 1);
        }

        if (M_FireBullet) {
            uint32_t np = il2cpp_method_get_param_count ? il2cpp_method_get_param_count(M_FireBullet) : 0;
            Log("[sdk] FireBullet: %u params ready=%d ret=%s", np, (int)FireBulletReady(),
                MethodReturnName(M_FireBullet));
            for (uint32_t i = 0; i < np; ++i) {
                const Il2CppType* t = il2cpp_method_get_param ? il2cpp_method_get_param(M_FireBullet, i) : nullptr;
                Log("[sdk]   param%d = %s", i, TypeNameCached(t));
            }
        }
        Log("[sdk] methods: shoot=%p fire=%p", M_shoot, M_FireBullet);
    }

    static bool ResolveAll() {
        kEntity = FindClass("Entity");
        kPlayerScript = FindClass("PlayerScript");
        kPlayer = FindClass("Player");
        kNPC = FindClass("NPC");
        kTransform = FindClass("Transform", "UnityEngine");
        kCamera = FindClass("Camera", "UnityEngine");
        kObject = FindClass("Object", "UnityEngine");
        kRaycastHit = FindClass("RaycastHit", "UnityEngine");
        kGunStats = FindClass("gunStats");
        if (!kGunStats) kGunStats = FindClass("GunStats");
        kVector3 = FindClass("Vector3", "UnityEngine");
        kQuaternion = FindClass("Quaternion", "UnityEngine");

        kAntiHack = FindAntiHackClass();
        if (!kAntiHack) {
            ListClassesContaining("AntiHack");
            ListClassesContaining("Anti");
            ListClassesContaining("Hack");
        }

        ResolveOffsets();
        ResolveMethods();
        BuildStringOffsets();
        ResolveRaycast();
        return kPlayerScript != nullptr || kEntity != nullptr;
    }

    bool Init() {
        g_phase = "sdk.init";
        Log("[sdk] Init: ENTROU");

        if (!IL2CPP_API::Resolve()) { Log("[sdk] Init: Resolve FALHOU"); return false; }

        Il2CppDomain* dom = se_domain_get();
        if (!dom) { Log("[sdk] Init: domain NULO"); return false; }
        se_thread_attach(dom);

        if (!g_inited) {
            InitializeCriticalSection(&g_targetCs);
            g_inited = true;
        }
        else {
            Log("[sdk] Init: ja inicializado, recarregando metadata");
            ClearRuntimeCaches();
        }

        BuildCache();
        LosInvalidate();
        ResolveAll();

        if (kAntiHack) {
            Log("[ac] classe=%s instance=%p", se_class_name(kAntiHack), AntiHackInstance());
            DumpFields(kAntiHack, "AntiHack");
        }

        g_phase = "sdk.done";
        Log("[sdk] Init: CONCLUIDO");
        return true;
    }

    int GetTeamId(Il2CppObject* o) {
        if (!PtrOk_(o)) return -1;
        int t = -1;
        if (!ReadTeamEx(o, t)) return -1;
        return t;
    }

    int GetLocalTeam() {
        return g_localTeam;
    }
} // namespace SDK
// ==== FIM DO ARQUIVO ====