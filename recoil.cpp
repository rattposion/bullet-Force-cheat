// recoil.cpp - bfi (v2)
//  - descobre a classe REAL da arma (nao confia em kGunStats)
//  - loga os campos candidatos (kick/recoil/climb/spread/accuracy/bloom)
//  - separa recuo e dispersao; zero so o que o config pede
#include "common.h"
#include "sdk.h"
#include "config.h"
#include "features.h"
#include <vector>
#include <cstring>

namespace Recoil {

    struct RF { uint32_t off; bool vec2; };

    static std::vector<RF> g_recoil;
    static std::vector<RF> g_spread;
    static Il2CppClass* g_cls = nullptr;
    static bool            g_dumped = false;
    static ULONGLONG       g_nextScan = 0;

    static const char* kRecoil[] = {
        "gunUpKick","gunBackKick","maxGunBackKick","muzzleClimb","muzzleClimbSideways",
        "muzzleClimbDamped","firstShotRecoilMultiplier","cameraBackKick",
        "horizontalRecoil","visualHorizontalRecoil","adsRecoilMult","recoilStability"
    };

    // nomes plausiveis de dispersao (o dump confirma os reais)
    static const char* kSpread[] = {
        "spread","minSpread","maxSpread","spreadPerShot","spreadDecrease","spreadIncrease",
        "hipSpread","adsSpread","bulletSpread","accuracy","accuracySpread","bloom","shotSpread"
    };

    static bool TypeOk(const char* tn, bool& vec2) {
        if (!tn) return false;
        if (strstr(tn, "Vector2")) { vec2 = true;  return true; }
        if (strstr(tn, "Single")) { vec2 = false; return true; }
        return false;
    }

    static void MatchInto(Il2CppClass* k, const char** list, size_t n, std::vector<RF>& out) {
        out.clear();
        if (!k) return;
        void* it = nullptr; FieldInfo* f;
        while ((f = il2cpp_class_get_fields(k, &it))) {
            const char* fn = il2cpp_field_get_name(f);
            if (!fn) continue;
            Il2CppType* t = il2cpp_field_get_type(f);
            const char* tn = t ? il2cpp_type_get_name(t) : nullptr;
            bool v2 = false;
            if (!TypeOk(tn, v2)) continue;
            for (size_t i = 0; i < n; ++i)
                if (!strcmp(fn, list[i])) { out.push_back({ (uint32_t)il2cpp_field_get_offset(f), v2 }); break; }
        }
    }

    static void DumpCandidates(Il2CppClass* k, const char* tag) {
        if (!k) { Log("[recoil] %s: NULO", tag); return; }
        Log("[recoil] %s classe=%s", tag, il2cpp_class_get_name(k));
        void* it = nullptr; FieldInfo* f; int n = 0;
        while ((f = il2cpp_class_get_fields(k, &it))) {
            const char* fn = il2cpp_field_get_name(f);
            Il2CppType* t = il2cpp_field_get_type(f);
            const char* tn = t ? il2cpp_type_get_name(t) : "?";
            if (fn && (strstr(fn, "kick") || strstr(fn, "Kick") || strstr(fn, "recoil") ||
                strstr(fn, "Recoil") || strstr(fn, "climb") || strstr(fn, "Climb") ||
                strstr(fn, "spread") || strstr(fn, "Spread") || strstr(fn, "accuracy") ||
                strstr(fn, "Accuracy") || strstr(fn, "bloom") || strstr(fn, "Bloom")))
                Log("[recoil]   %s :: %s : %s (off=0x%X)", tag, fn, tn,
                    (unsigned)il2cpp_field_get_offset(f));
            if (++n > 400) break;
        }
    }

    static void Rescan(void* weaponObj) {
        g_cls = nullptr;
        if (weaponObj && il2cpp_object_get_class)
            g_cls = il2cpp_object_get_class((const Il2CppObject*)weaponObj);
        if (!g_cls) g_cls = SDK::kGunStats;          // fallback

        if (!g_dumped && g_cls) { g_dumped = true; DumpCandidates(g_cls, "alvo"); }

        MatchInto(g_cls, kRecoil, sizeof(kRecoil) / sizeof(kRecoil[0]), g_recoil);
        MatchInto(g_cls, kSpread, sizeof(kSpread) / sizeof(kSpread[0]), g_spread);
        Log("[recoil] alvo=%s -> recoil=%d spread=%d",
            g_cls ? il2cpp_class_get_name(g_cls) : "?",
            (int)g_recoil.size(), (int)g_spread.size());
    }

    static void ZeroList(void* obj, const std::vector<RF>& list) {
        if (!obj || list.empty()) return;
        for (auto& r : list) {
            *(float*)((uint8_t*)obj + r.off) = 0.f;
            if (r.vec2) *(float*)((uint8_t*)obj + r.off + 4) = 0.f;
        }
    }

    void Install() { Rescan(nullptr); }

    void Tick()
    {
        if (!C.noRecoil && !C.noSpread) return;
        void* p = SDK::g_localPlayer;
        if (!p) return;

        // objetos das armas (offset cru; logado 1x pra validar)
        Il2CppArray* arr = *(Il2CppArray**)((uint8_t*)p + 0xBA8);
        void* cur = *(void**)((uint8_t*)p + 0x570);

        static bool loggedOnce = false;
        if (!loggedOnce) {
            loggedOnce = true;
            Il2CppClass* ca = (arr && il2cpp_object_get_class) ?
                il2cpp_object_get_class((const Il2CppObject*)arr) : nullptr;
            Il2CppClass* cc = (cur && il2cpp_object_get_class) ?
                il2cpp_object_get_class((const Il2CppObject*)cur) : nullptr;
            Log("[recoil] arr=%p(%s) cur=%p(%s) kGunStats=%p",
                (void*)arr, ca ? il2cpp_class_get_name(ca) : "?",
                cur, cc ? il2cpp_class_get_name(cc) : "?", (void*)SDK::kGunStats);
        }

        // re-scan periodico enquanto nao achou nada
        const ULONGLONG now = GetTickCount64();
        if ((g_recoil.empty() && g_spread.empty()) && now >= g_nextScan) {
            g_nextScan = now + 2000;
            Rescan(cur);
        }

        const std::vector<RF>& rl = C.noRecoil ? g_recoil : g_spread;
        if (rl.empty()) return;

        if (C.noRecoil) {
            if (cur) ZeroList(cur, g_recoil);
            if (arr) {
                const int len = *(int*)((uint8_t*)arr + 0x18);
                for (int i = 0; i < len; ++i)
                    ZeroList(*(void**)((uint8_t*)arr + 0x20 + (size_t)i * sizeof(void*)), g_recoil);
            }
        }
        if (C.noSpread) {
            if (cur) ZeroList(cur, g_spread);
            if (arr) {
                const int len = *(int*)((uint8_t*)arr + 0x18);
                for (int i = 0; i < len; ++i)
                    ZeroList(*(void**)((uint8_t*)arr + 0x20 + (size_t)i * sizeof(void*)), g_spread);
            }
        }
    }

} // namespace Recoil