#include "stdafx.h"

#include "main.h"

// Nocturne：資料夾樹的細捲軸（2026-11-08）。做法與限制見 main.h 的說明；數值跟 Columns UI 播放清單（ui_helpers 的 list_view_scroll.cpp）一致。
// 這裡的捲動單位是「列」（SysTreeView32 的捲軸範圍是可見的列數，不是像素）。
//
// 滑軌畫在一個獨立的透明浮層子視窗（WS_EX_LAYERED、逐像素 alpha，由 DWM 合成），不畫在樹自己的畫面上：
// 第一版畫在樹上，樹捲動（ScrollWindow）會把舊的滑軌像素一起捲走、重繪又晚一步，拖曳時看得到鬼影與抖動（使用者回報）。
// 整條軌道（16 DIP 寬）都是 alpha＝1 的「看不見但吃得到滑鼠」的像素，滑鼠靠近、按下、拖曳都由浮層自己接；靠近時不鋪軌道底色。

namespace {

constexpr char thin_scrollbar_config_key[] = "nocturne.thinScrollbar";
constexpr bool thin_scrollbar_default = true;

// 單位是 96 DPI 的像素，執行時依系統縮放換算
constexpr int thin_strip_width = 16;
constexpr int thin_thumb_right_inset = 3;
constexpr int thin_thumb_rest_width = 4;
constexpr int thin_thumb_near_width = 10;
constexpr int thin_thumb_min_height = 36;

// 不透明度（0～255）：平常 .24、靠近 .42、滑到滑塊上或拖曳中 .62
constexpr BYTE thin_alpha_rest = 61;
constexpr BYTE thin_alpha_near = 107;
constexpr BYTE thin_alpha_hot = 158;

// 按住軌道空白處連續翻頁：第一次延遲 400 ms，之後每 50 ms 翻一頁（跟原生滑軌的手感接近）
constexpr UINT_PTR thin_repeat_timer_id = 1;
constexpr UINT thin_repeat_first_delay_ms = 400;
constexpr UINT thin_repeat_interval_ms = 50;

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
    }

    s_thin_scrollbar_enabled = read_setting();
}

void AlbumListWindow::apply_thin_scrollbar()
{
    const auto wanted = s_thin_scrollbar_enabled && m_wnd_tv != nullptr;

    if (wanted == m_thin_active)
        return;

    end_thin_scrollbar_drag(true);

    m_thin_active = wanted;
    m_thin_near = false;
    m_thin_thumb_hot = false;

    // 關掉就把浮層拿掉；開啟（含重建樹之後）要重新放到最上層，免得被新建的樹蓋住
    if (!wanted)
        destroy_thin_overlay();
    else
        m_thin_overlay_shown = false;

    if (!m_wnd_tv)
        return;

    // 重排樹的寬度（開：多出原生捲軸的寬度讓它被裁掉；關：還原），再把原生捲軸與整棵樹重畫
    on_size();
    RedrawWindow(m_wnd_tv, nullptr, nullptr, RDW_INVALIDATE | RDW_FRAME | RDW_UPDATENOW);
    update_thin_overlay();
}

int AlbumListWindow::get_thin_extra_width() const
{
    if (!m_thin_active || !m_wnd_tv)
        return 0;

    // 沒有垂直捲軸（內容放得下）就不用把它擠到面板外面
    if (!(GetWindowLongPtr(m_wnd_tv, GWL_STYLE) & WS_VSCROLL))
        return 0;

    // 原生捲軸可見時量它實際的寬度（視窗所在螢幕的縮放不一定跟系統縮放相同）；量不到就沿用上次量到的或系統值
    SCROLLBARINFO info{};
    info.cbSize = sizeof(info);

    if (GetScrollBarInfo(m_wnd_tv, OBJID_VSCROLL, &info) && !(info.rgstate[0] & STATE_SYSTEM_INVISIBLE)) {
        const auto width = info.rcScrollBar.right - info.rcScrollBar.left;

        if (width > 0)
            m_thin_bar_width = width;
    }

    return m_thin_bar_width > 0 ? m_thin_bar_width : GetSystemMetrics(SM_CXVSCROLL);
}

