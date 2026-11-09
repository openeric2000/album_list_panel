#include "stdafx.h"

#include "main.h"

// Nocturne：資料夾樹的細捲軸（2026-11-08；垂直先做，水平 11/08 晚補）。做法與限制見 main.h 的說明；數值跟 Columns UI 播放清單（ui_helpers 的 list_view_scroll.cpp）一致。
// 垂直軸的捲動單位是「列」（SysTreeView32 的垂直捲軸範圍是可見的列數，不是像素），水平軸是像素。
//
// 滑軌畫在獨立的透明浮層子視窗（WS_EX_LAYERED、逐像素 alpha，由 DWM 合成），不畫在樹自己的畫面上：
// 第一版畫在樹上，樹捲動（ScrollWindow）會把舊的滑軌像素一起捲走、重繪又晚一步，拖曳時看得到鬼影與抖動（使用者回報）。
// 整條軌道（16 DIP 厚）都是 alpha＝1 的「看不見但吃得到滑鼠」的像素，滑鼠靠近、按下、拖曳都由浮層自己接；靠近時不鋪軌道底色。
// 垂直、水平各一個浮層；兩條同時出現時各自讓出右下角那一格，免得重疊（浮層重疊的話一次點擊會送給兩個視窗）。

namespace {

constexpr char thin_scrollbar_config_key[] = "nocturne.thinScrollbar";
constexpr bool thin_scrollbar_default = true;
// 平常隱藏的開關（Columns UI 的 thin_scrollbar.h 有同樣的鍵名，兩邊要一致）；預設兩條都隱藏
constexpr char thin_autohide_vertical_key[] = "nocturne.thinScrollbar.autoHideVertical";
constexpr char thin_autohide_horizontal_key[] = "nocturne.thinScrollbar.autoHideHorizontal";
constexpr bool thin_autohide_default = true;

// 單位是 96 DPI 的像素，執行時依系統縮放換算
constexpr int thin_strip_width = 16;
constexpr int thin_thumb_right_inset = 3;
constexpr int thin_thumb_rest_width = 4;
constexpr int thin_thumb_near_width = 10;
constexpr int thin_thumb_min_height = 36;

// 不透明度（0～255）：平常 .24（設成「平常隱藏」的那條是 0）、靠近 .42、滑到滑塊上或拖曳中 .62
// 2026-11-09 使用者先要水平那條（會擋到最下面一列的資料夾名稱）改成滑鼠移進軌道才出現，看過後垂直也一起改，再要求設定頁直、橫各一個開關
constexpr BYTE thin_alpha_rest = 61;
constexpr BYTE thin_alpha_hidden = 0;
constexpr BYTE thin_alpha_near = 107;
constexpr BYTE thin_alpha_hot = 158;

// 按住軌道空白處連續翻頁：第一次延遲 400 ms，之後每 50 ms 翻一頁（跟原生滑軌的手感接近）
constexpr UINT_PTR thin_repeat_timer_id = 1;
constexpr UINT thin_repeat_first_delay_ms = 400;
constexpr UINT thin_repeat_interval_ms = 50;

// 滑鼠離開後淡出（2026-11-09 使用者：移開時直接消失太突然）：300 ms、頭尾放慢（smoothstep），約每 16 ms 重畫一次
constexpr UINT_PTR thin_fade_timer_id = 2;
constexpr ULONGLONG thin_fade_ms = 300;
constexpr UINT thin_fade_tick_ms = 16;

// WM_HSCROLL(SB_THUMBTRACK) 的位置只有 16 位元
constexpr int thin_max_scroll_message_position = 0x7FFF;

constexpr wchar_t thin_overlay_class_name[] = L"NocturneAlbumListThinScrollbarOverlay";

// 視窗類別只登記一次，模組卸載時（靜態物件解構）才取消登記
class ThinOverlayWindowClass {
public:
    explicit ThinOverlayWindowClass(WNDPROC proc)
    {
        WNDCLASSEX window_class{};
        window_class.cbSize = sizeof(window_class);
        window_class.lpfnWndProc = proc;
        window_class.hInstance = core_api::get_my_instance();
        window_class.hCursor = LoadCursor(nullptr, IDC_ARROW);
        window_class.lpszClassName = thin_overlay_class_name;
        m_atom = RegisterClassEx(&window_class);
    }

    ~ThinOverlayWindowClass()
    {
        if (m_atom)
            UnregisterClass(MAKEINTATOM(m_atom), core_api::get_my_instance());
    }

    ThinOverlayWindowClass(const ThinOverlayWindowClass&) = delete;
    ThinOverlayWindowClass& operator=(const ThinOverlayWindowClass&) = delete;

