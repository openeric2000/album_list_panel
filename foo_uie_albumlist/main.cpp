#include "stdafx.h"

#include "autoplaylist.h"
#include "menu.h"
#include "node.h"
#include "tree_view_populator.h"
#include "playlist_utils.h"
#include "node_utils.h"
#include "version.h"

namespace {

// Nocturne：主題的重點紅（229,38,43），搜尋框取得焦點時底下那條線用
constexpr COLORREF nocturne_accent_colour = RGB(229, 38, 43);

// Nocturne：搜尋框的提示字。中文版「搜尋資料夾…」，英文版（編譯時加 NOCTURNE_LANG_EN）「Search folders…」
#ifdef NOCTURNE_LANG_EN
constexpr auto filter_cue_text = L"Search folders…";
#else
constexpr auto filter_cue_text = L"搜尋資料夾…";
#endif

// Nocturne：主選單命令「搜尋專輯清單」（可在 Preferences > Keyboard Shortcuts 綁定快速鍵，例如 Ctrl+F）
constexpr GUID search_album_list_command_guid
    = {0xc295c1ff, 0x9f73, 0x421c, {0xa5, 0x4b, 0xe6, 0x2a, 0xe9, 0x54, 0xb2, 0xeb}};

// Nocturne：自編版 Columns UI 的「切換播放頁」命令（View/Layout 底下）。展開的播放頁蓋住主畫面時，用它收回
constexpr GUID nocturne_toggle_now_playing_command_guid
    = {0x95dae386, 0xd80a, 0x4ef7, {0xb5, 0x48, 0xd3, 0x6d, 0x0e, 0x4c, 0xc1, 0x63}};

#ifdef NOCTURNE_LANG_EN
constexpr auto search_album_list_command_name = "Search Album list";
constexpr auto search_album_list_command_description
    = "Moves the focus to the Album list search box (collapses the now playing page and shows the box if needed).";
#else
constexpr auto search_album_list_command_name = "搜尋專輯清單";
constexpr auto search_album_list_command_description
    = "把焦點移到 Album list 的搜尋框（必要時先收回播放頁、顯示搜尋框）。";
#endif

// Nocturne：搜尋框各部分的尺寸，單位是 96 DPI 下的像素，使用時依視窗 DPI 換算
constexpr int filter_pill_min_height = 30;
constexpr int filter_pill_vertical_padding = 7; // 輸入框上下各留的空間（字高之外）
constexpr int filter_margin_x = 8;
constexpr int filter_margin_top = 6;
constexpr int filter_margin_bottom = 8;
constexpr int filter_corner_radius = 6;
constexpr int filter_icon_size = 14;
constexpr int filter_icon_left_padding = 11;
constexpr int filter_icon_gap = 8;
constexpr int filter_clear_width = 26;
constexpr int filter_clear_right_padding = 4;
constexpr int filter_underline_thickness = 2;
constexpr int filter_fill_same_tolerance = 6;     // 各色版差距在這之內就視為「跟背景一樣」
constexpr double filter_fill_lift_amount = 0.055; // 調亮（深色）／調暗（淺色）的幅度

int scale_for_dpi(int value, unsigned dpi)
{
    return MulDiv(value, gsl::narrow<int>(dpi), USER_DEFAULT_SCREEN_DPI);
}

// 以 amount（0～1）把 from 往 to 混合；用來算「比文字暗一點」的圖示色
COLORREF blend_colours(COLORREF from, COLORREF to, double amount)
{
    const auto mix = [amount](int a, int b) { return static_cast<int>(a + (b - a) * amount + 0.5); };
    return RGB(mix(GetRValue(from), GetRValue(to)), mix(GetGValue(from), GetGValue(to)),
        mix(GetBValue(from), GetBValue(to)));
}

} // namespace

// TODO: node name as field

DECLARE_COMPONENT_VERSION("Album list panel",

    album_list_panel::version,

    "allows you to browse through your media library\n\n"
    "based upon albumlist 3.1.0\n"
    "Nocturne fork: restyled search box (rounded fill, magnifier, clear button, focus underline)\n"
    "compiled: " COMPILATION_DATE "\n"
    "with Columns UI SDK version: " UI_EXTENSION_VERSION

)

const char* directory_structure_view_name = "by directory structure";

void AlbumListWindow::s_update_all_fonts()
{
    if (s_font) {
        const auto count = s_instances.get_count();
        for (size_t i{0}; i < count; i++) {
            auto wnd = s_instances[i]->m_wnd_tv;
            if (wnd)
                uih::set_window_font(wnd, nullptr, false);

            wnd = s_instances[i]->m_wnd_edit;
            if (wnd)
                uih::set_window_font(wnd, nullptr, false);
        }
    }

    s_font.reset(cui::fonts::helper(album_list_font_client_id).get_font());

    const auto count = s_instances.get_count();
    for (size_t i{0}; i < count; i++) {
        auto wnd = s_instances[i]->m_wnd_tv;
        if (wnd) {
            uih::set_window_font(wnd, s_font.get());
            TreeView_SetIndent(wnd, cfg_use_custom_indentation ? cfg_custom_indentation_amount : 0);

            wnd = s_instances[i]->m_wnd_edit;
            if (wnd) {
                uih::set_window_font(wnd, s_font.get());
                s_instances[i]->on_size();
            }
        }
    }
}

void AlbumListWindow::s_update_all_show_root_expand_button()
{
    for (auto&& instance : s_instances) {
        const auto wnd = instance->m_wnd_tv;

        if (!wnd)
            continue;

        auto styles = GetWindowLongPtr(wnd, GWL_STYLE);

        if (cfg_show_root_node_expand_button || !cfg_show_root_node)
            styles |= TVS_LINESATROOT;
        else
            styles &= ~TVS_LINESATROOT;

        SetWindowLongPtr(wnd, GWL_STYLE, styles);
    }
}

