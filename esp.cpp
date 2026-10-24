// ============================================================================
//  esp.cpp — publicacao (Tick, thread do jogo) + desenho (Draw, thread render)
//                                                                          v23
//
//  FIX v23: C4244 — keepFrame agora com cast explicito (uint32_t)s.frame.
//  FIX v22: instrumentacao completa do WorldToScreen (ok/semMatriz/atras/fora).
//  FIX v21: s_buf com 4 slots (leitor seguro por 3 escritas do Tick).
//  FIX v20: Draw nao da early-return por tamanho; Tick blinda o laco com __try.
// ============================================================================

#include <windows.h>
#include <GL/gl.h>

#include "common.h"
#include "features.h"
#include "sdk.h"
#include "hooks.h"

#include <imgui.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace ESP
{

    namespace
    {
        // ---------------------------------------------------------------- tuning
        constexpr int      kDeadHold = 4;       // hp<=0 seguido antes de descartar
        constexpr int      kVisHold = 3;       // "sem visao" seguido antes de trocar a cor
        constexpr uint32_t kLogMs = 1000;    // throttle dos logs
        constexpr uint32_t kLatchStale = 120;     // frames ate reciclar um latch orfao

        // >>> 4 slots: leitor seguro por 3 escritas do Tick (era 2 -> piscava)
        constexpr int      kSlots = 4;

        // --------------------------------------------------------- estado interno
        Snap             s_buf[kSlots];
        std::atomic<int> s_active{ 0 };
        std::atomic<uint32_t> s_seq{ 0 };

        std::atomic<int> s_count{ 0 };
        std::atomic<int> s_invis{ 0 };
        std::atomic<int> s_fLocal{ 0 }, s_fMorto{ 0 }, s_fRange{ 0 };
        std::atomic<int> s_fTime{ 0 }, s_fNpc{ 0 }, s_fCoord{ 0 };

        int  s_emptyStreak = 0;
        int  s_staleStreak = 0;

        Snap s_hold;                     // ultimo snap que realmente desenhou
        bool s_holdValid = false;

        struct Latch
        {
            void* p = nullptr;
            uint8_t  dead = 0;
            uint8_t  blocked = 0;
            bool     vis = false;
            uint32_t used = 0;
        };
        Latch    s_latch[kMaxEspEnts];
        uint32_t s_latchTick = 0;

        uint32_t g_lastEspLog = 0;
        uint32_t g_lastDrawLog = 0;

        inline uint32_t NowMs() { return static_cast<uint32_t>(GetTickCount64()); }
        inline bool     Fin(float v) { return std::isfinite(v); }

        inline ImU32 Col(const float* c)
        {
            return ImGui::ColorConvertFloat4ToU32(ImVec4(c[0], c[1], c[2], c[3]));
        }

        Latch& LatchFor(void* p)
        {
            ++s_latchTick;

            int slot = -1;
            for (int i = 0; i < kMaxEspEnts; ++i)
            {
                if (s_latch[i].p == p)
                {
                    s_latch[i].used = s_latchTick;
                    return s_latch[i];
                }
                if (slot < 0 && (s_latch[i].p == nullptr ||
                    s_latchTick - s_latch[i].used > kLatchStale))
                    slot = i;
            }

            if (slot < 0) slot = static_cast<int>(s_latchTick % kMaxEspEnts);

            Latch& L = s_latch[slot];
            L = Latch{};
            L.p = p;
            L.used = s_latchTick;
            return L;
        }

        inline bool RectOk(const Snap::Ent& e, float W, float H,
            float& x0, float& y0, float& x1, float& y1)
        {
            if (!Fin(e.head.x) || !Fin(e.head.y) || !Fin(e.feet.x) || !Fin(e.feet.y))
                return false;

            const float hgt = e.feet.y - e.head.y;
            if (!(hgt > 0.5f && hgt < 4000.0f))
                return false;

            float hw = hgt * 0.30f;
            if (hw < 1.5f) hw = 1.5f;

            const float cx = (e.head.x + e.feet.x) * 0.5f;

            x0 = cx - hw; x1 = cx + hw;
            y0 = e.head.y; y1 = e.feet.y;

            if (x1 < 0.0f || y1 < 0.0f || x0 > W || y0 > H)
                return false;

            return true;
        }

        inline int Desenhaveis(const Snap* s, float W, float H)
        {
            int k = 0;
            for (int i = 0; i < s->count; ++i)
            {
                float a, b, c, d;
                if (RectOk(s->ents[i], W, H, a, b, c, d)) ++k;
            }
            return k;
        }

        // tamanho de tela para o DESENHO: DisplaySize e' autoritativo (o
        // hooks.cpp seta a cada frame a partir da fonte unica). snap->w/h e'
        // so' fallback.
        inline void ScreenSize(const Snap* s, float& W, float& H)
        {
            ImGuiIO& io = ImGui::GetIO();
            W = io.DisplaySize.x;
            H = io.DisplaySize.y;
            if (W <= 1.0f || H <= 1.0f) { W = (float)s->w; H = (float)s->h; }
            if (W <= 1.0f || H <= 1.0f) { W = (float)Hooks::ViewW(); H = (float)Hooks::ViewH(); }
        }
    }

    // ============================================================================
    //  Tick — thread do jogo
    // ============================================================================
    void Tick()
    {
        const int cur = s_active.load(std::memory_order_relaxed);
        const int dst = (cur + 1) % kSlots;      // >>> 4 slots

        Snap& s = s_buf[dst];
        const Snap& lastGood = s_buf[cur];

        s.frame = s_seq.fetch_add(1, std::memory_order_relaxed) + 1;
        s.w = SDK::ScreenW();
        s.h = SDK::ScreenH();
        s.count = 0;

        if (!C.esp)
        {
            s_count.store(0, std::memory_order_relaxed);
            s_invis.store(0, std::memory_order_relaxed);
            s_active.store(dst, std::memory_order_release);
            return;
        }

        static int failStreak = 0;
        if (!SDK::g_localPlayer)
        {
            if (++failStreak <= kGraceFrames && lastGood.count > 0)
            {
                const uint32_t keepFrame = (uint32_t)s.frame;   // >>> FIX C4244
                s = lastGood;
                s.frame = keepFrame;
                s_active.store(dst, std::memory_order_release);
                return;
            }
            s.count = 0;
            s_count.store(0, std::memory_order_relaxed);
            s_active.store(dst, std::memory_order_release);
            return;
        }
        failStreak = 0;

        const Il2CppObject* local = SDK::g_localPlayer;
        const Il2CppObject* aimTarget = SDK::g_aimTarget;
        const int           localTeam = SDK::g_localTeam;

        int total = 0, out = 0, invis = 0;
        int fLocal = 0, fMorto = 0, fRange = 0, fTime = 0, fNpc = 0, fCoord = 0;

        // __try no laco: um AV no meio nao pode vazar o LockTargets.
        SDK::LockTargets();
        __try
        {
            const int n = SDK::g_targetCount;

            for (int i = 0; i < n && out < kMaxEspEnts; ++i)
            {
                const auto& t = SDK::g_targets[i];
                if (t.player == nullptr) continue;
                ++total;

                if (t.player == local) { ++fLocal; continue; }
                if (C.teamCheck && t.team == localTeam) { ++fTime;  continue; }
                if (t.dist > C.espMaxDist) { ++fRange; continue; }
                if (t.npc && !C.espNpc) { ++fNpc;   continue; }
                if (!PtrOk_(t.player)) { ++fCoord; continue; }

                // o WorldToScreen escreve SENTINELA (-1,-1) quando falha, entao
                // isto reprova sozinho a coordenada herdada do frame anterior.
                const float hgt = t.scrFeet.y - t.scrHead.y;
                if (!Fin(t.scrHead.x) || !Fin(t.scrHead.y) ||
                    !Fin(t.scrFeet.x) || !Fin(t.scrFeet.y) ||
                    !(hgt > 0.5f && hgt < 4000.0f)) {
                    ++fCoord; continue;
                }

                Latch& L = LatchFor(static_cast<void*>(t.player));

                if (t.health > 0.0f)
                    L.dead = 0;
                else if (L.dead < 0xFF)
                    ++L.dead;

                if (t.health <= 0.0f && L.dead >= kDeadHold) { ++fMorto; continue; }

                if (t.visible)
                {
                    L.blocked = 0;
                    L.vis = true;
                }
                else if (L.vis)
                {
                    if (L.blocked < 0xFF) ++L.blocked;
                    if (L.blocked >= kVisHold) L.vis = false;
                }

                Snap::Ent& e = s.ents[out];

                e.head = t.scrHead;
                e.feet = t.scrFeet;
                e.dist = t.dist;
                e.health = t.health;
                e.team = t.team;
                e.weapon = t.weapon;
                e.visible = L.vis;
                e.aim = (aimTarget != nullptr && t.player == aimTarget);
                e.npc = t.npc;
                strncpy_s(e.name, t.name ? t.name : "?", _TRUNCATE);

                if (!e.visible) ++invis;
                ++out;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { Log("[crash] ESP::Tick loop"); }
        SDK::UnlockTargets();

        s.count = out;   // >>> o fix que fazia o ESP nao desenhar nada

        if (out == 0 && lastGood.count > 0 && ++s_emptyStreak <= kGraceFrames) {
            const uint32_t keepFrame = (uint32_t)s.frame;       // >>> FIX C4244
            s = lastGood;
            s.frame = keepFrame;
        }
        else if (out > 0)
            s_emptyStreak = 0;

        s_count.store(s.count, std::memory_order_relaxed);
        s_invis.store(invis, std::memory_order_relaxed);
        s_fLocal.store(fLocal, std::memory_order_relaxed);
        s_fMorto.store(fMorto, std::memory_order_relaxed);
        s_fRange.store(fRange, std::memory_order_relaxed);
        s_fTime.store(fTime, std::memory_order_relaxed);
        s_fNpc.store(fNpc, std::memory_order_relaxed);
        s_fCoord.store(fCoord, std::memory_order_relaxed);

        s_active.store(dst, std::memory_order_release);

        if (C.beatLog)
        {
            const uint32_t now = NowMs();
            if (now - g_lastEspLog >= kLogMs)
            {
                g_lastEspLog = now;
                Log("[esp] total=%d publicados=%d invis=%d | local=%d morto=%d range=%d time=%d npc=%d coord=%d "
                    "| w2s ok=%u semMat=%u atras=%u fora=%u | maxDist=%.0f teamCheck=%d npcCheck=%d",
                    total, s.count, invis, fLocal, fMorto, fRange, fTime, fNpc, fCoord,
                    SDK::g_w2sOk.load(std::memory_order_relaxed),
                    SDK::g_w2sSemMatriz.load(std::memory_order_relaxed),
                    SDK::g_w2sAtras.load(std::memory_order_relaxed),
                    SDK::g_w2sFora.load(std::memory_order_relaxed),
                    static_cast<double>(C.espMaxDist),
                    C.teamCheck ? 1 : 0, C.espNpc ? 1 : 0);
            }
        }
    }

    // ============================================================================
    //  Draw — thread do render
    // ============================================================================
    void Draw()
    {
        if (!C.esp) { s_holdValid = false; return; }

        const Snap* snap = Latest();
        if (!snap) return;

        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        if (!dl) return;

        float W, H;
        ScreenSize(snap, W, H);          // >>> nunca bails por tamanho

        const Snap* use = snap;
        bool holding = false;

        if (snap->count == 0)
        {
            if (s_holdValid && s_staleStreak < kGraceFrames) {
                use = &s_hold; holding = true;
                s_hold.frame = snap->frame;      // >>> preserva o frame corrente
            }
            ++s_staleStreak;
        }
        else if (Desenhaveis(snap, W, H) > 0)
        {
            s_hold = *snap;
            s_holdValid = true;
            s_staleStreak = 0;
        }
        else
        {
            if (s_holdValid && s_staleStreak < kGraceFrames) {
                use = &s_hold; holding = true;
                s_hold.frame = snap->frame;
            }
            ++s_staleStreak;
        }

        int desenhados = 0, fora = 0;

        __try
        {
            for (int i = 0; i < use->count; ++i)
            {
                const Snap::Ent& e = use->ents[i];

                float x0, y0, x1, y1;
                if (!RectOk(e, W, H, x0, y0, x1, y1)) { ++fora; continue; }

                const ImU32 boxCol = e.aim ? IM_COL32(255, 255, 60, 255)
                    : (e.visible ? Col(C.espBoxColor) : Col(C.noVisColor));
                const ImU32 txtCol = e.visible ? Col(C.visColor) : Col(C.noVisColor);
                const float thick = C.espThickness;

                if (C.espBox) {
                    dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), boxCol, 0.0f, 0, thick);
                    if (C.espFill)
                        dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1),
                            (boxCol & 0x00FFFFFFu) | 0x1E000000u);
                }

                if (C.espSnapline) {
                    const float cx = (x0 + x1) * 0.5f;
                    dl->AddLine(ImVec2(W * 0.5f, H - 2.0f), ImVec2(cx, y1), boxCol, 1.0f);
                }

                float textY = y0 - C.espFontSize - 1.0f + C.espOffY;

                if (C.espName) {
                    dl->AddText(ImVec2(x0, textY), txtCol, e.name);
                    textY -= C.espFontSize;
                }

                if (C.espDistance) {
                    char buf[24];
                    sprintf_s(buf, "%.0fm", static_cast<double>(e.dist));
                    dl->AddText(ImVec2(x0, y1 + 2.0f), txtCol, buf);
                }

                if (C.espHealth && e.health > 0.0f)
                {
                    float frac = e.health / 100.0f;
                    if (frac < 0.0f) frac = 0.0f;
                    if (frac > 1.0f) frac = 1.0f;

                    const float bh = y1 - y0;
                    dl->AddRectFilled(ImVec2(x0 - 5.0f, y0), ImVec2(x0 - 2.0f, y1),
                        IM_COL32(0, 0, 0, 140));
                    dl->AddRectFilled(ImVec2(x0 - 5.0f, y1 - bh * frac), ImVec2(x0 - 2.0f, y1),
                        ImColor::HSV(frac * 0.33f, 0.85f, 1.0f));

                    if (C.espHealthNum) {
                        char hpBuf[16];
                        sprintf_s(hpBuf, "%.0f", static_cast<double>(e.health));
                        dl->AddText(ImVec2(x0 - 18.0f, y1 - bh * frac - 6.0f),
                            IM_COL32(255, 255, 255, 200), hpBuf);
                    }
                }

                ++desenhados;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { Log("[crash] ESP::Draw loop"); }

        if (C.beatLog)
        {
            const uint32_t now = NowMs();
            if (now - g_lastDrawLog >= kLogMs)
            {
                g_lastDrawLog = now;

                // scr x vp: se divergirem, e' ESCALA, nao camera.
                GLint vp[4] = { 0, 0, 0, 0 };
                glGetIntegerv(GL_VIEWPORT, vp);

                Log("[esp-draw] pub=%d dr=%d fora=%d scr=%.0fx%.0f vp=%dx%d hold=%d "
                    "| proj=%d frame=%llu torn=%u glErr=%u",
                    use->count, desenhados, fora, W, H, vp[2], vp[3], holding ? 1 : 0,
                    (int)SDK::g_viewProjOk,
                    (unsigned long long)SDK::g_frameId,
                    SDK::g_fdTorn.load(std::memory_order_relaxed),
                    SDK::g_glErrTotal.load(std::memory_order_relaxed));
            }
        }
    }

    // ============================================================================
    const Snap* Latest()
    {
        int i = s_active.load(std::memory_order_acquire);
        if (i < 0 || i >= kSlots) i = 0;
        return &s_buf[i];
    }

    int Count() { return s_count.load(std::memory_order_relaxed); }
    int Invisiveis() { return s_invis.load(std::memory_order_relaxed); }

    void Reset()
    {
        s_holdValid = false;
        s_emptyStreak = 0;
        s_staleStreak = 0;
        for (auto& L : s_latch) L = Latch{};
        for (auto& b : s_buf) { b.count = 0; b.w = 0; b.h = 0; }
        s_count.store(0, std::memory_order_relaxed);
        s_invis.store(0, std::memory_order_relaxed);
    }

} // namespace ESP