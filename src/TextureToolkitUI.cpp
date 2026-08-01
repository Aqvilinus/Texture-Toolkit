#include "TextureToolkitUI.h"
#include "TextureManager.h"
#include "OSDBanner.h"
#include "Logger.h"
#include <windows.h>
#include <shellapi.h>
#include <vector>
#include <string>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <imgui.h>

namespace TextureToolkit
{
    bool TextureToolkitUI::s_show_ui = false; // Default hidden; INSERT key toggles it
    static std::string s_status_message = "Ready";
    static uint32_t s_selected_texture_hash = 0;
    static char s_filter_buf[64] = "";

    static void SetStatusMessage(const std::string &msg)
    {
        s_status_message = msg;
        Logger::get().info("[UI] " + msg);
    }

    static void OpenDirectory(const std::filesystem::path &dir_path)
    {
        std::error_code ec;
        std::filesystem::create_directories(dir_path, ec);
        ShellExecuteW(nullptr, L"open", dir_path.c_str(), nullptr, nullptr, SW_SHOW);
    }

    void TextureToolkitUI::draw_ui(void *runtime)
    {
        // 1. Draw Sleek SpecialK-style OSD Notification Banner
        OSDBanner::get().draw_osd();

        // 2. If Main UI Control Panel is toggled off, exit
        if (!s_show_ui)
            return;

        TextureManager &tm = TextureManager::get();

        ImVec2 display_size = ImGui::GetIO().DisplaySize;
        if (display_size.x > 0 && display_size.y > 0)
        {
            ImGui::SetNextWindowPos(ImVec2(display_size.x * 0.5f, display_size.y * 0.5f), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
        }

        ImGui::SetNextWindowSize(ImVec2(800, 600), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Texture Toolkit v1.0.0 (Press INSERT to Hide)", &s_show_ui, ImGuiWindowFlags_MenuBar))
        {
            ImGui::End();
            return;
        }

        // Menu Bar
        if (ImGui::BeginMenuBar())
        {
            if (ImGui::BeginMenu("File"))
            {
                if (ImGui::MenuItem("Open Dump Folder"))
                {
                    OpenDirectory(tm.get_dump_dir());
                }
                if (ImGui::MenuItem("Open Inject Folder"))
                {
                    OpenDirectory(tm.get_inject_dir());
                }
                if (ImGui::MenuItem("Hot-Reload Injected Textures"))
                {
                    tm.rescan_injected();
                    SetStatusMessage("Scanned replacement textures in TT/inject");
                }
                ImGui::EndMenu();
            }
            ImGui::EndMenuBar();
        }

        // Configuration & Status
        if (ImGui::CollapsingHeader("Pipeline Configuration & Shortcuts", ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Checkbox("Enable Texture Injection", &tm.enable_injection);
            ImGui::SameLine();
            ImGui::Checkbox("Auto-Dump Textures on Load", &tm.auto_dump);
            ImGui::SameLine();
            ImGui::Checkbox("Filter Small Textures (< 16x16)", &tm.filter_small_textures);

            ImGui::Checkbox("Show Only Active Scene Textures", &tm.show_current_frame_only);

            // Format combos
            const char* dump_formats[] = { "DDS Only (Recommended)", "PNG Only", "Both DDS + PNG" };
            int current_dump_fmt = static_cast<int>(tm.dump_format_mode);
            if (ImGui::Combo("Dump Format", &current_dump_fmt, dump_formats, IM_ARRAYSIZE(dump_formats)))
            {
                tm.dump_format_mode = static_cast<DumpFormatMode>(current_dump_fmt);
            }

            ImGui::SameLine();

            const char* load_formats[] = { "DDS + PNG (Prioritize DDS)", "DDS Only", "PNG Only" };
            int current_load_fmt = static_cast<int>(tm.load_format_mode);
            if (ImGui::Combo("Load Format", &current_load_fmt, load_formats, IM_ARRAYSIZE(load_formats)))
            {
                tm.load_format_mode = static_cast<LoadFormatMode>(current_load_fmt);
            }

            if (ImGui::Button("Hot-Reload Textures"))
            {
                tm.rescan_injected();
                SetStatusMessage("User triggered texture hot-reload.");
            }
            ImGui::SameLine();
            if (ImGui::Button("Open Dump Folder"))
            {
                OpenDirectory(tm.get_dump_dir());
            }
            ImGui::SameLine();
            if (ImGui::Button("Open Inject Folder"))
            {
                OpenDirectory(tm.get_inject_dir());
            }
        }

        ImGui::Separator();

        // Active Textures Table & Search
        std::vector<TextureDetails> active_textures = tm.get_active_textures();
        size_t injected_count = 0;
        size_t dumped_count = 0;
        size_t original_count = 0;

        for (const auto &tex : active_textures)
        {
            if (tex.status == TextureStatus::INJECTED) injected_count++;
            else if (tex.status == TextureStatus::DUMPED) dumped_count++;
            else original_count++;
        }

        ImGui::Text("Tracked Textures (%zu total): ", active_textures.size());
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.3f, 1.0f), "Injected: %zu ", injected_count);
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.3f, 0.8f, 1.0f, 1.0f), "Dumped: %zu ", dumped_count);
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Original: %zu", original_count);

