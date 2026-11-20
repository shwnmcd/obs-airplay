#include "security-manager.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <util/base.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objidl.h>
using std::max;
using std::min;
#include <gdiplus.h>
#include <dwmapi.h>

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWA_BORDER_COLOR
#define DWMWA_BORDER_COLOR 34
#endif
#ifndef DWMWCP_ROUND
#define DWMWCP_ROUND 2
#endif
#endif

namespace obs_airplay {

namespace {

constexpr int kMaxDeniedBeforeCooldown = 3;
constexpr int kRateWindowSeconds = 120;
constexpr int kCooldownSeconds = 60;
constexpr int kApprovalTimeoutSeconds = 25;
constexpr int kPinAutoDismissSeconds = 90;

std::string sanitize_field(const std::string &s)
{
	std::string out = s;
	for (char &c : out) {
		if (c == '|' || c == '\r' || c == '\n') {
			c = ' ';
		}
	}
	return out;
}

#ifdef _WIN32

// Official OBS Studio "Yami" Dark Theme Palette
namespace obs_theme {
inline Gdiplus::Color BgWindow()    { return Gdiplus::Color(255, 39, 42, 51); }    // #272A33
inline Gdiplus::Color BgHeader()    { return Gdiplus::Color(255, 48, 52, 64); }    // #303440
inline Gdiplus::Color BgPanel()     { return Gdiplus::Color(255, 29, 32, 38); }    // #1D2026
inline Gdiplus::Color BgTile()      { return Gdiplus::Color(255, 34, 37, 46); }    // #22252E
inline Gdiplus::Color Border()      { return Gdiplus::Color(255, 60, 64, 77); }    // #3C404D
inline Gdiplus::Color BorderLight() { return Gdiplus::Color(255, 76, 82, 98); }    // #4C5262
inline Gdiplus::Color TextPrimary() { return Gdiplus::Color(255, 242, 243, 245); } // #F2F3F5
inline Gdiplus::Color TextMuted()   { return Gdiplus::Color(255, 164, 168, 181); } // #A4A8B5

// OBS Signature Primary Accent Blue
inline Gdiplus::Color Accent()      { return Gdiplus::Color(255, 40, 76, 184); }   // #284CB8
inline Gdiplus::Color AccentHover() { return Gdiplus::Color(255, 58, 95, 204); }   // #3A5FCC
inline Gdiplus::Color AccentPress() { return Gdiplus::Color(255, 30, 58, 146); }   // #1E3A92
inline Gdiplus::Color AccentLight() { return Gdiplus::Color(255, 108, 148, 255); } // #6C94FF

// OBS Neutral Button
inline Gdiplus::Color BtnBg()       { return Gdiplus::Color(255, 60, 64, 77); }    // #3C404D
inline Gdiplus::Color BtnHover()    { return Gdiplus::Color(255, 74, 79, 94); }    // #4A4F5E
inline Gdiplus::Color BtnPress()    { return Gdiplus::Color(255, 48, 52, 64); }    // #303440

// OBS Danger / Warning
inline Gdiplus::Color DangerBg()    { return Gdiplus::Color(255, 64, 34, 40); }    // #402228
inline Gdiplus::Color DangerHover() { return Gdiplus::Color(255, 182, 53, 59); }   // #B6353B
inline Gdiplus::Color DangerPress() { return Gdiplus::Color(255, 142, 38, 43); }   // #8E262B
inline Gdiplus::Color DangerText()  { return Gdiplus::Color(255, 242, 145, 149); } // #F29195
inline Gdiplus::Color DangerEdge()  { return Gdiplus::Color(255, 182, 53, 59); }   // #B6353B
inline Gdiplus::Color Warning()     { return Gdiplus::Color(255, 232, 168, 56); }  // #E8A838
inline Gdiplus::Color Success()     { return Gdiplus::Color(255, 59, 181, 120); }  // #3BB578
} // namespace obs_theme

constexpr const wchar_t *kPinWindowClass = L"ObsAirPlay2FAPinObsDarkV3";
constexpr const wchar_t *kApprovalWindowClass = L"ObsAirPlayApprovalObsDarkV3";

struct GdiplusRuntime {
	ULONG_PTR token = 0;
	bool ok = false;

	GdiplusRuntime()
	{
		Gdiplus::GdiplusStartupInput input;
		ok = (Gdiplus::GdiplusStartup(&token, &input, nullptr) == Gdiplus::Ok);
	}
	~GdiplusRuntime()
	{
		if (ok) {
			Gdiplus::GdiplusShutdown(token);
		}
	}
};

void ensure_gdiplus()
{
	static GdiplusRuntime runtime;
	(void)runtime;
}

std::wstring utf8_to_wide(const std::string &utf8)
{
	if (utf8.empty())
		return L"";
	int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
	if (needed <= 1)
		return L"";
	std::wstring wide(static_cast<size_t>(needed - 1), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &wide[0], needed);
	return wide;
}

// Finds OBS Studio's main top-level window in the current process (no Qt dependency needed)
HWND find_obs_main_window()
{
	struct Search {
		DWORD pid;
		HWND best;
		LONG best_area;
	} search = {GetCurrentProcessId(), nullptr, 0};

	EnumWindows(
		[](HWND hwnd, LPARAM lp) -> BOOL {
			auto *s = reinterpret_cast<Search *>(lp);
			DWORD wpid = 0;
			GetWindowThreadProcessId(hwnd, &wpid);
			if (wpid != s->pid)
				return TRUE;
			if (!IsWindowVisible(hwnd) || IsIconic(hwnd))
				return TRUE;
			if (GetWindow(hwnd, GW_OWNER) != nullptr)
				return TRUE;

			wchar_t cls[128] = {};
			GetClassNameW(hwnd, cls, 128);
			if (wcscmp(cls, kPinWindowClass) == 0 || wcscmp(cls, kApprovalWindowClass) == 0)
				return TRUE;

			RECT rc = {};
			if (GetWindowRect(hwnd, &rc)) {
				LONG w = rc.right - rc.left;
				LONG h = rc.bottom - rc.top;
				if (w >= 400 && h >= 300) {
					LONG area = w * h;
					if (area > s->best_area) {
						s->best_area = area;
						s->best = hwnd;
					}
				}
			}
			return TRUE;
		},
		reinterpret_cast<LPARAM>(&search));

	return search.best;
}

float get_window_dpi_scale(HWND hwnd)
{
	using GetDpiForWindowFn = UINT(WINAPI *)(HWND);
	static auto fn = reinterpret_cast<GetDpiForWindowFn>(
		reinterpret_cast<void *>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow")));
	if (fn && hwnd) {
		UINT dpi = fn(hwnd);
		if (dpi >= 96) {
			return static_cast<float>(dpi) / 96.0f;
		}
	}
	return 1.0f;
}

void apply_obs_dark_chrome(HWND hwnd, bool hide_from_capture)
{
	if (!hwnd)
		return;
	if (hide_from_capture) {
		SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE);
	}
	DWORD corner_pref = DWMWCP_ROUND;
	DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner_pref, sizeof(corner_pref));
	COLORREF border_color = RGB(60, 64, 77); // OBS Yami #3C404D border
	DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &border_color, sizeof(border_color));
}

