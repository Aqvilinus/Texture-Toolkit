#include "OSDBanner.h"
#include "Config.h"
#include "TextureManager.h"
#include <imgui.h>
#include <algorithm>

namespace TextureToolkit
{
    OSDBanner &OSDBanner::get()
    {
        static OSDBanner instance;
        return instance;
    }

    OSDBanner::OSDBanner()
    {
        reset();
    }

    void OSDBanner::reset()
    {
        m_start_time = std::chrono::steady_clock::now();
        m_active = true;
    }

    void OSDBanner::draw_osd()
    {
        const auto &config = ConfigManager::get().get_config();
        if (!config.show_osd_banner || !m_active)
            return;

        auto now = std::chrono::steady_clock::now();
        float elapsed = std::chrono::duration<float>(now - m_start_time).count();

        float duration = config.osd_duration_seconds;
        if (elapsed >= duration)
        {
            m_active = false;
            return;
        }

        float alpha = 1.0f;
        if (elapsed < 0.5f)
        {
            alpha = elapsed / 0.5f; // Fade in
        }
        else if (elapsed > duration - 1.5f)
        {
            alpha = (duration - elapsed) / 1.5f; // Fade out
        }
        alpha = (std::max)(0.0f, (std::min)(1.0f, alpha));

        ImVec2 display_size = ImGui::GetIO().DisplaySize;
        if (display_size.x <= 0.0f || display_size.y <= 0.0f)
            return;

        // Position at Center Bottom (display height minus 80px margin)
        ImVec2 pos = ImVec2(display_size.x * 0.5f, display_size.y - 80.0f);

        ImGui::SetNextWindowPos(pos, ImGuiCond_Always, ImVec2(0.5f, 1.0f));
        ImGui::SetNextWindowBgAlpha(0.92f * alpha);

        ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs;

        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.95f, 0.80f, 0.20f, alpha));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.5f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);

        if (ImGui::Begin("##TextureToolkitOSD", nullptr, flags))
        {
            ImGui::TextColored(ImVec4(0.95f, 0.80f, 0.20f, alpha), "Texture Toolkit v1.0.0");
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.70f, 0.70f, 0.70f, alpha), "by BadassBaboon");

            ImGui::Spacing();
            ImGui::TextColored(ImVec4(1.00f, 1.00f, 1.00f, alpha), "Press ");
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.20f, 1.00f, 0.40f, alpha), "INSERT");
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.00f, 1.00f, 1.00f, alpha), " to toggle Texture Toolkit Control Panel");

            ImGui::TextColored(ImVec4(0.60f, 0.80f, 1.00f, alpha), "Injection: %s | Auto-Dump: %s",
                TextureManager::get().enable_injection ? "Active" : "Disabled",
                TextureManager::get().auto_dump ? "Enabled" : "Disabled");
        }
        ImGui::End();

        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
    }
}
