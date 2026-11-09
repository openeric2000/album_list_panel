#pragma once

#include "node_formatter.h"
#include "node_utils.h"
#include "playlist_utils.h"

#define IDC_TREE 1000
#define IDC_FILTER 1001
#define EDIT_TIMER_ID 2001

namespace alp {

struct SavedScrollPosition {
    int horizontal_position{};
    int vertical_position{};
    int vertical_max{};
};

} // namespace alp

extern const char* directory_structure_view_name;

enum class ClickAction {
    none_or_default,
    send_to_playlist,
    add_to_playlist,
    send_to_new_playlist,
    send_to_autosend_playlist
};

class AlbumListWindow
    : public uie::container_uie_window_v3
    , public library_callback_dynamic
    , public library_callback_v2_dynamic {
    friend class font_notify;
    friend class Node;

public:
    static void s_update_all_tree_colours();
    static void s_update_all_tree_themes();
    static void s_update_all_edit_themes();
    static void s_update_all_edit_colours();
    static void s_update_all_item_heights();
    static void s_update_all_indents();
    static void s_update_all_labels();
    static void s_mark_tracks_unsorted();
    static void s_update_all_showhscroll();
    static void s_update_all_fonts();
    static void s_update_all_show_root_expand_button();
    static void s_refresh_all();
    static void s_on_view_script_change(const char* p_view_before, const char* p_view);
    static void s_update_all_window_frames();

    // Nocturne：主選單命令「搜尋專輯清單」的實作。找到一個 Album list 面板，必要時先收回展開的播放頁、顯示面板與搜尋框，
    // 然後把焦點給搜尋框並全選文字。沒有任何 Album list 面板時回傳 false
    static bool s_focus_filter();

    ~AlbumListWindow();

    bool is_bydir() const;
    const char* get_hierarchy() const;
    const char* get_view() const;
    void set_view(const char* view);
    void toggle_show_filter();

    void create_or_destroy_filter();
    void create_filter(bool set_focus = false);
    void destroy_filter();
    void create_tree();
    void destroy_tree(bool should_save_scroll_position);
    void recreate_tree(bool save_state);

    void save_scroll_position() const;
    void restore_scroll_position();
    void on_size(unsigned cx, unsigned cy);
    void on_size();

    void refresh_tree(bool preserve_state = false);
    void update_tree(metadb_handle_list_t<pfc::alloc_fast_aggressive>& to_add,
        metadb_handle_list_t<pfc::alloc_fast_aggressive>& to_remove, bool preserve_existing);
    void build_nodes(metadb_handle_list_t<pfc::alloc_fast_aggressive>& tracks, bool preserve_existing = false);
    void remove_nodes(metadb_handle_list_t<pfc::alloc_fast_aggressive>& p_tracks);

    void update_all_labels() const;
    void mark_tracks_unsorted() const;
    void update_tree_theme() const;
    void update_tree_colours();
    void update_tooltip_theme() const;
    void update_edit_theme() const;
    void update_edit_colours() const;
    void update_item_height();
    void on_view_script_change(const char* p_view_before, const char* p_view);

    const GUID& get_extension_guid() const override { return s_extension_guid; }

    void get_name(pfc::string_base& out) const override;
    void get_category(pfc::string_base& out) const override;

    void set_config(stream_reader* p_reader, t_size size, abort_callback& p_abort) override;
    void get_config(stream_writer* p_writer, abort_callback& p_abort) const override;
    void export_config(stream_writer* p_writer, abort_callback& p_abort) const override;
    void import_config(stream_reader* p_reader, t_size p_size, abort_callback& p_abort) override;

    unsigned get_type() const override { return ui_extension::type_panel; }

    // Nocturne：搜尋框那一列由本視窗自己用 Direct2D 一次畫完（背景＋圓角底＋圖示），所以不使用透明背景
    uie::container_window_v3_config get_window_config() override
    {
        return {L"{606E9CDD-45EE-4c3b-9FD5-49381CEBE8AE}", false};
    }

    void on_task_completion(t_uint32 task, t_uint32 code);
    void on_items_added(const pfc::list_base_const_t<metadb_handle_ptr>& p_data) override;
    void on_items_removed(const pfc::list_base_const_t<metadb_handle_ptr>& p_data) override;
    void on_items_modified(const pfc::list_base_const_t<metadb_handle_ptr>& p_data) override;
    void on_items_modified_v2(metadb_handle_list_cref items, metadb_io_callback_v2_data& data) override;
    void on_library_initialized() override;

    LRESULT on_message(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) override;
    LRESULT on_wm_contextmenu(POINT pt);
    std::optional<LRESULT> on_tree_view_wm_notify(LPNMHDR hdr);
    void get_menu_items(ui_extension::menu_hook_t& p_hook) override;

private:
    static LRESULT WINAPI s_tree_hook_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp);

    // Nocturne：搜尋框那一列的版面（單位：實體像素，座標相對於本視窗的工作區）
    struct FilterLayout {
        RECT strip{};  // 整列（含外距）
        RECT pill{};   // 圓角底
        RECT edit{};   // 輸入框
        RECT icon{};   // 放大鏡
        RECT clear{};  // 清除鈕的可點範圍
    };
    // 圓角底（與輸入框）實際使用的顏色：設定的搜尋框背景色若與樹狀清單背景色幾乎相同，就調亮（深色）或調暗（淺色）一點，
    // 否則圓角底會融進背景看不出來
    static COLORREF get_filter_fill_colour(COLORREF configured_fill, COLORREF strip_background);
    FilterLayout get_filter_layout(int cx, int cy) const;
    // 搜尋框圓角底離面板左右邊緣的距離（實際像素）；水平細捲軸的軌道左右端對齊它（見 main_thin_scrollbar.cpp）
    int get_filter_horizontal_margin() const;
    FilterLayout get_filter_layout() const;
    bool filter_has_text() const;
    bool is_over_filter_clear_button(POINT pt) const;
    void invalidate_filter_strip() const;
    void invalidate_filter_clear_button() const;
    void paint_filter_strip(HDC dc);
    void set_filter_clear_hot(bool hot);
    void clear_filter_text();

    LRESULT WINAPI on_tree_hooked_message(HWND wnd, UINT msg, WPARAM wp, LPARAM lp);
    std::optional<LRESULT> on_tree_lbuttondown(HWND wnd, UINT msg, WPARAM wp, LPARAM lp);
    std::optional<LRESULT> on_tree_lbuttonup(HWND wnd, UINT msg, WPARAM wp, LPARAM lp);
    void on_tree_rbuttondown(HWND wnd, UINT msg, WPARAM wp, LPARAM lp);
    std::optional<LRESULT> on_tree_rbuttonup(HWND wnd, UINT msg, WPARAM wp, LPARAM lp);
    void create_autoplaylist(std::span<const node_ptr> nodes) const;

    static inline pfc::ptr_list_t<AlbumListWindow> s_instances;
    static const GUID s_extension_guid;
    static const char* s_class_name;
    static inline wil::unique_hfont s_font;
    static inline wil::unique_hbrush s_filter_background_brush;

    bool do_click_action(ClickAction click_action);
    void collapse_other_nodes(const node_ptr& node) const;
    void deselect_selected_nodes(const node_ptr& skip = {}) const;
    void delete_all_nodes();
    node_ptr get_node_for_tree_item(HTREEITEM item) const;
    bool manually_select_tree_item(HTREEITEM item, bool selected) const;
    // Note: Does not select `to` (this is handled specially by the calling code)
    void select_range(const node_ptr& from, const node_ptr& to, bool expand) const;
    void autosend();

    void update_shift_start_node();
    void update_selection_holder();

    const std::vector<node_ptr>& get_cleaned_selection();

    // ---- Nocturne：細捲軸（2026-11-08，跟 Columns UI 播放清單同款：平常 4 px、滑鼠靠近變 10 px、不占版面）----
    // 樹狀清單是系統的 SysTreeView32，原生捲軸（垂直、水平各約 17 px）縮不掉也關不掉。做法：把樹視窗做得比面板大一個捲軸的厚度
    // （垂直捲軸可見時多出寬度、水平捲軸可見時多出高度），再用視窗區域（SetWindowRgn）把多出來的部分剪掉，
    // 原生捲軸因此不會顯示、也點不到，樹的可用範圍整個都在面板裡；再自己在樹視窗的右緣（垂直）與下緣（水平）各畫一條細軸，
    // 拖曳、點軌道轉成 WM_VSCROLL／WM_HSCROLL 交給樹處理，滾輪與鍵盤仍由樹原本處理。
    // 水平軸是 11/08 晚補的（使用者在設定裡開了「顯示水平捲軸」，看到還是原生粗軸）。
    // 開關是 fb2k 共用設定 nocturne.thinScrollbar（Columns UI 的設定頁寫入，兩邊讀同一個值，改了馬上切換，實作在 main_thin_scrollbar.cpp）
    enum class ThinAxis { vertical, horizontal };

    struct ThinScrollbarGeometry {
        RECT strip{};        // 浮層範圍（樹工作區座標）
        RECT thumb{};
        int track_start{};   // 軌道沿捲動方向的起點與長度（垂直＝y、水平＝x）
        int track_length{};
        int thumb_length{};
        int range_min{};
        int position_range{};
    };

    // 浮層（獨立的透明子視窗，滑軌畫在上面；說明見 main_thin_scrollbar.cpp）。座標用樹視窗的工作區座標
    struct ThinOverlayState {
        RECT strip{};
        RECT thumb{};
        COLORREF colour{};
        BYTE alpha{};

        bool operator==(const ThinOverlayState& other) const
        {
            return EqualRect(&strip, &other.strip) && EqualRect(&thumb, &other.thumb) && colour == other.colour
                && alpha == other.alpha;
        }
    };

    // 垂直、水平各一份狀態（各自一個浮層視窗）
    struct ThinAxisState {
        bool pointer_near{};
        bool thumb_hot{};
        bool dragging{};
        bool tracking_leave{};
        // 按住軌道空白處時連續翻頁（原生滑軌的行為）
        bool track_holding{};
        int drag_grab{};
        // 滑鼠離開後淡出（2026-11-09）：從離開當下的不透明度在 thin_fade_ms 內降到平常的值；淡出中滑塊維持靠近時的粗細
        bool fading{};
        BYTE fade_from{};
        ULONGLONG fade_start{};
        HWND overlay{};
        bool overlay_shown{};
        ThinOverlayState overlay_state{};
    };

    static inline bool s_thin_scrollbar_enabled{true};
    // 細捲軸平常隱藏（滑鼠移進軌道才出現），直、橫各一個開關（2026-11-09）；跟細捲軸開關一樣存在 fb2k 共用設定，
    // Columns UI 設定頁改了當場切換
    static inline bool s_thin_autohide_vertical{true};
    static inline bool s_thin_autohide_horizontal{true};
    static void s_sync_thin_scrollbar_setting();

    bool m_thin_active{};
    bool m_thin_adjusting{};
    // 樹視窗現在有沒有套視窗區域（剪掉多出來的原生捲軸）
    bool m_thin_clipped{};
    mutable int m_thin_bar_width{};
    mutable int m_thin_bar_height{};
    ThinAxisState m_thin_axes[2]{};

    ThinAxisState& thin_axis(ThinAxis axis) { return m_thin_axes[static_cast<size_t>(axis)]; }
    const ThinAxisState& thin_axis(ThinAxis axis) const { return m_thin_axes[static_cast<size_t>(axis)]; }

    static LRESULT CALLBACK s_thin_overlay_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT on_thin_overlay_message(HWND wnd, UINT msg, WPARAM wp, LPARAM lp);
    void update_thin_overlay();
    void update_thin_overlay(ThinAxis axis);
    BYTE thin_rest_alpha(ThinAxis axis) const;
    void start_thin_fade(ThinAxis axis);
    void stop_thin_fade(ThinAxis axis);
    void destroy_thin_overlay();
    void repeat_thin_track_page(ThinAxis axis);
    void end_thin_track_hold(ThinAxis axis);

    void apply_thin_scrollbar();
    int get_thin_extra_width() const;
    int get_thin_extra_height() const;
    void apply_thin_clip(int width, int height, bool clip);
    int get_tree_view_height(int cx, int cy) const;
    void sync_thin_scrollbar_layout();
    bool is_thin_axis_scrollable(ThinAxis axis) const;
    std::optional<ThinScrollbarGeometry> get_thin_scrollbar_geometry(ThinAxis axis) const;
    void update_thin_scrollbar_hover(ThinAxis axis, POINT pt);
    void drag_thin_scrollbar_to(ThinAxis axis, POINT pt);
    void end_thin_scrollbar_drag(ThinAxis axis, bool release_capture);
    void clear_thin_state();

    HWND m_wnd_tv{nullptr};
    HWND m_wnd_edit{nullptr};
    // Nocturne：搜尋框清除鈕的狀態與繪圖資源
    bool m_filter_clear_hot{};
    bool m_filter_clear_pressed{};
    bool m_filter_mouse_tracking{};
    uih::d2d::MainThreadD2D1Factory m_filter_d2d_factory;
    wil::com_ptr<ID2D1DCRenderTarget> m_filter_render_target;
    std::optional<cui::colours::helper> m_item_colours;
    wil::unique_htheme m_dd_theme;
    wil::unique_htheme m_tree_view_theme;
    WNDPROC m_treeproc{nullptr};
    bool m_initialised{false};
    bool m_populated{false};
    bool m_dragging{false};
    bool m_clicked{false};
    std::weak_ptr<Node> m_delayed_click_node;
    bool m_filter{false};
    bool m_timer{false};
    bool m_process_char{true};
    bool m_processing_multiselect{};
    POINT m_clickpoint{};
    mutable std::optional<alp::SavedScrollPosition> m_saved_scroll_position{};
    pfc::string8 m_view{"by artist/album"};
    node_ptr m_root;
    std::weak_ptr<Node> m_shift_start;
    std::optional<std::vector<node_ptr>> m_cleaned_selection;
    std::unordered_set<node_ptr> m_selection;
    std::optional<alp::SavedNodeState> m_node_state;
    std::optional<std::vector<std::weak_ptr<Node>>> m_previous_selection;
    pfc::string m_saved_filter_query;
    search_filter::ptr m_filter_ptr;
    ui_selection_holder::ptr m_selection_holder;
    NodeFormatter m_node_formatter;
    library_manager_v4::ptr m_library_v4;
    library_manager_v3::ptr m_library_v3;
    uih::direct_write::Context::Ptr m_direct_write_context;
    std::optional<uih::D2DDragImageCreator> m_drag_image_creator;
};
