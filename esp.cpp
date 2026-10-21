// ============================================================================
//  esp.cpp — desenho do ESP (thread de render, sem chamada il2cpp)
//  + visual check: verde = visivel, vermelho = parede (SDK::HasLineOfSight)
//  + linha do centro da mira ate o inimigo (toggle C.espVisLine)
//  + snapline da base da tela ate os pes (toggle C.espSnapline)
//
//  >>> FIX 1: o lock g_targetCs era segurado durante TODO o desenho do ImGui.
//      Agora copia a lista sob o lock e solta ANTES de desenhar -> a thread do
//      jogo nao fica esperando o overlay.
//  >>> FIX 2: sem sprintf por alvo/frame. Formatador de inteiro na mao.
//
//  >>> FIX 3 (ESP piscando/sumindo): o local player NAO e' mais pre-requisito
//      para desenhar. Ele so' serve para (a) excluir o proprio corpo e
//      (b) o filtro de time — que ja' tem fallback. Com o gate antigo
//      ("if (!g_localPlayer) return"), qualquer frame em que o tick nao
//      conseguisse validar o local zerava o desenho inteiro, mesmo com a
//      lista de alvos completa e valida.
//  >>> FIX 4: o local e' lido UMA vez no inicio. Antes era lido tres vezes
//      (gate + comparacao por alvo) e o tick pode troca-lo no meio do loop,
//      misturando dois frames.
//  >>> FIX 5: contadores por motivo de skip + log de 1 Hz com TODOS os gates
//      (local / viewProj / ready). E' o que discrimina, em runtime, se o ESP
//      esta vazio por dado invalido ou por gate.
// ============================================================================
#include "features.h"
#include "sdk.h"
#include "hooks.h"
#include "common.h"

#include <imgui.h>
#include <cstring>
#include <cmath>
#include <cstdio>

namespace ESP {

    static inline ImU32 Col(const float c[4], float aMul = 1.f) {
        return IM_COL32((int)(c[0] * 255.f), (int)(c[1] * 255.f),
            (int)(c[2] * 255.f), (int)(c[3] * 255.f * aMul));
    }