        ImGui::SetNextItemWidth(250.0f);
        ImGui::InputText("Filter Search", s_filter_buf, sizeof(s_filter_buf));

        std::string filter_str = s_filter_buf;
        std::transform(filter_str.begin(), filter_str.end(), filter_str.begin(), ::tolower);

        std::vector<TextureDetails> filtered_textures;
        filtered_textures.reserve(active_textures.size());
        for (const auto &tex : active_textures)
        {
            if (!filter_str.empty())
            {
                std::string hash_hex = tex.hash_hex;
                std::transform(hash_hex.begin(), hash_hex.end(), hash_hex.begin(), ::tolower);

                std::string dim_str = std::to_string(tex.width) + "x" + std::to_string(tex.height);

                if (hash_hex.find(filter_str) == std::string::npos &&
                    dim_str.find(filter_str) == std::string::npos &&
                    tex.format_short.find(filter_str) == std::string::npos)
                {
                    continue;
                }
            }
            filtered_textures.push_back(tex);
        }

        ImGui::BeginChild("TextureListTableChild", ImVec2(0, 200), true);
        static ImGuiTableFlags table_flags = ImGuiTableFlags_Resizable | ImGuiTableFlags_Reorderable |
                                             ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersOuter | ImGuiTableFlags_ScrollY;

        const TextureDetails* selected_texture_ptr = nullptr;

        if (ImGui::BeginTable("TextureTable", 5, table_flags))
        {
            ImGui::TableSetupColumn("Hash", ImGuiTableColumnFlags_WidthFixed, 110.0f);
            ImGui::TableSetupColumn("Dimensions", ImGuiTableColumnFlags_WidthFixed, 100.0f);
            ImGui::TableSetupColumn("Format", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 110.0f);
            ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, 80.0f);
            ImGui::TableHeadersRow();

            for (size_t i = 0; i < filtered_textures.size(); ++i)
            {
                const auto &tex = filtered_textures[i];
                if (s_selected_texture_hash == tex.hash)
                {
                    selected_texture_ptr = &tex;
                }

                ImGui::TableNextRow();

                // Column 0: Hash
                ImGui::TableSetColumnIndex(0);
                std::string hash_str = "0x" + tex.hash_hex;
                bool is_selected = (s_selected_texture_hash == tex.hash);
                if (ImGui::Selectable(hash_str.c_str(), is_selected, ImGuiSelectableFlags_SpanAllColumns))
                {
                    s_selected_texture_hash = tex.hash;
                }

                // Column 1: Dimensions
                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%u x %u", tex.width, tex.height);

                // Column 2: Format
                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(tex.format_short.c_str());

                // Column 3: Status
                ImGui::TableSetColumnIndex(3);
                if (tex.status == TextureStatus::INJECTED)
                {
                    ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.3f, 1.0f), "[INJECTED]");
                }
                else if (tex.status == TextureStatus::DUMPED)
                {
                    ImGui::TextColored(ImVec4(0.3f, 0.8f, 1.0f, 1.0f), "[DUMPED]");
                }
                else
                {
                    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "[ORIGINAL]");
                }

                // Column 4: Action
                ImGui::TableSetColumnIndex(4);
                ImGui::PushID(static_cast<int>(i));
                if (ImGui::Button("Dump"))
                {
                    s_selected_texture_hash = tex.hash;
                    tm.request_dump(tex.hash);
                    SetStatusMessage("Queued dump for texture 0x" + tex.hash_hex + " to TT/dump");
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::EndChild();

        // Detailed Texture Inspector Panel
        if (selected_texture_ptr != nullptr)
        {
            ImGui::Spacing();
            if (ImGui::CollapsingHeader("Selected Texture Inspector", ImGuiTreeNodeFlags_DefaultOpen))
            {
                ImGui::Text("Hash: 0x%s", selected_texture_ptr->hash_hex.c_str());
                ImGui::SameLine();
                if (ImGui::Button("Copy Hash"))
                {
                    std::string full_hash = "0x" + selected_texture_ptr->hash_hex;
                    ImGui::SetClipboardText(full_hash.c_str());
                    SetStatusMessage("Copied " + full_hash + " to clipboard.");
                }

                ImGui::Text("Dimensions: %u x %u", selected_texture_ptr->width, selected_texture_ptr->height);
                ImGui::Text("Format: %s", selected_texture_ptr->format_str.c_str());

                ImGui::Text("Status: ");
                ImGui::SameLine();
                if (selected_texture_ptr->status == TextureStatus::INJECTED)
                {
                    ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.3f, 1.0f), "INJECTED (%s)", selected_texture_ptr->filepath_injected.c_str());
                }
                else if (selected_texture_ptr->status == TextureStatus::DUMPED)
                {
                    ImGui::TextColored(ImVec4(0.3f, 0.8f, 1.0f, 1.0f), "DUMPED (%s)", selected_texture_ptr->filepath_dumped.c_str());
                }
                else
                {
                    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "ORIGINAL (No replacement file active)");
                }

                if (ImGui::Button("Dump Selected Texture"))
                {
                    tm.request_dump(selected_texture_ptr->hash);
                    SetStatusMessage("Queued dump for texture 0x" + selected_texture_ptr->hash_hex);
                }
            }
        }

        // Status Bar
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f), "Status: %s", s_status_message.c_str());

        ImGui::End();
    }
}