// Compute popup position anchored INSIDE OBS Studio's window (or monitor work area if OBS is minimized)
void compute_anchored_position(HWND obs_hwnd, int popup_w, int popup_h, float scale, bool top_banner,
			       int &out_x, int &out_y)
{
	RECT anchor = {};
	bool have_obs = (obs_hwnd && IsWindow(obs_hwnd) && IsWindowVisible(obs_hwnd) && !IsIconic(obs_hwnd) &&
			 GetWindowRect(obs_hwnd, &anchor));

	if (!have_obs) {
		SystemParametersInfoW(SPI_GETWORKAREA, 0, &anchor, 0);
	}

	const int aw = anchor.right - anchor.left;
	const int ah = anchor.bottom - anchor.top;
	out_x = anchor.left + (aw - popup_w) / 2;

	if (top_banner) {
		// Inside OBS, right below the OBS menu bar / top toolbar
		out_y = anchor.top + static_cast<int>(std::round(56.0f * scale));
	} else {
		// Dead center inside OBS Studio's main window
		out_y = anchor.top + (ah - popup_h) / 2;
	}

	// Clamp to the monitor that contains OBS so it never clips off-screen
	HMONITOR mon = MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST);
	MONITORINFO mi = {};
	mi.cbSize = sizeof(mi);
	if (GetMonitorInfoW(mon, &mi)) {
		const int pad = static_cast<int>(std::round(12.0f * scale));
		out_x = std::max<int>(mi.rcWork.left + pad, std::min<int>(out_x, mi.rcWork.right - popup_w - pad));
		out_y = std::max<int>(mi.rcWork.top + pad, std::min<int>(out_y, mi.rcWork.bottom - popup_h - pad));
	}
}

void add_rounded_rect(Gdiplus::GraphicsPath &path, float x, float y, float w, float h, float r)
{
	float d = std::min(r * 2.0f, std::min(w, h));
	if (d <= 0.5f) {
		path.AddRectangle(Gdiplus::RectF(x, y, w, h));
		return;
	}
	path.AddArc(x, y, d, d, 180.0f, 90.0f);
	path.AddArc(x + w - d, y, d, d, 270.0f, 90.0f);
	path.AddArc(x + w - d, y + h - d, d, d, 0.0f, 90.0f);
	path.AddArc(x, y + h - d, d, d, 90.0f, 90.0f);
	path.CloseFigure();
}

void fill_rounded_rect(Gdiplus::Graphics &g, const Gdiplus::Brush &brush,
		       float x, float y, float w, float h, float r)
{
	Gdiplus::GraphicsPath path;
	add_rounded_rect(path, x, y, w, h, r);
	g.FillPath(&brush, &path);
}

void stroke_rounded_rect(Gdiplus::Graphics &g, const Gdiplus::Pen &pen,
			 float x, float y, float w, float h, float r)
{
	Gdiplus::GraphicsPath path;
	add_rounded_rect(path, x, y, w, h, r);
	g.DrawPath(&pen, &path);
}

void draw_shield_icon(Gdiplus::Graphics &g, float cx, float cy, float size,
		      const Gdiplus::Color &badge_bg, const Gdiplus::Color &stroke_col)
{
	Gdiplus::SolidBrush badge_brush(badge_bg);
	fill_rounded_rect(g, badge_brush, cx - size * 0.5f, cy - size * 0.5f, size, size, 6.0f);

	float s = size * 0.28f;
	Gdiplus::PointF pts[6] = {
		{cx, cy - s * 1.15f},
		{cx + s * 0.92f, cy - s * 0.65f},
		{cx + s * 0.82f, cy + s * 0.35f},
		{cx, cy + s * 1.15f},
		{cx - s * 0.82f, cy + s * 0.35f},
		{cx - s * 0.92f, cy - s * 0.65f},
	};
	Gdiplus::GraphicsPath shield;
	shield.AddPolygon(pts, 6);

	Gdiplus::Pen pen(stroke_col, std::max(1.6f, size * 0.07f));
	pen.SetLineJoin(Gdiplus::LineJoinRound);
	g.DrawPath(&pen, &shield);
}