    [[nodiscard]] bool is_registered() const { return m_atom != 0; }

private:
    ATOM m_atom{};
};

// 在預乘 ARGB 的緩衝區裡畫一個兩端全圓的膠囊（邊緣用涵蓋率做抗鋸齒）；涵蓋率為 0 的像素保留原值（alpha＝1 的吃滑鼠像素）
void draw_pill(uint32_t* pixels, int buffer_width, const RECT& rc, COLORREF colour, BYTE alpha)
{
    const auto width = rc.right - rc.left;
    const auto height = rc.bottom - rc.top;

    if (width <= 0 || height <= 0 || alpha == 0)
        return;

    const auto red = GetRValue(colour);
    const auto green = GetGValue(colour);
    const auto blue = GetBValue(colour);

    const auto half_width = static_cast<float>(width) * .5f;
    const auto half_height = static_cast<float>(height) * .5f;
    const auto radius = std::min(half_width, half_height);

    for (int y = 0; y < height; ++y) {
        const auto py = std::abs(static_cast<float>(y) + .5f - half_height) - (half_height - radius);

        for (int x = 0; x < width; ++x) {
            const auto px = std::abs(static_cast<float>(x) + .5f - half_width) - (half_width - radius);
            const auto distance
                = std::hypot(std::max(px, 0.f), std::max(py, 0.f)) + std::min(std::max(px, py), 0.f) - radius;
            const auto coverage = std::clamp(.5f - distance, 0.f, 1.f);
            const auto a = static_cast<uint32_t>(std::lround(alpha * coverage));

            if (a == 0)
                continue;

            pixels[static_cast<size_t>(rc.top + y) * buffer_width + rc.left + x]
                = (a << 24) | (((red * a + 127) / 255) << 16) | (((green * a + 127) / 255) << 8) | ((blue * a + 127) / 255);
        }
    }
}

// 原生捲軸實際占的厚度＝視窗大小減工作區大小，再扣掉邊框。不用 GetScrollBarInfo：它回報的比實際少 1～3 px
// （2026-11-08 實測 169，水平軸實際 17 px、回報 14～16），樹的最下面一列就會多露出 1 px 原生捲軸。
// 這個值跟視窗現在多大無關（非工作區的厚度固定）；沒有該方向的捲軸時是 0
int measure_native_scrollbar_thickness(HWND wnd, bool horizontal)
{
    RECT window_rect{};
    RECT client_rect{};
    WINDOWINFO info{};
    info.cbSize = sizeof(info);

    if (!GetWindowRect(wnd, &window_rect) || !GetClientRect(wnd, &client_rect) || !GetWindowInfo(wnd, &info))
        return 0;

    if (horizontal)
        return (window_rect.bottom - window_rect.top) - client_rect.bottom - 2 * static_cast<int>(info.cyWindowBorders);

    return (window_rect.right - window_rect.left) - client_rect.right - 2 * static_cast<int>(info.cxWindowBorders);
}

} // namespace

LRESULT CALLBACK AlbumListWindow::s_thin_overlay_proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_MOUSEACTIVATE)
        return MA_NOACTIVATE;

    const auto window = reinterpret_cast<AlbumListWindow*>(GetWindowLongPtr(wnd, GWLP_USERDATA));

    if (window)
        return window->on_thin_overlay_message(wnd, msg, wp, lp);

    return DefWindowProc(wnd, msg, wp, lp);
}

// 讀共用設定並（第一次呼叫時）登記變更通知：Columns UI 設定頁改了開關，所有樹當場切換
void AlbumListWindow::s_sync_thin_scrollbar_setting()
{
    static bool notify_registered = false;

    const auto read_setting = [] {
        return fb2k::configStore::get()->getConfigBool(thin_scrollbar_config_key, thin_scrollbar_default);
    };

    if (!notify_registered) {
        notify_registered = true;
        fb2k::configStore::get()->addPermanentNotify(thin_scrollbar_config_key, [read_setting] {
            fb2k::inMainThread([read_setting] {
                const auto enabled = read_setting();

                if (enabled == s_thin_scrollbar_enabled)
                    return;

                s_thin_scrollbar_enabled = enabled;

                for (size_t i{}; i < s_instances.get_count(); ++i)
                    s_instances[i]->apply_thin_scrollbar();
            });
        });

        // 平常隱藏的兩個開關：改了只要重畫浮層（不透明度不同，update_thin_overlay 會判斷出有變）
        for (const char* key : std::initializer_list<const char*>{thin_autohide_vertical_key, thin_autohide_horizontal_key}) {
            fb2k::configStore::get()->addPermanentNotify(key, [] {
                fb2k::inMainThread([] {
                    const auto store = fb2k::configStore::get();
                    s_thin_autohide_vertical = store->getConfigBool(thin_autohide_vertical_key, thin_autohide_default);
                    s_thin_autohide_horizontal = store->getConfigBool(thin_autohide_horizontal_key, thin_autohide_default);

                    for (size_t i{}; i < s_instances.get_count(); ++i)
                        s_instances[i]->update_thin_overlay();
                });
            });
        }
    }

    s_thin_scrollbar_enabled = read_setting();
    s_thin_autohide_vertical = fb2k::configStore::get()->getConfigBool(thin_autohide_vertical_key, thin_autohide_default);
    s_thin_autohide_horizontal
        = fb2k::configStore::get()->getConfigBool(thin_autohide_horizontal_key, thin_autohide_default);
}