AlbumListWindow::~AlbumListWindow()
{
    if (m_initialised) {
        s_instances.remove_item(this);
        m_initialised = false;
    }
}

void AlbumListWindow::s_update_all_window_frames()
{
    long flags = 0;
    if (cfg_frame_style == 1)
        flags |= WS_EX_CLIENTEDGE;
    if (cfg_frame_style == 2)
        flags |= WS_EX_STATICEDGE;

    const auto count = s_instances.get_count();

    for (size_t i{0}; i < count; i++) {
        const auto wnd = s_instances[i]->m_wnd_tv;
        if (wnd) {
            SetWindowLongPtr(wnd, GWL_EXSTYLE, flags);
            SetWindowPos(wnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
        }
    }
}

void AlbumListWindow::s_update_all_tree_colours()
{
    const auto count = s_instances.get_count();
    for (size_t i{0}; i < count; i++) {
        const auto wnd = s_instances[i]->m_wnd_tv;
        if (wnd) {
            s_instances[i]->update_tree_colours();
        }
    }
}

void AlbumListWindow::s_update_all_tree_themes()
{
    for (auto&& window : s_instances) {
        window->update_tree_theme();
        window->update_tooltip_theme();
    }
}

void AlbumListWindow::s_update_all_edit_themes()
{
    for (auto&& window : s_instances)
        window->update_edit_theme();
}

void AlbumListWindow::s_update_all_edit_colours()
{
    s_filter_background_brush.reset();

    for (auto&& window : s_instances)
        window->update_edit_colours();
}

void AlbumListWindow::s_update_all_labels()
{
    const auto count = s_instances.get_count();
    for (size_t i{0}; i < count; i++) {
        const auto wnd = s_instances[i]->m_wnd_tv;
        if (wnd) {
            s_instances[i]->update_all_labels();
        }
    }
}

void AlbumListWindow::s_mark_tracks_unsorted()
{
    for (const auto& instance : s_instances)
        instance->mark_tracks_unsorted();
}

void AlbumListWindow::s_update_all_showhscroll()
{
    const auto count = s_instances.get_count();
    for (size_t i{0}; i < count; i++) {
        const auto wnd = s_instances[i]->m_wnd_tv;
        if (wnd) {
            uih::DisableRedrawScope disable_redrawing(s_instances[i]->get_wnd());
            s_instances[i]->recreate_tree(true);
        }
    }
}

void AlbumListWindow::s_on_view_script_change(const char* p_view_before, const char* p_view)
{
    const auto count = s_instances.get_count();
    for (size_t i{0}; i < count; i++) {
        const auto wnd = s_instances[i]->m_wnd_tv;
        if (wnd) {
            s_instances[i]->on_view_script_change(p_view_before, p_view);
        }
    }
}

void AlbumListWindow::s_refresh_all()
{
    const auto count = s_instances.get_count();
    for (size_t i{0}; i < count; i++) {
        const auto wnd = s_instances[i]->m_wnd_tv;
        if (wnd) {
            s_instances[i]->refresh_tree();
        }
    }
}

void AlbumListWindow::s_update_all_item_heights()
{
    const auto count = s_instances.get_count();
    for (size_t i{0}; i < count; i++) {
        const auto wnd = s_instances[i]->m_wnd_tv;
        if (wnd) {
            s_instances[i]->update_item_height();
        }
    }
}

void AlbumListWindow::s_update_all_indents()
{
    const auto count = s_instances.get_count();
    for (size_t i{0}; i < count; i++) {
        const auto wnd = s_instances[i]->get_wnd();
        if (wnd) {
            const auto indentation = cfg_use_custom_indentation ? cfg_custom_indentation_amount : 0;
            TreeView_SetIndent(s_instances[i]->m_wnd_tv, indentation);
        }
    }
}

bool AlbumListWindow::is_bydir() const
{
    return !stricmp_utf8(m_view, directory_structure_view_name);
}

const char* AlbumListWindow::get_hierarchy() const
{
    auto& views = get_views();

    const auto index = views.find_item(m_view);

    if (index != pfc_infinite)
        return views.get_title_format(index);

    return "View not found";
}

void AlbumListWindow::on_view_script_change(const char* p_view_before, const char* p_view)
{
    if (get_wnd()) {
        if (!stricmp_utf8(p_view_before, m_view)) {
            set_view(p_view);
        }
    }
}

void AlbumListWindow::update_all_labels() const
{
    if (m_root) {
        SendMessage(m_wnd_tv, WM_SETREDRAW, FALSE, 0);
        TreeViewPopulator::s_setup_tree(m_wnd_tv, TVI_ROOT, m_root, std::nullopt, 0, 0);
        SendMessage(m_wnd_tv, WM_SETREDRAW, TRUE, 0);
    }
}

void AlbumListWindow::mark_tracks_unsorted() const
{
    if (m_root)
        m_root->mark_tracks_unsorted();
}

void AlbumListWindow::update_tree_theme() const
{
    if (!m_wnd_tv)
        return;

    bool is_themed{};

    if (m_item_colours->get_bool(cui::colours::bool_dark_mode_enabled)) {
        SetWindowTheme(m_wnd_tv, L"DarkMode_Explorer", nullptr);
        is_themed = true;
    } else if (m_item_colours->get_themed()) {
        SetWindowTheme(m_wnd_tv, L"Explorer", nullptr);
        is_themed = true;
    } else {
        SetWindowTheme(m_wnd_tv, nullptr, nullptr);
    }

    auto styles = GetWindowLongPtr(m_wnd_tv, GWL_STYLE);

    if (is_themed)
        styles &= ~TVS_HASLINES;
    else
        styles |= TVS_HASLINES;

    SetWindowLongPtr(m_wnd_tv, GWL_STYLE, styles);
}

void AlbumListWindow::update_tree_colours()
{
    SetWindowRedraw(m_wnd_tv, FALSE);
    update_tree_theme();

    TreeView_SetBkColor(m_wnd_tv, m_item_colours->get_colour(cui::colours::colour_background));
    TreeView_SetLineColor(m_wnd_tv, m_item_colours->get_colour(cui::colours::colour_active_item_frame));
    TreeView_SetTextColor(m_wnd_tv, m_item_colours->get_colour(cui::colours::colour_text));
    RedrawWindow(m_wnd_tv, nullptr, nullptr, RDW_INVALIDATE | RDW_FRAME);
    SetWindowRedraw(m_wnd_tv, TRUE);

    // Nocturne：搜尋框那一列的外側底色取自樹狀清單的背景色
    invalidate_filter_strip();
}

void AlbumListWindow::update_tooltip_theme() const
{
    if (!m_wnd_tv)
        return;

    const HWND tooltip_wnd = TreeView_GetToolTips(m_wnd_tv);

    if (!tooltip_wnd)
        return;

    const auto is_dark = cui::colours::is_dark_mode_active();
    SetWindowTheme(tooltip_wnd, is_dark ? L"DarkMode_Explorer" : nullptr, nullptr);
}

void AlbumListWindow::update_edit_theme() const
{
    if (!m_wnd_edit)
        return;

    SetWindowTheme(m_wnd_edit, cui::colours::is_dark_mode_active() ? L"DarkMode_CFD" : nullptr, nullptr);
}

void AlbumListWindow::update_edit_colours() const
{
    if (!m_wnd_edit)
        return;

    RedrawWindow(m_wnd_edit, nullptr, nullptr, RDW_ERASE | RDW_INVALIDATE);
    // Nocturne：圓角底的顏色也取自同一組色彩設定
    invalidate_filter_strip();
}

void AlbumListWindow::update_item_height()
{
    const auto font = uih::get_window_font(m_wnd_tv);
    int font_height = -1;
    if (cfg_use_custom_vertical_item_padding) {
        font_height = uih::get_font_height(font) + cfg_custom_vertical_padding_amount;
        if (font_height < 1)
            font_height = 1;
    }
    TreeView_SetItemHeight(m_wnd_tv, font_height);
}

void AlbumListWindow::on_task_completion(t_uint32 task, t_uint32 code)
{
    if (task == 0)
        refresh_tree(true);
}

void AlbumListWindow::create_or_destroy_filter()
{
    if (m_filter)
        create_filter(true);
    else
        destroy_filter();
    on_size();
}

void AlbumListWindow::create_filter(bool set_focus)
{
    if (!m_filter || m_wnd_edit)
        return;

    // Nocturne：不使用 Windows 的凹陷邊框，外觀由 paint_filter_strip 畫在輸入框後面
    constexpr auto flags = 0;
    m_wnd_edit = CreateWindowEx(flags, WC_EDIT, pfc::stringcvt::string_wide_from_utf8(m_saved_filter_query),
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 0, 0, get_wnd(), HMENU(IDC_FILTER),
        core_api::get_my_instance(), nullptr);

    if (!m_wnd_edit)
        return;

    uih::enhance_edit_control(m_wnd_edit);
    update_edit_theme();
    uih::set_window_font(m_wnd_edit, s_font.get(), false);
    if (set_focus)
        SetFocus(m_wnd_edit);
    // Nocturne：取得焦點時也顯示提示字（第三個參數 TRUE），一點進去提示字就消失會像壞掉
    Edit_SetCueBannerTextFocused(m_wnd_edit, filter_cue_text, TRUE);
}

void AlbumListWindow::destroy_filter()
{
    if (m_wnd_edit) {
        uGetWindowText(m_wnd_edit, m_saved_filter_query);

        const auto was_focused = GetFocus() == m_wnd_edit;
        DestroyWindow(m_wnd_edit);
        m_wnd_edit = nullptr;

        if (m_wnd_tv) {
            if (m_populated)
                refresh_tree();
            if (was_focused)
                SetFocus(m_wnd_tv);
        }
    }
    m_filter_ptr.release();
}

void AlbumListWindow::on_size(unsigned cx, unsigned cy)
{
    const auto layout = get_filter_layout(gsl::narrow<int>(cx), gsl::narrow<int>(cy));
    const auto strip_height = layout.strip.bottom - layout.strip.top;
    const auto tv_height = gsl::narrow<int>(cy) - strip_height;

    HDWP dwp = BeginDeferWindowPos(2);
    dwp = DeferWindowPos(dwp, m_wnd_tv, nullptr, 0, 0, gsl::narrow<int>(cx), tv_height, SWP_NOZORDER);
    if (m_wnd_edit)
        dwp = DeferWindowPos(dwp, m_wnd_edit, nullptr, layout.edit.left, layout.edit.top,
            layout.edit.right - layout.edit.left, layout.edit.bottom - layout.edit.top, SWP_NOZORDER);
    EndDeferWindowPos(dwp);

    // 視窗沒有 CS_HREDRAW／CS_VREDRAW，改變大小時搜尋框那一列不會自動重畫
    if (m_wnd_edit && !IsRectEmpty(&layout.strip))
        InvalidateRect(get_wnd(), &layout.strip, FALSE);
}

void AlbumListWindow::on_size()
{
    RECT rc;
    GetClientRect(get_wnd(), &rc);
    on_size(rc.right, rc.bottom);
}

// ---- Nocturne：搜尋框（圓角填色底、放大鏡、清除鈕、焦點底線）----

COLORREF AlbumListWindow::get_filter_fill_colour(COLORREF configured_fill, COLORREF strip_background)
{
    const auto close = [](int a, int b) { return std::abs(a - b) <= filter_fill_same_tolerance; };
    const bool same_as_background = close(GetRValue(configured_fill), GetRValue(strip_background))
        && close(GetGValue(configured_fill), GetGValue(strip_background))
        && close(GetBValue(configured_fill), GetBValue(strip_background));

    if (!same_as_background)
        return configured_fill;

    // 簡單的亮度判斷：偏暗的底往白色混合，偏亮的底往黑色混合
    const auto brightness = (GetRValue(configured_fill) * 299 + GetGValue(configured_fill) * 587
                                + GetBValue(configured_fill) * 114)
        / 1000;
    return blend_colours(configured_fill, brightness < 128 ? RGB(255, 255, 255) : RGB(0, 0, 0),
        filter_fill_lift_amount);
}

AlbumListWindow::FilterLayout AlbumListWindow::get_filter_layout(int cx, int cy) const
{
    FilterLayout layout{};
    if (!m_wnd_edit)
        return layout;

    const auto dpi = uih::dpi::get_dpi_for_window(get_wnd());
    const auto px = [dpi](int value) { return scale_for_dpi(value, dpi); };

    const int font_height = gsl::narrow<int>(uGetFontHeight(s_font.get()));
    const int pill_height = std::max(px(filter_pill_min_height), font_height + 2 * px(filter_pill_vertical_padding));
    const int strip_height = pill_height + px(filter_margin_top) + px(filter_margin_bottom);

    // 視窗太矮放不下就整列不顯示，樹狀清單用滿整個高度
    if (strip_height >= cy)
        return layout;

    layout.strip = {0, cy - strip_height, cx, cy};

    layout.pill.left = px(filter_margin_x);
    layout.pill.right = std::max<int>(layout.pill.left, cx - px(filter_margin_x));
    layout.pill.top = layout.strip.top + px(filter_margin_top);
    layout.pill.bottom = layout.pill.top + pill_height;

    const int icon_size = px(filter_icon_size);
    layout.icon.left = layout.pill.left + px(filter_icon_left_padding);
    layout.icon.right = layout.icon.left + icon_size;
    layout.icon.top = layout.pill.top + (pill_height - icon_size) / 2;
    layout.icon.bottom = layout.icon.top + icon_size;

    // 清除鈕的位置固定保留（沒有文字時只是不畫），輸入框寬度才不會跟著文字有無而變動
    layout.clear.right = layout.pill.right - px(filter_clear_right_padding);
    layout.clear.left = std::max<int>(layout.icon.right, layout.clear.right - px(filter_clear_width));
    layout.clear.top = layout.pill.top;
    layout.clear.bottom = layout.pill.bottom;

    layout.edit.left = layout.icon.right + px(filter_icon_gap);
    layout.edit.right = std::max<int>(layout.edit.left, layout.clear.left);
    layout.edit.top = layout.pill.top + (pill_height - font_height) / 2;
    layout.edit.bottom = layout.edit.top + font_height;

    return layout;
}

AlbumListWindow::FilterLayout AlbumListWindow::get_filter_layout() const
{
    RECT rc{};
    GetClientRect(get_wnd(), &rc);
    return get_filter_layout(rc.right, rc.bottom);
}

bool AlbumListWindow::filter_has_text() const
{
    return m_wnd_edit && GetWindowTextLength(m_wnd_edit) > 0;
}

bool AlbumListWindow::is_over_filter_clear_button(POINT pt) const
{
    if (!filter_has_text())
        return false;

    const auto layout = get_filter_layout();
    return PtInRect(&layout.clear, pt) != FALSE;
}

void AlbumListWindow::invalidate_filter_strip() const
{
    const auto layout = get_filter_layout();
    if (!IsRectEmpty(&layout.strip))
        InvalidateRect(get_wnd(), &layout.strip, FALSE);
}

void AlbumListWindow::invalidate_filter_clear_button() const
{
    const auto layout = get_filter_layout();
    if (!IsRectEmpty(&layout.clear))
        InvalidateRect(get_wnd(), &layout.clear, FALSE);
}

void AlbumListWindow::set_filter_clear_hot(bool hot)
{
    if (m_filter_clear_hot == hot)
        return;

    m_filter_clear_hot = hot;
    invalidate_filter_clear_button();
}

void AlbumListWindow::clear_filter_text()
{
    if (!m_wnd_edit)
        return;

    // 清空文字會送出 EN_CHANGE，之後走跟手動刪光文字相同的「延遲重新篩選」流程
    SetWindowText(m_wnd_edit, L"");
    SetFocus(m_wnd_edit);
}

bool AlbumListWindow::s_focus_filter()
{
    if (s_instances.get_count() == 0)
        return false;

    // 展開的播放頁蓋住主畫面時先收回（命令由自編版 Columns UI 提供，沒有就略過；用「已勾選」判斷現在是不是展開）
    service_ptr_t<mainmenu_commands> toggle_command;
    uint32_t toggle_index{};
    if (menu_item_resolver::g_resolve_main_command(
            nocturne_toggle_now_playing_command_guid, toggle_command, toggle_index)) {
        pfc::string8 text;
        uint32_t flags{};
        if (toggle_command->get_display(toggle_index, text, flags) && (flags & mainmenu_commands::flag_checked))
            toggle_command->execute(toggle_index, nullptr);
    }

    // 優先挑畫面上看得到的面板，沒有就挑第一個（再請 Columns UI 把它顯示出來）
    AlbumListWindow* target{};
    for (auto&& instance : s_instances) {
        const auto wnd = instance->get_wnd();
        if (wnd && IsWindowVisible(wnd) && instance->get_host()->is_visible(wnd)) {
            target = instance;
            break;
        }
    }
    if (!target) {
        for (auto&& instance : s_instances) {
            if (instance->get_wnd()) {
                target = instance;
                break;
            }
        }
    }
    if (!target)
        return false;

    const auto wnd = target->get_wnd();
    if (!target->get_host()->is_visible(wnd))
        target->get_host()->set_window_visibility(wnd, true);

    if (!target->m_filter)
        target->toggle_show_filter();

    if (!target->m_wnd_edit)
        return false;

    SetFocus(target->m_wnd_edit);
    Edit_SetSel(target->m_wnd_edit, 0, -1);
    return true;
}

void AlbumListWindow::paint_filter_strip(HDC dc)
{
    if (!m_wnd_edit)
        return;

    const auto layout = get_filter_layout();
    if (IsRectEmpty(&layout.strip))
        return;

    if (!m_filter_d2d_factory)
        m_filter_d2d_factory = uih::d2d::create_main_thread_factory();
    const auto& factory = *m_filter_d2d_factory;

    if (!m_filter_render_target) {
        const auto properties = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f,
            D2D1_RENDER_TARGET_USAGE_NONE, D2D1_FEATURE_LEVEL_DEFAULT);
        if (FAILED(factory->CreateDCRenderTarget(&properties, m_filter_render_target.put())))
            return;
    }

    // 只畫整列的範圍；繪圖座標以這一列左上角為原點
    RECT strip = layout.strip;
    if (FAILED(m_filter_render_target->BindDC(dc, &strip)))
        return;

    const auto to_rect = [&strip](const RECT& rc) {
        return D2D1::RectF(static_cast<float>(rc.left - strip.left), static_cast<float>(rc.top - strip.top),
            static_cast<float>(rc.right - strip.left), static_cast<float>(rc.bottom - strip.top));
    };

    cui::colours::helper filter_colours(album_list_filter_colours_client_id);
    const auto strip_colour = m_item_colours->get_colour(cui::colours::colour_background);
    const auto fill_colour
        = get_filter_fill_colour(filter_colours.get_colour(cui::colours::colour_background), strip_colour);
    const auto text_colour = filter_colours.get_colour(cui::colours::colour_text);
    // 圖示用比文字暗一點的顏色，滑過清除鈕時才用文字色
    const auto dim_colour = blend_colours(text_colour, fill_colour, 0.45);

    const auto dpi = uih::dpi::get_dpi_for_window(get_wnd());
    const auto px = [dpi](int value) { return static_cast<float>(scale_for_dpi(value, dpi)); };

    const auto& rt = m_filter_render_target;
    rt->BeginDraw();
    rt->Clear(uih::d2d::colorref_to_d2d_color(strip_colour));

    wil::com_ptr<ID2D1SolidColorBrush> brush;
    if (SUCCEEDED(rt->CreateSolidColorBrush(uih::d2d::colorref_to_d2d_color(fill_colour), brush.put()))) {
        const auto pill_rect = to_rect(layout.pill);
        const auto pill = D2D1::RoundedRect(pill_rect, px(filter_corner_radius), px(filter_corner_radius));
        rt->FillRoundedRectangle(pill, brush.get());

        // 取得焦點：圓角底最下面一條重點紅底線（用圓角底當遮罩，兩端會跟著圓角收掉）
        if (GetFocus() == m_wnd_edit) {
            wil::com_ptr<ID2D1RoundedRectangleGeometry> geometry;
            if (SUCCEEDED(factory->CreateRoundedRectangleGeometry(pill, geometry.put()))) {
                brush->SetColor(uih::d2d::colorref_to_d2d_color(nocturne_accent_colour));
                rt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), geometry.get()), nullptr);
                rt->FillRectangle(D2D1::RectF(pill_rect.left, pill_rect.bottom - px(filter_underline_thickness),
                                      pill_rect.right, pill_rect.bottom),
                    brush.get());
                rt->PopLayer();
            }
        }

        wil::com_ptr<ID2D1StrokeStyle> round_stroke;
        factory->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                       D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND),
            nullptr, 0, round_stroke.put());

        const auto stroke_width = std::max(1.0f, px(1) * 1.5f);

        // 放大鏡：圓圈加一條斜柄
        {
            const auto icon = to_rect(layout.icon);
            const auto size = icon.right - icon.left;
            const auto centre = D2D1::Point2F(icon.left + size * 0.43f, icon.top + size * 0.43f);
            const auto radius = size * 0.30f;
            brush->SetColor(uih::d2d::colorref_to_d2d_color(dim_colour));
            rt->DrawEllipse(D2D1::Ellipse(centre, radius, radius), brush.get(), stroke_width, round_stroke.get());
            rt->DrawLine(D2D1::Point2F(centre.x + radius * 0.72f, centre.y + radius * 0.72f),
                D2D1::Point2F(icon.left + size * 0.92f, icon.top + size * 0.92f), brush.get(), stroke_width,
                round_stroke.get());
        }

        // 清除鈕：輸入框有文字才畫；滑過時背後多一個淡淡的圓、叉叉改用文字色
        if (filter_has_text()) {
            const auto clear = to_rect(layout.clear);
            const auto centre = D2D1::Point2F((clear.left + clear.right) / 2, (clear.top + clear.bottom) / 2);

            if (m_filter_clear_hot) {
                brush->SetColor(uih::d2d::colorref_to_d2d_color(blend_colours(fill_colour, text_colour, 0.14)));
                rt->FillEllipse(D2D1::Ellipse(centre, px(10), px(10)), brush.get());
            }

            const auto half = px(4);
            brush->SetColor(uih::d2d::colorref_to_d2d_color(m_filter_clear_hot ? text_colour : dim_colour));
            rt->DrawLine(D2D1::Point2F(centre.x - half, centre.y - half),
                D2D1::Point2F(centre.x + half, centre.y + half), brush.get(), stroke_width, round_stroke.get());
            rt->DrawLine(D2D1::Point2F(centre.x - half, centre.y + half),
                D2D1::Point2F(centre.x + half, centre.y - half), brush.get(), stroke_width, round_stroke.get());
        }
    }

    const auto hr = rt->EndDraw();
    if (uih::d2d::is_device_reset_error(hr))
        m_filter_render_target.reset();
}

