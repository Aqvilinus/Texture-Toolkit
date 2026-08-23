#include "ui/overlay.h"
#include "texture/texture_manager.h"
#include "ui/osd_banner.h"
#include "core/config.h"
#include "input/input_hook.h"
#include "core/logger.h"
#include <windows.h>
#include <shellapi.h>
#include <vector>
#include <string>
#include <algorithm>
#include <ranges>
#include <string_view>
#include <imgui.h>

namespace TextureToolkit
{
    std::atomic<bool> TextureToolkitUI::s_show_ui{false}; // Default hidden; the configured hotkey toggles it
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

        // Draw our own pointer and let the Win32 backend hide the OS one. It does that with
        // SetCursor(nullptr) from NewFrame and re-asserts it on WM_SETCURSOR -- no counter to
        // wind or unwind, and clearing this flag hands the cursor straight back to the game.
        io.MouseDrawCursor = true;

        // Poll the OS for cursor position and button state, reading through the trampoline so
        // our own masking hook does not answer zero. This is what makes clicks register under
        // exclusive-DirectInput games that post no window button messages.
        POINT p = {};
        if (GetCursorPos(&p) && hwnd != nullptr && ScreenToClient(hwnd, &p))
        {
            io.AddMousePosEvent(static_cast<float>(p.x), static_cast<float>(p.y));
        }

        io.AddMouseButtonEvent(0, (InputHook::real_async_key_state(VK_LBUTTON) & 0x8000) != 0);
        io.AddMouseButtonEvent(1, (InputHook::real_async_key_state(VK_RBUTTON) & 0x8000) != 0);
        io.AddMouseButtonEvent(2, (InputHook::real_async_key_state(VK_MBUTTON) & 0x8000) != 0);

