// ============================================================================
//  weapons.cpp — stats de arma + anti-recoil por neutralizacao de campos
//
//  RESPONSABILIDADES (separadas por secao, dentro do mesmo TU)
//    Util   -> texto
//    Cache  -> varredura de metadados + listas de campos (com validade)
//    Diag   -> dump/inspecao (gated por C.probeLog)
//    Apply  -> escrita de campos
//    Tick   -> coordenacao (so' decide QUANDO, nao COMO)
//
//  ---------------------------------------------------------------------------
//  POLITICA DE ERRO
//  ---------------------------------------------------------------------------
//  1) SEH (__try) e' usado SO' na varredura de metadados il2cpp, onde o
//     runtime pode devolver lixo. Os blocos tem APENAS locais POD — nenhum
//     objeto com destrutor — para nao depender de /EHa.
//  2) Excecao capturada => a varredura e' FALHA. O resultado e' publicado como
//     PARCIAL e o cache e' invalidado com retry agendado. Nunca se usa uma
//     lista parcial como se fosse completa e definitiva.
//  3) Escrita de campo (SafeWrite) e' BEST-EFFORT e nao retorna status: se o
//     offset nao foi resolvido, e' 0 e nada e' escrito. Quem precisa de
//     confirmacao usa TryWrite.
//  4) Diagnostico (dump) NUNCA invalida estado — se falhar, perde-se a saida
//     e mais nada.
//
//  ---------------------------------------------------------------------------
//  THREADS
//  ---------------------------------------------------------------------------
//  Este modulo NAO e' thread-safe por si. O contrato e':
//    - Tick() roda no thread que venceu o RunTickOnce (jogo OU render).
//    - O try-lock abaixo serializa ticks SOBREPOSTOS; se um tick ja' esta'
//      rodando, o segundo sai sem tocar em nada.
//    - As listas (std::vector) sao escritas e lidas SO' dentro de Tick().
//    - Init() roda no thread de init, antes de qualquer hook ficar vivo.
//
//  ---------------------------------------------------------------------------
//  >>> FIX FPS: resolucao do objeto de gunStats cacheada por 250 ms.
//      As ESCRITAS continuam por frame.
// ============================================================================
#ifndef BFI_WEAPONS_CPP
#define BFI_WEAPONS_CPP

#include "features.h"
#include "sdk.h"
#include "hooks.h"
#include "common.h"
#include "il2cpp_api.h"

#include <windows.h>
#include <cstring>
#include <vector>
#include <atomic>

// Marca de presenca do modulo (para o dump de modulos carregados).
extern "C" __declspec(selectany) int bfi_weapons_obj = 0xBF;

namespace Weapons {

    // =======================================================================
    //  NIVEIS DE LOG
    //    W_ERR  -> sempre. Falha que INVALIDA o resultado.
    //    W_WARN -> sempre. Degradacao, resultado ainda utilizavel.
    //    W_DIAG -> so' com C.probeLog. Inspecao/dump.
    //  O 'if' fica FORA da chamada: com o nivel desligado, os argumentos nem
    //  sao avaliados.
    // =======================================================================
#define W_ERR(...)   Log(__VA_ARGS__)
#define W_WARN(...)  Log(__VA_ARGS__)
#define W_DIAG(...)  do { if (C.probeLog) Log(__VA_ARGS__); } while (0)

    // =======================================================================
    //  ESTADO DO MODULO
    //  Tudo aqui e' privado do TU e tocado SO' de dentro de Tick()/Init(),
    //  salvo indicacao contraria.
    // =======================================================================

    // Offsets resolvidos de municao/spread/cadencia no objeto de stats.
    // 0 = nao resolvido => a escrita correspondente e' pulada.
    static uint32_t o_cur = 0, o_max = 0, o_reserve = 0;
    static uint32_t o_reload = 0, o_spread = 0, o_rate = 0, o_dmg = 0;