void AlbumListWindow::create_tree()
{
    const auto wnd = get_wnd();

    auto ex_styles = 0l;
    if (cfg_frame_style == 1)
        ex_styles |= WS_EX_CLIENTEDGE;
    else if (cfg_frame_style == 2)
        ex_styles |= WS_EX_STATICEDGE;

    const auto styles = TVS_SHOWSELALWAYS | TVS_HASBUTTONS | TVS_HASLINES | WS_CHILD | WS_VSCROLL | WS_VISIBLE
        | WS_TABSTOP | (cfg_show_root_node_expand_button || !cfg_show_root_node ? TVS_LINESATROOT : 0)
        | (!cfg_show_horizontal_scroll_bar ? TVS_NOHSCROLL : 0);

    m_wnd_tv = CreateWindowEx(ex_styles, WC_TREEVIEW, L"Album list", styles, 0, 0, 0, 0, wnd,
        reinterpret_cast<HMENU>(IDC_TREE), core_api::get_my_instance(), nullptr);

    if (m_wnd_tv) {
        TreeView_SetExtendedStyle(m_wnd_tv, TVS_EX_DOUBLEBUFFER, TVS_EX_DOUBLEBUFFER);
        update_tree_theme();
        update_tooltip_theme();

        if (!m_tree_view_theme)
            m_tree_view_theme.reset(OpenThemeData(m_wnd_tv, VSCLASS_TREEVIEW));

        if (s_font) {
            uih::set_window_font(m_wnd_tv, s_font.get(), false);
            TreeView_SetIndent(m_wnd_tv, cfg_use_custom_indentation ? cfg_custom_indentation_amount : 0);
        } else {
            s_update_all_fonts();
        }

        if (cfg_use_custom_vertical_item_padding)
            update_item_height();

        update_tree_colours();

        SetWindowLongPtr(m_wnd_tv, GWLP_USERDATA, reinterpret_cast<LPARAM>(this));
        m_treeproc = reinterpret_cast<WNDPROC>(
            SetWindowLongPtr(m_wnd_tv, GWLP_WNDPROC, reinterpret_cast<LPARAM>(s_tree_hook_proc)));

        if (m_populated) {
            refresh_tree();
        }
    }
}