void draw_privacy_badge(Gdiplus::Graphics &g, float right_x, float center_y, float scale, bool hidden)
{
	const wchar_t *label = hidden ? L"Capture-Safe" : L"Visible";
	Gdiplus::Font font(L"Segoe UI Semibold", 10.5f * scale, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
	Gdiplus::RectF bounds;
	g.MeasureString(label, -1, &font, Gdiplus::PointF(0, 0), &bounds);

	float pill_h = 20.0f * scale;
	float dot_d = 6.0f * scale;
	float pill_w = bounds.Width + 20.0f * scale;
	float x = right_x - pill_w;
	float y = center_y - pill_h * 0.5f;

	Gdiplus::SolidBrush bg(obs_theme::BgPanel());
	Gdiplus::Pen border(obs_theme::Border(), 1.0f);
	fill_rounded_rect(g, bg, x, y, pill_w, pill_h, 4.0f * scale);
	stroke_rounded_rect(g, border, x, y, pill_w, pill_h, 4.0f * scale);

	Gdiplus::SolidBrush dot(hidden ? obs_theme::Success() : obs_theme::Warning());
	g.FillEllipse(&dot, x + 7.0f * scale, center_y - dot_d * 0.5f, dot_d, dot_d);

	Gdiplus::SolidBrush txt(obs_theme::TextMuted());
	Gdiplus::StringFormat sf;
	sf.SetAlignment(Gdiplus::StringAlignmentNear);
	sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
	g.DrawString(label, -1, &font,
		     Gdiplus::RectF(x + 16.0f * scale, y, pill_w - 18.0f * scale, pill_h), &sf, &txt);
}

void draw_close_button(Gdiplus::Graphics &g, const Gdiplus::RectF &rc, bool hovered, float scale)
{
	if (hovered) {
		Gdiplus::SolidBrush hb(obs_theme::BtnHover());
		fill_rounded_rect(g, hb, rc.X, rc.Y, rc.Width, rc.Height, 4.0f * scale);
	}
	Gdiplus::Pen xpen(hovered ? obs_theme::TextPrimary() : obs_theme::TextMuted(), 1.6f * scale);
	xpen.SetStartCap(Gdiplus::LineCapRound);
	xpen.SetEndCap(Gdiplus::LineCapRound);
	float pad = rc.Width * 0.31f;
	g.DrawLine(&xpen, rc.X + pad, rc.Y + pad, rc.X + rc.Width - pad, rc.Y + rc.Height - pad);
	g.DrawLine(&xpen, rc.X + rc.Width - pad, rc.Y + pad, rc.X + pad, rc.Y + rc.Height - pad);
}

bool rect_contains(const Gdiplus::RectF &rc, float x, float y)
{
	return x >= rc.X && x <= (rc.X + rc.Width) && y >= rc.Y && y <= (rc.Y + rc.Height);
}

void sync_popup_to_obs_window(HWND popup_hwnd, HWND obs_hwnd, float scale, bool top_banner,
			      bool user_dragged, RECT &last_obs_rc, bool &was_hidden_for_minimize)
{
	if (!obs_hwnd || !IsWindow(obs_hwnd))
		return;

	if (IsIconic(obs_hwnd) || !IsWindowVisible(obs_hwnd)) {
		if (!was_hidden_for_minimize) {
			ShowWindow(popup_hwnd, SW_HIDE);
			was_hidden_for_minimize = true;
		}
		return;
	}

	if (was_hidden_for_minimize) {
		ShowWindow(popup_hwnd, SW_SHOWNOACTIVATE);
		was_hidden_for_minimize = false;
	}

	// Keep popup anchored inside OBS if the user moves/resizes OBS (unless manually dragged)
	RECT obs_rc = {};
	if (!user_dragged && GetWindowRect(obs_hwnd, &obs_rc)) {
		if (obs_rc.left != last_obs_rc.left || obs_rc.top != last_obs_rc.top ||
		    obs_rc.right != last_obs_rc.right || obs_rc.bottom != last_obs_rc.bottom) {
			last_obs_rc = obs_rc;
			RECT prc = {};
			GetWindowRect(popup_hwnd, &prc);
			int pw = prc.right - prc.left;
			int ph = prc.bottom - prc.top;
			int nx = 0, ny = 0;
			compute_anchored_position(obs_hwnd, pw, ph, scale, top_banner, nx, ny);
			SetWindowPos(popup_hwnd, nullptr, nx, ny, 0, 0,
				     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
		}
	}

	// Keep popup layered cleanly right above OBS when OBS is active (and not covering other apps)
	HWND fg = GetForegroundWindow();
	bool obs_or_self_active = (fg == obs_hwnd || fg == popup_hwnd || GetWindow(fg, GW_OWNER) == obs_hwnd);
	if (obs_or_self_active) {
		SetWindowPos(popup_hwnd, HWND_TOPMOST, 0, 0, 0, 0,
			     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
	} else {
		SetWindowPos(popup_hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
			     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
	}
}

// ---------------------------------------------------------------------------
// 1. OBS Dark (Yami) In-App 2FA Verification Code Card
// ---------------------------------------------------------------------------
constexpr UINT_PTR kPinAnimTimerId = 1001;

struct PinCardState {
	std::wstring digits;
	std::wstring server_subtitle;
	bool hidden_from_capture = true;
	HWND obs_hwnd = nullptr;
	RECT last_obs_rc = {};
	bool user_dragged = false;
	bool hidden_for_minimize = false;
	float scale = 1.0f;
	int alpha = 0;
	int elapsed_ms = 0;
	int total_ms = kPinAutoDismissSeconds * 1000;
	bool close_hovered = false;
	bool tracking_mouse = false;
	Gdiplus::RectF close_rc;
};

void render_pin_card(HWND hwnd, HDC hdc, PinCardState *st)
{
	RECT client;
	GetClientRect(hwnd, &client);
	const int w = client.right;
	const int h = client.bottom;
	if (w <= 0 || h <= 0 || !st)
		return;

	HDC mem_dc = CreateCompatibleDC(hdc);
	HBITMAP mem_bmp = CreateCompatibleBitmap(hdc, w, h);
	HGDIOBJ old_bmp = SelectObject(mem_dc, mem_bmp);

	{
		Gdiplus::Graphics g(mem_dc);
		g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
		g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);

		const float s = st->scale;
		const float fw = static_cast<float>(w);
		const float fh = static_cast<float>(h);

		// OBS Yami window base (#272A33)
		Gdiplus::SolidBrush bg(obs_theme::BgWindow());
		g.FillRectangle(&bg, 0.0f, 0.0f, fw, fh);

		// OBS Dock/Dialog Header Strip (#303440) + 1px divider
		const float hdr_h = 44.0f * s;
		Gdiplus::SolidBrush hdr_bg(obs_theme::BgHeader());
		g.FillRectangle(&hdr_bg, 0.0f, 0.0f, fw, hdr_h);

		Gdiplus::Pen divider(obs_theme::Border(), 1.0f);
		g.DrawLine(&divider, 0.0f, hdr_h, fw, hdr_h);

		const float hdr_cy = hdr_h * 0.5f;
		draw_shield_icon(g, 24.0f * s, hdr_cy, 24.0f * s,
				 obs_theme::Accent(), obs_theme::TextPrimary());

		Gdiplus::Font title_font(L"Segoe UI Semibold", 13.5f * s, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
		Gdiplus::SolidBrush title_brush(obs_theme::TextPrimary());
		Gdiplus::StringFormat sf_left;
		sf_left.SetAlignment(Gdiplus::StringAlignmentNear);
		sf_left.SetLineAlignment(Gdiplus::StringAlignmentCenter);
		g.DrawString(L"AirPlay 2FA Code", -1, &title_font,
			     Gdiplus::RectF(44.0f * s, 0.0f, 170.0f * s, hdr_h), &sf_left, &title_brush);

		st->close_rc = Gdiplus::RectF(fw - 34.0f * s, (hdr_h - 24.0f * s) * 0.5f, 24.0f * s, 24.0f * s);
		draw_close_button(g, st->close_rc, st->close_hovered, s);
		draw_privacy_badge(g, st->close_rc.X - 8.0f * s, hdr_cy, s, st->hidden_from_capture);

		// Recessed OBS panel well holding the 4 digit tiles
		const float well_x = 18.0f * s;
		const float well_y = hdr_h + 14.0f * s;
		const float well_w = fw - 36.0f * s;
		const float well_h = 92.0f * s;
		Gdiplus::SolidBrush well_bg(obs_theme::BgPanel());
		Gdiplus::Pen well_border(obs_theme::Border(), 1.0f);
		fill_rounded_rect(g, well_bg, well_x, well_y, well_w, well_h, 6.0f * s);
		stroke_rounded_rect(g, well_border, well_x, well_y, well_w, well_h, 6.0f * s);

		const size_t n = std::max<size_t>(1, st->digits.size());
		const float gap = 10.0f * s;
		const float pad = 12.0f * s;
		const float tile_w = (well_w - pad * 2.0f - gap * static_cast<float>(n - 1)) / static_cast<float>(n);
		const float tile_h = well_h - pad * 2.0f;
		const float tile_y = well_y + pad;

		Gdiplus::Font digit_font(L"Segoe UI", 38.0f * s, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
		Gdiplus::StringFormat sf_center;
		sf_center.SetAlignment(Gdiplus::StringAlignmentCenter);
		sf_center.SetLineAlignment(Gdiplus::StringAlignmentCenter);

		for (size_t i = 0; i < st->digits.size(); ++i) {
			float tx = well_x + pad + static_cast<float>(i) * (tile_w + gap);
			Gdiplus::SolidBrush tile_bg(obs_theme::BgWindow());
			Gdiplus::Pen tile_border(obs_theme::BorderLight(), 1.0f);
			fill_rounded_rect(g, tile_bg, tx, tile_y, tile_w, tile_h, 6.0f * s);
			stroke_rounded_rect(g, tile_border, tx, tile_y, tile_w, tile_h, 6.0f * s);

			wchar_t ch[2] = {st->digits[i], L'\0'};
			Gdiplus::SolidBrush digit_brush(obs_theme::TextPrimary());
			g.DrawString(ch, 1, &digit_font,
				     Gdiplus::RectF(tx, tile_y + 1.0f * s, tile_w, tile_h), &sf_center, &digit_brush);
		}

		// Subtitle footer
		Gdiplus::Font sub_font(L"Segoe UI", 12.0f * s, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
		Gdiplus::SolidBrush sub_brush(obs_theme::TextMuted());
		g.DrawString(st->server_subtitle.c_str(), -1, &sub_font,
			     Gdiplus::RectF(18.0f * s, well_y + well_h + 10.0f * s, fw - 36.0f * s, 22.0f * s),
			     &sf_center, &sub_brush);

		// OBS Signature Blue countdown bar along bottom edge
		float frac = 1.0f - static_cast<float>(st->elapsed_ms) / static_cast<float>(std::max(1, st->total_ms));
		frac = std::max(0.0f, std::min(1.0f, frac));
		const float bar_h = 3.0f * s;
		Gdiplus::SolidBrush track(obs_theme::BgPanel());
		g.FillRectangle(&track, 0.0f, fh - bar_h, fw, bar_h);
		Gdiplus::SolidBrush fill(obs_theme::AccentHover());
		g.FillRectangle(&fill, 0.0f, fh - bar_h, fw * frac, bar_h);
	}

	BitBlt(hdc, 0, 0, w, h, mem_dc, 0, 0, SRCCOPY);
	SelectObject(mem_dc, old_bmp);
	DeleteObject(mem_bmp);
	DeleteDC(mem_dc);
}

LRESULT CALLBACK pin_card_wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
	auto *st = reinterpret_cast<PinCardState *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

	switch (msg) {
	case WM_CREATE: {
		auto *cs = reinterpret_cast<CREATESTRUCTW *>(lparam);
		st = reinterpret_cast<PinCardState *>(cs->lpCreateParams);
		SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
		SetLayeredWindowAttributes(hwnd, 0, 0, LWA_ALPHA);
		SetTimer(hwnd, kPinAnimTimerId, 33, nullptr);
		return 0;
	}
	case WM_TIMER:
		if (wparam == kPinAnimTimerId && st) {
			if (st->alpha < 255) {
				st->alpha = std::min(255, st->alpha + 48);
				SetLayeredWindowAttributes(hwnd, 0, static_cast<BYTE>(st->alpha), LWA_ALPHA);
			}
			sync_popup_to_obs_window(hwnd, st->obs_hwnd, st->scale, true,
						 st->user_dragged, st->last_obs_rc, st->hidden_for_minimize);
			st->elapsed_ms += 33;
			if (st->elapsed_ms >= st->total_ms) {
				DestroyWindow(hwnd);
				return 0;
			}
			InvalidateRect(hwnd, nullptr, FALSE);
		}
		return 0;
	case WM_MOUSEMOVE:
		if (st) {
			if (!st->tracking_mouse) {
				TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, hwnd, 0};
				TrackMouseEvent(&tme);
				st->tracking_mouse = true;
			}
			float mx = static_cast<float>(static_cast<short>(LOWORD(lparam)));
			float my = static_cast<float>(static_cast<short>(HIWORD(lparam)));
			bool hov = rect_contains(st->close_rc, mx, my);
			if (hov != st->close_hovered) {
				st->close_hovered = hov;
				SetCursor(LoadCursorW(nullptr,
						      reinterpret_cast<LPCWSTR>(hov ? IDC_HAND : IDC_ARROW)));
				InvalidateRect(hwnd, nullptr, FALSE);
			}
		}
		return 0;
	case WM_MOUSELEAVE:
		if (st) {
			st->tracking_mouse = false;
			if (st->close_hovered) {
				st->close_hovered = false;
				InvalidateRect(hwnd, nullptr, FALSE);
			}
		}
		return 0;
	case WM_LBUTTONDOWN:
		if (st) {
			float mx = static_cast<float>(static_cast<short>(LOWORD(lparam)));
			float my = static_cast<float>(static_cast<short>(HIWORD(lparam)));
			if (rect_contains(st->close_rc, mx, my)) {
				DestroyWindow(hwnd);
				return 0;
			}
			st->user_dragged = true;
			ReleaseCapture();
			SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
		}
		return 0;
	case WM_KEYDOWN:
		if (wparam == VK_ESCAPE) {
			DestroyWindow(hwnd);
			return 0;
		}
		break;
	case WM_ERASEBKGND:
		return 1;
	case WM_PAINT: {
		PAINTSTRUCT ps;
		HDC hdc = BeginPaint(hwnd, &ps);
		render_pin_card(hwnd, hdc, st);
		EndPaint(hwnd, &ps);
		return 0;
	}
	case WM_CLOSE:
		DestroyWindow(hwnd);
		return 0;
	case WM_DESTROY:
		KillTimer(hwnd, kPinAnimTimerId);
		PostQuitMessage(0);
		return 0;
	default:
		break;
	}
	return DefWindowProcW(hwnd, msg, wparam, lparam);
}

// ---------------------------------------------------------------------------
// 2. OBS Dark (Yami) In-App Incoming Cast Request Approval Dialog
// ---------------------------------------------------------------------------
constexpr UINT_PTR kApprovalAnimTimerId = 2010;

enum class HoverTarget {
	None = 0,
	Close,
	AllowTrust,
	AllowOnce,
	Deny,
};

struct ApprovalCardState {
	std::wstring device_name;
	std::wstring device_meta;
	bool hidden_from_capture = true;
	HWND obs_hwnd = nullptr;
	RECT last_obs_rc = {};
	bool user_dragged = false;
	bool hidden_for_minimize = false;
	float scale = 1.0f;
	int alpha = 0;
	int elapsed_ms = 0;
	int total_ms = kApprovalTimeoutSeconds * 1000;
	ApprovalDecision decision = ApprovalDecision::Deny;

	HoverTarget hovered = HoverTarget::None;
	HoverTarget pressed = HoverTarget::None;
	bool tracking_mouse = false;

	Gdiplus::RectF close_rc;
	Gdiplus::RectF btn_trust_rc;
	Gdiplus::RectF btn_once_rc;
	Gdiplus::RectF btn_deny_rc;
};

void draw_obs_button(Gdiplus::Graphics &g, const Gdiplus::RectF &rc, const std::wstring &label,
		     const Gdiplus::Color &bg_col, const Gdiplus::Color &border_col,
		     const Gdiplus::Color &txt_col, float scale)
{
	const float r = 6.0f * scale; // OBS Yami crisp 6px button radius
	Gdiplus::SolidBrush bg(bg_col);
	fill_rounded_rect(g, bg, rc.X, rc.Y, rc.Width, rc.Height, r);

	if (border_col.GetA() > 0) {
		Gdiplus::Pen border(border_col, 1.0f);
		stroke_rounded_rect(g, border, rc.X, rc.Y, rc.Width, rc.Height, r);
	}

	Gdiplus::Font font(L"Segoe UI Semibold", 12.5f * scale, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
	Gdiplus::SolidBrush tb(txt_col);
	Gdiplus::StringFormat sf;
	sf.SetAlignment(Gdiplus::StringAlignmentCenter);
	sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
	g.DrawString(label.c_str(), -1, &font, rc, &sf, &tb);
}

void render_approval_card(HWND hwnd, HDC hdc, ApprovalCardState *st)
{
	RECT client;
	GetClientRect(hwnd, &client);
	const int w = client.right;
	const int h = client.bottom;
	if (w <= 0 || h <= 0 || !st)
		return;

	HDC mem_dc = CreateCompatibleDC(hdc);
	HBITMAP mem_bmp = CreateCompatibleBitmap(hdc, w, h);
	HGDIOBJ old_bmp = SelectObject(mem_dc, mem_bmp);

	{
		Gdiplus::Graphics g(mem_dc);
		g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
		g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);

		const float s = st->scale;
		const float fw = static_cast<float>(w);
		const float fh = static_cast<float>(h);

		// OBS Yami dialog surface (#272A33)
		Gdiplus::SolidBrush bg(obs_theme::BgWindow());
		g.FillRectangle(&bg, 0.0f, 0.0f, fw, fh);

		// OBS Dialog Header Strip (#303440) + 1px divider
		const float hdr_h = 46.0f * s;
		Gdiplus::SolidBrush hdr_bg(obs_theme::BgHeader());
		g.FillRectangle(&hdr_bg, 0.0f, 0.0f, fw, hdr_h);

		Gdiplus::Pen divider(obs_theme::Border(), 1.0f);
		g.DrawLine(&divider, 0.0f, hdr_h, fw, hdr_h);

		const float hdr_cy = hdr_h * 0.5f;
		draw_shield_icon(g, 26.0f * s, hdr_cy, 24.0f * s,
				 Gdiplus::Color(48, 232, 168, 56), obs_theme::Warning());

		Gdiplus::Font title_font(L"Segoe UI Semibold", 14.0f * s, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
		Gdiplus::SolidBrush title_brush(obs_theme::TextPrimary());
		Gdiplus::StringFormat sf_left;
		sf_left.SetAlignment(Gdiplus::StringAlignmentNear);
		sf_left.SetLineAlignment(Gdiplus::StringAlignmentCenter);
		sf_left.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
		sf_left.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
		g.DrawString(L"Incoming AirPlay Cast Request", -1, &title_font,
			     Gdiplus::RectF(46.0f * s, 0.0f, 230.0f * s, hdr_h), &sf_left, &title_brush);

		st->close_rc = Gdiplus::RectF(fw - 34.0f * s, (hdr_h - 24.0f * s) * 0.5f, 24.0f * s, 24.0f * s);
		draw_close_button(g, st->close_rc, st->hovered == HoverTarget::Close, s);
		draw_privacy_badge(g, st->close_rc.X - 8.0f * s, hdr_cy, s, st->hidden_from_capture);

		// OBS Recessed Device Info Panel (#1D2026)
		const float card_x = 18.0f * s;
		const float card_y = hdr_h + 16.0f * s;
		const float card_w = fw - 36.0f * s;
		const float card_h = 78.0f * s;

		Gdiplus::SolidBrush card_bg(obs_theme::BgPanel());
		Gdiplus::Pen card_border(obs_theme::Border(), 1.0f);
		fill_rounded_rect(g, card_bg, card_x, card_y, card_w, card_h, 6.0f * s);
		stroke_rounded_rect(g, card_border, card_x, card_y, card_w, card_h, 6.0f * s);

		// Crisp vector iPhone icon inside panel
		const float px = card_x + 18.0f * s;
		const float py = card_y + 18.0f * s;
		const float pw = 22.0f * s;
		const float ph = 40.0f * s;
		Gdiplus::Pen phone_pen(obs_theme::AccentLight(), 1.8f * s);
		stroke_rounded_rect(g, phone_pen, px, py, pw, ph, 4.5f * s);
		Gdiplus::SolidBrush notch(obs_theme::AccentLight());
		fill_rounded_rect(g, notch, px + pw * 0.3f, py + 4.0f * s, pw * 0.4f, 2.2f * s, 1.0f * s);

		Gdiplus::Font dev_font(L"Segoe UI Semibold", 15.5f * s, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
		Gdiplus::SolidBrush dev_brush(obs_theme::TextPrimary());
		g.DrawString(st->device_name.c_str(), -1, &dev_font,
			     Gdiplus::RectF(px + pw + 16.0f * s, card_y + 13.0f * s,
					    card_w - (pw + 46.0f * s), 26.0f * s),
			     &sf_left, &dev_brush);

		Gdiplus::Font meta_font(L"Segoe UI", 12.0f * s, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
		Gdiplus::SolidBrush meta_brush(obs_theme::TextMuted());
		g.DrawString(st->device_meta.c_str(), -1, &meta_font,
			     Gdiplus::RectF(px + pw + 16.0f * s, card_y + 41.0f * s,
					    card_w - (pw + 46.0f * s), 22.0f * s),
			     &sf_left, &meta_brush);

		// Prompt caption
		Gdiplus::Font hint_font(L"Segoe UI", 12.0f * s, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
		g.DrawString(L"Allow this device to cast its screen and audio to OBS Studio?", -1, &hint_font,
			     Gdiplus::RectF(20.0f * s, card_y + card_h + 12.0f * s, fw - 40.0f * s, 20.0f * s),
			     &sf_left, &meta_brush);

		// OBS Footer Button Bar (#303440 strip with 1px top border)
		const float bar_h = 3.0f * s;
		const float footer_h = 58.0f * s;
		const float footer_y = fh - footer_h - bar_h;
		Gdiplus::SolidBrush footer_bg(obs_theme::BgHeader());
		g.FillRectangle(&footer_bg, 0.0f, footer_y, fw, footer_h);
		g.DrawLine(&divider, 0.0f, footer_y, fw, footer_y);

		const float btn_h = 34.0f * s;
		const float btn_y = footer_y + (footer_h - btn_h) * 0.5f;
		const float btn_gap = 8.0f * s;
		const float total_btn_w = fw - 36.0f * s;
		const float w_trust = total_btn_w * 0.39f;
		const float w_once = total_btn_w * 0.30f;
		const float w_deny = total_btn_w - w_trust - w_once - btn_gap * 2.0f;

		st->btn_trust_rc = Gdiplus::RectF(18.0f * s, btn_y, w_trust, btn_h);
		st->btn_once_rc = Gdiplus::RectF(st->btn_trust_rc.X + w_trust + btn_gap, btn_y, w_once, btn_h);
		st->btn_deny_rc = Gdiplus::RectF(st->btn_once_rc.X + w_once + btn_gap, btn_y, w_deny, btn_h);

		int rem_sec = std::max(0, (st->total_ms - st->elapsed_ms + 999) / 1000);
		std::wstring deny_label = L"Deny (" + std::to_wstring(rem_sec) + L"s)";

		// Allow & Trust (OBS Primary Blue)
		Gdiplus::Color trust_bg = (st->pressed == HoverTarget::AllowTrust)
						  ? obs_theme::AccentPress()
						  : (st->hovered == HoverTarget::AllowTrust
							     ? obs_theme::AccentHover()
							     : obs_theme::Accent());
		draw_obs_button(g, st->btn_trust_rc, L"Allow & Trust", trust_bg,
				obs_theme::AccentHover(), obs_theme::TextPrimary(), s);

		// Allow Once (OBS Standard Dark Button)
		Gdiplus::Color once_bg = (st->pressed == HoverTarget::AllowOnce)
						 ? obs_theme::BtnPress()
						 : (st->hovered == HoverTarget::AllowOnce
							    ? obs_theme::BtnHover()
							    : obs_theme::BtnBg());
		draw_obs_button(g, st->btn_once_rc, L"Allow Once", once_bg,
				obs_theme::BorderLight(), obs_theme::TextPrimary(), s);

		// Deny (OBS Danger Button)
		bool deny_hov = (st->hovered == HoverTarget::Deny);
		Gdiplus::Color deny_bg = (st->pressed == HoverTarget::Deny)
						 ? obs_theme::DangerPress()
						 : (deny_hov ? obs_theme::DangerHover()
							     : obs_theme::DangerBg());
		Gdiplus::Color deny_txt = deny_hov ? obs_theme::TextPrimary() : obs_theme::DangerText();
		draw_obs_button(g, st->btn_deny_rc, deny_label, deny_bg,
				obs_theme::DangerEdge(), deny_txt, s);

		// Auto-deny progress bar at very bottom
		float frac = 1.0f - static_cast<float>(st->elapsed_ms) / static_cast<float>(std::max(1, st->total_ms));
		frac = std::max(0.0f, std::min(1.0f, frac));
		Gdiplus::SolidBrush track(obs_theme::BgPanel());
		g.FillRectangle(&track, 0.0f, fh - bar_h, fw, bar_h);
		Gdiplus::SolidBrush fill(obs_theme::DangerHover());
		g.FillRectangle(&fill, 0.0f, fh - bar_h, fw * frac, bar_h);
	}

	BitBlt(hdc, 0, 0, w, h, mem_dc, 0, 0, SRCCOPY);
	SelectObject(mem_dc, old_bmp);
	DeleteObject(mem_bmp);
	DeleteDC(mem_dc);
}

HoverTarget hit_test_approval(const ApprovalCardState *st, float x, float y)
{
	if (!st)
		return HoverTarget::None;
	if (rect_contains(st->close_rc, x, y))
		return HoverTarget::Close;
	if (rect_contains(st->btn_trust_rc, x, y))
		return HoverTarget::AllowTrust;
	if (rect_contains(st->btn_once_rc, x, y))
		return HoverTarget::AllowOnce;
	if (rect_contains(st->btn_deny_rc, x, y))
		return HoverTarget::Deny;
	return HoverTarget::None;
}

LRESULT CALLBACK approval_card_wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
	auto *st = reinterpret_cast<ApprovalCardState *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

	switch (msg) {
	case WM_CREATE: {
		auto *cs = reinterpret_cast<CREATESTRUCTW *>(lparam);
		st = reinterpret_cast<ApprovalCardState *>(cs->lpCreateParams);
		SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
		SetLayeredWindowAttributes(hwnd, 0, 0, LWA_ALPHA);
		SetTimer(hwnd, kApprovalAnimTimerId, 33, nullptr);
		return 0;
	}
	case WM_TIMER:
		if (wparam == kApprovalAnimTimerId && st) {
			if (st->alpha < 255) {
				st->alpha = std::min(255, st->alpha + 48);
				SetLayeredWindowAttributes(hwnd, 0, static_cast<BYTE>(st->alpha), LWA_ALPHA);
			}
			sync_popup_to_obs_window(hwnd, st->obs_hwnd, st->scale, false,
						 st->user_dragged, st->last_obs_rc, st->hidden_for_minimize);
			st->elapsed_ms += 33;
			if (st->elapsed_ms >= st->total_ms) {
				st->decision = ApprovalDecision::Deny;
				DestroyWindow(hwnd);
				return 0;
			}
			InvalidateRect(hwnd, nullptr, FALSE);
		}
		return 0;
	case WM_MOUSEMOVE:
		if (st) {
			if (!st->tracking_mouse) {
				TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, hwnd, 0};
				TrackMouseEvent(&tme);
				st->tracking_mouse = true;
			}
			float mx = static_cast<float>(static_cast<short>(LOWORD(lparam)));
			float my = static_cast<float>(static_cast<short>(HIWORD(lparam)));
			HoverTarget target = hit_test_approval(st, mx, my);
			if (target != st->hovered) {
				st->hovered = target;
				SetCursor(LoadCursorW(nullptr,
						      reinterpret_cast<LPCWSTR>(
							      target == HoverTarget::None ? IDC_ARROW : IDC_HAND)));
				InvalidateRect(hwnd, nullptr, FALSE);
			}
		}
		return 0;
	case WM_MOUSELEAVE:
		if (st) {
			st->tracking_mouse = false;
			st->hovered = HoverTarget::None;
			st->pressed = HoverTarget::None;
			InvalidateRect(hwnd, nullptr, FALSE);
		}
		return 0;
	case WM_LBUTTONDOWN:
		if (st) {
			float mx = static_cast<float>(static_cast<short>(LOWORD(lparam)));
			float my = static_cast<float>(static_cast<short>(HIWORD(lparam)));
			HoverTarget target = hit_test_approval(st, mx, my);
			if (target != HoverTarget::None) {
				st->pressed = target;
				SetCapture(hwnd);
				InvalidateRect(hwnd, nullptr, FALSE);
			} else {
				st->user_dragged = true;
				ReleaseCapture();
				SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
			}
		}
		return 0;
	case WM_LBUTTONUP:
		if (st && st->pressed != HoverTarget::None) {
			ReleaseCapture();
			float mx = static_cast<float>(static_cast<short>(LOWORD(lparam)));
			float my = static_cast<float>(static_cast<short>(HIWORD(lparam)));
			HoverTarget released = hit_test_approval(st, mx, my);
			HoverTarget clicked = (released == st->pressed) ? released : HoverTarget::None;
			st->pressed = HoverTarget::None;

			if (clicked == HoverTarget::AllowTrust) {
				st->decision = ApprovalDecision::AllowAndTrust;
				DestroyWindow(hwnd);
				return 0;
			} else if (clicked == HoverTarget::AllowOnce) {
				st->decision = ApprovalDecision::AllowOnce;
				DestroyWindow(hwnd);
				return 0;
			} else if (clicked == HoverTarget::Deny || clicked == HoverTarget::Close) {
				st->decision = ApprovalDecision::Deny;
				DestroyWindow(hwnd);
				return 0;
			}
			InvalidateRect(hwnd, nullptr, FALSE);
		}
		return 0;
	case WM_KEYDOWN:
		if ((wparam == VK_ESCAPE || wparam == VK_RETURN) && st) {
			st->decision = ApprovalDecision::Deny;
			DestroyWindow(hwnd);
			return 0;
		}
		break;
	case WM_ERASEBKGND:
		return 1;
	case WM_PAINT: {
		PAINTSTRUCT ps;
		HDC hdc = BeginPaint(hwnd, &ps);
		render_approval_card(hwnd, hdc, st);
		EndPaint(hwnd, &ps);
		return 0;
	}
	case WM_CLOSE:
		if (st) {
			st->decision = ApprovalDecision::Deny;
		}
		DestroyWindow(hwnd);
		return 0;
	case WM_DESTROY:
		KillTimer(hwnd, kApprovalAnimTimerId);
		PostQuitMessage(0);
		return 0;
	default:
		break;
	}
	return DefWindowProcW(hwnd, msg, wparam, lparam);
}

#endif // _WIN32

} // namespace

std::string SecurityManager::default_config_directory()
{
	std::filesystem::path dir;
#ifdef _WIN32
	const char *appdata = std::getenv("APPDATA");
	if (appdata && appdata[0] != '\0') {
		dir = std::filesystem::path(appdata) / "obs-studio" / "plugin_config" / "obs-airplay";
	} else {
		dir = std::filesystem::temp_directory_path() / "obs-airplay";
	}
#else
	const char *home = std::getenv("HOME");
	if (home && home[0] != '\0') {
		dir = std::filesystem::path(home) / ".config" / "obs-studio" / "plugin_config" / "obs-airplay";
	} else {
		dir = std::filesystem::temp_directory_path() / "obs-airplay";
	}
#endif
	return dir.string();
}

SecurityManager::SecurityManager(const std::string &config_dir)
{
	config_dir_ = config_dir.empty() ? default_config_directory() : config_dir;
	std::error_code ec;
	std::filesystem::create_directories(config_dir_, ec);

	trusted_file_path_ = (std::filesystem::path(config_dir_) / "trusted_devices.txt").string();
	server_key_path_ = (std::filesystem::path(config_dir_) / "airplay_server_key.pem").string();

	std::lock_guard<std::mutex> lock(mutex_);
	load_trusted_devices_locked();
}

SecurityManager::~SecurityManager()
{
	dismiss_pin_popup();
}

void SecurityManager::set_policy(const SecurityPolicy &policy)
{
	std::lock_guard<std::mutex> lock(mutex_);
	policy_ = policy;
}

SecurityPolicy SecurityManager::policy() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return policy_;
}

std::string SecurityManager::server_key_path() const
{
	return server_key_path_;
}

void SecurityManager::load_trusted_devices_locked()
{
	trusted_devices_.clear();
	std::ifstream in(trusted_file_path_);
	if (!in.is_open()) {
		return;
	}

	std::string line;
	while (std::getline(in, line)) {
		if (line.empty() || line[0] == '#')
			continue;

		std::vector<std::string> parts;
		std::stringstream ss(line);
		std::string item;
		while (std::getline(ss, item, '|')) {
			parts.push_back(item);
		}
		if (parts.empty() || parts[0].empty())
			continue;

		TrustedDevice dev;
		dev.device_id = parts[0];
		if (parts.size() > 1)
			dev.name = parts[1];
		if (parts.size() > 2)
			dev.model = parts[2];
		if (parts.size() > 3)
			dev.public_key_b64 = parts[3];
		trusted_devices_.push_back(std::move(dev));
	}
}

void SecurityManager::save_trusted_devices_locked() const
{
	std::error_code ec;
	std::filesystem::create_directories(config_dir_, ec);

	std::ofstream out(trusted_file_path_, std::ios::trunc);
	if (!out.is_open()) {
		blog(LOG_WARNING, "[obs-airplay:security] Unable to write trusted devices file: %s",
		     trusted_file_path_.c_str());
		return;
	}

	out << "# obs-airplay trusted devices (device_id|name|model|ed25519_pk_b64)\n";
	for (const auto &d : trusted_devices_) {
		out << sanitize_field(d.device_id) << "|"
		    << sanitize_field(d.name) << "|"
		    << sanitize_field(d.model) << "|"
		    << sanitize_field(d.public_key_b64) << "\n";
	}
}

bool SecurityManager::is_device_trusted(const std::string &device_id) const
{
	if (device_id.empty())
		return false;
	std::lock_guard<std::mutex> lock(mutex_);
	for (const auto &d : trusted_devices_) {
		if (d.device_id == device_id)
			return true;
	}
	return false;
}

bool SecurityManager::is_public_key_registered(const std::string &pk_b64) const
{
	if (pk_b64.empty())
		return false;
	std::lock_guard<std::mutex> lock(mutex_);
	for (const auto &d : trusted_devices_) {
		if (!d.public_key_b64.empty() && d.public_key_b64 == pk_b64)
			return true;
	}
	return false;
}

void SecurityManager::trust_device(const std::string &device_id,
				   const std::string &name,
				   const std::string &model,
				   const std::string &pk_b64)
{
	if (device_id.empty() && pk_b64.empty())
		return;

	std::lock_guard<std::mutex> lock(mutex_);
	for (auto &d : trusted_devices_) {
		if ((!device_id.empty() && d.device_id == device_id) ||
		    (!pk_b64.empty() && !d.public_key_b64.empty() && d.public_key_b64 == pk_b64)) {
			if (!device_id.empty())
				d.device_id = device_id;
			if (!name.empty())
				d.name = name;
			if (!model.empty())
				d.model = model;
			if (!pk_b64.empty())
				d.public_key_b64 = pk_b64;
			save_trusted_devices_locked();
			return;
		}
	}

	TrustedDevice dev;
	dev.device_id = device_id;
	dev.name = name.empty() ? "Apple Device" : name;
	dev.model = model;
	dev.public_key_b64 = pk_b64;
	trusted_devices_.push_back(std::move(dev));
	save_trusted_devices_locked();

	blog(LOG_INFO, "[obs-airplay:security] Trusted device saved: %s (%s)",
	     name.c_str(), device_id.c_str());
}

void SecurityManager::revoke_all_trusted()
{
	std::lock_guard<std::mutex> lock(mutex_);
	trusted_devices_.clear();
	rate_limits_.clear();
	save_trusted_devices_locked();

	std::error_code ec;
	std::filesystem::remove(server_key_path_, ec);
	blog(LOG_INFO, "[obs-airplay:security] Revoked all trusted devices and rotated pairing key.");
}

std::vector<TrustedDevice> SecurityManager::list_trusted_devices() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return trusted_devices_;
}

std::string SecurityManager::trusted_devices_summary() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (trusted_devices_.empty()) {
		return "Trusted Devices: None saved yet";
	}
	std::ostringstream oss;
	oss << "Trusted Devices (" << trusted_devices_.size() << "): ";
	for (size_t i = 0; i < trusted_devices_.size(); ++i) {
		if (i > 0)
			oss << ", ";
		oss << trusted_devices_[i].name << " [" << trusted_devices_[i].device_id << "]";
	}
	return oss.str();
}

bool SecurityManager::is_device_rate_limited(const std::string &device_id) const
{
	if (device_id.empty())
		return false;
	std::lock_guard<std::mutex> lock(mutex_);
	auto it = rate_limits_.find(device_id);
	if (it == rate_limits_.end())
		return false;
	return std::chrono::steady_clock::now() < it->second.blocked_until;
}

void SecurityManager::record_denied_attempt(const std::string &device_id)
{
	if (device_id.empty())
		return;
	std::lock_guard<std::mutex> lock(mutex_);
	const auto now = std::chrono::steady_clock::now();
	auto &st = rate_limits_[device_id];
	if (st.denied_count == 0 ||
	    std::chrono::duration_cast<std::chrono::seconds>(now - st.window_start).count() > kRateWindowSeconds) {
		st.window_start = now;
		st.denied_count = 1;
	} else {
		st.denied_count++;
	}

	if (st.denied_count >= kMaxDeniedBeforeCooldown) {
		st.blocked_until = now + std::chrono::seconds(kCooldownSeconds);
		blog(LOG_WARNING,
		     "[obs-airplay:security] Rate-limiting device %s for %d seconds after %d denied attempts",
		     device_id.c_str(), kCooldownSeconds, st.denied_count);
	}
}

void SecurityManager::clear_denied_attempts(const std::string &device_id)
{
	if (device_id.empty())
		return;
	std::lock_guard<std::mutex> lock(mutex_);
	rate_limits_.erase(device_id);
}

ApprovalDecision SecurityManager::prompt_user_approval(const std::string &device_id,
						       const std::string &model,
						       const std::string &name) const
{
	const SecurityPolicy pol = policy();
	if (!pol.interactive_prompts_enabled) {
		return ApprovalDecision::Deny;
	}

#ifdef _WIN32
	ensure_gdiplus();
	HINSTANCE hinst = GetModuleHandleW(nullptr);

	WNDCLASSEXW wc = {};
	wc.cbSize = sizeof(wc);
	wc.lpfnWndProc = approval_card_wnd_proc;
	wc.hInstance = hinst;
	wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
	wc.lpszClassName = kApprovalWindowClass;
	RegisterClassExW(&wc);

	ApprovalCardState st;
	st.device_name = utf8_to_wide(name.empty() ? "Unknown Apple Device" : name);
	std::string meta = (model.empty() ? "iOS Device" : model) + "  •  " +
			   (device_id.empty() ? "Unknown ID" : device_id);
	st.device_meta = utf8_to_wide(meta);
	st.hidden_from_capture = pol.hide_popups_from_capture;
	st.obs_hwnd = find_obs_main_window();
	if (st.obs_hwnd) {
		GetWindowRect(st.obs_hwnd, &st.last_obs_rc);
	}

	const float base_w = 450.0f;
	const float base_h = 250.0f;

	HWND hwnd = CreateWindowExW(
		WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
		kApprovalWindowClass,
		L"OBS AirPlay - Incoming Cast Request",
		WS_POPUP,
		0, 0, static_cast<int>(base_w), static_cast<int>(base_h),
		nullptr, nullptr, hinst, &st);

	if (hwnd) {
		st.scale = get_window_dpi_scale(st.obs_hwnd ? st.obs_hwnd : hwnd);
		const int w = static_cast<int>(std::round(base_w * st.scale));
		const int h = static_cast<int>(std::round(base_h * st.scale));
		int x = 0, y = 0;
		compute_anchored_position(st.obs_hwnd, w, h, st.scale, false, x, y);

		apply_obs_dark_chrome(hwnd, pol.hide_popups_from_capture);
		SetWindowPos(hwnd, HWND_TOPMOST, x, y, w, h, SWP_SHOWWINDOW);
		SetForegroundWindow(hwnd);
		UpdateWindow(hwnd);
		MessageBeep(MB_ICONEXCLAMATION);

		MSG msg;
		while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
	}

	return st.decision;
#else
	(void)device_id;
	(void)model;
	(void)name;
	return ApprovalDecision::Deny;
#endif
}

bool SecurityManager::evaluate_client_request(const std::string &device_id,
					      const std::string &model,
					      const std::string &name,
					      std::string &out_reason)
{
	const SecurityPolicy pol = policy();

	if (is_device_rate_limited(device_id)) {
		out_reason = "Blocked (rate-limited after repeated denials)";
		blog(LOG_WARNING, "[obs-airplay:security] Auto-blocked rate-limited device %s (%s)",
		     name.c_str(), device_id.c_str());
		return false;
	}

	const bool trusted = is_device_trusted(device_id);
	if (trusted) {
		clear_denied_attempts(device_id);
		out_reason = "Trusted device";
		return true;
	}

	if (pol.strict_trusted_only) {
		record_denied_attempt(device_id);
		out_reason = "Blocked (Strict Trusted Devices Only mode)";
		blog(LOG_WARNING, "[obs-airplay:security] Denied untrusted device %s (%s) [strict allowlist]",
		     name.c_str(), device_id.c_str());
		return false;
	}

	if (pol.require_pc_approval) {
		blog(LOG_INFO, "[obs-airplay:security] Requesting PC approval for %s (%s, id=%s)",
		     name.c_str(), model.c_str(), device_id.c_str());
		const ApprovalDecision dec = prompt_user_approval(device_id, model, name);
		if (dec == ApprovalDecision::AllowAndTrust) {
			trust_device(device_id, name, model);
			clear_denied_attempts(device_id);
			out_reason = "Approved & trusted by PC user";
			return true;
		}
		if (dec == ApprovalDecision::AllowOnce) {
			clear_denied_attempts(device_id);
			out_reason = "Approved once by PC user";
			return true;
		}
		record_denied_attempt(device_id);
		out_reason = "Denied by PC user (or timed out)";
		blog(LOG_WARNING, "[obs-airplay:security] Denied connection from %s (%s)",
		     name.c_str(), device_id.c_str());
		return false;
	}

	out_reason = "Allowed";
	return true;
}

void SecurityManager::show_pin_popup(const std::string &pin, const std::string &server_name)
{
	dismiss_pin_popup();

	{
		std::lock_guard<std::mutex> lock(pin_mutex_);
		active_pin_ = pin;
	}
	pin_cancel_.store(false, std::memory_order_release);

	const SecurityPolicy pol = policy();
	if (!pol.interactive_prompts_enabled) {
		return;
	}

#ifdef _WIN32
	std::string pin_copy = pin;
	std::string name_copy = server_name;
	bool hide_capture = pol.hide_popups_from_capture;

	pin_thread_ = std::thread([this, pin_copy, name_copy, hide_capture]() {
		if (pin_cancel_.load(std::memory_order_acquire))
			return;

		ensure_gdiplus();
		HINSTANCE hinst = GetModuleHandleW(nullptr);

		WNDCLASSEXW wc = {};
		wc.cbSize = sizeof(wc);
		wc.lpfnWndProc = pin_card_wnd_proc;
		wc.hInstance = hinst;
		wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
		wc.lpszClassName = kPinWindowClass;
		RegisterClassExW(&wc);

		PinCardState st;
		st.digits = utf8_to_wide(pin_copy);
		st.server_subtitle = utf8_to_wide("Enter on your iPhone for \"" + name_copy + "\"");
		st.hidden_from_capture = hide_capture;
		st.obs_hwnd = find_obs_main_window();
		if (st.obs_hwnd) {
			GetWindowRect(st.obs_hwnd, &st.last_obs_rc);
		}

		const float base_w = 408.0f;
		const float base_h = 196.0f;

		HWND hwnd = CreateWindowExW(
			WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED,
			kPinWindowClass,
			L"OBS AirPlay 2FA Code",
			WS_POPUP,
			0, 0, static_cast<int>(base_w), static_cast<int>(base_h),
			nullptr, nullptr, hinst, &st);

		if (hwnd) {
			st.scale = get_window_dpi_scale(st.obs_hwnd ? st.obs_hwnd : hwnd);
			const int w = static_cast<int>(std::round(base_w * st.scale));
			const int h = static_cast<int>(std::round(base_h * st.scale));
			int x = 0, y = 0;
			compute_anchored_position(st.obs_hwnd, w, h, st.scale, true, x, y);

			apply_obs_dark_chrome(hwnd, hide_capture);
			pin_hwnd_.store(hwnd, std::memory_order_release);

			if (pin_cancel_.load(std::memory_order_acquire)) {
				DestroyWindow(hwnd);
			} else {
				SetWindowPos(hwnd, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
				UpdateWindow(hwnd);
				MessageBeep(MB_OK);
			}

			MSG msg;
			while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
				TranslateMessage(&msg);
				DispatchMessageW(&msg);
			}
		}

		pin_hwnd_.store(nullptr, std::memory_order_release);
	});
#else
	(void)server_name;
#endif
}

void SecurityManager::dismiss_pin_popup()
{
	pin_cancel_.store(true, std::memory_order_release);
#ifdef _WIN32
	HWND hwnd = static_cast<HWND>(pin_hwnd_.load(std::memory_order_acquire));
	if (hwnd && IsWindow(hwnd)) {
		PostMessageW(hwnd, WM_CLOSE, 0, 0);
	}
#endif
	if (pin_thread_.joinable()) {
		pin_thread_.join();
	}
	std::lock_guard<std::mutex> lock(pin_mutex_);
	active_pin_.clear();
}

std::string SecurityManager::current_active_pin() const
{
	std::lock_guard<std::mutex> lock(pin_mutex_);
	return active_pin_;
}

} // namespace obs_airplay
