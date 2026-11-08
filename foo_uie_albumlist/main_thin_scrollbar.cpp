#include "stdafx.h"

#include "main.h"

#pragma comment(lib, "msimg32.lib")

// Nocturne：資料夾樹的細捲軸（2026-11-08）。做法與限制見 main.h 的說明；畫法與數值跟 Columns UI 播放清單（ui_helpers 的 list_view_scroll.cpp）一致。
// 這裡的捲動單位是「列」（SysTreeView32 的捲軸範圍是可見的列數，不是像素）

namespace {

constexpr char thin_scrollbar_config_key[] = "nocturne.thinScrollbar";
constexpr bool thin_scrollbar_default = true;

// 單位是 96 DPI 的像素，執行時依系統縮放換算
constexpr int thin_strip_width = 16;
constexpr int thin_thumb_right_inset = 3;
constexpr int thin_thumb_rest_width = 4;
constexpr int thin_thumb_near_width = 10;
constexpr int thin_thumb_min_height = 36;

// 不透明度（0～255）：平常 .24、靠近 .42、滑到滑塊上或拖曳中 .62；靠近時整條軌道淡淡鋪一層 .055
constexpr BYTE thin_alpha_rest = 61;
constexpr BYTE thin_alpha_near = 107;
constexpr BYTE thin_alpha_hot = 158;
constexpr BYTE thin_alpha_track = 14;

// 把一個顏色以指定不透明度疊到 dc 的 rc 範圍上；pill 為真時畫成兩端全圓的膠囊（邊緣用涵蓋率做抗鋸齒）
void blend_colour(HDC dc, const RECT& rc, COLORREF colour, BYTE alpha, bool pill)
{
    const auto width = rc.right - rc.left;
    const auto height = rc.bottom - rc.top;

    if (width <= 0 || height <= 0 || alpha == 0)
        return;

    const auto source_width = pill ? width : 1;
    const auto source_height = pill ? height : 1;

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = source_width;
    info.bmiHeader.biHeight = -source_height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void* bits{};
    wil::unique_hbitmap bitmap{CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0)};

    if (!bitmap || !bits)
        return;

    const auto red = GetRValue(colour);
    const auto green = GetGValue(colour);
    const auto blue = GetBValue(colour);

    const auto write_pixel = [&](uint32_t* pixel, float coverage) {
        // AlphaBlend 要的是預乘的 ARGB
        const auto a = static_cast<uint32_t>(std::lround(alpha * std::clamp(coverage, 0.f, 1.f)));
        *pixel = (a << 24) | (((red * a + 127) / 255) << 16) | (((green * a + 127) / 255) << 8) | ((blue * a + 127) / 255);
    };

    auto* pixels = static_cast<uint32_t*>(bits);

    if (!pill) {
        write_pixel(pixels, 1.f);
    } else {
        const auto half_width = static_cast<float>(width) * .5f;
        const auto half_height = static_cast<float>(height) * .5f;
        const auto radius = std::min(half_width, half_height);

        for (int y = 0; y < height; ++y) {
            const auto py = std::abs(static_cast<float>(y) + .5f - half_height) - (half_height - radius);

            for (int x = 0; x < width; ++x) {
                const auto px = std::abs(static_cast<float>(x) + .5f - half_width) - (half_width - radius);
                const auto distance
                    = std::hypot(std::max(px, 0.f), std::max(py, 0.f)) + std::min(std::max(px, py), 0.f) - radius;
                write_pixel(pixels + static_cast<size_t>(y) * width + x, .5f - distance);
            }
        }
    }

    wil::unique_hdc memory_dc{CreateCompatibleDC(dc)};

    if (!memory_dc)
        return;

    const auto _ = wil::SelectObject(memory_dc.get(), bitmap.get());

    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;

    AlphaBlend(dc, rc.left, rc.top, width, height, memory_dc.get(), 0, 0, source_width, source_height, blend);
}

} // namespace

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

    if (!m_wnd_tv)
        return;

    // 重排樹的寬度（開：多出原生捲軸的寬度讓它被裁掉；關：還原），再把原生捲軸與整棵樹重畫
    on_size();
    RedrawWindow(m_wnd_tv, nullptr, nullptr, RDW_INVALIDATE | RDW_FRAME | RDW_UPDATENOW);
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

// 樹自己畫完之後呼叫，直接把滑軌疊畫在樹視窗上（WM_PAINT 之前已把整條軌道併進要重畫的範圍，所以底下是新畫的、不會疊兩次透明度）
void AlbumListWindow::paint_thin_scrollbar() const
{
    const auto geometry = get_thin_scrollbar_geometry();

    if (!geometry || !m_item_colours)
        return;

    const auto dc = GetDC(m_wnd_tv);

    if (!dc)
        return;

    const auto release = gsl::finally([this, dc] { ReleaseDC(m_wnd_tv, dc); });

    IntersectClipRect(dc, geometry->strip.left, geometry->strip.top, geometry->strip.right, geometry->strip.bottom);

    const auto text_colour = m_item_colours->get_colour(cui::colours::colour_text);
    const auto is_near = m_thin_near || m_thin_dragging;

    if (is_near)
        blend_colour(dc, geometry->strip, text_colour, thin_alpha_track, false);

    const auto alpha = m_thin_dragging || m_thin_thumb_hot ? thin_alpha_hot
        : is_near                                          ? thin_alpha_near
                                                           : thin_alpha_rest;
    blend_colour(dc, geometry->thumb, text_colour, alpha, true);
}