// 原生捲軸出現或消失後（新增節點、展開收合、縮放視窗），樹視窗的寬度也要跟著換：有捲軸時多出捲軸寬度、沒有時剛好等於面板寬
void AlbumListWindow::sync_thin_scrollbar_layout()
{
    if (!m_thin_active || m_thin_adjusting || !m_wnd_tv)
        return;

    RECT panel_rect{};
    GetClientRect(get_wnd(), &panel_rect);

    RECT tree_rect{};
    GetWindowRect(m_wnd_tv, &tree_rect);

    if (tree_rect.right - tree_rect.left == panel_rect.right + get_thin_extra_width())
        return;

    pfc::vartoggle_t guard(m_thin_adjusting, true);
    on_size();
}

std::optional<AlbumListWindow::ThinScrollbarGeometry> AlbumListWindow::get_thin_scrollbar_geometry() const
{
    if (!m_thin_active || !m_wnd_tv)
        return std::nullopt;

    SCROLLINFO info{};
    info.cbSize = sizeof(info);
    info.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;

    if (!GetScrollInfo(m_wnd_tv, SB_VERT, &info))
        return std::nullopt;

    const auto total = static_cast<int64_t>(info.nMax) - info.nMin + 1;

    if (info.nPage == 0 || total <= static_cast<int64_t>(info.nPage))
        return std::nullopt;

    RECT client{};
    GetClientRect(m_wnd_tv, &client);

    const int track_height = client.bottom - client.top;

    if (track_height <= 0)
        return std::nullopt;

    ThinScrollbarGeometry geometry{};
    geometry.track_top = client.top;
    geometry.track_height = track_height;
    geometry.range_min = info.nMin;
    geometry.position_range = static_cast<int>(total - static_cast<int64_t>(info.nPage));

    const auto proportional_height = static_cast<int>(static_cast<int64_t>(track_height) * info.nPage / total);
    geometry.thumb_height
        = std::clamp(proportional_height, std::min(uih::scale_dpi_value(thin_thumb_min_height), track_height), track_height);

    const auto position = std::clamp(info.nPos - info.nMin, 0, geometry.position_range);
    const auto room = track_height - geometry.thumb_height;
    const auto offset = room > 0 ? static_cast<int>(static_cast<int64_t>(room) * position / geometry.position_range) : 0;

    const auto width
        = uih::scale_dpi_value(m_thin_near || m_thin_dragging ? thin_thumb_near_width : thin_thumb_rest_width);
    const auto right = client.right - uih::scale_dpi_value(thin_thumb_right_inset);

    geometry.thumb = {right - width, client.top + offset, right, client.top + offset + geometry.thumb_height};
    geometry.strip = {client.right - uih::scale_dpi_value(thin_strip_width), client.top, client.right, client.bottom};
    return geometry;
}

void AlbumListWindow::destroy_thin_overlay()
{
    if (m_thin_overlay) {
        DestroyWindow(m_thin_overlay);
        m_thin_overlay = nullptr;
    }

    m_thin_overlay_shown = false;
    m_thin_tracking_leave = false;
    m_thin_track_holding = false;
}

