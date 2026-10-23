// ============================================================================
//  hooks.cpp  (v36)
//
//  >>> PATCH R7  (crash fatal: AV fase=hk_swap derrubou a thread de render)
//   1) LoadGlExt() agora e' chamado no Init — antes so' rodava se fbo!=0 e o
//      fbo e' SEMPRE 0, entao p_glBindFramebuffer/p_glActiveTexture ficavam
//      nullptr a sessao toda.
//   2) CORPO INTEIRO do swap dentro de UM __try + __except com unwind da
//      critical section (CsUnwind). Nenhum trecho desprotegido.
//   3) __try separado em volta do o_swap(hdc) — se o present do jogo estourar,
//      a gente sobrevive e o jogo continua.
//   4) FASES FINAS: swap:pre / swap:ctx / swap:vp / swap:frame / swap:draw /
//      swap:present. O proximo AV diz exatamente onde.
//
//  >>> PATCH R6  (glDepthMask/colorMask matando o mapa) — SNAPSHOT+FORCE+RESTORE
//  >>> PATCH R5  (stencil — overlay invisivel vivo)
//  >>> PATCH A13 / A12 / A11 / R1-R4 / B2 / A1 / A2
// ============================================================================
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <atomic>
#include <cmath>
#include <vector>
#include <GL/gl.h>

#include "MinHook.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_opengl3.h"

#include "il2cpp.h"
#include "il2cpp_api.h"
#include "common.h"
#include "memory.h"
#include "config.h"
#include "sdk.h"
#include "hooks.h"
#include "features.h"
#include "aimbot.h"

#pragma comment(lib, "opengl32.lib")

#define WATCH_MAX_LINES   12
#define PROBE_DIFF_MAX    8
#define TICK_WATCHDOG_MS  400
#define WND_REAPPLY_MS    500
#define MOUSE_FALLBACK_MS 120
#define SWAP_MUTE_MS      1500

#define INPUT_TEST_LEVEL  7

#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER 0x8D40
#endif
#ifndef GL_FRAMEBUFFER_BINDING
#define GL_FRAMEBUFFER_BINDING 0x8CA6
#endif
#ifndef GL_READ_FRAMEBUFFER_BINDING
#define GL_READ_FRAMEBUFFER_BINDING 0x8CAA
#endif
#ifndef GL_FRAMEBUFFER_SRGB
#define GL_FRAMEBUFFER_SRGB 0x8DB9
#endif
#ifndef GL_BACK
#define GL_BACK 0x0405
#endif
#ifndef GL_POLYGON_OFFSET_FILL
#define GL_POLYGON_OFFSET_FILL 0x8037
#endif
#ifndef GL_STENCIL_WRITEMASK
#define GL_STENCIL_WRITEMASK 0x0B98
#endif
#ifndef GL_ACTIVE_TEXTURE
#define GL_ACTIVE_TEXTURE 0x84E0
#endif
#ifndef GL_TEXTURE0
#define GL_TEXTURE0 0x84C0
#endif

extern const char* g_phase;
extern std::atomic<bool> g_unload;
#ifndef PHASE
#define PHASE(x) (g_phase = (x))
#endif

namespace Hooks {

    typedef void (APIENTRY* PFN_glBindFramebuffer)(GLenum, GLuint);
    typedef void (APIENTRY* PFN_glActiveTexture)(GLenum);
    static PFN_glBindFramebuffer p_glBindFramebuffer = nullptr;
    static PFN_glActiveTexture   p_glActiveTexture = nullptr;

    static bool LoadGlExt() {
        if (!p_glBindFramebuffer)
            p_glBindFramebuffer = (PFN_glBindFramebuffer)wglGetProcAddress("glBindFramebuffer");
        if (!p_glBindFramebuffer)
            p_glBindFramebuffer = (PFN_glBindFramebuffer)wglGetProcAddress("glBindFramebufferEXT");
        if (!p_glActiveTexture)
            p_glActiveTexture = (PFN_glActiveTexture)wglGetProcAddress("glActiveTexture");
        return p_glBindFramebuffer != nullptr && p_glActiveTexture != nullptr;
    }

    std::atomic<bool>     g_hooksLive{ false };
    std::atomic<bool>     g_imguiReady{ false };
    std::atomic<bool>     g_sdkReady{ false };
    std::atomic<uint64_t> g_frameCounter{ 0 };
    std::atomic<uint32_t> g_imguiGeneration{ 0 };
    std::atomic<int>      g_viewW{ 1920 };
    std::atomic<int>      g_viewH{ 1080 };

    static std::atomic<int> g_stage{ (int)Stage::Down };

    static std::atomic<uint32_t> g_frameDrops{ 0 };
    static std::atomic<uint32_t> g_wndProcCalls{ 0 };
    static std::atomic<uint32_t> g_cwpCalls{ 0 };
    static std::atomic<uint32_t> g_avCount{ 0 };
    static std::atomic<uint32_t> g_vehCount{ 0 };

    static std::atomic<uint32_t> g_abortPre{ 0 };
    static std::atomic<uint32_t> g_abortMenu{ 0 };
    static std::atomic<uint32_t> g_abortEsp{ 0 };
    static std::atomic<uint32_t> g_abortMisc{ 0 };
    static std::atomic<uint32_t> g_abortRender{ 0 };
    static std::atomic<uint32_t> g_abortDraw{ 0 };
    static std::atomic<uint32_t> g_abortBody{ 0 };      // [R7]
    static std::atomic<uint32_t> g_abortPresent{ 0 };   // [R7]

    static std::atomic<ULONGLONG> g_lastWndMsg{ 0 };
    static std::atomic<ULONGLONG> g_lastMouseMsg{ 0 };

    static std::atomic<int> s_dirSlot{ -1 };

    static std::atomic<uint32_t>  g_swapCalls{ 0 };
    static std::atomic<ULONGLONG> g_lastSwapMs{ 0 };
    static std::atomic<uint32_t>  g_noDrawStreak{ 0 };
    static std::atomic<const char*> g_noDrawReason{ "inicio" };
    static std::atomic<uint32_t>  g_vtxTotal{ 0 };
    static std::atomic<uint32_t>  g_zeroVtxStreak{ 0 };
    static std::atomic<uint32_t>  g_ctxChanges{ 0 };
    static std::atomic<uint32_t>  g_espBranch{ 0 };
    static std::atomic<ULONGLONG> g_lastDrawMs{ 0 };
    static std::atomic<int>       g_lastFbo{ -1 };
    static std::atomic<int>       g_fboLogged{ -2 };
    static std::atomic<uint32_t>  g_fboFrames{ 0 };
    static std::atomic<uint32_t>  g_stencilHostile{ 0 };
    static std::atomic<uint32_t>  g_maskHostile{ 0 };

    static HGLRC g_glrc = nullptr;
    static HANDLE g_watchThread = nullptr;
    static std::atomic<bool> g_watchRun{ false };

    static LRESULT CALLBACK hk_wndProc(HWND, UINT, WPARAM, LPARAM);

    uint32_t FrameId() { return (uint32_t)g_frameCounter.load(std::memory_order_relaxed); }

    namespace Lvl {
        constexpr int Menu = 0;
        constexpr int Esp = 1;
        constexpr int Tick = 2;
        constexpr int Scp = 3;
        constexpr int CursorFn = 4;
        constexpr int Subclass = 5;
        constexpr int BlockKeys = 6;
        constexpr int Watchdog = 7;
    }
    inline bool AtLvl(int l) { return INPUT_TEST_LEVEL >= l; }