    // Ultimo objeto de stats para o qual os offsets acima foram resolvidos.
    // Serve SO' para detectar troca de arma. Nao e' dono do objeto.
    static Il2CppObject* g_stats = nullptr;

    // Dumps one-shot: nao repetem enquanto o modulo estiver vivo.
    static bool g_dumped = false;
    static bool g_ammoDumped = false;

    // Teto de campos de recoil guardados por classe. Estourar = truncar.
    static const int kMaxRecFields = 96;

    // TTL da resolucao do objeto de stats (ms). Escritas seguem por frame.
    static const uint32_t kStatsTtlMs = 250;

    // Throttle do re-log de valores de recoil: no maximo kRecLogMax linhas,
    // com pelo menos kRecLogGapMs entre elas.
    static const int       kRecLogMax = 8;
    static const uint32_t  kRecLogGapMs = 2000;

    static ULONGLONG g_lastRecLog = 0;
    static int       g_recLogs = 0;

    // =======================================================================
    //  Offsets EMPIRICOS do dump de armas.
    //  Foram observados no binario atual — revalide se o jogo atualizar.
    // =======================================================================
    namespace Off {
        const uint32_t kArrayLen = 0x18;   // Il2CppArray::max_length
        const uint32_t kArrayData = 0x20;  // Il2CppArray::vector
        const uint32_t kGunRpm = 0x48;
        const uint32_t kGunMagSize = 0xFC;
        const uint32_t kGunAmmo = 0x90;
        const uint32_t kGunObj = 0x148;
    }

    enum { MODO_ZERO = 0, MODO_NEUTRO = 1 };

    struct RecField { uint32_t off; int kind; int modo; char name[48]; };