// 依目前的捲動範圍、位置與滑鼠狀態更新浮層：沒有可捲的內容就藏起來；內容沒變就什麼都不做
void AlbumListWindow::update_thin_overlay()
{
    const auto panel = get_wnd();

    if (!panel || !m_wnd_tv)
        return;

    const auto geometry = m_thin_active ? get_thin_scrollbar_geometry() : std::nullopt;

    if (!geometry) {
        if (m_thin_overlay && m_thin_overlay_shown)
            ShowWindow(m_thin_overlay, SW_HIDE);

        m_thin_overlay_shown = false;
        return;
    }

    static const ThinOverlayWindowClass window_class(&AlbumListWindow::s_thin_overlay_proc);

    if (!window_class.is_registered() || !m_item_colours)
        return;

    const auto colour = m_item_colours->get_colour(cui::colours::colour_text);
    const BYTE alpha = m_thin_dragging || m_thin_thumb_hot ? thin_alpha_hot
        : m_thin_near                                      ? thin_alpha_near
                                                           : thin_alpha_rest;

    ThinOverlayState state{geometry->strip, geometry->thumb, colour, alpha};

    if (m_thin_overlay && m_thin_overlay_shown && state == m_thin_overlay_state)
        return;

    if (!m_thin_overlay) {
        m_thin_overlay = CreateWindowEx(WS_EX_LAYERED, thin_overlay_class_name, L"", WS_CHILD, 0, 0, 0, 0, panel,
            nullptr, core_api::get_my_instance(), nullptr);

        if (!m_thin_overlay)
            return;

        SetWindowLongPtr(m_thin_overlay, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        m_thin_overlay_shown = false;
    }

    const auto width = state.strip.right - state.strip.left;
    const auto height = state.strip.bottom - state.strip.top;

    if (width <= 0 || height <= 0)
        return;

    // 位置或大小變了（或第一次顯示）才動視窗；放最上層，免得被樹（後建立的兄弟視窗）蓋住
    if (!m_thin_overlay_shown || !EqualRect(&state.strip, &m_thin_overlay_state.strip)) {
        POINT origin{state.strip.left, state.strip.top};
        MapWindowPoints(m_wnd_tv, panel, &origin, 1);   // 樹的工作區座標 → 面板的工作區座標
        SetWindowPos(m_thin_overlay, HWND_TOP, origin.x, origin.y, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
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

    if (UpdateLayeredWindow(m_thin_overlay, screen_dc.get(), nullptr, &size, memory_dc.get(), &source, 0, &blend, ULW_ALPHA)) {
        m_thin_overlay_state = state;
        m_thin_overlay_shown = true;
    }
}

void AlbumListWindow::update_thin_scrollbar_hover(POINT pt)
{
    const auto geometry = get_thin_scrollbar_geometry();

    if (!geometry)
        return;

    // 滑塊的「熱區」用靠近時的寬度算，y 在滑塊範圍內就算（不用精準點到 4 px 細條）
    const bool on_thumb = pt.y >= geometry->thumb.top && pt.y < geometry->thumb.bottom
        && pt.x >= geometry->strip.right - uih::scale_dpi_value(thin_thumb_right_inset + thin_thumb_near_width);

    if (!m_thin_tracking_leave && m_thin_overlay) {
        TRACKMOUSEEVENT track_mouse_event{sizeof(TRACKMOUSEEVENT), TME_LEAVE, m_thin_overlay, 0};
        m_thin_tracking_leave = TrackMouseEvent(&track_mouse_event) != 0;
    }

    if (m_thin_near && on_thumb == m_thin_thumb_hot)
        return;

    m_thin_near = true;
    m_thin_thumb_hot = on_thumb;
    update_thin_overlay();
}

void AlbumListWindow::drag_thin_scrollbar_to(int mouse_y)
{
    const auto geometry = get_thin_scrollbar_geometry();

    if (!geometry)
        return;

    const auto room = geometry->track_height - geometry->thumb_height;

    if (room <= 0)
        return;

    const auto wanted_top = std::clamp(mouse_y - m_thin_drag_grab - geometry->track_top, 0, room);
    const auto row = geometry->range_min
        + static_cast<int>(std::llround(static_cast<double>(wanted_top) * static_cast<double>(geometry->position_range)
              / static_cast<double>(room)));

    // 合成的 WM_VSCROLL(SB_THUMBTRACK) 樹不理（它去讀原生捲軸自己的追蹤位置，實測拖了不會捲），
    // 所以改成：從目前第一個可見項目往上或往下走到目標那一列，再把它設成第一個可見項目（只捲動、不改選取）
    SCROLLINFO info{};
    info.cbSize = sizeof(info);
    info.fMask = SIF_POS;

    if (!GetScrollInfo(m_wnd_tv, SB_VERT, &info))
        return;

    if (row == info.nPos)
        return;

    auto item = TreeView_GetFirstVisible(m_wnd_tv);
    auto steps = row - info.nPos;
    const auto direction = steps > 0 ? TVGN_NEXTVISIBLE : TVGN_PREVIOUSVISIBLE;
    steps = std::abs(steps);

    for (; steps > 0 && item; --steps) {
        const auto next = TreeView_GetNextItem(m_wnd_tv, item, direction);

        if (!next)
            break;

        item = next;
    }

    if (item)
        TreeView_SelectSetFirstVisible(m_wnd_tv, item);

    update_thin_overlay();
}

// 計時器每次響：游標還在軌道上、而且在滑塊上方或下方，就再翻一頁；滑塊追到游標底下就先不翻（游標移開滑塊又會繼續）
void AlbumListWindow::repeat_thin_track_page()
{
    const auto geometry = get_thin_scrollbar_geometry();

    if (!geometry || !m_wnd_tv)
        return;

    POINT pt{};
    GetCursorPos(&pt);
    ScreenToClient(m_wnd_tv, &pt);

    if (!PtInRect(&geometry->strip, pt))
        return;

    if (pt.y < geometry->thumb.top)
        SendMessage(m_wnd_tv, WM_VSCROLL, MAKEWPARAM(SB_PAGEUP, 0), 0);
    else if (pt.y >= geometry->thumb.bottom)
        SendMessage(m_wnd_tv, WM_VSCROLL, MAKEWPARAM(SB_PAGEDOWN, 0), 0);

    update_thin_overlay();
}

void AlbumListWindow::end_thin_track_hold()
{
    if (!m_thin_track_holding)
        return;

    m_thin_track_holding = false;

    if (m_thin_overlay) {
        KillTimer(m_thin_overlay, thin_repeat_timer_id);

        if (GetCapture() == m_thin_overlay)
            ReleaseCapture();
    }

    update_thin_overlay();
}

void AlbumListWindow::end_thin_scrollbar_drag(bool release_capture)
{
    if (!m_thin_dragging)
        return;

    m_thin_dragging = false;

    if (release_capture && m_thin_overlay && GetCapture() == m_thin_overlay)
        ReleaseCapture();

    // 放開後依游標目前的位置決定要不要維持「靠近」的樣子
    POINT pt{};
    GetCursorPos(&pt);

    RECT overlay_rect{};
    const auto over_overlay = m_thin_overlay && m_thin_overlay_shown && GetWindowRect(m_thin_overlay, &overlay_rect)
        && PtInRect(&overlay_rect, pt);

    m_thin_near = false;
    m_thin_thumb_hot = false;

    if (over_overlay && m_wnd_tv) {
        ScreenToClient(m_wnd_tv, &pt);
        update_thin_scrollbar_hover(pt);
    }

    update_thin_overlay();
}

LRESULT AlbumListWindow::on_thin_overlay_message(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    // 浮層的座標換成樹視窗工作區的座標
    const auto to_tree = [this](LPARAM l) {
        return POINT{GET_X_LPARAM(l) + m_thin_overlay_state.strip.left, GET_Y_LPARAM(l) + m_thin_overlay_state.strip.top};
    };

    switch (msg) {
    case WM_SETCURSOR: {
        static const auto cursor = LoadCursor(nullptr, IDC_ARROW);
        SetCursor(cursor);
        return TRUE;
    }
    case WM_MOUSEMOVE: {
        const auto pt = to_tree(lp);

        if (m_thin_dragging) {
            drag_thin_scrollbar_to(pt.y);
            return 0;
        }

        update_thin_scrollbar_hover(pt);
        return 0;
    }
    case WM_MOUSELEAVE:
        m_thin_tracking_leave = false;

        if (!m_thin_dragging && (m_thin_near || m_thin_thumb_hot)) {
            m_thin_near = false;
            m_thin_thumb_hot = false;
            update_thin_overlay();
        }

        return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        const auto pt = to_tree(lp);
        const auto geometry = get_thin_scrollbar_geometry();

        if (!geometry || !m_wnd_tv)
            return 0;

        // 右邊緣這條是滑軌：不選取底下的項目、不開始拖放，也不搶焦點（跟原生滑軌一樣）
        m_thin_near = true;

        if (pt.y >= geometry->thumb.top && pt.y < geometry->thumb.bottom) {
            m_thin_dragging = true;
            m_thin_drag_grab = pt.y - geometry->thumb.top;
            SetCapture(wnd);
        } else {
            // 軌道空白處：先翻一頁，按住不放就連續翻（見 repeat_thin_track_page）
            SendMessage(m_wnd_tv, WM_VSCROLL, MAKEWPARAM(pt.y < geometry->thumb.top ? SB_PAGEUP : SB_PAGEDOWN, 0), 0);
            m_thin_track_holding = true;
            SetCapture(wnd);
            SetTimer(wnd, thin_repeat_timer_id, thin_repeat_first_delay_ms, nullptr);
        }

        update_thin_overlay();
        return 0;
    }
    case WM_TIMER:
        if (wp == thin_repeat_timer_id && m_thin_track_holding) {
            SetTimer(wnd, thin_repeat_timer_id, thin_repeat_interval_ms, nullptr);
            repeat_thin_track_page();
            return 0;
        }

        return DefWindowProc(wnd, msg, wp, lp);
    case WM_LBUTTONUP:
        end_thin_scrollbar_drag(true);
        end_thin_track_hold();
        return 0;
    case WM_CAPTURECHANGED:
    case WM_CANCELMODE:
        end_thin_scrollbar_drag(false);
        end_thin_track_hold();
        return 0;
    default:
        return DefWindowProc(wnd, msg, wp, lp);
    }
}