    Stage StageNow() { return (Stage)g_stage.load(std::memory_order_acquire); }
    bool  IsLive() { return StageNow() == Stage::Live; }

    bool AtLeast(Stage s) {
        const Stage cur = StageNow();
        if (cur == Stage::Tearing) return false;
        return (int)cur >= (int)s;
    }

    const char* StageName(Stage s) {
        switch (s) {
        case Stage::Down:    return "Down";
        case Stage::Init:    return "Init";
        case Stage::Sdk:     return "Sdk";
        case Stage::Hooks:   return "Hooks";
        case Stage::ImGui:   return "ImGui";
        case Stage::Live:    return "Live";
        case Stage::Tearing: return "Tearing";
        }
        return "?";
    }

    void SetStage(Stage s) {
        const int v = (int)s;
        g_stage.store(v, std::memory_order_release);
        g_hooksLive.store(v >= (int)Stage::Hooks, std::memory_order_release);
        g_imguiReady.store(v >= (int)Stage::ImGui, std::memory_order_release);
    }

    void Advance(Stage s) {
        const int cur = (int)g_stage.load(std::memory_order_acquire);
        if (cur == (int)Stage::Tearing) return;
        if ((int)s > cur) SetStage(s);
    }

    void SetViewSize(int w, int h) {
        if (w > 0) g_viewW.store(w, std::memory_order_relaxed);
        if (h > 0) g_viewH.store(h, std::memory_order_relaxed);
    }
    ViewSize GetViewSize() {
        return ViewSize{ g_viewW.load(std::memory_order_relaxed),
                         g_viewH.load(std::memory_order_relaxed) };
    }
    int ViewW() { return g_viewW.load(std::memory_order_relaxed); }
    int ViewH() { return g_viewH.load(std::memory_order_relaxed); }

    static HWND   g_hwnd = nullptr;
    static bool   g_imguiInit = false;
    static bool   g_win32Backend = false;
    static CRITICAL_SECTION g_imCs;
    static bool   g_imCsInit = false;
    static HHOOK  g_cwp = nullptr;
    static PVOID  g_veh = nullptr;

    // >>> [R7] critical section com profundidade — permite unwind em AV
    static thread_local int t_csDepth = 0;
    static inline void CsEnter() { if (g_imCsInit) { EnterCriticalSection(&g_imCs); ++t_csDepth; } }
    static inline void CsLeave() { if (g_imCsInit && t_csDepth > 0) { --t_csDepth; LeaveCriticalSection(&g_imCs); } }
    static inline void CsUnwind() { while (t_csDepth > 0) CsLeave(); }

    typedef BOOL(WINAPI* tSwapBuffers)(HDC);
    typedef BOOL(WINAPI* tSetCursorPos)(int, int);
    typedef void(__fastcall* tLateUpdate)(void*, void*);
    typedef bool(__fastcall* tFireBullet)(void*, int, float, Vec3, Vec3, bool, double, int, void*);

    static tSwapBuffers  o_swap = nullptr;
    static tSetCursorPos o_setCursorPos = nullptr;
    static tLateUpdate   o_lateUpdate = nullptr;
    static tFireBullet   o_fire7 = nullptr;
    static WNDPROC       o_wndProc = nullptr;

    static std::atomic<uint64_t>  g_lastTickFrame{ ~0ull };
    static std::atomic<ULONGLONG> g_lastLuMs{ 0 };

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
    static bool PlausibleObj(const void* p) {
        if (!Plausible(p)) return false;
        return Plausible(*(const void**)p);
    }

    static inline float Len(const Vec3& v) {
        return sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
    }
    static inline float Dist(const Vec3& a, const Vec3& b) {
        const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
        return sqrtf(dx * dx + dy * dy + dz * dz);
    }