void AlbumListWindow::apply_thin_scrollbar()
{
    const auto wanted = s_thin_scrollbar_enabled && m_wnd_tv != nullptr;

    if (wanted == m_thin_active)
        return;

    end_thin_scrollbar_drag(ThinAxis::vertical, true);
    end_thin_scrollbar_drag(ThinAxis::horizontal, true);

    m_thin_active = wanted;

    for (auto& state : m_thin_axes) {
        state.pointer_near = false;
        state.thumb_hot = false;
    }

    stop_thin_fade(ThinAxis::vertical);
    stop_thin_fade(ThinAxis::horizontal);

    // 關掉就把浮層拿掉；開啟（含重建樹之後）要重新放到最上層，免得被新建的樹蓋住
    if (!wanted) {
        destroy_thin_overlay();
    } else {
        for (auto& state : m_thin_axes)
            state.overlay_shown = false;
    }

    if (!m_wnd_tv)
        return;

    // 重排樹的大小（開：多出原生捲軸的厚度再剪掉；關：還原、拿掉視窗區域），再把原生捲軸與整棵樹重畫
    on_size();
    RedrawWindow(m_wnd_tv, nullptr, nullptr, RDW_INVALIDATE | RDW_FRAME | RDW_UPDATENOW);
    update_thin_overlay();
}

// 視窗要銷毀了（或重建樹之前）：狀態一併清掉，不呼叫 apply_thin_scrollbar，免得去重排一個正在銷毀的視窗
void AlbumListWindow::clear_thin_state()
{
    m_thin_active = false;
    m_thin_clipped = false;

    for (auto& state : m_thin_axes) {
        state.pointer_near = false;
        state.thumb_hot = false;
        state.dragging = false;
        state.fading = false;
    }

    destroy_thin_overlay();
}

int AlbumListWindow::get_thin_extra_width() const
{
    if (!m_thin_active || !m_wnd_tv)
        return 0;

    // 沒有垂直捲軸（內容放得下）就不用把它擠到面板外面
    if (!(GetWindowLongPtr(m_wnd_tv, GWL_STYLE) & WS_VSCROLL))
        return 0;

    // 原生捲軸可見時量它實際的寬度（視窗所在螢幕的縮放不一定跟系統縮放相同）；量不到就沿用上次量到的或系統值
    const auto width = measure_native_scrollbar_thickness(m_wnd_tv, false);

    if (width > 0)
        m_thin_bar_width = width;

    return m_thin_bar_width > 0 ? m_thin_bar_width : GetSystemMetrics(SM_CXVSCROLL);
}

// 跟 get_thin_extra_width 一樣，水平捲軸的版本：有水平捲軸時樹視窗要多出捲軸的高度，下緣那一條再被剪掉
int AlbumListWindow::get_thin_extra_height() const
{
    if (!m_thin_active || !m_wnd_tv)
        return 0;

    if (!(GetWindowLongPtr(m_wnd_tv, GWL_STYLE) & WS_HSCROLL))
        return 0;

    const auto height = measure_native_scrollbar_thickness(m_wnd_tv, true);

    if (height > 0)
        m_thin_bar_height = height;

    return m_thin_bar_height > 0 ? m_thin_bar_height : GetSystemMetrics(SM_CYHSCROLL);
}

// 把樹視窗剪成「面板裡該給樹的那一塊」（視窗座標，樹視窗左上角就是 (0,0)）：多出來的原生捲軸就看不到也點不到。
// 沒有多出來的部分時拿掉區域，行為跟沒開細捲軸時完全一樣
void AlbumListWindow::apply_thin_clip(int width, int height, bool clip)
{
    if (!m_wnd_tv)
        return;

    if (clip) {
        wil::unique_hrgn region{CreateRectRgn(0, 0, std::max(width, 0), std::max(height, 0))};

        // SetWindowRgn 成功之後區域歸系統所有，不能再自己釋放
        if (region && SetWindowRgn(m_wnd_tv, region.get(), TRUE)) {
            region.release();
            m_thin_clipped = true;
        }
    } else if (m_thin_clipped) {
        SetWindowRgn(m_wnd_tv, nullptr, TRUE);
        m_thin_clipped = false;
    }
}

