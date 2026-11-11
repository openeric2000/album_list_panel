#pragma once

namespace alp {

// 自編版新增（2026-11-11）：暫時隱藏伴奏版。開關是 fb2k 共用設定（Columns UI 的 Nocturne 設定頁「介面」分頁寫入），
// 開著時送進自動送出播放清單（「目前選取資料夾」）的歌會略過檔名或歌名含 Instrumental、Off Vocal、カラオケ 等字的版本；
// 只影響自動送出的清單，右鍵「送到播放清單」等其他送法不變
inline constexpr char hide_instrumental_config_key[] = "nocturne.folderTree.hideInstrumental";
inline constexpr bool hide_instrumental_default = false;
// 關鍵字（設定頁可改，用分號分隔、不分大小寫，比對檔名與歌名）；預設值要跟 Columns UI 的 tab_next_track_toast.cpp 一致。
// 「inst」只放有括號或句點的寫法，免得把 Instinct 之類的歌名也藏掉
inline constexpr char hide_instrumental_keywords_config_key[] = "nocturne.folderTree.hideInstrumental.keywords";
inline constexpr char hide_instrumental_keywords_default[]
    = "instrumental;inst.;(inst;[inst;inst);inst];off vocal;off-vocal;offvocal;karaoke;\xE3\x82\xAB\xE3\x83\xA9\xE3\x82"
      "\xAA\xE3\x82\xB1;backing track;without vocal;\xE4\xBC\xB4\xE5\xA5\x8F"; // …;カラオケ;…;伴奏
// 「自訂關鍵字」開關：關著（預設）一律用上面的預設值，開著才用設定頁輸入的關鍵字
inline constexpr char hide_instrumental_custom_config_key[] = "nocturne.folderTree.hideInstrumental.customKeywords";
inline constexpr bool hide_instrumental_custom_default = false;
bool is_hide_instrumental_enabled();

void send_tracks_to_playlist(const metadb_handle_list& tracks, bool replace_contents);
void send_nodes_to_playlist(const std::vector<node_ptr>& nodes, bool replace_contents, bool create_new);
// activate＝送完切到那個播放清單（設定改了重新整理時傳 false，不要把使用者正在看的分頁換掉）
void send_nodes_to_autosend_playlist(
    const std::vector<node_ptr>& nodes, const char* view, bool b_play, bool activate = true);

} // namespace alp
