
#include "features.h"
#include "sdk.h"
#include "hooks.h"
#include "common.h"

#include <windows.h>
#include <imgui.h>
#include <cmath>
#include <cstdio>
#include <algorithm>

namespace Misc {
    namespace {

        constexpr float kMargin = 12.0f;
        constexpr float kFpsAlpha = 0.10f;
        constexpr float kHitmarkerDuration = 0.25f;

        static ImU32 ColorU32(const float color[4], float alphaMultiplier = 1.0f)
        {
            if (!color)
                return IM_COL32(255, 255, 255, 255);

            auto channel = [](float v) -> int {
                if (!std::isfinite(v))
                    return 0;

                return static_cast<int>(
                    std::clamp(v, 0.0f, 1.0f) * 255.0f
                    );
                };

            return IM_COL32(
                channel(color[0]),
                channel(color[1]),
                channel(color[2]),
                channel(color[3] * std::clamp(alphaMultiplier, 0.0f, 1.0f))
            );
        }

        static bool ValidViewport(float width, float height)
        {
            return std::isfinite(width) &&
                std::isfinite(height) &&
                width > 0.0f &&
                height > 0.0f &&
                width <= 32768.0f &&
                height <= 32768.0f;
        }

        static void DrawWatermark(ImDrawList* drawList)
        {
            if (!drawList)
                return;

            char buffer[128]{};

            std::snprintf(
                buffer,
                sizeof(buffer),
                "bfi | alvos %d | desenhados %d",
                SDK::g_targetCount,
                SDK::dbg_drawn
            );

            drawList->AddText(
                ImVec2(kMargin, 10.0f),
                IM_COL32(255, 255, 255, 230),
                buffer
            );
        }

        static void DrawFps(ImDrawList* drawList)
        {
            if (!drawList)
                return;

            static ULONGLONG previousTime = 0;
            static float smoothedFps = 0.0f;

            const ULONGLONG now = GetTickCount64();

            if (previousTime != 0 && now > previousTime)
            {
                const ULONGLONG elapsed = now - previousTime;

                // Ignora intervalos anormais para evitar picos no indicador.
                if (elapsed > 0 && elapsed <= 10000)
                {
                    const float instantFps =
                        1000.0f / static_cast<float>(elapsed);

                    if (std::isfinite(instantFps))
                    {
                        if (smoothedFps <= 0.0f)
                            smoothedFps = instantFps;
                        else
                            smoothedFps =
                            smoothedFps * (1.0f - kFpsAlpha) +
                            instantFps * kFpsAlpha;
                    }
                }
            }

            previousTime = now;

            char buffer[32]{};
            std::snprintf(
                buffer,
                sizeof(buffer),
                "%.0f FPS",
                static_cast<double>(std::max(0.0f, smoothedFps))
            );

            drawList->AddText(
                ImVec2(kMargin, 30.0f),
                IM_COL32(180, 220, 180, 220),
                buffer
            );
        }

        static void DrawProfiler(ImDrawList* drawList)
        {
            if (!drawList || !C.showFpsGraph)
                return;

            char buffer[96]{};
            float y = 54.0f;

            std::snprintf(
                buffer,
                sizeof(buffer),
                "tick %.2f ms",
                static_cast<double>(Combat::g_tickMs)
            );

            drawList->AddText(
                ImVec2(kMargin, y),
                IM_COL32(255, 210, 120, 235),
                buffer
            );

            y += 16.0f;

            // Limita a quantidade de linhas exibidas.
            const int count = std::clamp(
                Combat::g_statsN,
                0,
                Combat::kMaxSteps
            );

            for (int i = 0; i < count; ++i)
            {
                const auto& stat = Combat::g_stats[i];

                char name[24]{};
                std::memcpy(name, stat.name, sizeof(name) - 1);
                name[sizeof(name) - 1] = '\0';

                const float ms =
                    std::isfinite(stat.ms) ? std::max(0.0f, stat.ms) : 0.0f;

                std::snprintf(
                    buffer,
                    sizeof(buffer),
                    "%-14s %6.2f ms",
                    name,
                    static_cast<double>(ms)
                );

                drawList->AddText(
                    ImVec2(kMargin, y),
                    IM_COL32(200, 220, 200, 225),
                    buffer
                );

                y += 14.0f;

                // Evita que o profiler desenhe indefinidamente fora da tela.
                if (y > 2000.0f)
                    break;
            }
        }

