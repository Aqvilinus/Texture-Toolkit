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

    void TextureToolkitUI::feed_overlay_mouse(HWND hwnd)
    {
        ImGuiIO &io = ImGui::GetIO();

        // Software cursor: the OS hardware cursor is unreliable in fullscreen (the game
        // hides it, e.g. NFS/DX11), so hide it and let ImGui draw the pointer instead.
        io.MouseDrawCursor = true;

        // Pin the OS cursor-display counter at exactly -1 (hidden). A plain
        // ShowCursor(FALSE) every frame would drive the counter unbounded-negative,
        // making it impossible for the game to re-show its cursor afterwards.
        int cursor_count = ShowCursor(FALSE);
        while (cursor_count >= 0)
            cursor_count = ShowCursor(FALSE);
        while (cursor_count < -1)
            cursor_count = ShowCursor(TRUE);

        // Poll the OS for cursor position and button state. Callers invoke this inside
        // the g_inside_imgui_render window, so the hooked GetAsyncKeyState passes through
        // to the real state instead of being masked. This is what makes clicks register
        // under exclusive-DirectInput games that post no window button messages.
        POINT p = {};
        if (GetCursorPos(&p) && hwnd != nullptr && ScreenToClient(hwnd, &p))
        {
            io.AddMousePosEvent(static_cast<float>(p.x), static_cast<float>(p.y));
        }

        io.AddMouseButtonEvent(0, (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0);
        io.AddMouseButtonEvent(1, (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0);
        io.AddMouseButtonEvent(2, (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0);
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

            ImGui::TextDisabled("Pipeline: DDS-only injection and DDS export (BC1-BC7 + uncompressed).");

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
                if (ImGui::Selectable(hash_str.c_str(), is_selected))
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

                // Column 4: Action (allow item overlap so button receives click priority over row selectable)
                ImGui::TableSetColumnIndex(4);
                ImGui::PushID(static_cast<int>(i));
                ImGui::SetNextItemAllowOverlap();
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

        // Detailed Texture Inspector Panel with Live Preview
        if (selected_texture_ptr != nullptr)
        {
            ImGui::Spacing();
            if (ImGui::CollapsingHeader("Selected Texture Inspector", ImGuiTreeNodeFlags_DefaultOpen))
            {
                uint64_t tex_handle = 0;
                if (selected_texture_ptr->replacement_handle != 0)
                {
                    tex_handle = selected_texture_ptr->replacement_handle;
                }
                else if (selected_texture_ptr->format_short.rfind("D3D9_", 0) == 0)
                {
                    tex_handle = selected_texture_ptr->resource_handle;
                }

                if (tex_handle != 0)
                {
                    float aspect = (selected_texture_ptr->height > 0) ? static_cast<float>(selected_texture_ptr->width) / static_cast<float>(selected_texture_ptr->height) : 1.0f;
                    float preview_h = 130.0f;
                    float preview_w = preview_h * aspect;
                    if (preview_w > 240.0f)
                    {
                        preview_w = 240.0f;
                        preview_h = preview_w / aspect;
                    }

                    ImGui::BeginGroup();
                    ImGui::Text("Live Preview:");
                    ImGui::Image(static_cast<ImTextureID>(tex_handle), ImVec2(preview_w, preview_h), ImVec2(0, 0), ImVec2(1, 1), ImVec4(1, 1, 1, 1), ImVec4(0.4f, 0.4f, 0.4f, 1.0f));
                    ImGui::EndGroup();
                    ImGui::SameLine(0.0f, 20.0f);
                }

                ImGui::BeginGroup();
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
                ImGui::EndGroup();
            }
        }

        // Status Bar
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f), "Status: %s", s_status_message.c_str());

        ImGui::End();
    }
}