// 原生捲軸出現或消失後（新增節點、展開收合、縮放視窗），樹視窗的大小也要跟著換：
// 有垂直捲軸時多出捲軸寬度、有水平捲軸時多出捲軸高度，沒有時剛好等於面板給樹的那一塊
void AlbumListWindow::sync_thin_scrollbar_layout()
{
    if (!m_thin_active || m_thin_adjusting || !m_wnd_tv)
        return;

    RECT panel_rect{};
    GetClientRect(get_wnd(), &panel_rect);

    RECT tree_rect{};
    GetWindowRect(m_wnd_tv, &tree_rect);

    const auto expected_width = panel_rect.right + get_thin_extra_width();
    const auto expected_height = get_tree_view_height(panel_rect.right, panel_rect.bottom) + get_thin_extra_height();

    if (tree_rect.right - tree_rect.left == expected_width && tree_rect.bottom - tree_rect.top == expected_height)
        return;

    pfc::vartoggle_t guard(m_thin_adjusting, true);
    on_size();
}

bool AlbumListWindow::is_thin_axis_scrollable(ThinAxis axis) const
{
    if (!m_thin_active || !m_wnd_tv)
        return false;

    SCROLLINFO info{};
    info.cbSize = sizeof(info);
    info.fMask = SIF_RANGE | SIF_PAGE;

    if (!GetScrollInfo(m_wnd_tv, axis == ThinAxis::vertical ? SB_VERT : SB_HORZ, &info))
        return false;

    const auto total = static_cast<int64_t>(info.nMax) - info.nMin + 1;
    return info.nPage != 0 && total > static_cast<int64_t>(info.nPage);
}

std::optional<AlbumListWindow::ThinScrollbarGeometry> AlbumListWindow::get_thin_scrollbar_geometry(ThinAxis axis) const
{
    if (!m_thin_active || !m_wnd_tv)
        return std::nullopt;

    const auto vertical = axis == ThinAxis::vertical;

    SCROLLINFO info{};
    info.cbSize = sizeof(info);
    info.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;

    if (!GetScrollInfo(m_wnd_tv, vertical ? SB_VERT : SB_HORZ, &info))
        return std::nullopt;

    const auto total = static_cast<int64_t>(info.nMax) - info.nMin + 1;

    if (info.nPage == 0 || total <= static_cast<int64_t>(info.nPage))
        return std::nullopt;

    RECT client{};
    GetClientRect(m_wnd_tv, &client);

    // 兩條都出現時，垂直軸的軌道在右下角讓出一格給水平軸（垂直軸比較長，所以讓的是它；水平軸的右端不受影響，兩條的軌道不會重疊）
    const auto strip_size = uih::scale_dpi_value(thin_strip_width);
    const auto corner = vertical && is_thin_axis_scrollable(ThinAxis::horizontal) ? strip_size : 0;

    // 水平軸的軌道左右端對齊下方搜尋框圓角底的左右邊緣（使用者 11/08 晚上說滑塊拉到最右邊會比搜尋框多出一點，要求對齊它）；
    // 沒有搜尋框時也留一樣的距離，兩種情況看起來一致。垂直軸不動
    const int align = get_filter_horizontal_margin();
    const int track_start = vertical ? client.top : client.left + align;
    const int track_end = vertical ? client.bottom - corner : client.right - align;
    const int track_length = track_end - track_start;

    if (track_length <= 0)
        return std::nullopt;

    ThinScrollbarGeometry geometry{};
    geometry.track_start = track_start;
    geometry.track_length = track_length;
    geometry.range_min = info.nMin;
    geometry.position_range = static_cast<int>(total - static_cast<int64_t>(info.nPage));

    const auto proportional_length = static_cast<int>(static_cast<int64_t>(track_length) * info.nPage / total);
    geometry.thumb_length
        = std::clamp(proportional_length, std::min(uih::scale_dpi_value(thin_thumb_min_height), track_length), track_length);

    const auto position = std::clamp(info.nPos - info.nMin, 0, geometry.position_range);
    const auto room = track_length - geometry.thumb_length;
    const auto offset = room > 0 ? static_cast<int>(static_cast<int64_t>(room) * position / geometry.position_range) : 0;

    const auto& state = thin_axis(axis);
    const auto thickness
        = uih::scale_dpi_value(state.pointer_near || state.dragging || state.fading ? thin_thumb_near_width : thin_thumb_rest_width);
    const auto inset = uih::scale_dpi_value(thin_thumb_right_inset);

    if (vertical) {
        const auto right = client.right - inset;
        geometry.thumb = {right - thickness, track_start + offset, right, track_start + offset + geometry.thumb_length};
        geometry.strip = {client.right - strip_size, track_start, client.right, track_end};
    } else {
        const auto bottom = client.bottom - inset;
        geometry.thumb = {track_start + offset, bottom - thickness, track_start + offset + geometry.thumb_length, bottom};
        geometry.strip = {track_start, client.bottom - strip_size, track_end, client.bottom};
    }

    return geometry;
}

