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

    // Colors used across the panel.
    static const ImVec4 kColInjected(0.35f, 0.85f, 0.45f, 1.0f);
    static const ImVec4 kColDumped(0.35f, 0.70f, 1.00f, 1.0f);
    static const ImVec4 kColOriginal(0.70f, 0.70f, 0.70f, 1.0f);
    static const ImVec4 kColMuted(0.60f, 0.60f, 0.62f, 1.0f);

    static const ImVec4 &status_color(TextureStatus s)
    {
        if (s == TextureStatus::INJECTED) return kColInjected;
        if (s == TextureStatus::DUMPED)   return kColDumped;
        return kColOriginal;
    }

    static const char *status_label(TextureStatus s)
    {
        if (s == TextureStatus::INJECTED) return "Injected";
        if (s == TextureStatus::DUMPED)   return "Dumped";
        return "Original";
    }

    // Draws one labeled thumbnail. handle is a native texture id (IDirect3DBaseTexture9*
    // on DX9, ID3D11ShaderResourceView* on DX11). 0 draws a placeholder.
    static void DrawThumbnail(const char *label, uint64_t handle, uint32_t w, uint32_t h, const char *empty_hint)
    {
        const float box = 150.0f;
        ImGui::BeginGroup();
        ImGui::TextUnformatted(label);

        if (handle != 0)
        {
            float aspect = (h > 0) ? static_cast<float>(w) / static_cast<float>(h) : 1.0f;
            float pw = box, ph = box;
            if (aspect >= 1.0f) ph = box / aspect;
            else                pw = box * aspect;

            ImGui::ImageWithBg(ImTextureRef(static_cast<ImTextureID>(handle)),
                               ImVec2(pw, ph), ImVec2(0, 0), ImVec2(1, 1),
                               ImVec4(0.12f, 0.12f, 0.14f, 1.0f), ImVec4(1, 1, 1, 1));
        }
        else
        {
            ImVec2 p0 = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2(box, box));
            ImDrawList *dl = ImGui::GetWindowDrawList();
            dl->AddRect(p0, ImVec2(p0.x + box, p0.y + box), ImGui::GetColorU32(ImGuiCol_Border));
            ImVec2 ts = ImGui::CalcTextSize(empty_hint, nullptr, false, box - 16.0f);
            dl->AddText(nullptr, 0.0f, ImVec2(p0.x + (box - ts.x) * 0.5f, p0.y + (box - ts.y) * 0.5f),
                        ImGui::GetColorU32(kColMuted), empty_hint);
        }
        ImGui::EndGroup();
    }

    static void MetaRow(const char *label, const char *value)
    {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextColored(kColMuted, "%s", label);
        ImGui::TableSetColumnIndex(1);
        ImGui::TextUnformatted(value);
    }

    static void DrawInspector(TextureManager &tm, const TextureDetails &tex)
    {
        ImGui::SeparatorText("Inspector");

        std::string hash = "0x" + tex.hash_hex;
        ImGui::TextColored(status_color(tex.status), "%s", hash.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Copy"))
        {
            ImGui::SetClipboardText(hash.c_str());
            SetStatusMessage("Copied " + hash + " to clipboard.");
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Dump"))
        {
            tm.request_dump(tex.hash);
            SetStatusMessage("Queued dump for " + hash + " to TT/dump.");
        }

        ImGui::Spacing();

        // Previews: the live original (pinned when the texture is bound this frame) and
        // the injected replacement (if one is active).
        uint64_t original_handle = tm.get_original_preview_handle();
        DrawThumbnail("Original", original_handle, tex.width, tex.height,
                      original_handle ? "" : "Not in current scene");
        if (tex.replacement_handle != 0)
        {
            ImGui::SameLine(0.0f, 16.0f);
            DrawThumbnail("Replacement", tex.replacement_handle, tex.width, tex.height, "");
        }

        ImGui::Spacing();

        // Metadata grid.
        if (ImGui::BeginTable("meta", 2, ImGuiTableFlags_SizingFixedFit))
        {
            char buf[128];

            std::snprintf(buf, sizeof(buf), "%u x %u", tex.width, tex.height);
            MetaRow("Dimensions", buf);

            std::snprintf(buf, sizeof(buf), "%u", tex.mip_levels);
            MetaRow("Mip levels", buf);

            std::snprintf(buf, sizeof(buf), "%u  (%s)", tex.format_id, tex.format_str.c_str());
            MetaRow("Format", buf);

            MetaRow("Compressed", tex.is_compressed ? "Yes" : "No");
            MetaRow("sRGB", tex.is_srgb ? "Yes" : "No");

            if (tex.is_dx11)
            {
                std::snprintf(buf, sizeof(buf), "%u", tex.array_size);
                MetaRow("Array size", buf);
                std::snprintf(buf, sizeof(buf), "0x%04X", tex.bind_flags);
                MetaRow("Bind flags", buf);
                std::snprintf(buf, sizeof(buf), "%u", tex.usage);
                MetaRow("Usage", buf);
                std::snprintf(buf, sizeof(buf), "0x%02X", tex.misc_flags);
                MetaRow("Misc flags", buf);
            }

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextColored(kColMuted, "Status");
            ImGui::TableSetColumnIndex(1);
            ImGui::TextColored(status_color(tex.status), "%s", status_label(tex.status));

            ImGui::EndTable();
        }

        if (tex.status == TextureStatus::INJECTED && !tex.filepath_injected.empty())
        {
            ImGui::TextColored(kColMuted, "File");
            ImGui::SameLine();
            ImGui::TextWrapped("%s", tex.filepath_injected.c_str());
        }
        else if (tex.status == TextureStatus::DUMPED && !tex.filepath_dumped.empty())
        {
            ImGui::TextColored(kColMuted, "File");
            ImGui::SameLine();
            ImGui::TextWrapped("%s", tex.filepath_dumped.c_str());
        }
    }

    void TextureToolkitUI::draw_ui(void *runtime)
    {
        OSDBanner::get().draw_osd();

        if (!s_show_ui)
        {
            // Drop the pinned preview reference while the panel is hidden.
            TextureManager::get().set_preview_target(0);
            return;
        }

        TextureManager &tm = TextureManager::get();

        ImVec2 display_size = ImGui::GetIO().DisplaySize;
        if (display_size.x > 0 && display_size.y > 0)
        {
            ImGui::SetNextWindowPos(ImVec2(display_size.x * 0.5f, display_size.y * 0.5f), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
        }

        ImGui::SetNextWindowSize(ImVec2(920, 620), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Texture Toolkit  (INSERT to close)", &s_show_ui))
        {
            ImGui::End();
            return;
        }

        // Toolbar: toggles and folder shortcuts.
        ImGui::Checkbox("Injection", &tm.enable_injection);
        ImGui::SameLine();
        ImGui::Checkbox("Auto-dump", &tm.auto_dump);
        ImGui::SameLine();
        ImGui::Checkbox("Skip < 16px", &tm.filter_small_textures);
        ImGui::SameLine();
        ImGui::Checkbox("Scene only", &tm.show_current_frame_only);

        ImGui::SameLine(0.0f, 24.0f);
        if (ImGui::Button("Reload injects"))
        {
            tm.rescan_injected();
            SetStatusMessage("Rescanned TT/inject for DDS replacements.");
        }
        ImGui::SameLine();
        if (ImGui::Button("Dump folder"))
            OpenDirectory(tm.get_dump_dir());
        ImGui::SameLine();
        if (ImGui::Button("Inject folder"))
            OpenDirectory(tm.get_inject_dir());

        // Counts.
        std::vector<TextureDetails> textures = tm.get_active_textures();
        size_t injected = 0, dumped = 0, original = 0;
        for (const auto &t : textures)
        {
            if (t.status == TextureStatus::INJECTED) injected++;
            else if (t.status == TextureStatus::DUMPED) dumped++;
            else original++;
        }

        ImGui::Text("%zu tracked", textures.size());
        ImGui::SameLine();
        ImGui::TextColored(kColInjected, "  %zu injected", injected);
        ImGui::SameLine();
        ImGui::TextColored(kColDumped, "  %zu dumped", dumped);
        ImGui::SameLine();
        ImGui::TextColored(kColOriginal, "  %zu original", original);

        // Filter.
        ImGui::SetNextItemWidth(260.0f);
        ImGui::InputTextWithHint("##filter", "Filter by hash, size or format", s_filter_buf, sizeof(s_filter_buf));

        std::string filter_str = s_filter_buf;
        std::transform(filter_str.begin(), filter_str.end(), filter_str.begin(), ::tolower);

        // Two panes: list on the left, inspector on the right.
        const float inspector_w = 360.0f;
        float avail_h = ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing();

        ImGui::BeginChild("list", ImVec2(-inspector_w, avail_h), ImGuiChildFlags_Borders);
        {
            ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                    ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV |
                                    ImGuiTableFlags_SizingStretchProp;

            if (ImGui::BeginTable("textures", 5, flags))
            {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("Hash", ImGuiTableColumnFlags_WidthFixed, 90.0f);
                ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 82.0f);
                ImGui::TableSetupColumn("Mips", ImGuiTableColumnFlags_WidthFixed, 38.0f);
                ImGui::TableSetupColumn("Format", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 70.0f);
                ImGui::TableHeadersRow();

                for (const auto &tex : textures)
                {
                    if (!filter_str.empty())
                    {
                        std::string h = tex.hash_hex;
                        std::transform(h.begin(), h.end(), h.begin(), ::tolower);
                        std::string dim = std::to_string(tex.width) + "x" + std::to_string(tex.height);
                        std::string fmt = tex.format_short;
                        std::transform(fmt.begin(), fmt.end(), fmt.begin(), ::tolower);
                        if (h.find(filter_str) == std::string::npos &&
                            dim.find(filter_str) == std::string::npos &&
                            fmt.find(filter_str) == std::string::npos)
                            continue;
                    }

                    ImGui::TableNextRow();

                    ImGui::TableSetColumnIndex(0);
                    std::string hash_str = "0x" + tex.hash_hex;
                    bool selected = (s_selected_texture_hash == tex.hash);
                    if (ImGui::Selectable(hash_str.c_str(), selected, ImGuiSelectableFlags_SpanAllColumns))
                        s_selected_texture_hash = tex.hash;

                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%ux%u", tex.width, tex.height);

                    ImGui::TableSetColumnIndex(2);
                    ImGui::Text("%u", tex.mip_levels);

                    ImGui::TableSetColumnIndex(3);
                    // format_short is "DX11_"/"D3D9_" + name; show the name part only.
                    size_t us = tex.format_short.find('_');
                    ImGui::TextUnformatted(us == std::string::npos ? tex.format_short.c_str()
                                                                   : tex.format_short.c_str() + us + 1);

                    ImGui::TableSetColumnIndex(4);
                    ImGui::TextColored(status_color(tex.status), "%s", status_label(tex.status));
                }
                ImGui::EndTable();
            }
        }
        ImGui::EndChild();

        ImGui::SameLine();

        ImGui::BeginChild("inspector", ImVec2(0, avail_h), ImGuiChildFlags_Borders);
        {
            // Keep the live-preview capture aimed at the current selection.
            tm.set_preview_target(s_selected_texture_hash);

            const TextureDetails *sel = nullptr;
            for (const auto &t : textures)
                if (t.hash == s_selected_texture_hash) { sel = &t; break; }

            if (sel != nullptr)
                DrawInspector(tm, *sel);
            else
                ImGui::TextColored(kColMuted, "Select a texture to inspect it.");
        }
        ImGui::EndChild();

        ImGui::TextColored(kColMuted, "%s", s_status_message.c_str());

        ImGui::End();
    }
}
