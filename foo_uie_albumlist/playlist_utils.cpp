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

std::wstring to_lower_wide(const char* utf8)
{
    std::wstring text(pfc::stringcvt::string_wide_from_utf8(utf8 ? utf8 : "").get_ptr());
    if (!text.empty())
        CharLowerBuffW(text.data(), static_cast<DWORD>(text.size()));
    return text;
}

// 設定裡的關鍵字：分號分隔、去掉前後空白、轉小寫（空的略過）
std::vector<std::wstring> read_instrumental_keywords()
{
    const auto store = fb2k::configStore::get();
    const auto custom = store->getConfigBool(hide_instrumental_custom_config_key, hide_instrumental_custom_default);
    const auto all = custom
        ? to_lower_wide(
              store->getConfigString(hide_instrumental_keywords_config_key, hide_instrumental_keywords_default)->c_str())
        : to_lower_wide(hide_instrumental_keywords_default);
    std::vector<std::wstring> keywords;
    size_t start{};
    while (start <= all.size()) {
        auto end = all.find(L';', start);
        if (end == std::wstring::npos)
            end = all.size();
        auto keyword = all.substr(start, end - start);
        const auto first = keyword.find_first_not_of(L" \t");
        const auto last = keyword.find_last_not_of(L" \t");
        if (first != std::wstring::npos)
            keywords.push_back(keyword.substr(first, last - first + 1));
        start = end + 1;
    }
    return keywords;
}

bool contains_any(const char* utf8, const std::vector<std::wstring>& keywords)
{
    if (!utf8 || !*utf8)
        return false;
    const auto text = to_lower_wide(utf8);
    return std::ranges::any_of(
        keywords, [&text](const std::wstring& keyword) { return text.find(keyword) != std::wstring::npos; });
}

// 檔名或歌名含任一個關鍵字就當成伴奏版
bool is_instrumental(const metadb_handle_ptr& track, const std::vector<std::wstring>& keywords)
{
    if (contains_any(pfc::string_filename(track->get_path()).c_str(), keywords))
        return true;

    metadb_info_container::ptr info;
    if (track->get_info_ref(info))
        return contains_any(info->info().meta_get("title", 0), keywords);
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
        const auto keywords = read_instrumental_keywords();
        if (!keywords.empty()) {
            metadb_handle_list kept;
            for (size_t i{}; i < tracks.get_count(); ++i) {
                if (!is_instrumental(tracks[i], keywords))
                    kept.add_item(tracks[i]);
            }
            tracks = std::move(kept);
        }
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