    static bool Writable(const void* p) {
        uintptr_t a = (uintptr_t)p;
        if (a < 0x10000 || a > 0x00007FFFFFFFFFFFULL) return false;
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(p, &mbi, sizeof(mbi))) return false;
        if (mbi.State != MEM_COMMIT) return false;
        const DWORD ruim = PAGE_NOACCESS | PAGE_GUARD | PAGE_READONLY |
            PAGE_EXECUTE | PAGE_EXECUTE_READ;
        return (mbi.Protect & ruim) == 0;
    }

    static uint32_t DrainGlErrors(const char* tag) {
        uint32_t n = 0;
        GLenum e;
        while ((e = glGetError()) != GL_NO_ERROR && n < 8) {
            Log("[gl] %s erro=0x%X", tag, (unsigned)e);
            ++n;
        }
        if (n) SDK::g_glErrTotal.fetch_add(n, std::memory_order_relaxed);
        return n;
    }

    static void NoteNoDraw(const char* why) {
        const char* prev = g_noDrawReason.exchange(why);
        if (prev != why) Log("[render] SEM DESENHO: %s", why);
        g_noDrawStreak.fetch_add(1, std::memory_order_relaxed);
    }
    static void NoteDraw(uint32_t vtx) {
        g_noDrawStreak.store(0, std::memory_order_relaxed);
        g_noDrawReason.store("ok", std::memory_order_relaxed);
        g_vtxTotal.store(vtx, std::memory_order_relaxed);
        if (vtx == 0) g_zeroVtxStreak.fetch_add(1, std::memory_order_relaxed);
        else          g_zeroVtxStreak.store(0, std::memory_order_relaxed);
        g_lastDrawMs.store(GetTickCount64(), std::memory_order_relaxed);
    }

    static void ReinitGlBackend(const char* why) {
        if (!g_imguiInit) return;
        ImGui_ImplOpenGL3_Shutdown();
        if (!ImGui_ImplOpenGL3_Init()) Log("[render] reinit GL3 FALHOU (%s)", why);
        else                           Log("[render] backend GL3 recriado (%s)", why);
    }

    struct GlSnap {
        GLboolean depthMask = 1, colorMask[4] = { 1,1,1,1 };
        GLint     stencilMask = 0xFF;
        GLboolean depthTest = 1, stencilTest = 0, cullFace = 0, blend = 1, scissorTest = 0;
        GLint     blendSrc = 1, blendDst = 0;
        GLint     scissorBox[4] = { 0,0,0,0 };
        GLint     activeTex = GL_TEXTURE0;
    };

    static void GlSnapNow(GlSnap& s) {
        glGetBooleanv(GL_DEPTH_WRITEMASK, &s.depthMask);
        glGetBooleanv(GL_COLOR_WRITEMASK, s.colorMask);
        glGetIntegerv(GL_STENCIL_WRITEMASK, &s.stencilMask);
        s.depthTest = (GLboolean)glIsEnabled(GL_DEPTH_TEST);
        s.stencilTest = (GLboolean)glIsEnabled(GL_STENCIL_TEST);
        s.cullFace = (GLboolean)glIsEnabled(GL_CULL_FACE);
        s.blend = (GLboolean)glIsEnabled(GL_BLEND);
        s.scissorTest = (GLboolean)glIsEnabled(GL_SCISSOR_TEST);
        glGetIntegerv(GL_BLEND_SRC, &s.blendSrc);
        glGetIntegerv(GL_BLEND_DST, &s.blendDst);
        glGetIntegerv(GL_SCISSOR_BOX, s.scissorBox);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &s.activeTex);
    }

    static void GlRestoreNow(const GlSnap& s) {
        glDepthMask(s.depthMask);
        glColorMask(s.colorMask[0], s.colorMask[1], s.colorMask[2], s.colorMask[3]);
        glStencilMask((GLuint)s.stencilMask);
        if (s.depthTest)   glEnable(GL_DEPTH_TEST);   else glDisable(GL_DEPTH_TEST);
        if (s.stencilTest) glEnable(GL_STENCIL_TEST); else glDisable(GL_STENCIL_TEST);
        if (s.cullFace)    glEnable(GL_CULL_FACE);    else glDisable(GL_CULL_FACE);
        if (s.blend)       glEnable(GL_BLEND);        else glDisable(GL_BLEND);
        glBlendFunc((GLenum)s.blendSrc, (GLenum)s.blendDst);
        glScissor(s.scissorBox[0], s.scissorBox[1], s.scissorBox[2], s.scissorBox[3]);
        if (s.scissorTest) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
        if (p_glActiveTexture) p_glActiveTexture((GLenum)s.activeTex);
    }

    static void LogStateDiff(const GlSnap& s) {
        static uint32_t lastSig = 0;
        uint32_t sig = ((uint32_t)s.depthMask << 0) | ((uint32_t)s.colorMask[0] << 1) |
            ((uint32_t)s.colorMask[1] << 2) | ((uint32_t)s.colorMask[2] << 3) |
            ((uint32_t)s.colorMask[3] << 4) | ((uint32_t)(s.stencilMask & 1) << 5) |
            ((uint32_t)s.scissorTest << 6) | ((uint32_t)s.blend << 7) |
            ((uint32_t)(s.scissorBox[2] << 8)) | ((uint32_t)(s.scissorBox[3] << 20));
        if (sig == lastSig) return;
        lastSig = sig;

        const bool bad = (!s.depthMask) || (!s.colorMask[0]) || (!s.colorMask[1]) ||
            (!s.colorMask[2]) || (!s.colorMask[3]) || (s.stencilMask != 0xFF) ||
            (s.scissorTest && (s.scissorBox[2] < 100 || s.scissorBox[3] < 100));
        if (bad) {
            g_maskHostile.fetch_add(1, std::memory_order_relaxed);
            Log("[render] ESTADO GL DO JOGO: depthMask=%d colorMask=%d%d%d%d stencilMask=0x%X "
                "scissorTest=%d scissorBox=%d,%d,%d,%d blend=%d -> devolvido no fim do frame",
                (int)s.depthMask, (int)s.colorMask[0], (int)s.colorMask[1],
                (int)s.colorMask[2], (int)s.colorMask[3], (unsigned)s.stencilMask,
                (int)s.scissorTest, s.scissorBox[0], s.scissorBox[1],
                s.scissorBox[2], s.scissorBox[3], (int)s.blend);
        }
    }

    static const char* GlFuncName(GLint f) {
        switch (f) {
        case 0x0200: return "NEVER";
        case 0x0201: return "LESS";
        case 0x0202: return "EQUAL";
        case 0x0203: return "LEQUAL";
        case 0x0204: return "GREATER";
        case 0x0205: return "NOTEQUAL";
        case 0x0206: return "GEQUAL";
        case 0x0207: return "ALWAYS";
        }
        return "?";
    }

    static void LogGlState() {
        static ULONGLONG last = 0;
        const ULONGLONG now = GetTickCount64();
        if (now - last < 1000) return;
        last = now;

        GLint fbo = -1, vp[4] = { 0, 0, 0, 0 };
        GLint sFunc = 0, sRef = 0, sMask = 0, dFunc = 0;
        GLboolean cm[4] = { 0, 0, 0, 0 };
        GLboolean dm = 0;

        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
        glGetIntegerv(GL_VIEWPORT, vp);
        glGetIntegerv(GL_STENCIL_FUNC, &sFunc);
        glGetIntegerv(GL_STENCIL_REF, &sRef);
        glGetIntegerv(GL_STENCIL_VALUE_MASK, &sMask);
        glGetIntegerv(GL_DEPTH_FUNC, &dFunc);
        glGetBooleanv(GL_COLOR_WRITEMASK, cm);
        glGetBooleanv(GL_DEPTH_WRITEMASK, &dm);

        g_lastFbo.store((int)fbo, std::memory_order_relaxed);

        Vec3 camPos{};
        Il2CppObject* cam = SDK::GetCamera();
        if (cam) camPos = SDK::GetPos(cam);

        Log("[glstate] fbo=%d vp=%d,%d,%d,%d depth=%d depthFunc=%s depthMask=%d scissor=%d "
            "blend=%d cull=%d stencil=%d sFunc=%s sRef=%d sMask=0x%X colorMask=%d%d%d%d sRGB=%d "
            "|| cam=%p camPos=(%.1f,%.1f,%.1f)",
            (int)fbo, vp[0], vp[1], vp[2], vp[3],
            (int)glIsEnabled(GL_DEPTH_TEST), GlFuncName(dFunc), (int)dm,
            (int)glIsEnabled(GL_SCISSOR_TEST), (int)glIsEnabled(GL_BLEND),
            (int)glIsEnabled(GL_CULL_FACE), (int)glIsEnabled(GL_STENCIL_TEST),
            GlFuncName(sFunc), (int)sRef, (unsigned)sMask,
            (int)cm[0], (int)cm[1], (int)cm[2], (int)cm[3],
            (int)glIsEnabled(GL_FRAMEBUFFER_SRGB),
            cam, camPos.x, camPos.y, camPos.z);

        const bool hostileStencil = glIsEnabled(GL_STENCIL_TEST) != 0 && sFunc != 0x0207;
        if (hostileStencil) {
            g_stencilHostile.fetch_add(1, std::memory_order_relaxed);
            static uint32_t lastLogged = 0;
            if (lastLogged != g_stencilHostile.load()) {
                lastLogged = g_stencilHostile.load();
                Log("[render] STENCIL HOSTIL: sFunc=%s sRef=%d", GlFuncName(sFunc), (int)sRef);
            }
        }
    }

    static void LogEspDbg() {
        static ULONGLONG last = 0;
        if (GetTickCount64() - last < 2000) return;
        last = GetTickCount64();

        std::vector<Target> snap;
        SDK::SnapshotInto(snap);
        int n = 0;
        for (auto& t : snap) {
            Vec2 h{}, f{};
            const bool okH = SDK::WorldToScreen(t.head, h);
            const bool okF = SDK::WorldToScreen(t.feet, f);
            Log("[espdbg] t%d '%s' dist=%.1f hp=%.0f | wHead=(%.1f,%.1f,%.1f) scrHead=(%.0f,%.0f,%d) "
                "| wFeet=(%.1f,%.1f,%.1f) scrFeet=(%.0f,%.0f,%d) | h=%.0f onScr=%d",
                n, t.name, t.dist, t.health,
                t.head.x, t.head.y, t.head.z, h.x, h.y, (int)okH,
                t.feet.x, t.feet.y, t.feet.z, f.x, f.y, (int)okF,
                f.y - h.y, (int)t.onScreen);
            if (++n >= 3) break;
        }
        Log("[espdbg] scr=%dx%d alvos=%d", SDK::ScreenW(), SDK::ScreenH(), SDK::g_targetCount);
    }

    static DWORD WINAPI RenderWatchThread(LPVOID) {
        while (g_watchRun.load(std::memory_order_relaxed) &&
            !g_unload.load(std::memory_order_relaxed)) {
            Sleep(500);
            if (!g_hooksLive.load(std::memory_order_relaxed)) continue;
            const ULONGLONG now = GetTickCount64();

            const ULONGLONG lastSwap = g_lastSwapMs.load(std::memory_order_relaxed);
            if (lastSwap && (now - lastSwap) > SWAP_MUTE_MS) {
                static ULONGLONG lastMute = 0;
                if (now - lastMute > SWAP_MUTE_MS) {
                    lastMute = now;
                    Log("[render] SWAP MUDO: %llums sem wglSwapBuffers (stage=%s fase=%s)",
                        (unsigned long long)(now - lastSwap), StageName(StageNow()), g_phase);
                }
            }

            const ULONGLONG lastDraw = g_lastDrawMs.load(std::memory_order_relaxed);
            if (lastDraw && (now - lastDraw) > 1500 && StageNow() >= Stage::Live) {
                static ULONGLONG lastNd = 0;
                if (now - lastNd > 1500) {
                    lastNd = now;
                    Log("[render] SEM DESENHO ha %llums: motivo=%s streak=%u vtx=%u fbo=%d",
                        (unsigned long long)(now - lastDraw),
                        g_noDrawReason.load(std::memory_order_relaxed),
                        g_noDrawStreak.load(std::memory_order_relaxed),
                        g_vtxTotal.load(std::memory_order_relaxed),
                        g_lastFbo.load(std::memory_order_relaxed));
                }
            }
        }
        return 0;
    }

    static MethodInfo* FindMethodByArity(Il2CppClass* k, const char* name, uint32_t argc) {
        if (!k || !name) return nullptr;
        void* iter = nullptr;
        for (;;) {
            const MethodInfo* m = il2cpp_class_get_methods(k, &iter);
            if (!m) break;
            if (il2cpp_method_get_param_count(m) == argc &&
                strcmp(il2cpp_method_get_name(m), name) == 0)
                return (MethodInfo*)m;
        }
        return nullptr;
    }
    static Il2CppClass* FindClassNamed(const char* name) {
        if (!name) return nullptr;
        Il2CppClass* k = SDK::FindClass(name);
        if (!k) return nullptr;
        const char* n = il2cpp_class_get_name(k);
        if (!n || strcmp(n, name) != 0) return nullptr;
        return k;
    }
    static void* FindMethodPtr(const char* klass, const char* method, uint32_t argc) {
        Il2CppClass* k = FindClassNamed(klass);
        if (!k) return nullptr;
        MethodInfo* m = FindMethodByArity(k, method, argc);
        return m ? SDK::MethodPtr(m) : nullptr;
    }
    static Il2CppClass* ClassOf(void* o) {
        if (!il2cpp_object_get_class || !PlausibleObj(o)) return nullptr;
        return il2cpp_object_get_class((const Il2CppObject*)o);
    }

    static bool KeyDown(int vk) {
        if (vk <= 0 || vk > 255) return false;
        return (GetAsyncKeyState(vk) & 0x8000) != 0;
    }
    static bool Edge(int vk) {
        static bool prev[256] = {};
        if (vk <= 0 || vk > 255) return false;
        const bool now = KeyDown(vk);
        const bool was = prev[vk];
        prev[vk] = now;
        return now && !was;
    }

    static inline bool MenuWantsMouse() {
        return C.menuOpen && g_imguiReady.load(std::memory_order_relaxed);
    }

    static LONG CALLBACK VehHandler(EXCEPTION_POINTERS* ep) {
        if (!ep || !ep->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;
        if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
            g_vehCount.fetch_add(1, std::memory_order_relaxed);
        }
        return EXCEPTION_CONTINUE_SEARCH;
    }

    int FontCount() {
        if (!g_imguiReady.load() || !ImGui::GetCurrentContext()) return 0;
        ImGuiIO& io = ImGui::GetIO();
        return io.Fonts ? io.Fonts->Fonts.Size : 0;
    }
    ImFont& SafeFont(int idx) {
        if (g_imguiReady.load() && ImGui::GetCurrentContext()) {
            ImGuiIO& io = ImGui::GetIO();
            const int n = io.Fonts ? io.Fonts->Fonts.Size : 0;
            if (n > 0) {
                if (idx < 0) idx = 0;
                if (idx >= n) idx = n - 1;
                if (ImFont* f = io.Fonts->Fonts[idx]) return *f;
            }
            if (ImFont* f = ImGui::GetFont()) return *f;
        }
        static ImFont s_dummy;
        return s_dummy;
    }

    [[maybe_unused]] static void ApplyNoRecoil() {
        if (!C.noRecoil || !SDK::g_localPlayer) return;
        __try {
            Il2CppObject* ct = SDK::CamTfObj();
            if (!ct || !Writable(ct)) return;
            void* dst = (uint8_t*)ct + SDK::O_rotation;
            if (!Writable(dst)) return;
            ::Quat pr = SDK::GetPlayerRot(SDK::g_localPlayer);
            SDK::SetRot(ct, pr);
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { g_avCount.fetch_add(1, std::memory_order_relaxed); }
    }

    static bool g_cursorFreed = false;
    static bool g_cursorWasShowing = true;

    static bool IsCursorShowing() {
        CURSORINFO ci{};
        ci.cbSize = sizeof(ci);
        if (!GetCursorInfo(&ci)) return true;
        return (ci.flags & CURSOR_SHOWING) != 0;
    }
    static void ForceCursor(bool show) {
        if (IsCursorShowing() == show) return;
        int guard = 0;
        if (show) { while (!IsCursorShowing() && ++guard < 64) ShowCursor(TRUE); }
        else { while (IsCursorShowing() && ++guard < 64) ShowCursor(FALSE); }
    }
    static void ApplyCursorOwnership() {
        const bool want = MenuWantsMouse();
        if (want) {
            if (!g_cursorFreed) {
                g_cursorWasShowing = IsCursorShowing();
                g_cursorFreed = true;
                Log("[cursor] menu aberto: cursor LIVRE");
            }
            ForceCursor(true);
            ClipCursor(nullptr);
        }
        else if (g_cursorFreed) {
            g_cursorFreed = false;
            ForceCursor(g_cursorWasShowing);
            Log("[cursor] menu fechado: cursor devolvido");
        }
    }

    static BOOL WINAPI hk_SetCursorPos(int x, int y) {
        if (MenuWantsMouse()) return TRUE;
        return o_setCursorPos ? o_setCursorPos(x, y) : TRUE;
    }

    bool InstallCursor() {
        HMODULE u32 = GetModuleHandleA("user32.dll");
        if (!u32) return false;
        void* p = (void*)GetProcAddress(u32, "SetCursorPos");
        if (!p) return false;
        if (MH_CreateHook(p, &hk_SetCursorPos, (void**)&o_setCursorPos) != MH_OK) return false;
        MH_EnableHook(p);
        Log("[cursor] hook SetCursorPos OK");
        return true;
    }

    void LogWndProcInfo() {
        if (!g_hwnd) { Log("[wndproc] hwnd=null"); return; }
        LONG_PTR cur = GetWindowLongPtrA(g_hwnd, GWLP_WNDPROC);
        Log("[wndproc] hwnd=%p cur=%p orig=%p", (void*)g_hwnd, (void*)cur, (void*)o_wndProc);
    }

    static thread_local int t_wndDepth = 0;

    static bool IsMouseMsg(UINT m) {
        switch (m) {
        case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP:
        case WM_LBUTTONDBLCLK: case WM_RBUTTONDOWN: case WM_RBUTTONUP:
        case WM_RBUTTONDBLCLK: case WM_MBUTTONDOWN: case WM_MBUTTONUP:
        case WM_MBUTTONDBLCLK: case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
            return true;
        }
        return false;
    }

    static LRESULT CALLBACK hk_wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        g_wndProcCalls.fetch_add(1, std::memory_order_relaxed);
        const ULONGLONG now = GetTickCount64();
        g_lastWndMsg.store(now, std::memory_order_relaxed);
        if (IsMouseMsg(msg)) g_lastMouseMsg.store(now, std::memory_order_relaxed);

        if (t_wndDepth > 0)
            return o_wndProc ? CallWindowProcA(o_wndProc, h, msg, wp, lp)
            : DefWindowProcA(h, msg, wp, lp);

        ++t_wndDepth;
        if (C.menuOpen && msg == WM_SETCURSOR) {
            SetCursor(LoadCursorA(nullptr, (LPCSTR)IDC_ARROW));
            --t_wndDepth;
            return TRUE;
        }

        bool ate = false;
        if (g_imguiInit && g_imCsInit && C.menuOpen) {
            EnterCriticalSection(&g_imCs);
            ImGui_ImplWin32_WndProcHandler(h, msg, wp, lp);
            ImGuiIO& io = ImGui::GetIO();
            ate = io.WantCaptureMouse || io.WantCaptureKeyboard;
            LeaveCriticalSection(&g_imCs);
            if (ate && IsMouseMsg(msg)) { --t_wndDepth; return TRUE; }
        }

        LRESULT r = o_wndProc ? CallWindowProcA(o_wndProc, h, msg, wp, lp)
            : DefWindowProcA(h, msg, wp, lp);
        --t_wndDepth;
        return r;
    }

    static LRESULT CALLBACK CwpProc(int code, WPARAM wp, LPARAM lp) {
        if (code == HC_ACTION && lp) {
            const CWPSTRUCT* c = (const CWPSTRUCT*)lp;
            g_cwpCalls.fetch_add(1, std::memory_order_relaxed);
            const ULONGLONG now = GetTickCount64();
            const bool wndAlive = (now - g_lastWndMsg.load(std::memory_order_relaxed)) < 50;
            if (!wndAlive && g_imguiInit && g_imCsInit && c->hwnd == g_hwnd) {
                EnterCriticalSection(&g_imCs);
                ImGui_ImplWin32_WndProcHandler(c->hwnd, c->message, c->wParam, c->lParam);
                LeaveCriticalSection(&g_imCs);
            }
        }
        return CallNextHookEx(nullptr, code, wp, lp);
    }

    static void ShutdownImGui() {
        if (!g_imguiInit) return;
        ImGui_ImplOpenGL3_Shutdown();
        if (g_win32Backend) { ImGui_ImplWin32_Shutdown(); g_win32Backend = false; }
        ImGui::DestroyContext();
        g_imguiGeneration.fetch_add(1, std::memory_order_acq_rel);
        g_imguiInit = false;
        Advance(Stage::Hooks);
    }

    static bool InitImGui(HWND hwnd) {
        if (g_imguiInit) return true;
        if (!hwnd) return false;

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        g_imguiGeneration.fetch_add(1, std::memory_order_acq_rel);

        ImGui::GetIO().IniFilename = nullptr;
        ImGui::StyleColorsDark();
        Menu::ApplyStyle();

        if (!ImGui_ImplWin32_Init(hwnd)) { Log("[imgui] Win32 init FALHOU"); return false; }
        g_win32Backend = true;

        if (!ImGui_ImplOpenGL3_Init()) {
            Log("[imgui] OpenGL3 init FALHOU");
            ImGui_ImplWin32_Shutdown();
            g_win32Backend = false;
            return false;
        }
        g_imguiInit = true;
        Log("[imgui] contexto criado");
        Advance(Stage::ImGui);
        return true;
    }

    static HWND ResolveWindow(HDC hdc) {
        HWND h = hdc ? WindowFromDC(hdc) : nullptr;
        if (h && h != GetDesktopWindow()) return h;
        if (g_hwnd && IsWindow(g_hwnd)) return g_hwnd;
        HWND fg = GetForegroundWindow();
        if (fg && fg != GetDesktopWindow()) return fg;
        return FindWindowA("UnityWndClass", nullptr);
    }

    static void RebindWindow(HWND hwnd) {
        if (!hwnd || hwnd == GetDesktopWindow()) return;
        CsEnter();

        HWND oldWnd = g_hwnd;
        LONG_PTR prev = GetWindowLongPtrA(hwnd, GWLP_WNDPROC);

        if (prev == (LONG_PTR)hk_wndProc && hwnd == g_hwnd) { CsLeave(); return; }

        SetWindowLongPtrA(hwnd, GWLP_WNDPROC, (LONG_PTR)hk_wndProc);
        if (prev && prev != (LONG_PTR)hk_wndProc) o_wndProc = (WNDPROC)prev;

        g_hwnd = hwnd;
        Log("[hooks] janela = %p (anterior=%p) orig=%p", (void*)hwnd, (void*)oldWnd, (void*)o_wndProc);

        if (!g_imguiInit) {
            InitImGui(hwnd);
        }
        else if (g_win32Backend) {
            ImGui_ImplWin32_Shutdown();
            g_win32Backend = false;
            if (!ImGui_ImplWin32_Init(hwnd)) {
                g_imguiReady.store(false, std::memory_order_release);
            }
            else {
                g_win32Backend = true;
                ImGui::GetIO().IniFilename = nullptr;
                g_imguiReady.store(true, std::memory_order_release);
            }
            ReinitGlBackend("janela trocada");
        }
        CsLeave();
    }

    static void RunTickOnce() {
        const uint64_t f = g_frameCounter.load(std::memory_order_relaxed);
        uint64_t previous = g_lastTickFrame.load(std::memory_order_relaxed);
        for (;;) {
            if (previous == f) return;
            if (g_lastTickFrame.compare_exchange_weak(
                previous, f, std::memory_order_acq_rel, std::memory_order_relaxed))
                break;
        }
        SDK::g_frameId = (uint32_t)f;
        __try { Combat::Tick(); }
        __except (EXCEPTION_EXECUTE_HANDLER) { Log("[crash] Combat::Tick"); }
    }

    static void __fastcall hk_lateUpdate(void* self, void* method) {
        PHASE("hk_lateUpdate");
        if (AtLvl(Lvl::Tick)) {
            g_lastLuMs.store(GetTickCount64(), std::memory_order_relaxed);
            RunTickOnce();
        }
        if (o_lateUpdate) o_lateUpdate(self, method);
    }

    static void Watchdog() {
        if (!AtLvl(Lvl::Watchdog)) return;
        if (!g_sdkReady.load(std::memory_order_acquire)) return;
        const ULONGLONG now = GetTickCount64();
        if (now - g_lastLuMs.load(std::memory_order_relaxed) < TICK_WATCHDOG_MS) return;
        g_lastLuMs.store(now, std::memory_order_relaxed);
        RunTickOnce();
    }

    // -----------------------------------------------------------------------
    //  FIRE
    // -----------------------------------------------------------------------
    static int PickDirSlot(const Vec3& a, const Vec3& b) {
        const float la = Len(a), lb = Len(b);
        const float da = fabsf(la - 1.f), db = fabsf(lb - 1.f);
        if (da + 0.05f < db) return 0;
        if (db + 0.05f < da) return 1;
        const float pa = (a - SDK::g_localPos).len();
        const float pb = (b - SDK::g_localPos).len();
        return (pa < pb) ? 1 : 0;
    }

    static bool IsLocalShooter(void* self) {
        if (!SDK::g_localPlayer) return true;
        if (self == (void*)SDK::g_localPlayer) return true;
        Il2CppObject* a = SDK::GetTransform(self);
        Il2CppObject* b = SDK::GetTransform(SDK::g_localPlayer);
        return a && b && a == b;
    }

    static bool __fastcall hk_fire7(void* self, int p0, float p1,
        Vec3 p2, Vec3 p3, bool p4, double p5, int p6, void* method) {
        PHASE("hk_fire");
        if (!o_fire7) return false;

        if (StageNow() < Stage::Live || !self || !g_sdkReady.load(std::memory_order_relaxed))
            return o_fire7(self, p0, p1, p2, p3, p4, p5, p6, method);

        const bool isLocalShot = IsLocalShooter(self);
        static int probeAll = 0, probeLocal = 0;
        if (probeAll < 8 || (isLocalShot && probeLocal < 8)) {
            if (probeAll < 8) ++probeAll;
            if (isLocalShot)  ++probeLocal;

            Vec3 camPos{}, selfPos{};
            if (Il2CppObject* ct = SDK::CamTfObj()) camPos = SDK::GetPos(ct);
            selfPos = SDK::GetPos((Il2CppObject*)self);
            Log("[fire-probe] LOCAL=%d self=%p p2=(%.2f,%.2f,%.2f)|%.3f| dSelf=%.1f dCam=%.1f "
                "|| p3=(%.2f,%.2f,%.2f)|%.3f| || args p0=%d p1=%.3f p4=%d p6=%d",
                (int)isLocalShot, self,
                p2.x, p2.y, p2.z, Len(p2), Dist(p2, selfPos), Dist(p2, camPos),
                p3.x, p3.y, p3.z, Len(p3), p0, p1, (int)p4, p6);
        }

        const int slot = PickDirSlot(p2, p3);
        s_dirSlot.store(slot);
        SDK::g_fireDirSlot.store(slot);

        static int lastSlot = -2;
        if (slot != lastSlot) {
            lastSlot = slot;
            Log("[fire] dirSlot=%d |p2|=%.3f |p3|=%.3f", slot, Len(p2), Len(p3));
        }

        const Vec3 keep2 = p2;
        const Vec3 keep3 = p3;

        const bool silentOn = isLocalShot && C.silentAim &&
            (C.aimToggle ? Aimbot::IsActive() : C.aimKeyDown);

        if (silentOn) {
            __try {
                Vec3& dir = (slot == 0) ? p2 : p3;
                Vec3& org = (slot == 0) ? p3 : p2;
                const Vec3 keepDir = dir;

                Vec3 origin = org;
                const float lo = Len(origin);
                const Vec3 selfPos = SDK::GetPos((Il2CppObject*)self);
                const bool originOk = (lo > 0.5f) && (lo < 20000.f) &&
                    (selfPos.zero() || Dist(origin, selfPos) < 6.f);
                if (!originOk) origin = SDK::MuzzlePos();

                PHASE("fire:aim");
                const bool ok = Aimbot::Silent::Override(origin, dir);

                if (!ok) dir = keepDir;
                else {
                    const float L = Len(dir);
                    if (L > 1e-4f) { dir.x /= L; dir.y /= L; dir.z /= L; }
                }
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                p2 = keep2; p3 = keep3;
                Log("[fire] AV no silent aim");
            }
        }

        Quat camBefore{};
        bool camOk = false;
        if (C.noRecoil) {
            __try {
                Il2CppObject* ct = SDK::CamTfObj();
                camOk = ct && SDK::GetRot(ct, camBefore);
            }
            __except (EXCEPTION_EXECUTE_HANDLER) { camOk = false; }
        }

        __try {
            PHASE("fire:orig");
            const bool r = o_fire7(self, p0, p1, p2, p3, p4, p5, p6, method);
            PHASE("fire:post");
            if (camOk) {
                __try { if (Il2CppObject* ct = SDK::CamTfObj()) SDK::SetRot(ct, camBefore); }
                __except (EXCEPTION_EXECUTE_HANDLER) {}
            }
            return r;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { Log("[fire] AV no original"); return false; }
    }

    // -----------------------------------------------------------------------
    //  SWAP
    // -----------------------------------------------------------------------
    static bool ImGuiInFrame() {
        ImGuiContext* g = ImGui::GetCurrentContext();
        return g && g->WithinFrameScope;
    }

    // corpo do swap — SEM __try interno grande; o chamador protege tudo
    static void SwapBody(HDC hdc) {
        __try {
            static int winPoll = 0;
            if (!g_hwnd || ++winPoll >= 8) {
                winPoll = 0;
                HWND hwnd = ResolveWindow(hdc);
                if (hwnd && hwnd != g_hwnd) RebindWindow(hwnd);
            }

            if (g_hwnd && o_wndProc) {
                static ULONGLONG lastWndCheck = 0;
                const ULONGLONG nowc = GetTickCount64();
                if (nowc - lastWndCheck > WND_REAPPLY_MS) {
                    lastWndCheck = nowc;
                    LONG_PTR cur = GetWindowLongPtrA(g_hwnd, GWLP_WNDPROC);
                    if (cur != (LONG_PTR)hk_wndProc) {
                        Log("[hooks] wndproc restaurado — reaplicando");
                        SetWindowLongPtrA(g_hwnd, GWLP_WNDPROC, (LONG_PTR)hk_wndProc);
                    }
                }
            }

            if (AtLvl(Lvl::CursorFn)) ApplyCursorOwnership();
            HandleKeys();
            Watchdog();
            LogGlState();
            LogEspDbg();

            static ULONGLONG lastBeat = 0;
            const ULONGLONG nowb = GetTickCount64();
            if (nowb - lastBeat >= 1000) {
                lastBeat = nowb;
                Log("[beat] swap=%u noDraw=%u(%s) ctx=%u vtx=%u fbo=%d hostile=%u maskH=%u "
                    "abortBody=%u abortPres=%u alvos=%d invis=%d esp=%u menu=%d stage=%s av=%u veh=%u",
                    g_swapCalls.load(), g_noDrawStreak.load(),
                    g_noDrawReason.load(), g_ctxChanges.load(), g_vtxTotal.load(),
                    g_lastFbo.load(), g_stencilHostile.load(), g_maskHostile.load(),
                    g_abortBody.load(), g_abortPresent.load(),
                    SDK::g_targetCount, ESP::Invisiveis(), g_espBranch.load(),
                    (int)C.menuOpen, StageName(StageNow()),
                    g_avCount.load(), g_vehCount.load());
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { Log("[crash] swap bloco1 (AV)"); }

        if (!g_imguiInit || !g_hwnd) { NoteNoDraw("sem imgui/hwnd"); return; }

        PHASE("swap:ctx");
        {
            HGLRC rc = wglGetCurrentContext();
            if (rc != g_glrc) {
                Log("[render] CONTEXTO GL MUDOU %p -> %p — recriando backend GL3", g_glrc, rc);
                g_glrc = rc;
                g_ctxChanges.fetch_add(1, std::memory_order_relaxed);
                ReinitGlBackend("contexto GL mudou");
            }
        }

        PHASE("swap:vp");
        GLint vp[4] = { 0, 0, 0, 0 };
        glGetIntegerv(GL_VIEWPORT, vp);

        int w = 0, h = 0;
        {
            if (vp[2] > 0 && vp[3] > 0) { w = vp[2]; h = vp[3]; }
            else {
                RECT rc{};
                if (GetClientRect(g_hwnd, &rc)) { w = rc.right - rc.left; h = rc.bottom - rc.top; }
            }
            if (w <= 0 || h <= 0) { NoteNoDraw("viewport 0x0"); return; }
            SDK::SetViewport(w, h);
            SetViewSize(w, h);
        }

        CsEnter();
        PHASE("swap:frame");

        bool frameOk = false;
        __try {
            if (ImGuiInFrame()) ImGui::EndFrame();

            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplWin32_NewFrame();

            ImGuiIO& io = ImGui::GetIO();
            io.DisplaySize = ImVec2((float)SDK::ScreenW(), (float)SDK::ScreenH());

            if (C.menuOpen) {
                const bool wndMute =
                    (GetTickCount64() - g_lastMouseMsg.load(std::memory_order_relaxed))
        > MOUSE_FALLBACK_MS;
        if (wndMute) {
            POINT pt{};
            if (GetCursorPos(&pt) && ScreenToClient(g_hwnd, &pt))
                io.AddMousePosEvent((float)pt.x, (float)pt.y);
            io.AddMouseButtonEvent(0, (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0);
            io.AddMouseButtonEvent(1, (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0);
            io.AddMouseButtonEvent(2, (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0);
        }
            }

            ImGui::NewFrame();

            {
                ImDrawList* dl = ImGui::GetBackgroundDrawList();
                const float W = (float)SDK::ScreenW(), H = (float)SDK::ScreenH();
                const float t = (float)(GetTickCount64() % 1000) / 1000.f;
                dl->AddRect(ImVec2(2.f, 2.f), ImVec2(W - 2.f, H - 2.f),
                    IM_COL32(255, 0, 0, 255), 0.f, 0, 4.f);
                dl->AddRectFilled(ImVec2(10.f, 10.f), ImVec2(80.f, 80.f),
                    IM_COL32(255, 0, 0, (int)(120 + 135 * t)));
                char tb[128];
                snprintf(tb, sizeof(tb), "TESTE f=%llu fbo=%d vtx=%u tgt=%d host=%u mask=%u",
                    (unsigned long long)g_frameCounter.load(std::memory_order_relaxed),
                    g_lastFbo.load(std::memory_order_relaxed),
                    g_vtxTotal.load(std::memory_order_relaxed), SDK::g_targetCount,
                    g_stencilHostile.load(std::memory_order_relaxed),
                    g_maskHostile.load(std::memory_order_relaxed));
                dl->AddText(ImVec2(88.f, 16.f), IM_COL32(255, 255, 0, 255), tb);
            }
            frameOk = true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { g_abortPre.fetch_add(1, std::memory_order_relaxed); }

        if (!frameOk) NoteNoDraw("frame falhou");

        if (frameOk) {
            if (C.menuOpen) {
                __try { Menu::Render(); }
                __except (EXCEPTION_EXECUTE_HANDLER) { g_abortMenu.fetch_add(1, std::memory_order_relaxed); }
            }

            if (AtLeast(Stage::Live) && AtLvl(Lvl::Esp)) {
                g_espBranch.fetch_add(1, std::memory_order_relaxed);
                __try { ESP::Draw(); }
                __except (EXCEPTION_EXECUTE_HANDLER) { g_abortEsp.fetch_add(1, std::memory_order_relaxed); }
                __try { Misc::Draw(); }
                __except (EXCEPTION_EXECUTE_HANDLER) { g_abortMisc.fetch_add(1, std::memory_order_relaxed); }
            }
            else {
                NoteNoDraw(StageNow() < Stage::Live ? "stage<Live" : "lvl<Esp");
            }

            __try { ImGui::Render(); }
            __except (EXCEPTION_EXECUTE_HANDLER) { g_abortRender.fetch_add(1, std::memory_order_relaxed); }

            PHASE("swap:draw");
            ImDrawData* dd = ImGui::GetDrawData();
            if (dd && dd->Valid && dd->CmdListsCount > 0) {
                NoteDraw((uint32_t)dd->TotalVtxCount);

                GlSnap snap{};
                __try { GlSnapNow(snap); LogStateDiff(snap); }
                __except (EXCEPTION_EXECUTE_HANDLER) {}

                GLint prevFbo = 0;
                glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
                bool bound = false;
                if (prevFbo != 0 && p_glBindFramebuffer) {
                    g_fboFrames.fetch_add(1, std::memory_order_relaxed);
                    p_glBindFramebuffer(GL_FRAMEBUFFER, 0);
                    bound = true;
                }
                glDrawBuffer(GL_BACK);

                __try {
                    glDisable(GL_DEPTH_TEST);
                    glDisable(GL_STENCIL_TEST);
                    glDisable(GL_CULL_FACE);
                    glDisable(GL_POLYGON_OFFSET_FILL);
                    glDisable(GL_SCISSOR_TEST);
                    glEnable(GL_BLEND);
                    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                    glDepthMask(GL_FALSE);
                    glStencilMask(0xFF);
                    if (p_glActiveTexture) p_glActiveTexture(GL_TEXTURE0);
                }
                __except (EXCEPTION_EXECUTE_HANDLER) {}

                glViewport(0, 0, SDK::ScreenW(), SDK::ScreenH());
                __try { ImGui_ImplOpenGL3_RenderDrawData(dd); }
                __except (EXCEPTION_EXECUTE_HANDLER) { g_abortDraw.fetch_add(1, std::memory_order_relaxed); }

                // DEVOLVE o estado — salva o clear do proximo frame do jogo
                __try { GlRestoreNow(snap); }
                __except (EXCEPTION_EXECUTE_HANDLER) {}

                if (bound) p_glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFbo);
            }
            else {
                NoteNoDraw("drawdata vazio");
            }
        }

        PHASE("swap:post");
        glViewport(vp[0], vp[1], vp[2], vp[3]);
        CsLeave();
    }

    static BOOL WINAPI hk_wglSwapBuffers(HDC hdc) {
        PHASE("swap:pre");
        g_frameCounter.fetch_add(1, std::memory_order_relaxed);
        g_swapCalls.fetch_add(1, std::memory_order_relaxed);
        g_lastSwapMs.store(GetTickCount64(), std::memory_order_relaxed);
        SDK::g_frameId = (uint32_t)g_frameCounter.load(std::memory_order_relaxed);

        if (!o_swap) return TRUE;

        // >>> [R7] corpo inteiro protegido; qualquer AV nao derruba a thread
        __try { SwapBody(hdc); }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            g_abortBody.fetch_add(1, std::memory_order_relaxed);
            Log("[crash] swap CORPO (AV) — fase=%s", g_phase);
            CsUnwind();
        }

        PHASE("swap:present");
        __try { return o_swap(hdc); }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            g_abortPresent.fetch_add(1, std::memory_order_relaxed);
            Log("[crash] o_swap (AV)");
            return TRUE;
        }
    }

    void HandleKeys() {
        if (Edge(C.menuKey))   C.menuOpen = !C.menuOpen;
        if (Edge(C.panicKey))  Misc::Panic();
        if (Edge(C.saveKey))   ConfigIO::Save();
        if (Edge(C.unloadKey)) { ConfigIO::Save(); g_unload.store(true, std::memory_order_relaxed); }
        if (C.aimKey)          C.aimKeyDown = KeyDown(C.aimKey);
    }

    bool InstallSwap() {
        HMODULE gl = GetModuleHandleA("opengl32.dll");
        if (!gl) { Log("[hooks] opengl32.dll nao carregada"); return false; }
        void* p = (void*)GetProcAddress(gl, "wglSwapBuffers");
        if (!p) { Log("[hooks] wglSwapBuffers NAO encontrado"); return false; }
        if (MH_CreateHook(p, &hk_wglSwapBuffers, (void**)&o_swap) != MH_OK) {
            Log("[hooks] MH_CreateHook(wglSwapBuffers) FALHOU");
            return false;
        }
        MH_EnableHook(p);
        Log("[hooks] wglSwapBuffers hook OK");
        return true;
    }

    bool InstallMainThread() {
        void* p = FindMethodPtr("PlayerScript", "LateUpdate", 0);
        if (!p) { Log("[hooks] PlayerScript::LateUpdate NAO encontrado"); return false; }
        if (MH_CreateHook(p, &hk_lateUpdate, (void**)&o_lateUpdate) != MH_OK) {
            Log("[hooks] MH_CreateHook(LateUpdate) FALHOU");
            return false;
        }
        MH_EnableHook(p);
        Log("[hooks] main thread: PlayerScript::LateUpdate");
        return true;
    }

    bool InstallCombat() {
        Il2CppClass* k = FindClassNamed("PlayerScript");
        MethodInfo* m = k ? FindMethodByArity(k, "FireBullet", 7) : nullptr;
        if (m) Log("[fire] FireBullet(7) return=%s", SDK::MethodReturnName(m));
        if (!m) { Log("[hooks] FireBullet(7) NAO encontrado"); return false; }

        void* p = SDK::MethodPtr(m);
        if (!p) { Log("[hooks] FireBullet(7) sem ponteiro"); return false; }
        if (MH_CreateHook(p, &hk_fire7, (void**)&o_fire7) != MH_OK) {
            Log("[hooks] MH_CreateHook(FireBullet) FALHOU");
            return false;
        }
        MH_EnableHook(p);
        Log("[hooks] FireBullet(7) hook OK (%p)", p);
        return true;
    }

    bool InstallCwp() {
        g_cwp = SetWindowsHookExA(WH_CALLWNDPROC, CwpProc, nullptr, GetCurrentThreadId());
        if (!g_cwp) { Log("[input] SetWindowsHookEx(CALLWNDPROC) FALHOU"); return false; }
        Log("[input] cwp=%p tid=%u lvl=%d (fallback)", (void*)g_cwp, GetCurrentThreadId(), INPUT_TEST_LEVEL);
        return true;
    }

    bool Init() {
        if (!g_imCsInit) { InitializeCriticalSection(&g_imCs); g_imCsInit = true; }
        if (MH_Initialize() != MH_OK) { Log("[hooks] MH_Initialize FALHOU"); return false; }

        SetStage(Stage::Init);
        Log("[input] INPUT_TEST_LEVEL=%d", INPUT_TEST_LEVEL);

        // >>> [R7] resolve as extensoes GL AGORA (antes ficava nullptr sempre)
        if (!LoadGlExt())
            Log("[gl] extensoes GL parciais: bindFbo=%p activeTex=%p",
                (void*)p_glBindFramebuffer, (void*)p_glActiveTexture);
        else
            Log("[gl] extensoes GL ok: bindFbo=%p activeTex=%p",
                (void*)p_glBindFramebuffer, (void*)p_glActiveTexture);

        if (!g_veh) g_veh = AddVectoredExceptionHandler(1, VehHandler);

        if (AtLvl(Lvl::CursorFn)) InstallCursor();
        InstallCwp();

        if (!InstallSwap()) return false;
        Advance(Stage::Hooks);

        if (!InstallMainThread()) Log("[hooks] LateUpdate falhou — watchdog assume");
        if (!InstallCombat())     Log("[hooks] FireBullet falhou (aimbot ok)");

        if (!g_watchThread) {
            g_watchRun.store(true, std::memory_order_relaxed);
            g_watchThread = CreateThread(nullptr, 0, RenderWatchThread, nullptr, 0, nullptr);
            Log("[render] vigia do render iniciado");
        }

        g_lastLuMs.store(GetTickCount64(), std::memory_order_relaxed);
        SetStage(Stage::Live);
        Log("[hooks] init ok stage=%s menuKey=0x%X", StageName(StageNow()), C.menuKey);
        return true;
    }

    bool Reinstall() {
        if (g_hwnd) {
            LONG_PTR prev = GetWindowLongPtrA(g_hwnd, GWLP_WNDPROC);
            if (prev != (LONG_PTR)hk_wndProc) {
                o_wndProc = (WNDPROC)prev;
                SetWindowLongPtrA(g_hwnd, GWLP_WNDPROC, (LONG_PTR)hk_wndProc);
            }
        }
        MH_DisableHook(MH_ALL_HOOKS);
        MH_EnableHook(MH_ALL_HOOKS);
        Log("[hooks] Reinstall ok");
        return true;
    }

    bool Shutdown() {
        if (StageNow() == Stage::Down) return true;
        SetStage(Stage::Tearing);
        g_hooksLive.store(false, std::memory_order_release);
        g_imguiReady.store(false, std::memory_order_release);

        g_watchRun.store(false, std::memory_order_relaxed);
        if (g_watchThread) {
            WaitForSingleObject(g_watchThread, 800);
            CloseHandle(g_watchThread);
            g_watchThread = nullptr;
        }

        SDK::g_recoilHookActive = false;
        if (g_cursorFreed) { g_cursorFreed = false; ForceCursor(g_cursorWasShowing); }

        if (o_wndProc && g_hwnd)
            SetWindowLongPtrA(g_hwnd, GWLP_WNDPROC, (LONG_PTR)o_wndProc);
        o_wndProc = nullptr;

        if (g_cwp) { UnhookWindowsHookEx(g_cwp); g_cwp = nullptr; }
        if (g_veh) { RemoveVectoredExceptionHandler(g_veh); g_veh = nullptr; }

        MH_DisableHook(MH_ALL_HOOKS);
        Sleep(120);

        CsEnter(); ShutdownImGui(); CsLeave();

        MH_Uninitialize();
        g_lastLuMs.store(0, std::memory_order_relaxed);
        g_glrc = nullptr;
        SetStage(Stage::Down);
        Log("[hooks] Shutdown concluido");
        return true;
    }

} // namespace Hooks