void AlbumListWindow::destroy_thin_overlay()
{
    for (auto& state : m_thin_axes) {
        if (state.overlay) {
            DestroyWindow(state.overlay);
            state.overlay = nullptr;
        }

        state.overlay_shown = false;
        state.tracking_leave = false;
        state.track_holding = false;
    }
}

void AlbumListWindow::update_thin_overlay()
{
    update_thin_overlay(ThinAxis::vertical);
    update_thin_overlay(ThinAxis::horizontal);
}

// 依目前的捲動範圍、位置與滑鼠狀態更新浮層：沒有可捲的內容就藏起來；內容沒變就什麼都不做
void AlbumListWindow::update_thin_overlay(ThinAxis axis)
{
    const auto panel = get_wnd();

    if (!panel || !m_wnd_tv)
        return;

    auto& axis_state = thin_axis(axis);

    // 淡出時間到了就先結束（要在算位置之前：結束後滑塊回到平常的粗細）
    if (axis_state.fading && GetTickCount64() - axis_state.fade_start >= thin_fade_ms)
        stop_thin_fade(axis);

    const auto geometry = m_thin_active ? get_thin_scrollbar_geometry(axis) : std::nullopt;

    if (!geometry) {
        stop_thin_fade(axis);

        if (axis_state.overlay && axis_state.overlay_shown)
            ShowWindow(axis_state.overlay, SW_HIDE);

        axis_state.overlay_shown = false;
        return;
    }

    static const ThinOverlayWindowClass window_class(&AlbumListWindow::s_thin_overlay_proc);

    if (!window_class.is_registered() || !m_item_colours)
        return;

    const auto colour = m_item_colours->get_colour(cui::colours::colour_text);
    BYTE alpha = thin_rest_alpha(axis);

    if (axis_state.dragging || axis_state.thumb_hot || axis_state.pointer_near) {
        // 滑鼠又回來了：淡出中就立刻停掉
        stop_thin_fade(axis);
        alpha = axis_state.dragging || axis_state.thumb_hot ? thin_alpha_hot : thin_alpha_near;
    } else if (axis_state.fading) {
        const auto t = static_cast<double>(GetTickCount64() - axis_state.fade_start) / static_cast<double>(thin_fade_ms);
        const auto eased = t * t * (3.0 - 2.0 * t);
        alpha = static_cast<BYTE>(std::lround(axis_state.fade_from + (alpha - axis_state.fade_from) * eased));
    }

    ThinOverlayState state{geometry->strip, geometry->thumb, colour, alpha};

    if (axis_state.overlay && axis_state.overlay_shown && state == axis_state.overlay_state)
        return;

    if (!axis_state.overlay) {
        axis_state.overlay = CreateWindowEx(WS_EX_LAYERED, thin_overlay_class_name, L"", WS_CHILD, 0, 0, 0, 0, panel,
            nullptr, core_api::get_my_instance(), nullptr);

        if (!axis_state.overlay)
            return;

        SetWindowLongPtr(axis_state.overlay, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        axis_state.overlay_shown = false;
    }

    const auto width = state.strip.right - state.strip.left;
    const auto height = state.strip.bottom - state.strip.top;

    if (width <= 0 || height <= 0)
        return;

    // 位置或大小變了（或第一次顯示）才動視窗；放最上層，免得被樹（後建立的兄弟視窗）蓋住
    if (!axis_state.overlay_shown || !EqualRect(&state.strip, &axis_state.overlay_state.strip)) {
        POINT origin{state.strip.left, state.strip.top};
        MapWindowPoints(m_wnd_tv, panel, &origin, 1);   // 樹的工作區座標 → 面板的工作區座標
        SetWindowPos(axis_state.overlay, HWND_TOP, origin.x, origin.y, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* bits{};
    wil::unique_hbitmap bitmap{CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0)};

    if (!bitmap || !bits)
        return;

    auto* pixels = static_cast<uint32_t*>(bits);
    std::fill_n(pixels, static_cast<size_t>(width) * height, 0x01000000u);

    const RECT thumb_in_overlay{state.thumb.left - state.strip.left, state.thumb.top - state.strip.top,
        state.thumb.right - state.strip.left, state.thumb.bottom - state.strip.top};
    draw_pill(pixels, width, thumb_in_overlay, state.colour, state.alpha);

    const auto screen_dc = wil::GetDC(nullptr);
    wil::unique_hdc memory_dc{CreateCompatibleDC(screen_dc.get())};

    if (!memory_dc)
        return;

    const auto _ = wil::SelectObject(memory_dc.get(), bitmap.get());

    SIZE size{width, height};
    POINT source{0, 0};
    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;

    if (UpdateLayeredWindow(axis_state.overlay, screen_dc.get(), nullptr, &size, memory_dc.get(), &source, 0, &blend, ULW_ALPHA)) {
        axis_state.overlay_state = state;
        axis_state.overlay_shown = true;
    }
}

// 平常（滑鼠不在旁邊）的不透明度：設成自動隱藏的那條是 0
BYTE AlbumListWindow::thin_rest_alpha(ThinAxis axis) const
{
    const auto hidden = axis == ThinAxis::vertical ? s_thin_autohide_vertical : s_thin_autohide_horizontal;
    return hidden ? thin_alpha_hidden : thin_alpha_rest;
}

// 從目前畫著的不透明度開始淡出到平常的值；本來就不比平常亮（或浮層沒顯示）就不用淡
void AlbumListWindow::start_thin_fade(ThinAxis axis)
{
    auto& state = thin_axis(axis);

    if (!state.overlay || !state.overlay_shown || state.overlay_state.alpha <= thin_rest_alpha(axis))
        return;

    state.fading = true;
    state.fade_from = state.overlay_state.alpha;
    state.fade_start = GetTickCount64();
    SetTimer(state.overlay, thin_fade_timer_id, thin_fade_tick_ms, nullptr);
}

void AlbumListWindow::stop_thin_fade(ThinAxis axis)
{
    auto& state = thin_axis(axis);

    if (!state.fading)
        return;

    state.fading = false;

    if (state.overlay)
        KillTimer(state.overlay, thin_fade_timer_id);
}

void AlbumListWindow::update_thin_scrollbar_hover(ThinAxis axis, POINT pt)
{
    const auto geometry = get_thin_scrollbar_geometry(axis);

    if (!geometry)
        return;

    auto& state = thin_axis(axis);
    const auto vertical = axis == ThinAxis::vertical;

    const auto along = vertical ? pt.y : pt.x;
    const auto across = vertical ? pt.x : pt.y;
    const auto thumb_start = vertical ? geometry->thumb.top : geometry->thumb.left;
    const auto thumb_end = vertical ? geometry->thumb.bottom : geometry->thumb.right;
    const auto strip_end = vertical ? geometry->strip.right : geometry->strip.bottom;

    // 滑塊的「熱區」用靠近時的厚度算，沿捲動方向在滑塊範圍內就算（不用精準點到 4 px 細條）
    const bool on_thumb = along >= thumb_start && along < thumb_end
        && across >= strip_end - uih::scale_dpi_value(thin_thumb_right_inset + thin_thumb_near_width);

    if (!state.tracking_leave && state.overlay) {
        TRACKMOUSEEVENT track_mouse_event{sizeof(TRACKMOUSEEVENT), TME_LEAVE, state.overlay, 0};
        state.tracking_leave = TrackMouseEvent(&track_mouse_event) != 0;
    }

    if (state.pointer_near && on_thumb == state.thumb_hot)
        return;

    state.pointer_near = true;
    state.thumb_hot = on_thumb;
    update_thin_overlay(axis);
}

void AlbumListWindow::drag_thin_scrollbar_to(ThinAxis axis, POINT pt)
{
    const auto geometry = get_thin_scrollbar_geometry(axis);

    if (!geometry)
        return;

    const auto vertical = axis == ThinAxis::vertical;
    const auto room = geometry->track_length - geometry->thumb_length;

    if (room <= 0)
        return;

    const int along = vertical ? pt.y : pt.x;
    const auto wanted_start = std::clamp(along - thin_axis(axis).drag_grab - geometry->track_start, 0, room);
    const auto position = geometry->range_min
        + static_cast<int>(std::llround(static_cast<double>(wanted_start) * static_cast<double>(geometry->position_range)
              / static_cast<double>(room)));

    SCROLLINFO info{};
    info.cbSize = sizeof(info);
    info.fMask = SIF_POS;

    if (!GetScrollInfo(m_wnd_tv, vertical ? SB_VERT : SB_HORZ, &info))
        return;

    if (position == info.nPos)
        return;

    if (vertical) {
        // 合成的 WM_VSCROLL(SB_THUMBTRACK) 樹不理（它去讀原生捲軸自己的追蹤位置，實測拖了不會捲），
        // 所以改成：從目前第一個可見項目往上或往下走到目標那一列，再把它設成第一個可見項目（只捲動、不改選取）
        auto item = TreeView_GetFirstVisible(m_wnd_tv);
        auto steps = position - info.nPos;
        const auto direction = steps > 0 ? TVGN_NEXTVISIBLE : TVGN_PREVIOUSVISIBLE;
        steps = std::abs(steps);

        for (; steps > 0 && item; --steps) {
            const auto next = TreeView_GetNextItem(m_wnd_tv, item, direction);

            if (!next)
                break;

            item = next;
        }

        if (item) {
            // 設定第一個可見項目時，樹會順便改掉水平捲動位置（實測拖垂直滑塊，內容會左右跳），原生垂直捲軸不會這樣，所以事後還原。
            // 「設定」與「還原」之間樹只要畫出來，水平被推走的過渡畫面就會被螢幕顯示（大幅拖曳時肉眼可見的殘影：
            // 內容左右跳、列被切成一條條），所以這段期間暫停重繪，兩步都做完再一次性重畫成最終畫面
            SCROLLINFO horizontal{};
            horizontal.cbSize = sizeof(horizontal);
            horizontal.fMask = SIF_POS;
            const auto had_horizontal = GetScrollInfo(m_wnd_tv, SB_HORZ, &horizontal);

            SendMessage(m_wnd_tv, WM_SETREDRAW, FALSE, 0);

            TreeView_SelectSetFirstVisible(m_wnd_tv, item);

            SCROLLINFO after{};
            after.cbSize = sizeof(after);
            after.fMask = SIF_POS;

            if (had_horizontal && GetScrollInfo(m_wnd_tv, SB_HORZ, &after) && after.nPos != horizontal.nPos)
                SendMessage(m_wnd_tv, WM_HSCROLL,
                    MAKEWPARAM(SB_THUMBTRACK, std::clamp(horizontal.nPos, 0, thin_max_scroll_message_position)), 0);

            SendMessage(m_wnd_tv, WM_SETREDRAW, TRUE, 0);
            RedrawWindow(m_wnd_tv, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
        }
    } else {
        // 水平軸的單位是像素；實測（獨立的 TreeView）WM_HSCROLL(SB_THUMBTRACK, 位置) 會直接跳到那個像素位置，不用像垂直那樣繞路
        SendMessage(m_wnd_tv, WM_HSCROLL,
            MAKEWPARAM(SB_THUMBTRACK, std::clamp(position, 0, thin_max_scroll_message_position)), 0);
    }

    update_thin_overlay(axis);
}

// 計時器每次響：游標還在軌道上、而且在滑塊前面或後面，就再翻一頁；滑塊追到游標底下就先不翻（游標移開滑塊又會繼續）
void AlbumListWindow::repeat_thin_track_page(ThinAxis axis)
{
    const auto geometry = get_thin_scrollbar_geometry(axis);

    if (!geometry || !m_wnd_tv)
        return;

    POINT pt{};
    GetCursorPos(&pt);
    ScreenToClient(m_wnd_tv, &pt);

    if (!PtInRect(&geometry->strip, pt))
        return;

    const auto vertical = axis == ThinAxis::vertical;
    const auto along = vertical ? pt.y : pt.x;
    const auto thumb_start = vertical ? geometry->thumb.top : geometry->thumb.left;
    const auto thumb_end = vertical ? geometry->thumb.bottom : geometry->thumb.right;
    const auto message = vertical ? WM_VSCROLL : WM_HSCROLL;

    if (along < thumb_start)
        SendMessage(m_wnd_tv, message, MAKEWPARAM(vertical ? SB_PAGEUP : SB_PAGELEFT, 0), 0);
    else if (along >= thumb_end)
        SendMessage(m_wnd_tv, message, MAKEWPARAM(vertical ? SB_PAGEDOWN : SB_PAGERIGHT, 0), 0);

    update_thin_overlay(axis);
}

void AlbumListWindow::end_thin_track_hold(ThinAxis axis)
{
    auto& state = thin_axis(axis);

    if (!state.track_holding)
        return;

    state.track_holding = false;

    if (state.overlay) {
        KillTimer(state.overlay, thin_repeat_timer_id);

        if (GetCapture() == state.overlay)
            ReleaseCapture();
    }

    update_thin_overlay(axis);
}

void AlbumListWindow::end_thin_scrollbar_drag(ThinAxis axis, bool release_capture)
{
    auto& state = thin_axis(axis);

    if (!state.dragging)
        return;

    state.dragging = false;

    if (release_capture && state.overlay && GetCapture() == state.overlay)
        ReleaseCapture();

    // 放開後依游標目前的位置決定要不要維持「靠近」的樣子
    POINT pt{};
    GetCursorPos(&pt);

    RECT overlay_rect{};
    const auto over_overlay = state.overlay && state.overlay_shown && GetWindowRect(state.overlay, &overlay_rect)
        && PtInRect(&overlay_rect, pt);

    // 放開時游標已經在滑軌外面：淡出（不是直接消失）
    if (!over_overlay)
        start_thin_fade(axis);

    state.pointer_near = false;
    state.thumb_hot = false;

    if (over_overlay && m_wnd_tv) {
        ScreenToClient(m_wnd_tv, &pt);
        update_thin_scrollbar_hover(axis, pt);
    }

    update_thin_overlay(axis);
}

LRESULT AlbumListWindow::on_thin_overlay_message(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    // 這個浮層是垂直的還是水平的
    const auto axis = wnd == thin_axis(ThinAxis::horizontal).overlay ? ThinAxis::horizontal : ThinAxis::vertical;
    const auto vertical = axis == ThinAxis::vertical;
    auto& state = thin_axis(axis);

    // 浮層的座標換成樹視窗工作區的座標
    const auto to_tree = [&state](LPARAM l) {
        return POINT{GET_X_LPARAM(l) + state.overlay_state.strip.left, GET_Y_LPARAM(l) + state.overlay_state.strip.top};
    };

    switch (msg) {
    case WM_SETCURSOR: {
        static const auto cursor = LoadCursor(nullptr, IDC_ARROW);
        SetCursor(cursor);
        return TRUE;
    }
    case WM_MOUSEMOVE: {
        const auto pt = to_tree(lp);

        if (state.dragging) {
            drag_thin_scrollbar_to(axis, pt);
            return 0;
        }

        update_thin_scrollbar_hover(axis, pt);
        return 0;
    }
    case WM_MOUSELEAVE:
        state.tracking_leave = false;

        if (!state.dragging && (state.pointer_near || state.thumb_hot)) {
            start_thin_fade(axis);
            state.pointer_near = false;
            state.thumb_hot = false;
            update_thin_overlay(axis);
        }

        return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        const auto pt = to_tree(lp);
        const auto geometry = get_thin_scrollbar_geometry(axis);

        if (!geometry || !m_wnd_tv)
            return 0;

        const auto along = vertical ? pt.y : pt.x;
        const auto thumb_start = vertical ? geometry->thumb.top : geometry->thumb.left;
        const auto thumb_end = vertical ? geometry->thumb.bottom : geometry->thumb.right;

        // 這條邊緣是滑軌：不選取底下的項目、不開始拖放，也不搶焦點（跟原生滑軌一樣）
        state.pointer_near = true;

        if (along >= thumb_start && along < thumb_end) {
            state.dragging = true;
            state.drag_grab = along - thumb_start;
            SetCapture(wnd);
        } else {
            // 軌道空白處：先翻一頁，按住不放就連續翻（見 repeat_thin_track_page）
            const auto before = along < thumb_start;
            SendMessage(m_wnd_tv, vertical ? WM_VSCROLL : WM_HSCROLL,
                MAKEWPARAM(vertical ? (before ? SB_PAGEUP : SB_PAGEDOWN) : (before ? SB_PAGELEFT : SB_PAGERIGHT), 0), 0);
            state.track_holding = true;
            SetCapture(wnd);
            SetTimer(wnd, thin_repeat_timer_id, thin_repeat_first_delay_ms, nullptr);
        }

        update_thin_overlay(axis);
        return 0;
    }
    case WM_TIMER:
        if (wp == thin_fade_timer_id) {
            update_thin_overlay(axis);
            return 0;
        }

        if (wp == thin_repeat_timer_id && state.track_holding) {
            SetTimer(wnd, thin_repeat_timer_id, thin_repeat_interval_ms, nullptr);
            repeat_thin_track_page(axis);
            return 0;
        }

        return DefWindowProc(wnd, msg, wp, lp);
    case WM_LBUTTONUP:
        end_thin_scrollbar_drag(axis, true);
        end_thin_track_hold(axis);
        return 0;
    case WM_CAPTURECHANGED:
    case WM_CANCELMODE:
        end_thin_scrollbar_drag(axis, false);
        end_thin_track_hold(axis);
        return 0;
    default:
        return DefWindowProc(wnd, msg, wp, lp);
    }
}