void AlbumListWindow::invalidate_thin_scrollbar() const
{
    if (!m_wnd_tv)
        return;

    RECT client{};
    GetClientRect(m_wnd_tv, &client);
    const RECT strip{client.right - uih::scale_dpi_value(thin_strip_width), client.top, client.right, client.bottom};
    InvalidateRect(m_wnd_tv, &strip, FALSE);
}

void AlbumListWindow::update_thin_scrollbar_hover(POINT pt)
{
    const auto geometry = get_thin_scrollbar_geometry();
    const bool in_strip = geometry && PtInRect(&geometry->strip, pt);

    // 滑塊的「熱區」用靠近時的寬度算，y 在滑塊範圍內就算（不用精準點到 4 px 細條）
    const bool on_thumb = in_strip && pt.y >= geometry->thumb.top && pt.y < geometry->thumb.bottom
        && pt.x >= geometry->strip.right - uih::scale_dpi_value(thin_thumb_right_inset + thin_thumb_near_width);

    if (in_strip && !m_thin_tracking_leave) {
        TRACKMOUSEEVENT track_mouse_event{sizeof(TRACKMOUSEEVENT), TME_LEAVE, m_wnd_tv, 0};
        m_thin_tracking_leave = TrackMouseEvent(&track_mouse_event) != 0;
    }

    if (in_strip == m_thin_near && on_thumb == m_thin_thumb_hot)
        return;

    m_thin_near = in_strip;
    m_thin_thumb_hot = on_thumb;
    invalidate_thin_scrollbar();
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

    invalidate_thin_scrollbar();
}

void AlbumListWindow::end_thin_scrollbar_drag(bool release_capture)
{
    if (!m_thin_dragging)
        return;

    m_thin_dragging = false;

    if (release_capture && m_wnd_tv && GetCapture() == m_wnd_tv)
        ReleaseCapture();

    if (!m_wnd_tv)
        return;

    SendMessage(m_wnd_tv, WM_VSCROLL, MAKEWPARAM(SB_ENDSCROLL, 0), 0);

    // 放開後依游標目前的位置決定要不要維持「靠近」的樣子
    POINT pt{};
    GetCursorPos(&pt);
    ScreenToClient(m_wnd_tv, &pt);

    m_thin_near = false;
    m_thin_thumb_hot = false;
    update_thin_scrollbar_hover(pt);
    invalidate_thin_scrollbar();
}

std::optional<LRESULT> AlbumListWindow::on_thin_scrollbar_message(HWND wnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_MOUSEMOVE: {
        const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};

        if (m_thin_dragging) {
            drag_thin_scrollbar_to(pt.y);
            return 0;
        }

        // 按著左鍵（框選、拖放項目）時不理會滑軌的懸停
        if ((wp & (MK_LBUTTON | MK_RBUTTON)) == 0)
            update_thin_scrollbar_hover(pt);

        return std::nullopt;
    }
    case WM_MOUSELEAVE:
        m_thin_tracking_leave = false;

        if (!m_thin_dragging && (m_thin_near || m_thin_thumb_hot)) {
            m_thin_near = false;
            m_thin_thumb_hot = false;
            invalidate_thin_scrollbar();
        }

        return std::nullopt;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        const auto geometry = get_thin_scrollbar_geometry();

        if (!geometry || !PtInRect(&geometry->strip, pt))
            return std::nullopt;

        // 右邊緣這條是滑軌：不選取底下的項目、不開始拖放，也不搶焦點（跟原生滑軌一樣）
        m_thin_near = true;

        if (pt.y >= geometry->thumb.top && pt.y < geometry->thumb.bottom) {
            m_thin_dragging = true;
            m_thin_drag_grab = pt.y - geometry->thumb.top;
            SetCapture(wnd);
        } else {
            SendMessage(wnd, WM_VSCROLL, MAKEWPARAM(pt.y < geometry->thumb.top ? SB_PAGEUP : SB_PAGEDOWN, 0), 0);
        }

        invalidate_thin_scrollbar();
        return 0;
    }
    case WM_LBUTTONUP:
        if (m_thin_dragging) {
            end_thin_scrollbar_drag(true);
            return 0;
        }

        return std::nullopt;
    case WM_CAPTURECHANGED:
    case WM_CANCELMODE:
        end_thin_scrollbar_drag(false);
        return std::nullopt;
    default:
        return std::nullopt;
    }
}