        static void DrawCrosshair(
            ImDrawList* drawList,
            float centerX,
            float centerY)
        {
            if (!drawList ||
                !std::isfinite(centerX) ||
                !std::isfinite(centerY))
                return;

            const float size = std::clamp(C.crosshairSize, 1.0f, 200.0f);
            constexpr float gap = 2.0f;

            const ImU32 color = ColorU32(C.crosshairColor);

            drawList->AddLine(
                ImVec2(centerX - size - gap, centerY),
                ImVec2(centerX - gap, centerY),
                color,
                1.5f
            );

            drawList->AddLine(
                ImVec2(centerX + gap, centerY),
                ImVec2(centerX + size + gap, centerY),
                color,
                1.5f
            );

            drawList->AddLine(
                ImVec2(centerX, centerY - size - gap),
                ImVec2(centerX, centerY - gap),
                color,
                1.5f
            );

            drawList->AddLine(
                ImVec2(centerX, centerY + gap),
                ImVec2(centerX, centerY + size + gap),
                color,
                1.5f
            );
        }

        static void DrawHitmarker(
            ImDrawList* drawList,
            float centerX,
            float centerY)
        {
            if (!drawList || !C.hitmarker)
                return;

            const float now =
                static_cast<float>(GetTickCount64()) / 1000.0f;

            const float elapsed = now - C.lastHitTime;

            if (!std::isfinite(elapsed) ||
                elapsed < 0.0f ||
                elapsed > kHitmarkerDuration)
                return;

            const float fade =
                std::clamp(1.0f - elapsed / kHitmarkerDuration, 0.0f, 1.0f);

            const float size = 8.0f + elapsed * 20.0f;

            const ImU32 color = IM_COL32(
                255, 255, 255,
                static_cast<int>(255.0f * fade)
            );

            constexpr float innerFactor = 0.4f;

            drawList->AddLine(
                ImVec2(centerX - size, centerY - size),
                ImVec2(centerX - size * innerFactor,
                    centerY - size * innerFactor),
                color,
                2.0f
            );

            drawList->AddLine(
                ImVec2(centerX + size, centerY - size),
                ImVec2(centerX + size * innerFactor,
                    centerY - size * innerFactor),
                color,
                2.0f
            );

            drawList->AddLine(
                ImVec2(centerX - size, centerY + size),
                ImVec2(centerX - size * innerFactor,
                    centerY + size * innerFactor),
                color,
                2.0f
            );

            drawList->AddLine(
                ImVec2(centerX + size, centerY + size),
                ImVec2(centerX + size * innerFactor,
                    centerY + size * innerFactor),
                color,
                2.0f
            );
        }

    } // namespace

    void Draw()
    {
        // Deve ser chamado durante um frame válido do ImGui.
        ImDrawList* drawList = ImGui::GetBackgroundDrawList();

        if (!drawList)
            return;

        const float width = static_cast<float>(SDK::ScreenW());
        const float height = static_cast<float>(SDK::ScreenH());

        if (!ValidViewport(width, height))
            return;

        const float centerX = width * 0.5f;
        const float centerY = height * 0.5f;

        if (C.watermark)
        {
            DrawWatermark(drawList);

            if (C.fps)
                DrawFps(drawList);
        }

        DrawProfiler(drawList);

        if (C.crosshair)
            DrawCrosshair(drawList, centerX, centerY);

        DrawHitmarker(drawList, centerX, centerY);
    }

} // namespace Misc
