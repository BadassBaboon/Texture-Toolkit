#include "TextureToolkitUI.h"
#include "TextureManager.h"
#include "OSDBanner.h"
#include "Config.h"
#include "Logger.h"
#include <windows.h>
#include <shellapi.h>
#include <vector>
#include <string>
#include <algorithm>
#include <imgui.h>

namespace TextureToolkit
{
    bool TextureToolkitUI::s_show_ui = false; // Default hidden; INSERT key toggles it
    static std::string s_status_message = "Ready";
    static uint64_t s_selected_texture_hash = 0;
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

        // Feed the [ and ] keys here too (same reliable point as the mouse poll, before
        // NewFrame). The UI reads ImGui's key state to step through the texture list; the
        // poll in draw_ui itself returns nothing on some titles.
        io.AddKeyEvent(ImGuiKey_LeftBracket,  (GetAsyncKeyState(VK_OEM_4) & 0x8000) != 0);
        io.AddKeyEvent(ImGuiKey_RightBracket, (GetAsyncKeyState(VK_OEM_6) & 0x8000) != 0);
    }

    // Colors used across the panel.
    static const ImVec4 kColInjected(0.35f, 0.85f, 0.45f, 1.0f);
    static const ImVec4 kColDumped(0.35f, 0.70f, 1.00f, 1.0f);
    static const ImVec4 kColOriginal(0.70f, 0.70f, 0.70f, 1.0f);
    static const ImVec4 kColMuted(0.60f, 0.60f, 0.62f, 1.0f);
    static const ImVec4 kColPending(0.95f, 0.80f, 0.25f, 1.0f);

    static const ImVec4 &status_color(TextureStatus s)
    {
        if (s == TextureStatus::INJECTED) return kColInjected;
        if (s == TextureStatus::DUMPED)   return kColDumped;
        if (s == TextureStatus::PENDING)  return kColPending;
        return kColOriginal;
    }

    // Takes the texture rather than the bare status, so an SK-named replacement can say so: that
    // is the difference between "my file loaded" and "the Special K pack I dropped in loaded".
    static const char *status_label(const TextureDetails &tex)
    {
        if (tex.status == TextureStatus::INJECTED)
            return tex.injected_via_sk_name ? "SK Injected" : "Injected";
        const TextureStatus s = tex.status;
        if (s == TextureStatus::DUMPED)   return "Dumped";
        if (s == TextureStatus::PENDING)  return "Pending";
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

    static void DrawInspector(TextureManager &tm, const TextureDetails &tex)
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
            handle = tm.get_file_preview_handle(tex.hash, tex.filepath_dumped, tex.is_dx11);
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
            ImGui::TextColored(status_color(tex.status), "%s", status_label(tex));

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

        // Toolbar: toggles and folder shortcuts. Toggling any option persists to the INI.
        bool changed = false;
        changed |= ImGui::Checkbox("Injection", &tm.enable_injection);
        ImGui::SetItemTooltip("Replace textures that have a matching DDS in TT/inject.");
        ImGui::SameLine();
        changed |= ImGui::Checkbox("Auto-dump", &tm.auto_dump);
        ImGui::SetItemTooltip("Save every texture to TT/dump as it loads.");
        ImGui::SameLine();
        changed |= ImGui::Checkbox("Skip < 16px", &tm.filter_small_textures);
        ImGui::SetItemTooltip("Ignore textures smaller than 16x16.");
        ImGui::SameLine();
        changed |= ImGui::Checkbox("Scene only", &tm.show_current_frame_only);
        ImGui::SetItemTooltip("Show only textures drawn in the current scene.");
        ImGui::SameLine();
        changed |= ImGui::Checkbox("SK names", &tm.accept_sk_names);
        ImGui::SetItemTooltip("Also load texture packs named the way Special K names them "
                              "(8 hex digits, the CRC-32C of the top mip). These show as SK Injected.");
        if (changed)
        {
            Configuration &cfg = ConfigManager::get().get_config();
            cfg.enable_injection = tm.enable_injection;
            cfg.auto_dump = tm.auto_dump;
            cfg.filter_small_textures = tm.filter_small_textures;
            cfg.show_current_frame_only = tm.show_current_frame_only;
            cfg.accept_sk_names = tm.accept_sk_names;
            ConfigManager::get().save();
        }

        if (ImGui::Button("Reload injected textures"))
        {
            tm.rescan_injected();
            SetStatusMessage("Rescanned TT/inject for DDS replacements.");
        }
        ImGui::SetItemTooltip("Rescan TT/inject and reload the replacement DDS files.");
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

        // Counts and total memory.
        std::vector<TextureDetails> textures = tm.get_active_textures();
        size_t injected = 0, dumped = 0, original = 0, pending = 0;
        uint64_t total_bytes = 0;
        for (const auto &t : textures)
        {
            if (t.status == TextureStatus::INJECTED) injected++;
            else if (t.status == TextureStatus::PENDING) pending++;
            else if (t.status == TextureStatus::DUMPED) dumped++;
            else original++;
            total_bytes += t.data_size;
        }

        ImGui::Text("%zu tracked", textures.size());
        ImGui::SameLine();
        ImGui::TextColored(kColInjected, "  %zu injected", injected);
        if (pending > 0)
        {
            ImGui::SameLine();
            ImGui::TextColored(kColPending, "  %zu pending", pending);
            ImGui::SetItemTooltip("An inject file exists for these, but no replacement is bound yet. "
                                  "If it stays pending, the file was rejected - check the log.");
        }
        ImGui::SameLine();
        ImGui::TextColored(kColDumped, "  %zu dumped", dumped);
        ImGui::SameLine();
        ImGui::TextColored(kColOriginal, "  %zu original", original);
        ImGui::SameLine();
        ImGui::TextColored(kColMuted, "   %.2f MiB", total_bytes / (1024.0 * 1024.0));

        // Injection health. "Applied" lags "found" until each texture is next drawn, so this is a
        // live readout rather than a verdict; "failed" is the one that needs the user's attention.
        TextureManager::InjectionStats inj = tm.get_injection_stats();
        if (inj.files_found > 0 || inj.failed > 0)
        {
            ImGui::TextColored(kColMuted, "Inject: %zu file(s), %zu applied", inj.files_found, inj.applied);
            if (inj.failed > 0)
            {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.00f, 0.45f, 0.40f, 1.0f), "  %zu failed", inj.failed);
                ImGui::SetItemTooltip("These DDS files could not be loaded or created. The log line "
                                      "for each says why (wrong format, truncated file, unsupported mapping).");
            }
        }

        // Filter.
        ImGui::SetNextItemWidth(260.0f);
        ImGui::InputTextWithHint("##filter", "Filter by hash, size or format", s_filter_buf, sizeof(s_filter_buf));

        std::string filter_str = s_filter_buf;
        std::transform(filter_str.begin(), filter_str.end(), filter_str.begin(), ::tolower);

        // Two resizable panes: list on the left, inspector on the right. Both scale with
        // the window; drag the divider between them to change the split.
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
                                    ImGuiTableFlags_SizingStretchProp;

            if (ImGui::BeginTable("textures", 5, flags))
            {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("Hash", ImGuiTableColumnFlags_WidthFixed, 150.0f);
                ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 82.0f);
                ImGui::TableSetupColumn("Mips", ImGuiTableColumnFlags_WidthFixed, 38.0f);
                ImGui::TableSetupColumn("Format", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 70.0f);
                ImGui::TableHeadersRow();

                for (const TextureDetails *ptex : shown)
                {
                    const TextureDetails &tex = *ptex;
                    ImGui::TableNextRow();

                    ImGui::TableSetColumnIndex(0);
                    std::string hash_str = "0x" + tex.hash_hex;
                    bool selected = (s_selected_texture_hash == tex.hash);
                    if (ImGui::Selectable(hash_str.c_str(), selected, ImGuiSelectableFlags_SpanAllColumns))
                        s_selected_texture_hash = tex.hash;

                    // Follow the selection when it was moved with the keyboard.
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
                    ImGui::TextColored(status_color(tex.status), "%s", status_label(tex));
                }
                ImGui::EndTable();
            }
        }
        ImGui::EndChild();

        // Step through the list with [ and ] while it is hovered. Keys are polled so this
        // works under exclusive-DirectInput games that post no key messages; draw_ui runs
        // inside the g_inside_imgui_render window, so the hooked GetAsyncKeyState returns
        // the real key state here.
        if (list_hovered)
        {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted("Press [ or ] to step through textures.");
            ImGui::EndTooltip();
        }
        {
            // [ and ] are fed into ImGui from feed_overlay_mouse; here we just read the
            // per-frame pressed state and step the selection when the list is hovered.
            bool go_prev = ImGui::IsKeyPressed(ImGuiKey_LeftBracket, false);
            bool go_next = ImGui::IsKeyPressed(ImGuiKey_RightBracket, false);

            int dir = list_hovered ? (go_prev ? -1 : (go_next ? 1 : 0)) : 0;

            if (dir != 0 && !shown.empty())
            {
                int cur = -1;
                for (int i = 0; i < static_cast<int>(shown.size()); ++i)
                    if (shown[i]->hash == s_selected_texture_hash) { cur = i; break; }

                int next = (cur < 0) ? (dir > 0 ? 0 : static_cast<int>(shown.size()) - 1) : cur + dir;
                next = next < 0 ? 0 : (next >= static_cast<int>(shown.size()) ? static_cast<int>(shown.size()) - 1 : next);
                s_selected_texture_hash = shown[next]->hash;
                s_scroll_to_sel = true;
            }
        }

        // Draggable divider.
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::InvisibleButton("##splitter", ImVec2(splitter_w, avail_h));
        if (ImGui::IsItemActive())
            s_split += ImGui::GetIO().MouseDelta.x / (total_w - splitter_w);
        s_split = s_split < 0.25f ? 0.25f : (s_split > 0.80f ? 0.80f : s_split);
        if (ImGui::IsItemHovered() || ImGui::IsItemActive())
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        ImGui::SameLine(0.0f, 0.0f);

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