    // =======================================================================
    //  Util
    // =======================================================================
    static void Lower(char* dst, size_t n, const char* src) {
        size_t i = 0;
        if (!dst || !n) return;
        if (!src) { dst[0] = 0; return; }
        for (; src[i] && i + 1 < n; ++i) {
            char c = src[i];
            dst[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
        }
        dst[i] = 0;
    }

    static bool NameHasRecoil(const char* n) {
        char l[64];
        Lower(l, sizeof(l), n);
        if (!strstr(l, "recoil") && !strstr(l, "kick") &&
            !strstr(l, "punch") && !strstr(l, "climb")) return false;
        if (strstr(l, "speed") || strstr(l, "recovery") || strstr(l, "lerp") ||
            strstr(l, "smooth") || strstr(l, "time") || strstr(l, "rate")) return false;
        return true;
    }

    static int ModeForName(const char* n) {
        char l[64];
        Lower(l, sizeof(l), n);
        if (strstr(l, "stability") || strstr(l, "scale") || strstr(l, "factor") ||
            strstr(l, "mult") || strstr(l, "modifier") || strstr(l, "strength") ||
            strstr(l, "damp") || strstr(l, "resist")) return MODO_NEUTRO;
        return MODO_ZERO;
    }

    // Objeto plausivel: faixa valida, alinhado, e a cabeca (klass) legivel.
    // Heuristica de VIVO — nao prova que o objeto sobreviveu a um GC, mas
    // pega o caso comum de ponteiro pendurado.
    static bool ObjOk(const void* p) {
        if (!p) return false;
        const uintptr_t a = (uintptr_t)p;
        if (!InUserRange(a) || (a & 7)) return false;
        return SafeRead<const void*>(a, nullptr) != nullptr;
    }

    // =======================================================================
    //  Cache de campos de recoil
    // =======================================================================
    //  Um RecSet guarda a CLASSE de origem. Se a classe mudar (troca de arma,
    //  troca de personagem), o cache e' reconstruido — offsets sao por classe,
    //  nao por instancia.
    struct RecSet {
        std::vector<RecField> fields;
        Il2CppClass* klass = nullptr;   // classe para a qual 'fields' vale
        bool         built = false;     // varredura concluida com SUCESSO
        bool         partial = false;   // publicada, mas a varredura abortou
        ULONGLONG    retryAt = 0;       // quando tentar de novo (se partial)
    };

    static RecSet g_gunRec, g_viewRec, g_plRec;

    // Varredura. Preenche buffer POD dentro do __try e so' publica no vector
    // DEPOIS, fora do bloco protegido (push_back pode lancar bad_alloc, e
    // __except so' pega SEH com /EHsc).
    // Retorno: true = varredura completa; false = abortou (lista e' parcial).
    static bool CollectRec(Il2CppClass* k, std::vector<RecField>& out, const char* tag) {
        out.clear();
        if (!k) { W_ERR("[recoil] %s: classe nula", tag); return false; }

        static RecField tmp[kMaxRecFields];   // estatico: 96*60B nao cabe bonito na pilha
        int  n = 0;
        bool ok = true;

        __try {
            for (Il2CppClass* c = k; c && n < kMaxRecFields; c = il2cpp_class_get_parent(c)) {
                void* it = nullptr;
                for (;;) {
                    FieldInfo* f = il2cpp_class_get_fields(c, &it);
                    if (!f || n >= kMaxRecFields) break;

                    const char* fn = il2cpp_field_get_name(f);
                    if (!fn || !NameHasRecoil(fn)) continue;

                    const Il2CppType* t = il2cpp_field_get_type(f);
                    const char* tn = (t && il2cpp_type_get_name) ? il2cpp_type_get_name(t) : nullptr;

                    int kind = -1;
                    if (tn) {
                        if (strstr(tn, "Single")) kind = 0;
                        else if (strstr(tn, "Vector2")) kind = 1;
                        else if (strstr(tn, "Vector3")) kind = 2;
                    }
                    if (kind < 0) continue;

                    const uint32_t off = (uint32_t)il2cpp_field_get_offset(f);
                    if (!off) continue;

                    tmp[n].off = off;
                    tmp[n].kind = kind;
                    tmp[n].modo = ModeForName(fn);
                    strncpy_s(tmp[n].name, fn, _TRUNCATE);   // respeita 48 e termina
                    ++n;

                    W_DIAG("[recoil] %s: %s @0x%X (%s, modo=%s)", tag, fn, off,
                        kind == 0 ? "float" : (kind == 1 ? "vec2" : "vec3"),
                        tmp[n - 1].modo == MODO_NEUTRO ? "NEUTRO/1.0" : "ZERO/0.0");
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            ok = false;
            W_ERR("[recoil] %s: SEH na varredura apos %d campos", tag, n);
        }

        out.assign(tmp, tmp + n);   // fora do __try
        return ok;
    }

    static void LogRecValues(Il2CppObject* o, const std::vector<RecField>& v, const char* tag) {
        if (!o) return;
        for (size_t i = 0; i < v.size(); ++i) {
            const uintptr_t a = (uintptr_t)o + v[i].off;
            if (v[i].kind == 0) {
                W_DIAG("[recoil] %s valor %s = %.4f", tag, v[i].name, SafeRead<float>(a, 0.f));
            }
            else if (v[i].kind == 1) {
                W_DIAG("[recoil] %s valor %s = (%.3f, %.3f)", tag, v[i].name,
                    SafeRead<float>(a, 0.f), SafeRead<float>(a + 4, 0.f));
            }
            else {
                W_DIAG("[recoil] %s valor %s = (%.3f, %.3f, %.3f)", tag, v[i].name,
                    SafeRead<float>(a, 0.f), SafeRead<float>(a + 4, 0.f),
                    SafeRead<float>(a + 8, 0.f));
            }
        }
    }

    // Garante que 'rs' esteja valido para o objeto 'o'. Reconstroi se:
    //   - nunca foi construido, ou
    //   - a classe de 'o' mudou, ou
    //   - a ultima tentativa falhou e o retry venceu.
    static void EnsureRec(RecSet& rs, Il2CppObject* o, const char* tag) {
        if (!o) return;

        Il2CppClass* k = il2cpp_object_get_class((Il2CppObject*)o);
        if (!k) return;

        const ULONGLONG now = GetTickCount64();

        if (rs.built && rs.klass == k) return;
        if (rs.partial && rs.klass == k && now < rs.retryAt) return;

        rs.klass = k;
        const bool ok = CollectRec(k, rs.fields, tag);

        rs.built = ok;
        rs.partial = !ok;
        rs.retryAt = ok ? 0 : (now + 500);

        if (ok) {
            W_DIAG("[recoil] %s: %d campos casados", tag, (int)rs.fields.size());
            LogRecValues(o, rs.fields, tag);
        }
        else {
            W_ERR("[recoil] %s: %d campos PARCIAIS publicados, retry em 500 ms",
                tag, (int)rs.fields.size());
        }
    }

    // =======================================================================
    //  Diag — nunca invalida estado
    // =======================================================================
    static void DumpAmmoFields(Il2CppClass* k) {
        if (!k) return;
        W_DIAG("[ammo] ---- campos de municao em %s ----",
            (il2cpp_class_get_name ? il2cpp_class_get_name(k) : "?"));

        __try {
            for (Il2CppClass* c = k; c; c = il2cpp_class_get_parent(c)) {
                void* it = nullptr;
                for (;;) {
                    FieldInfo* f = il2cpp_class_get_fields(c, &it);
                    if (!f) break;

                    const char* n = il2cpp_field_get_name(f);
                    if (!n) continue;

                    char l[64];
                    Lower(l, sizeof(l), n);
                    if (!strstr(l, "ammo") && !strstr(l, "mag") &&
                        !strstr(l, "clip") && !strstr(l, "bullet") &&
                        !strstr(l, "round")) continue;

                    const Il2CppType* t = il2cpp_field_get_type(f);
                    const char* tn = (t && il2cpp_type_get_name) ? il2cpp_type_get_name(t) : "?";

                    W_DIAG("[ammo]   %-30s : %-16s @0x%X", n, tn ? tn : "?",
                        (unsigned)il2cpp_field_get_offset(f));
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { W_ERR("[ammo] SEH na varredura (saida parcial)"); }

        W_DIAG("[ammo] ---- fim ----");
    }

    static void DumpGunArray(const char* tag, void* arr) {
        if (!arr) return;
        __try {
            int len = 0;
            if (il2cpp_array_length) len = (int)il2cpp_array_length((const Il2CppArray*)arr);
            if (len <= 0) len = *(int*)((uintptr_t)arr + Off::kArrayLen);

            W_DIAG("[guns] %s: len=%d", tag, len);
            // len desconhecido (<=0) => assume o teto. Trunca acima dele.
            if (len <= 0 || len > 64) len = 64;

            for (int i = 0; i < len; ++i) {
                void* e = *(void**)((uintptr_t)arr + Off::kArrayData + (size_t)i * 8);
                if (!e) { W_DIAG("[guns]   [%d] null", i); continue; }

                Il2CppClass* k = il2cpp_object_get_class((Il2CppObject*)e);
                const char* kn = (k && il2cpp_class_get_name) ? il2cpp_class_get_name(k) : "?";

                W_DIAG("[guns]   [%d] %s @%p | RPM=%.1f magSize=%d ammo=%d gunObj=%p",
                    i, kn, e,
                    SafeRead<float>((uintptr_t)e + Off::kGunRpm, -1.f),
                    SafeRead<int>((uintptr_t)e + Off::kGunMagSize, -1),
                    SafeRead<int>((uintptr_t)e + Off::kGunAmmo, -1),
                    SDK::SafeReadObjPub(e, Off::kGunObj));
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { W_ERR("[guns] %s: SEH (saida parcial)", tag); }
    }

    static void DumpGunCandidates() {
        Il2CppObject* lp = SDK::g_localPlayer;
        if (!lp || !ObjOk(lp)) return;

        Il2CppClass* k = il2cpp_object_get_class(lp);
        if (!k) return;

        W_DIAG("[guns] O_gunStats=0x%X  cls=%s", SDK::O_gunStats, il2cpp_class_get_name(k));
        W_DIAG("[guns] ---- campos tipo Gun/Weapon em %s ----", il2cpp_class_get_name(k));

        __try {
            for (Il2CppClass* c = k; c; c = il2cpp_class_get_parent(c)) {
                void* it = nullptr;
                for (;;) {
                    FieldInfo* f = il2cpp_class_get_fields(c, &it);
                    if (!f) break;

                    const char* fn = il2cpp_field_get_name(f);
                    const Il2CppType* t = il2cpp_field_get_type(f);
                    const char* tn = (t && il2cpp_type_get_name) ? il2cpp_type_get_name(t) : nullptr;
                    if (!tn) continue;
                    if (!strstr(tn, "Gun") && !strstr(tn, "gun") &&
                        !strstr(tn, "Weapon") && !strstr(tn, "weapon")) continue;

                    const uint32_t off = (uint32_t)il2cpp_field_get_offset(f);
                    if (!off) continue;

                    void* p = SDK::SafeReadObjPub(lp, off);
                    if (!p) { W_DIAG("[guns]   %-26s : %-30s @0x%X = null", fn, tn, off); continue; }

                    Il2CppClass* ok = il2cpp_object_get_class((Il2CppObject*)p);
                    const char* ocn = ok ? il2cpp_class_get_name(ok) : "?";

                    if (ocn && (strstr(ocn, "gunStats") || strstr(ocn, "GunStats"))) {
                        W_DIAG("[guns]   %-26s : %s @0x%X -> %s @%p | RPM=%.1f magSize=%d ammo=%d gunObj=%p",
                            fn, tn, off, ocn, p,
                            SafeRead<float>((uintptr_t)p + Off::kGunRpm, -1.f),
                            SafeRead<int>((uintptr_t)p + Off::kGunMagSize, -1),
                            SafeRead<int>((uintptr_t)p + Off::kGunAmmo, -1),
                            SDK::SafeReadObjPub(p, Off::kGunObj));
                        if (strstr(tn, "[]")) DumpGunArray(fn, p);
                    }
                    else {
                        W_DIAG("[guns]   %-26s : %s @0x%X -> %s @%p", fn, tn, off, ocn, p);
                    }
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { W_ERR("[guns] SEH na varredura (saida parcial)"); }

        if (SDK::O_gunView) {
            Il2CppObject* view = SDK::SafeReadObjPub(lp, SDK::O_gunView);
            if (view) {
                W_DIAG("[guns] ---- fields da fpsGunView ----");
                SDK::DumpFieldsWithValues(view);
            }
        }

        W_DIAG("[guns] ---- fim ----");
    }

    // =======================================================================
    //  Apply — best-effort, sem status
    // =======================================================================
    static void NeutralizeAll(Il2CppObject* o, const std::vector<RecField>& v) {
        if (!o) return;
        for (size_t i = 0; i < v.size(); ++i) {
            const uintptr_t a = (uintptr_t)o + v[i].off;
            const float val = (v[i].modo == MODO_NEUTRO) ? 1.f : 0.f;

            if (v[i].kind == 0) {
                SafeWrite(a, (const void*)&val, sizeof(float));
            }
            else if (v[i].kind == 1) {
                float z[2] = { val, val };
                SafeWrite(a, (const void*)z, sizeof(z));
            }
            else {
                float z[3] = { val, val, val };
                SafeWrite(a, (const void*)z, sizeof(z));
            }
        }
    }

    // =======================================================================
    //  Init
    // =======================================================================
    void Init() {
        Log("[weapons] Init: statsTTL=%ums camposMax=%d retry=500ms logMax=%d/%ums",
            (unsigned)kStatsTtlMs, kMaxRecFields, kRecLogMax, (unsigned)kRecLogGapMs);
    }

    // =======================================================================
    //  Tick — so' coordena. Nao sabe COMO neutralizar, so' QUANDO.
    // =======================================================================
    void Tick() {
        if (!(C.infAmmo || C.noReload || C.noSpread || C.rapidFire || C.noRecoil)) return;

        // Serializa ticks SOBREPOSTOS. O CAS do RunTickOnce garante 1 por frame,
        // mas nao impede que o frame N+1 comece com o N ainda em execucao.
        // Sem isso, os std::vector abaixo seriam tocados em paralelo.
        static std::atomic_flag busy = ATOMIC_FLAG_INIT;
        if (busy.test_and_set(std::memory_order_acquire)) return;
        struct Unflag { ~Unflag() { busy.clear(std::memory_order_release); } } unflag;

        if (!SDK::g_localPlayer || !ObjOk(SDK::g_localPlayer)) return;

        const ULONGLONG now = GetTickCount64();

        // --- resolucao do objeto de stats, com validacao do cache ---
        static Il2CppObject* s_stats = nullptr;
        static ULONGLONG     s_statsNext = 0;

        if (s_stats && !ObjOk(s_stats)) {   // objeto morreu: cai o cache
            s_stats = nullptr;
            s_statsNext = 0;
        }

        if (now >= s_statsNext) {
            s_statsNext = now + kStatsTtlMs;
            s_stats = SDK::FindGunStatsObj();
        }

        Il2CppObject* stats = s_stats;
        if (stats) {
            if (stats != g_stats) {
                g_stats = stats;

                SDK::ResolveWeaponFields(stats,
                    o_cur, o_max, o_reserve, o_reload, o_spread, o_rate, o_dmg);

                if (!g_dumped) { g_dumped = true; SDK::DumpFieldsWithValues(stats); }

                if (!g_ammoDumped) {
                    g_ammoDumped = true;
                    DumpAmmoFields(il2cpp_object_get_class(stats));
                    DumpGunCandidates();
                }

                EnsureRec(g_gunRec, stats, "gun");

                g_recLogs = 0;
                g_lastRecLog = 0;
            }

            if (C.infAmmo) {
                if (o_cur) {
                    const int mx = o_max ? SafeRead<int>((uintptr_t)stats + o_max, 0) : 0;
                    SafeWrite((uintptr_t)stats + o_cur, (mx > 0 && mx < 10000) ? mx : 999);
                }
                if (o_reserve) SafeWrite((uintptr_t)stats + o_reserve, 999);
            }

            if (C.noReload && o_reload) SafeWrite<bool>((uintptr_t)stats + o_reload, false);
            if (C.noSpread && o_spread) SafeWrite((uintptr_t)stats + o_spread, 0.f);

            if (C.rapidFire && o_rate) {
                if (SDK::g_rateIsRpm) {
                    SafeWrite((uintptr_t)stats + o_rate, 3000.f);
                }
                else {
                    const float cur = SafeRead<float>((uintptr_t)stats + o_rate, 0.f);
                    SafeWrite((uintptr_t)stats + o_rate, (cur >= 1.f) ? 100.f : 0.01f);
                }
            }

            if (C.noRecoil) {
                EnsureRec(g_gunRec, stats, "gun");      // revalida se a classe mudou
                NeutralizeAll(stats, g_gunRec.fields);

                if (g_recLogs < kRecLogMax && now - g_lastRecLog > kRecLogGapMs) {
                    g_lastRecLog = now;
                    ++g_recLogs;
                    LogRecValues(stats, g_gunRec.fields, "gun*");
                }
            }
        }

        if (C.noRecoil) {
            EnsureRec(g_plRec, SDK::g_localPlayer, "player");
            NeutralizeAll(SDK::g_localPlayer, g_plRec.fields);

            if (SDK::O_gunView) {
                Il2CppObject* view = SDK::SafeReadObjPub(SDK::g_localPlayer, SDK::O_gunView);
                if (view) {
                    EnsureRec(g_viewRec, view, "gunview");
                    NeutralizeAll(view, g_viewRec.fields);
                }
            }
        }
    }

#undef W_ERR
#undef W_WARN
#undef W_DIAG

} // namespace Weapons

#endif // BFI_WEAPONS_CPP