    // inteiro -> string, sem sprintf
    static inline int AppendInt(char* out, int v) {
        char tmp[12]; int n = 0; int neg = 0;
        if (v < 0) { neg = 1; v = -v; }
        do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v);
        int p = 0;
        if (neg) out[p++] = '-';
        for (int i = 0; i < n; ++i) out[p + i] = tmp[n - 1 - i];
        out[p + n] = 0;
        return p + n;
    }

    void Draw() {
        SDK::dbg_drawn = 0;

        if (!C.esp) return;

        // >>> FIX 3/4: o local player nao e' mais pre-requisito. Lido UMA vez.
        Il2CppObject* local = SDK::g_localPlayer;

        // >>> FIX 1: snapshot sob o lock, desenho fora dele.
        static Target snap[MAX_TARGETS];
        int n = 0;
        SDK::LockTargets();
        n = SDK::g_targetCount;
        if (n > MAX_TARGETS) n = MAX_TARGETS;
        if (n > 0) memcpy(snap, SDK::g_targets, sizeof(Target) * (size_t)n);
        SDK::UnlockTargets();

        ImDrawList* dl = ImGui::GetBackgroundDrawList();

        ImVec2 disp = ImGui::GetIO().DisplaySize;
        float scrW = disp.x > 1.f ? disp.x : (float)SDK::ScreenW();
        float scrH = disp.y > 1.f ? disp.y : (float)SDK::ScreenH();

        ImU32 colBox = Col(C.espBoxColor);
        ImU32 colText = Col(C.espTextColor);
        ImU32 colSkel = Col(C.espSkelColor);
        ImU32 colVis = Col(C.visColor);
        ImU32 colNoVis = Col(C.noVisColor);
        float th = C.espThickness;
        if (th < 1.f) th = 1.f;

        int drawn = 0, visCount = 0, snapCount = 0;
        int skSelf = 0, skDead = 0, skNpc = 0, skTeam = 0, skOff = 0, skDist = 0;
        float cx = scrW * 0.5f;
        float cyMid = scrH * 0.5f;
        float cyBot = scrH;

        for (int i = 0; i < n; ++i) {
            Target& t = snap[i];

            if (!t.player) continue;
            if (local && t.player == local) { ++skSelf; continue; }
            if (!t.alive) { ++skDead; continue; }
            if (C.espNpc && t.npc) { ++skNpc; continue; }
            if ((C.teamCheck || C.aimTeam) && SDK::g_localTeam >= 0 &&
                t.team == SDK::g_localTeam) {
                ++skTeam; continue;
            }
            if (!t.onScreen) { ++skOff; continue; }
            if (C.espMaxDist > 1.f && t.dist > C.espMaxDist) { ++skDist; continue; }

            float top = t.scrHead.y + C.espOffY;
            float h = (t.scrFeet.y - top) * C.espScaleY;
            if (h < 4.f || h > 2000.f) continue;

            float w = h * 0.5f * (C.espBodyH / 1.8f);
            float x0 = t.scrHead.x - w * 0.5f;
            float y0 = top;
            float x1 = t.scrHead.x + w * 0.5f;
            float y1 = top + h;

            const bool vis = t.visible;
            if (vis) ++visCount;
            ImU32 colState = vis ? colVis : colNoVis;

            // ---- visual check: marcador + linha da mira ----
            if (C.espVisCheck) {
                dl->AddCircleFilled(ImVec2(t.scrHead.x, t.scrHead.y), 3.f, colState, 12);
                if (C.espVisLine)
                    dl->AddLine(ImVec2(cx, cyMid), ImVec2(t.scrHead.x, t.scrHead.y), colState, th);
            }

            // ---- snapline: SEMPRE da base da tela ate os pes ----
            if (C.espSnapline) {
                dl->AddLine(ImVec2(cx, cyBot), ImVec2(t.scrFeet.x, t.scrFeet.y),
                    C.espVisCheck ? colState : colBox, th);
                ++snapCount;
            }

            ImU32 boxCol = C.espVisCheck ? colState : colBox;

            if (C.espFill)
                dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), Col(C.espBoxColor, 0.20f));

            if (C.espBox)
                dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), boxCol, 0.f, 0, th);

            if (t.player == SDK::g_aimTarget)
                dl->AddRect(ImVec2(x0 - 2.f, y0 - 2.f), ImVec2(x1 + 2.f, y1 + 2.f),
                    colState, 0.f, 0, th + 1.f);

            if (C.espHealth) {
                float hp = t.health * 0.01f;
                if (!(hp >= 0.f && hp <= 1.f)) hp = 1.f;

                float bw = 4.f;
                float bx = x0 - bw - 2.f;
                dl->AddRectFilled(ImVec2(bx, y0), ImVec2(bx + bw, y1), IM_COL32(0, 0, 0, 180));
                dl->AddRectFilled(ImVec2(bx, y1 - h * hp), ImVec2(bx + bw, y1),
                    IM_COL32((int)(255.f * (1.f - hp)), (int)(255.f * hp), 0, 255));

                if (C.espHealthNum) {
                    // >>> FIX 2: sem sprintf
                    char hpBuf[16];
                    AppendInt(hpBuf, (int)(t.health + 0.5f));
                    dl->AddText(ImVec2(bx - 22.f, y0), colText, hpBuf);
                }
            }

            if (C.espSkeleton) {
                dl->AddLine(ImVec2(t.scrHead.x, y0), ImVec2(t.scrHead.x, y1), colSkel, th);
                float sy = y0 + h * 0.22f;
                dl->AddLine(ImVec2(x0, sy), ImVec2(x1, sy), colSkel, th);
                dl->AddLine(ImVec2(x0, sy), ImVec2(t.scrHead.x, y0 + h * 0.06f), colSkel, th);
                dl->AddLine(ImVec2(x1, sy), ImVec2(t.scrHead.x, y0 + h * 0.06f), colSkel, th);
            }

            if (C.espName && t.name[0]) {
                float tw = (float)strlen(t.name) * (C.espFontSize * 0.55f);
                dl->AddText(ImVec2(t.scrHead.x - tw * 0.5f, y0 - C.espFontSize - 2.f), colText, t.name);
            }

            if (C.espWeapon) {
                char wbuf[32];
                int wn = AppendInt(wbuf, t.weapon);
                wbuf[wn] = 0;
                dl->AddText(ImVec2(t.scrHead.x - 20.f, y1 + 2.f), colText, wbuf);
            }

            if (C.espDistance) {
                // >>> FIX 2: sem sprintf
                char dbuf[16];
                int dn = AppendInt(dbuf, (int)(t.dist + 0.5f));
                dbuf[dn++] = 'm'; dbuf[dn] = 0;
                dl->AddText(ImVec2(t.scrHead.x - 16.f, y1 + C.espFontSize + 2.f), colText, dbuf);
            }

            drawn++;
        }

        SDK::dbg_drawn = drawn;

        static ULONGLONG lastLog = 0;
        ULONGLONG now = GetTickCount64();
        if (now - lastLog >= 1000) {
            lastLog = now;
            // >>> FIX 5: todos os gates no mesmo lugar. Se 'desenhados' cai a
            //     zero, o motivo esta' explicito: local/viewProj/ready, ou a
            //     distribuicao dos 'skip'.
            Log("[esp] alvos=%d desenhados=%d visiveis=%d snap=%d/%d visCheck=%d los=%s "
                "| skip eu=%d morto=%d npc=%d time=%d fora=%d dist=%d "
                "| local=%d viewProj=%d ready=%d",
                n, drawn, visCount, snapCount, C.espSnapline ? 1 : 0,
                C.espVisCheck ? 1 : 0, SDK::LosReady() ? "OK" : "OFF",
                skSelf, skDead, skNpc, skTeam, skOff, skDist,
                local ? 1 : 0, (int)SDK::g_viewProjOk, (int)Hooks::g_sdkReady);
        }
    }

} // namespace ESP