void AlbumListWindow::destroy_tree(bool should_save_scroll_position)
{
    if (m_wnd_tv) {
        if (should_save_scroll_position)
            save_scroll_position();

        DestroyWindow(m_wnd_tv);
        m_wnd_tv = nullptr;
    }
}

void AlbumListWindow::recreate_tree(bool save_state)
{
    if (!m_wnd_tv)
        return;

    if (m_root && save_state)
        m_node_state = m_root->get_state(m_selection);

    SetWindowRedraw(get_wnd(), FALSE);

    destroy_tree(save_state);
    create_tree();
    on_size();

    SetWindowRedraw(get_wnd(), TRUE);
    RedrawWindow(get_wnd(), nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
}

void AlbumListWindow::save_scroll_position() const
{
    if (!m_wnd_tv)
        return;

    m_saved_scroll_position.reset();

    SCROLLINFO si_horz{};
    si_horz.cbSize = sizeof(SCROLLINFO);
    si_horz.fMask = SIF_POS;

    SCROLLINFO si_vert{};
    si_vert.cbSize = sizeof(SCROLLINFO);
    si_vert.fMask = SIF_POS | SIF_RANGE;

    if (!GetScrollInfo(m_wnd_tv, SB_HORZ, &si_horz) || !GetScrollInfo(m_wnd_tv, SB_VERT, &si_vert))
        return;

    m_saved_scroll_position = {si_horz.nPos, si_vert.nPos, si_vert.nMax};
}

void AlbumListWindow::restore_scroll_position()
{
    if (!m_wnd_tv || !m_saved_scroll_position)
        return;

    // Ensure scroll bar state has been updated; apparently the tree
    // view does this when rendering
    UpdateWindow(m_wnd_tv);

    SCROLLINFO si_vert_current{};
    si_vert_current.cbSize = sizeof(SCROLLINFO);
    si_vert_current.fMask = SIF_RANGE | SIF_POS;

    if (!GetScrollInfo(m_wnd_tv, SB_VERT, &si_vert_current))
        return;

    SCROLLINFO si{};
    si.cbSize = sizeof(SCROLLINFO);
    si.fMask = SIF_POS;
    si.nPos = m_saved_scroll_position->horizontal_position;
    const auto new_horizontal_position = SetScrollInfo(m_wnd_tv, SB_HORZ, &si, true);

    si.nPos = MulDiv(
        si_vert_current.nMax, m_saved_scroll_position->vertical_position, m_saved_scroll_position->vertical_max);
    const auto new_vertical_position = SetScrollInfo(m_wnd_tv, SB_VERT, &si, true);

    SendMessage(m_wnd_tv, WM_HSCROLL, MAKEWPARAM(SB_THUMBPOSITION, new_horizontal_position), 0);
    SendMessage(m_wnd_tv, WM_VSCROLL, MAKEWPARAM(SB_THUMBPOSITION, new_vertical_position), 0);

    m_saved_scroll_position.reset();
}

void AlbumListWindow::get_config(stream_writer* p_writer, abort_callback& p_abort) const
{
    save_scroll_position();

    p_writer->write_string(m_view, p_abort);
    p_writer->write_lendian_t(m_filter, p_abort);
    const auto saved_scroll_position = m_saved_scroll_position.value_or(alp::SavedScrollPosition{});
    p_writer->write_lendian_t(saved_scroll_position.horizontal_position, p_abort);
    p_writer->write_lendian_t(saved_scroll_position.vertical_position, p_abort);
    p_writer->write_lendian_t(saved_scroll_position.vertical_max, p_abort);

    if (get_wnd() && m_root) {
        write_node_state(p_writer, m_root->get_state(m_selection), p_abort);
    } else if (m_node_state) {
        write_node_state(p_writer, *m_node_state, p_abort);
    } else {
        p_writer->write_lendian_t(uint32_t{}, p_abort);
    }

    pfc::string filter_query;

    if (get_wnd() && m_wnd_edit) {
        filter_query = uGetWindowText(m_wnd_edit);
    } else {
        filter_query = m_saved_filter_query;
    }

    p_writer->write_string(filter_query.get_ptr(), filter_query.get_length(), p_abort);
}

void AlbumListWindow::export_config(stream_writer* p_writer, abort_callback& p_abort) const
{
    p_writer->write_string(m_view, p_abort);
    p_writer->write_lendian_t(m_filter, p_abort);
}

void AlbumListWindow::import_config(stream_reader* p_reader, t_size p_size, abort_callback& p_abort)
{
    if (p_size == 0)
        return;

    p_reader->read_string(m_view, p_abort);

    try {
        p_reader->read_lendian_t(m_filter, p_abort);
    } catch (const exception_io_data_truncation&) {
    }
}

void AlbumListWindow::get_name(pfc::string_base& out) const
{
    out.set_string("Album list");
}

void AlbumListWindow::get_category(pfc::string_base& out) const
{
    out.set_string("Panels");
}

void AlbumListWindow::set_config(stream_reader* p_reader, t_size psize, abort_callback& p_abort)
{
    if (!psize)
        return;

    stream_reader_limited_ref limited_reader(p_reader, psize);

    limited_reader.read_string(m_view, p_abort);
    try {
        limited_reader.read_lendian_t(m_filter, p_abort);
        const auto horizontal_scroll_position = limited_reader.read_lendian_t<int32_t>(p_abort);
        const auto vertical_scroll_position = limited_reader.read_lendian_t<int32_t>(p_abort);
        const auto vertical_scroll_max = limited_reader.read_lendian_t<int32_t>(p_abort);

        const auto next_element_size = limited_reader.read_lendian_t<uint32_t>(p_abort);

        // Handle bad config data written by versions from 2.0.0 to 2.0.3
        if (next_element_size == limited_reader.get_remaining()) {
            limited_reader.read_string_ex(m_saved_filter_query, next_element_size, p_abort);
            return;
        }

        if (next_element_size > 0) {
            m_node_state = alp::read_node_state(&limited_reader, p_abort, next_element_size);

            // Only set scroll positions if expansion state was read
            m_saved_scroll_position
                = alp::SavedScrollPosition{horizontal_scroll_position, vertical_scroll_position, vertical_scroll_max};
        }

        limited_reader.read_string(m_saved_filter_query, p_abort);
    } catch (exception_io_data_truncation&) {
    }
}

void AlbumListWindow::toggle_show_filter()
{
    m_filter = !m_filter;
    create_or_destroy_filter();
}

const char* AlbumListWindow::get_view() const
{
    return m_view;
}

void AlbumListWindow::set_view(const char* view)
{
    m_view = view;
    recreate_tree(false);
}

void AlbumListWindow::get_menu_items(ui_extension::menu_hook_t& p_hook)
{
    const auto node_settings
        = uie::menu_node_ptr{new uie::simple_command_menu_node{"Settings", "Shows Album list panel settings", 0,
            [] { static_api_ptr_t<ui_control>()->show_preferences(album_list_panel_preferences_page_id); }}};
    const auto node_filter = uie::menu_node_ptr{new uie::simple_command_menu_node{"Filter", "Shows the filter bar",
        m_filter ? uie::menu_node_t::state_checked : 0,
        [instance = service_ptr_t<AlbumListWindow>{this}] { instance->toggle_show_filter(); }}};

    p_hook.add_node(ui_extension::menu_node_ptr(new MenuNodeSelectView(this)));
    p_hook.add_node(node_filter);
    p_hook.add_node(node_settings);
}

bool AlbumListWindow::do_click_action(ClickAction click_action)
{
    auto cleaned_selection = get_cleaned_selection();

    switch (click_action) {
    case ClickAction::send_to_playlist:
        alp::send_nodes_to_playlist(cleaned_selection, true, false);
        break;
    case ClickAction::add_to_playlist:
        alp::send_nodes_to_playlist(cleaned_selection, false, false);
        break;
    case ClickAction::send_to_new_playlist:
        alp::send_nodes_to_playlist(cleaned_selection, true, true);
        break;
    case ClickAction::send_to_autosend_playlist:
        alp::send_nodes_to_autosend_playlist(cleaned_selection, m_view, true);
        break;
    default:
        return false;
    }
    return true;
}

void AlbumListWindow::collapse_other_nodes(const node_ptr& node) const
{
    auto current = node;
    auto parent = current->get_parent();

    while (parent) {
        auto expanded_siblings = parent->get_children()
            | ranges::views::filter([&current](auto& sibling) { return sibling->is_expanded() && sibling != current; });

        for (const auto& sibling : expanded_siblings) {
            TreeView_Expand(m_wnd_tv, sibling->m_ti, TVE_COLLAPSE);
            sibling->set_expanded(false);
        }

        current = parent;
        parent = current->get_parent();
    }
}

void AlbumListWindow::deselect_selected_nodes(const node_ptr& skip) const
{
    const auto selection = m_selection;
    for (auto& node : selection) {
        if (skip && node == skip)
            continue;

        manually_select_tree_item(node->m_ti, false);
    }
}

void AlbumListWindow::delete_all_nodes()
{
    TreeView_DeleteAllItems(m_wnd_tv);
    m_selection.clear();
    m_cleaned_selection.reset();
    m_root.reset();
}

node_ptr AlbumListWindow::get_node_for_tree_item(HTREEITEM item) const
{
    TVITEMEX tvi{};
    tvi.hItem = item;
    tvi.mask = TVIF_PARAM;
    if (!TreeView_GetItem(m_wnd_tv, &tvi))
        return {};

    return reinterpret_cast<Node*>(tvi.lParam)->shared_from_this();
}

bool AlbumListWindow::manually_select_tree_item(HTREEITEM item, bool selected) const
{
    TVITEMEX tvi{};
    tvi.hItem = item;
    tvi.mask = TVIF_STATE;
    tvi.stateMask = TVIS_SELECTED;
    tvi.state = selected ? TVIS_SELECTED : 0;
    return TreeView_SetItem(m_wnd_tv, &tvi) != 0;
}

void AlbumListWindow::select_range(const node_ptr& from, const node_ptr& to, bool expand) const
{
    std::unordered_set new_selection{to, from};
    const auto old_selection = m_selection;

    if (from != to) {
        if (!m_selection.contains(from))
            manually_select_tree_item(from->m_ti, true);

        const auto shift_hierarchy = from->get_hierarchy();
        const auto click_hierarchy = to->get_hierarchy();

        const auto max_level = std::min(shift_hierarchy.size(), click_hierarchy.size()) - 1;
        size_t compare_level{};

        for (auto [left, right] : ranges::views::zip(shift_hierarchy, click_hierarchy)) {
            if (left != right || compare_level == max_level)
                break;

            ++compare_level;
        }

        assert(compare_level < std::min(shift_hierarchy.size(), click_hierarchy.size()));

        const auto is_click_before_shift_item = [&] {
            if (shift_hierarchy[compare_level] == click_hierarchy[compare_level])
                return click_hierarchy.size() < shift_hierarchy.size();

            const auto click_index = click_hierarchy[compare_level]->get_display_index().value_or(0);
            const auto shift_index = shift_hierarchy[compare_level]->get_display_index().value_or(0);
            return click_index < shift_index;
        }();

        const auto next_item_arg = is_click_before_shift_item ? TVGN_PREVIOUSVISIBLE : TVGN_NEXTVISIBLE;

        HTREEITEM item = from->m_ti;
        while ((item = TreeView_GetNextItem(m_wnd_tv, item, next_item_arg)) != to->m_ti && item) {
            if (auto node = get_node_for_tree_item(item)) {
                if (!m_selection.contains(node))
                    manually_select_tree_item(item, true);

                new_selection.emplace(node);
            }
        }
    }

    if (!expand) {
        std::vector<node_ptr> to_deselect{};
        ranges::copy_if(old_selection, std::back_inserter(to_deselect),
            [&new_selection](auto& node) { return !new_selection.contains(node); });

        for (const auto& node : to_deselect) {
            manually_select_tree_item(node->m_ti, false);
        }
    }
}

void AlbumListWindow::autosend()
{
    if (cfg_autosend)
        alp::send_nodes_to_autosend_playlist(get_cleaned_selection(), m_view, false);
}

void AlbumListWindow::update_shift_start_node()
{
    if ((GetKeyState(VK_SHIFT) & 0x8000) == 0) {
        m_shift_start.reset();
        return;
    }

    const auto caret = TreeView_GetSelection(m_wnd_tv);
    const auto shift_start_item = caret ? caret : TreeView_GetRoot(m_wnd_tv);
    m_shift_start = get_node_for_tree_item(shift_start_item);
}

void AlbumListWindow::update_selection_holder()
{
    if (m_selection_holder.is_valid())
        m_selection_holder->set_selection(alp::get_node_tracks(get_cleaned_selection()).tracks());
}

const std::vector<node_ptr>& AlbumListWindow::get_cleaned_selection()
{
    if (!m_cleaned_selection)
        m_cleaned_selection = alp::clean_selection(m_selection);

    return *m_cleaned_selection;
}

void AlbumListWindow::create_autoplaylist(const std::span<const node_ptr> nodes) const
{
    const auto is_by_dir = is_bydir();

    pfc::string8 filter;

    if (m_wnd_edit)
        uGetWindowText(m_wnd_edit, filter);

    const auto has_root = ranges::contains(nodes, m_root);

    auto labels
        = has_root ? std::vector<std::vector<wil::zstring_view>>{} : nodes | ranges::views::transform([&](auto&& node) {
              auto hierarchy = node->get_hierarchy();
              return hierarchy | ranges::views::drop(1)
                  | ranges::views::transform([](const auto& node) { return node->get_name(); }) | ranges::to_vector;
          }) | ranges::to_vector;

    alp::create_autoplaylist(
        is_by_dir, is_by_dir ? "" : get_hierarchy(), std::move(labels), {filter.c_str(), filter.length()});
}

// {606E9CDD-45EE-4c3b-9FD5-49381CEBE8AE}
const GUID AlbumListWindow::s_extension_guid
    = {0x606e9cdd, 0x45ee, 0x4c3b, {0x9f, 0xd5, 0x49, 0x38, 0x1c, 0xeb, 0xe8, 0xae}};

ui_extension::window_factory<AlbumListWindow> blah;

namespace {

// Nocturne：放在「檢視」選單底下的命令，主要用途是讓使用者把 Ctrl+F 綁過來
class SearchAlbumListCommand : public mainmenu_commands {
public:
    uint32_t get_command_count() override { return 1; }
    GUID get_command(uint32_t index) override { return search_album_list_command_guid; }
    void get_name(uint32_t index, pfc::string_base& out) override { out = search_album_list_command_name; }
    bool get_description(uint32_t index, pfc::string_base& out) override
    {
        out = search_album_list_command_description;
        return true;
    }
    GUID get_parent() override { return mainmenu_groups::view; }
    void execute(uint32_t index, service_ptr callback) override { AlbumListWindow::s_focus_filter(); }
};

mainmenu_commands_factory_t<SearchAlbumListCommand> g_search_album_list_command;

} // namespace