        // Feed the [ and ] keys here too (same reliable point as the mouse poll, before
        // NewFrame). The UI reads ImGui's key state to step through the texture list; the
        // poll in draw_ui itself returns nothing on some titles.
        io.AddKeyEvent(ImGuiKey_LeftBracket,  (InputHook::real_async_key_state(VK_OEM_4) & 0x8000) != 0);
        io.AddKeyEvent(ImGuiKey_RightBracket, (InputHook::real_async_key_state(VK_OEM_6) & 0x8000) != 0);
    }

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
    static void DrawThumbnail(const char *label, uint64_t handle, uint32_t w, uint32_t h, float box, const char *empty_hint)
    {
        ImGui::BeginGroup();
        ImGui::Text("%s  %ux%u", label, w, h);

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
        ImGui::TextWrapped("%s", value);
    }

    // The filter runs over every tracked texture every frame; lowercasing copies of three strings
    // per row was the only allocation left on that path. `needle` is already lowercase.
    static bool contains_nocase(std::string_view hay, std::string_view needle)
    {
        return needle.empty() ||
               !std::ranges::search(hay, needle, [](char a, char b) {
                    return static_cast<char>(std::tolower(static_cast<unsigned char>(a))) == b;
               }).empty();
    }

    static void DrawInspector(TextureManagerBase &tm, const TextureDetails &tex)
    {
        ImGui::SeparatorText("Inspector");

        std::string hash = "0x" + tex.hash_hex;
        ImGui::TextColored(status_color(tex.status), "%s", hash.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Copy hash"))
        {
            ImGui::SetClipboardText(hash.c_str());
            SetStatusMessage("Copied " + hash + " to clipboard.");
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Dump"))
        {
            if (tm.request_dump(tex.hash))
                SetStatusMessage("Dumped " + hash + " to TT/dump.");
            else
                SetStatusMessage("Could not dump " + hash + ". For default-pool textures, turn on Auto-dump (see log).");
        }

        ImGui::Spacing();

        // One preview, chosen by what is available and useful:
        //   injected  -> the replacement we injected
        //   in scene  -> the live original the game is binding
        //   dumped    -> the dumped .dds loaded from disk
        uint64_t handle = 0;
        const char *src = "Original";
        uint32_t pw = tex.width, ph = tex.height;
        if (tex.replacement_handle != 0)
        {
            handle = tex.replacement_handle;
            src = "Replacement";
            pw = tex.repl_width;
            ph = tex.repl_height;
        }
        else if ((handle = tm.get_original_preview_handle()) != 0)
        {
            src = "Original (in scene)";
        }
        else if (!tex.filepath_dumped.empty())
        {
            handle = tm.get_file_preview_handle(tex.hash, tex.filepath_dumped);
            if (handle != 0)
                src = "Original (from dump)";
        }

        float box = (std::min)(ImGui::GetContentRegionAvail().x, 256.0f);
        DrawThumbnail(src, handle, pw, ph, box, "Not in scene, no dump");

        ImGui::Spacing();

        // Metadata grid: fixed label column, stretching value column so a long format
        // name does not force the whole panel wider.
        if (ImGui::BeginTable("meta", 2, ImGuiTableFlags_None))
        {
            ImGui::TableSetupColumn("k", ImGuiTableColumnFlags_WidthFixed, 84.0f);
            ImGui::TableSetupColumn("v", ImGuiTableColumnFlags_WidthStretch);

            char buf[128];

            std::snprintf(buf, sizeof(buf), "%u x %u", tex.width, tex.height);
            MetaRow("Dimensions", buf);

            if (tex.replacement_handle != 0)
            {
                std::snprintf(buf, sizeof(buf), "%u x %u", tex.repl_width, tex.repl_height);
                MetaRow("Replacement", buf);
            }

            std::snprintf(buf, sizeof(buf), "%u", tex.mip_levels);
            MetaRow("Mip levels", buf);

            std::snprintf(buf, sizeof(buf), "%.3f MiB", tex.data_size / (1024.0 * 1024.0));
            MetaRow("Data size", buf);

            std::snprintf(buf, sizeof(buf), "%u  (%s)", tex.format_id, tex.format_str.c_str());
            MetaRow("Format", buf);

            if (tex.view_format_id != 0 && tex.view_format_id != tex.format_id)
            {
                std::snprintf(buf, sizeof(buf), "%u  (%s)", tex.view_format_id, tex.view_format_str.c_str());
                MetaRow("Sampled as", buf);
            }

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
                std::snprintf(buf, sizeof(buf), "0x%02X", tex.cpu_access);
                MetaRow("CPU access", buf);
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

    void TextureToolkitUI::draw_ui()
    {
        OSDBanner::get().draw_osd();

        if (!s_show_ui)
        {
                TextureManagerBase::active()->set_preview_target(0);
            return;
        }

        TextureManagerBase *tm_ptr = TextureManagerBase::active();
        if (tm_ptr == nullptr)
            return;
        TextureManagerBase &tm = *tm_ptr;

        ImVec2 display_size = ImGui::GetIO().DisplaySize;
        if (display_size.x > 0 && display_size.y > 0)
        {
            ImGui::SetNextWindowPos(ImVec2(display_size.x * 0.5f, display_size.y * 0.5f), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
        }

        ImGui::SetNextWindowSize(ImVec2(920, 620), ImGuiCond_FirstUseEver);
        const std::string title = "Texture Toolkit  (" + hotkey_name(ConfigManager::get().get_config().hotkey) + " to close)";

        // ImGui writes through this to close the window, so it gets a plain bool and the result is
        // published back to the atomic the input side reads.
        bool open = is_visible();
        const bool drawing = ImGui::Begin(title.c_str(), &open);
        set_visible(open);

        if (!drawing)
        {
            ImGui::End();
            return;
        }

        // Toolbar: toggles and folder shortcuts. Toggling any option persists to the INI.
        bool changed = false;
        changed |= ImGui::Checkbox("Injection", &tm.enable_injection);
        ImGui::SetItemTooltip("Replace textures that have a matching DDS in TT/inject.");
        ImGui::SameLine();
        changed |= ImGui::Checkbox("Auto-dump", &tm.auto_dump);
        ImGui::SetItemTooltip("Save every texture to TT/dump as the game creates it. Textures already loaded are not caught -- use Dump all for those.");
        ImGui::SameLine();
        changed |= ImGui::Checkbox("Skip < 16px", &tm.filter_small_textures);
        ImGui::SetItemTooltip("Ignore textures smaller than 16x16.");
        ImGui::SameLine();
        changed |= ImGui::Checkbox("Scene only", &tm.show_current_frame_only);
        ImGui::SetItemTooltip("Show only textures drawn in the current scene.");
        if (changed)
        {
            Configuration &cfg = ConfigManager::get().get_config();
            cfg.enable_injection = tm.enable_injection;
            cfg.auto_dump = tm.auto_dump;
            cfg.filter_small_textures = tm.filter_small_textures;
            cfg.show_current_frame_only = tm.show_current_frame_only;
            ConfigManager::get().save();
        }

        if (ImGui::Button("Reload injected textures"))
        {
            const size_t refreshed = tm.rescan_injected();
            SetStatusMessage("Rescanned TT/inject; refreshed " + std::to_string(refreshed) + " texture(s) in place.");
        }
        ImGui::SetItemTooltip("Rescan TT/inject and apply what changed, including files added since the game started.");
        ImGui::SameLine();
        if (ImGui::Button("Dump all"))
        {
            size_t n = tm.dump_all(tm.show_current_frame_only);
            SetStatusMessage("Dump-all queued " + std::to_string(n) + (tm.show_current_frame_only ? " active" : " tracked") + " texture(s); written as they draw.");
        }
        ImGui::SetItemTooltip("Dump every tracked texture, or just the current scene when Scene only is ticked.\nEach is written the next time it is drawn.");
        ImGui::SameLine();
        if (ImGui::Button("Open dump folder"))
            OpenDirectory(tm.get_dump_dir());
        ImGui::SetItemTooltip("Open TT/dump in Explorer.");
        ImGui::SameLine();
        if (ImGui::Button("Open inject folder"))
            OpenDirectory(tm.get_inject_dir());
        ImGui::SetItemTooltip("Open TT/inject in Explorer.");

        std::vector<TextureDetails> textures = tm.get_active_textures();
        size_t injected = 0, dumped = 0, original = 0;
        uint64_t total_bytes = 0;
        // Injected and original split every texture between them: either we replaced it or the
        // game's own is in use. Having a dump on disk says nothing about which, so it is counted
        // separately and overlaps both.
        for (const auto &t : textures)
        {
            if (t.status == TextureStatus::INJECTED) injected++;
            else original++;
            if (!t.filepath_dumped.empty()) dumped++;
            total_bytes += t.data_size;
        }

        ImGui::Text("%zu tracked", textures.size());
        ImGui::SameLine();
        ImGui::TextColored(kColInjected, "  %zu injected", injected);
        ImGui::SameLine();
        ImGui::TextColored(kColDumped, "  %zu dumped", dumped);
        ImGui::SameLine();
        ImGui::TextColored(kColOriginal, "  %zu original", original);
        ImGui::SameLine();
        ImGui::TextColored(kColMuted, "   %.2f MiB", total_bytes / (1024.0 * 1024.0));

        ImGui::SetNextItemWidth(260.0f);
        ImGui::InputTextWithHint("##filter", "Filter by hash, size or format", s_filter_buf, sizeof(s_filter_buf));

        std::string filter_str = s_filter_buf;
        std::transform(filter_str.begin(), filter_str.end(), filter_str.begin(), ::tolower);

        static float s_split = 0.55f; // list fraction of the width; the rest is the inspector
        const float splitter_w = 6.0f;
        float total_w = ImGui::GetContentRegionAvail().x;
        float avail_h = ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing();
        float list_w = (total_w - splitter_w) * s_split;

        // Build the filtered list once so [ / ] navigation can step through it by index.
        std::vector<const TextureDetails *> shown;
        shown.reserve(textures.size());
        for (const auto &tex : textures)
        {
            if (!filter_str.empty())
            {
                char dim[32];
                std::snprintf(dim, sizeof dim, "%ux%u", tex.width, tex.height);
                if (!contains_nocase(tex.hash_hex, filter_str) && !contains_nocase(dim, filter_str) &&
                    !contains_nocase(tex.format_short, filter_str))
                    continue;
            }
            shown.push_back(&tex);
        }

        static bool s_scroll_to_sel = false;
        bool list_hovered = false;

        ImGui::BeginChild("list", ImVec2(list_w, avail_h), ImGuiChildFlags_Borders);
        {
            // True whenever the mouse is over the list. ChildWindows is required because a
            // ScrollY table creates its own inner scroll window, so hovering a row makes
            // that inner window (not this one) the hovered window.
            list_hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows);

            ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                    ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV |
                                    ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Sortable;

            if (ImGui::BeginTable("textures", 5, flags))
            {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("Hash", ImGuiTableColumnFlags_WidthFixed, 90.0f);
                ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 82.0f);
                ImGui::TableSetupColumn("Mips", ImGuiTableColumnFlags_WidthFixed, 38.0f);
                ImGui::TableSetupColumn("Format", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 70.0f);
                ImGui::TableHeadersRow();

                if (ImGuiTableSortSpecs *sort = ImGui::TableGetSortSpecs())
                {
                    if (sort->SpecsCount > 0)
                    {
                        const ImGuiTableColumnSortSpecs &spec = sort->Specs[0];
                        const bool ascending = spec.SortDirection == ImGuiSortDirection_Ascending;

                        std::stable_sort(shown.begin(), shown.end(),
                                         [&](const TextureDetails *a, const TextureDetails *b) {
                            int order = 0;
                            switch (spec.ColumnIndex)
                            {
                                case 0: order = (a->hash < b->hash) ? -1 : (a->hash > b->hash); break;
                                case 1: {
                                    const uint64_t sa = uint64_t{a->width} * a->height;
                                    const uint64_t sb = uint64_t{b->width} * b->height;
                                    order = (sa < sb) ? -1 : (sa > sb);
                                    break;
                                }
                                case 2: order = (a->mip_levels < b->mip_levels) ? -1 : (a->mip_levels > b->mip_levels); break;
                                case 3: order = a->format_short.compare(b->format_short); break;
                                default: order = static_cast<int>(a->status) - static_cast<int>(b->status); break;
                            }
                            return ascending ? order < 0 : order > 0;
                        });
                    }
                }

                for (const TextureDetails *ptex : shown)
                {
                    const TextureDetails &tex = *ptex;
                    ImGui::TableNextRow();

                    ImGui::TableSetColumnIndex(0);
                    std::string hash_str = "0x" + tex.hash_hex;
                    bool selected = (s_selected_texture_hash == tex.hash);
                    if (ImGui::Selectable(hash_str.c_str(), selected, ImGuiSelectableFlags_SpanAllColumns))
                        s_selected_texture_hash = tex.hash;

                    if (selected && s_scroll_to_sel)
                    {
                        ImGui::SetScrollHereY(0.5f);
                        s_scroll_to_sel = false;
                    }

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

        // Polled, and through the trampoline: exclusive-DirectInput games post no key messages,
        // and our own masking hook would answer zero.
        if (list_hovered)
        {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted("Press [ or ] to step through textures.");
            ImGui::EndTooltip();
        }
        {
            // Fed into ImGui from feed_overlay_mouse.
            bool go_prev = ImGui::IsKeyPressed(ImGuiKey_LeftBracket, false);
            bool go_next = ImGui::IsKeyPressed(ImGuiKey_RightBracket, false);

            int dir = list_hovered ? (go_prev ? -1 : (go_next ? 1 : 0)) : 0;

            if (dir != 0 && !shown.empty())
            {
                int cur = -1;
                for (int i = 0; i < static_cast<int>(shown.size()); ++i)
                    if (shown[i]->hash == s_selected_texture_hash) { cur = i; break; }

                int next = (cur < 0) ? (dir > 0 ? 0 : static_cast<int>(shown.size()) - 1) : cur + dir;
                next = std::clamp(next, 0, static_cast<int>(shown.size()) - 1);
                s_selected_texture_hash = shown[next]->hash;
                s_scroll_to_sel = true;
            }
        }

        ImGui::SameLine(0.0f, 0.0f);
        ImGui::InvisibleButton("##splitter", ImVec2(splitter_w, avail_h));
        if (ImGui::IsItemActive())
            s_split += ImGui::GetIO().MouseDelta.x / (total_w - splitter_w);
        s_split = std::clamp(s_split, 0.25f, 0.80f);
        if (ImGui::IsItemHovered() || ImGui::IsItemActive())
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        ImGui::SameLine(0.0f, 0.0f);

        ImGui::BeginChild("inspector", ImVec2(0, avail_h), ImGuiChildFlags_Borders);
        {
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
