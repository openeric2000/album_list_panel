#include "stdafx.h"
#include "playlist_utils.h"

namespace alp {

namespace {

class TitleformatHookView : public titleformat_hook {
public:
    bool process_field(
        titleformat_text_out* p_out, const char* p_name, size_t p_name_length, bool& p_found_flag) override
    {
        p_found_flag = false;
        if (m_view) {
            if (!stricmp_utf8_ex(p_name, p_name_length, "_view", 5)) {
                p_out->write(titleformat_inputtypes::meta, m_view, pfc_infinite);
                p_found_flag = true;
                return true;
            }
        }
        return false;
    }

    bool process_function(titleformat_text_out* p_out, const char* p_name, size_t p_name_length,
        titleformat_hook_function_params* p_params, bool& p_found_flag) override
    {
        p_found_flag = false;
        return false;
    }

    explicit TitleformatHookView(const char* p_view) : m_view{p_view} {}

private:
    const char* m_view;
};

// 檔名或歌名含這些字（不分大小寫）就當成伴奏版。「inst」只認有括號或句點的寫法，免得把 Instinct 之類的歌名也藏掉
constexpr std::wstring_view instrumental_keywords[] = {L"instrumental", L"inst.", L"(inst", L"[inst", L"inst)", L"inst]",
    L"off vocal", L"off-vocal", L"offvocal", L"karaoke", L"カラオケ" /* カラオケ */, L"backing track",
    L"without vocal", L"伴奏" /* 伴奏 */};

bool contains_instrumental_keyword(const char* utf8)
{
    if (!utf8 || !*utf8)
        return false;
    std::wstring text(pfc::stringcvt::string_wide_from_utf8(utf8).get_ptr());
    if (text.empty())
        return false;
    CharLowerBuffW(text.data(), static_cast<DWORD>(text.size()));
    return std::ranges::any_of(
        instrumental_keywords, [&text](std::wstring_view keyword) { return text.find(keyword) != std::wstring::npos; });
}

bool is_instrumental(const metadb_handle_ptr& track)
{
    if (contains_instrumental_keyword(pfc::string_filename(track->get_path()).c_str()))
        return true;

    metadb_info_container::ptr info;
    if (track->get_info_ref(info))
        return contains_instrumental_keyword(info->info().meta_get("title", 0));
    return false;
}

} // namespace

bool is_hide_instrumental_enabled()
{
    return fb2k::configStore::get()->getConfigBool(hide_instrumental_config_key, hide_instrumental_default);
}

void send_tracks_to_playlist(const metadb_handle_list& tracks, bool replace_contents)
{
    auto api = playlist_manager::get();
    const bool select = !!cfg_add_items_select;
    api->activeplaylist_undo_backup();

    if (replace_contents)
        api->activeplaylist_clear();
    else if (select)
        api->activeplaylist_clear_selection();

    api->activeplaylist_add_items(tracks, bit_array_val(select));

    if (select && !replace_contents) {
        const size_t num = api->activeplaylist_get_item_count();
        if (num > 0) {
            api->activeplaylist_set_focus_item(num - 1);
        }
    }
}

void send_nodes_to_playlist(const std::vector<node_ptr>& nodes, bool replace_contents, bool create_new)
{
    const auto tracks = get_node_tracks(nodes);

    if (tracks.tracks().size() == 0)
        return;

    if (create_new) {
        auto node_names = nodes | ranges::views::transform([](auto& node) { return node->get_display_name(); })
            | ranges::views::take(3);
        auto playlist_name = mmh::join<decltype(node_names)&, std::string_view, std::string>(node_names, ", ");

        if (nodes.size() > 3)
            playlist_name += u8", …"_pcc;

        const auto api = playlist_manager::get();
        if (const auto index
            = fbh::as_optional(api->create_playlist(playlist_name.data(), playlist_name.size(), pfc_infinite))) {
            api->set_active_playlist(*index);
            send_tracks_to_playlist(tracks.tracks(), true);
        }
    } else
        send_tracks_to_playlist(tracks.tracks(), replace_contents);

    if (replace_contents && cfg_play_on_send) {
        playlist_manager::get()->reset_playing_playlist();
        play_control::get()->play_start();
    }
}

void send_nodes_to_autosend_playlist(const std::vector<node_ptr>& nodes, const char* view, bool b_play, bool activate)
{
    const auto node_tracks = get_node_tracks(nodes);

    if (node_tracks.tracks().size() == 0)
        return;

    metadb_handle_list tracks = node_tracks.tracks();
    if (is_hide_instrumental_enabled()) {
        metadb_handle_list kept;
        for (size_t i{}; i < tracks.get_count(); ++i) {
            if (!is_instrumental(tracks[i]))
                kept.add_item(tracks[i]);
        }
        tracks = std::move(kept);
    }

    const auto api = playlist_manager::get();
    pfc::string8 playlist_name;

    TitleformatHookView tf_hook{view};
    titleformat_compiler::get()->run(&tf_hook, playlist_name, cfg_autosend_playlist_name);
    const auto index = api->find_or_create_playlist(playlist_name, pfc_infinite);

    api->playlist_undo_backup(index);
    api->playlist_clear(index);
    api->playlist_add_items(index, tracks, bit_array_val(cfg_add_items_select != 0));

    if (activate)
        api->set_active_playlist(index);

    if (b_play && cfg_play_on_send) {
        api->reset_playing_playlist();
        play_control::get()->play_start();
    }
}
} // namespace alp
