#pragma once

namespace alp {

// 自編版新增（2026-11-11）：暫時隱藏伴奏版。開關是 fb2k 共用設定（Columns UI 的 Nocturne 設定頁「介面」分頁寫入），
// 開著時送進自動送出播放清單（「目前選取資料夾」）的歌會略過檔名或歌名含 Instrumental、Off Vocal、カラオケ 等字的版本；
// 只影響自動送出的清單，右鍵「送到播放清單」等其他送法不變
inline constexpr char hide_instrumental_config_key[] = "nocturne.folderTree.hideInstrumental";
inline constexpr bool hide_instrumental_default = false;
bool is_hide_instrumental_enabled();

void send_tracks_to_playlist(const metadb_handle_list& tracks, bool replace_contents);
void send_nodes_to_playlist(const std::vector<node_ptr>& nodes, bool replace_contents, bool create_new);
// activate＝送完切到那個播放清單（設定改了重新整理時傳 false，不要把使用者正在看的分頁換掉）
void send_nodes_to_autosend_playlist(
    const std::vector<node_ptr>& nodes, const char* view, bool b_play, bool activate = true);

} // namespace alp
