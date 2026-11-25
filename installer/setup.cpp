#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objidl.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <dwmapi.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using std::max;
using std::min;
#include <gdiplus.h>

#include "resource.h"

#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWA_BORDER_COLOR
#define DWMWA_BORDER_COLOR 34
#endif
#ifndef DWMWCP_ROUND
#define DWMWCP_ROUND 2
#endif

namespace {

constexpr const wchar_t *kAppName = L"OBS AirPlay Screen Cast";
constexpr const wchar_t *kAppVersion = L"1.3.0";
constexpr const wchar_t *kInstallDir = L"C:\\ProgramData\\obs-studio\\plugins\\obs-airplay";
constexpr const wchar_t *kUninstallRegKey = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\OBS-AirPlay";
constexpr const wchar_t *kSetupWindowClass = L"ObsAirPlaySetupWindowV1";

namespace theme {
inline Gdiplus::Color BgWindow()    { return Gdiplus::Color(255, 39, 42, 51); }
inline Gdiplus::Color BgHeader()    { return Gdiplus::Color(255, 48, 52, 64); }
inline Gdiplus::Color BgPanel()     { return Gdiplus::Color(255, 29, 32, 38); }
inline Gdiplus::Color Border()      { return Gdiplus::Color(255, 60, 64, 77); }
inline Gdiplus::Color BorderLight() { return Gdiplus::Color(255, 76, 82, 98); }
inline Gdiplus::Color TextPrimary() { return Gdiplus::Color(255, 242, 243, 245); }
inline Gdiplus::Color TextMuted()   { return Gdiplus::Color(255, 164, 168, 181); }
inline Gdiplus::Color Accent()      { return Gdiplus::Color(255, 40, 76, 184); }
inline Gdiplus::Color AccentHover() { return Gdiplus::Color(255, 58, 95, 204); }
inline Gdiplus::Color AccentPress() { return Gdiplus::Color(255, 30, 58, 146); }
inline Gdiplus::Color BtnBg()       { return Gdiplus::Color(255, 60, 64, 77); }
inline Gdiplus::Color BtnHover()    { return Gdiplus::Color(255, 74, 79, 94); }
inline Gdiplus::Color Success()     { return Gdiplus::Color(255, 59, 181, 120); }
inline Gdiplus::Color Warning()     { return Gdiplus::Color(255, 232, 168, 56); }
inline Gdiplus::Color Danger()      { return Gdiplus::Color(255, 182, 53, 59); }
inline Gdiplus::Color DangerHover() { return Gdiplus::Color(255, 210, 65, 72); }
} // namespace theme

std::wstring detect_obs_exe()
{
	const wchar_t *candidates[] = {
		L"C:\\Program Files\\obs-studio\\bin\\64bit\\obs64.exe",
		L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\OBS Studio\\bin\\64bit\\obs64.exe",
		L"D:\\SteamLibrary\\steamapps\\common\\OBS Studio\\bin\\64bit\\obs64.exe",
		L"C:\\Program Files (x86)\\obs-studio\\bin\\64bit\\obs64.exe",
	};
	for (const wchar_t *c : candidates) {
		if (GetFileAttributesW(c) != INVALID_FILE_ATTRIBUTES) {
			return c;
		}
	}

	HKEY key = nullptr;
	if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\OBS Studio", 0, KEY_READ | KEY_WOW64_32KEY, &key) ==
	    ERROR_SUCCESS) {
		wchar_t buf[MAX_PATH] = {};
		DWORD sz = sizeof(buf);
		if (RegQueryValueExW(key, nullptr, nullptr, nullptr, reinterpret_cast<LPBYTE>(buf), &sz) ==
		    ERROR_SUCCESS) {
			RegCloseKey(key);
			std::wstring path = std::wstring(buf) + L"\\bin\\64bit\\obs64.exe";
			if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
				return path;
			}
		} else {
			RegCloseKey(key);
		}
	}
	return L"C:\\Program Files\\obs-studio\\bin\\64bit\\obs64.exe";
}

bool is_obs_running()
{
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snap == INVALID_HANDLE_VALUE)
		return false;
	PROCESSENTRY32W pe = {};
	pe.dwSize = sizeof(pe);
	bool found = false;
	if (Process32FirstW(snap, &pe)) {
		do {
			if (_wcsicmp(pe.szExeFile, L"obs64.exe") == 0) {
				found = true;
				break;
			}
		} while (Process32NextW(snap, &pe));
	}
	CloseHandle(snap);
	return found;
}

bool close_obs_gracefully()
{
	// Ask OBS main windows to close cleanly first
	EnumWindows(
		[](HWND hwnd, LPARAM) -> BOOL {
			DWORD pid = 0;
			GetWindowThreadProcessId(hwnd, &pid);
			HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
			if (h) {
				wchar_t exe[MAX_PATH] = {};
				DWORD len = MAX_PATH;
				if (QueryFullProcessImageNameW(h, 0, exe, &len)) {
					std::wstring p(exe);
					if (p.size() >= 9 &&
					    _wcsicmp(p.c_str() + p.size() - 9, L"obs64.exe") == 0) {
						PostMessageW(hwnd, WM_CLOSE, 0, 0);
					}
				}
				CloseHandle(h);
			}
			return TRUE;
		},
		0);

	for (int i = 0; i < 30; ++i) {
		if (!is_obs_running())
			return true;
		Sleep(150);
	}
	return !is_obs_running();
}

bool run_hidden_command(const std::wstring &cmd)
{
	STARTUPINFOW si = {};
	si.cb = sizeof(si);
	si.dwFlags = STARTF_USESHOWWINDOW;
	si.wShowWindow = SW_HIDE;
	PROCESS_INFORMATION pi = {};
	std::wstring mutable_cmd = cmd;
	if (!CreateProcessW(nullptr, &mutable_cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
			    nullptr, nullptr, &si, &pi)) {
		return false;
	}
	WaitForSingleObject(pi.hProcess, 15000);
	DWORD exit_code = 1;
	GetExitCodeProcess(pi.hProcess, &exit_code);
	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
	return exit_code == 0;
}

bool configure_firewall_rules(const std::wstring &obs_exe)
{
	run_hidden_command(L"netsh advfirewall firewall delete rule name=\"OBS AirPlay Mirroring (TCP)\"");
	run_hidden_command(L"netsh advfirewall firewall delete rule name=\"OBS AirPlay Mirroring (UDP)\"");

	std::wstring tcp_cmd =
		L"netsh advfirewall firewall add rule name=\"OBS AirPlay Mirroring (TCP)\" "
		L"dir=in action=allow program=\"" + obs_exe +
		L"\" protocol=TCP localport=7000,7001,7100 remoteip=localsubnet profile=any enable=yes";
	std::wstring udp_cmd =
		L"netsh advfirewall firewall add rule name=\"OBS AirPlay Mirroring (UDP)\" "
		L"dir=in action=allow program=\"" + obs_exe +
		L"\" protocol=UDP localport=5353,6000,6001,7011 remoteip=localsubnet profile=any enable=yes";

	return run_hidden_command(tcp_cmd) && run_hidden_command(udp_cmd);
}

void remove_firewall_rules()
{
	run_hidden_command(L"netsh advfirewall firewall delete rule name=\"OBS AirPlay Mirroring (TCP)\"");
	run_hidden_command(L"netsh advfirewall firewall delete rule name=\"OBS AirPlay Mirroring (UDP)\"");
}

bool extract_resource_to_file(int res_id, const std::filesystem::path &dest, std::wstring &err)
{
	HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(res_id), MAKEINTRESOURCEW(10)); // RT_RCDATA = 10
	if (!res) {
		err = L"Embedded resource missing (ID " + std::to_wstring(res_id) + L").";
		return false;
	}
	DWORD sz = SizeofResource(nullptr, res);
	HGLOBAL glob = LoadResource(nullptr, res);
	if (!glob || sz == 0) {
		err = L"Failed to load embedded resource (ID " + std::to_wstring(res_id) + L").";
		return false;
	}
	const void *ptr = LockResource(glob);
	if (!ptr) {
		err = L"Failed to lock embedded resource.";
		return false;
	}

	std::error_code ec;
	std::filesystem::create_directories(dest.parent_path(), ec);

	std::ofstream out(dest, std::ios::binary | std::ios::trunc);
	if (!out.is_open()) {
		err = L"Cannot write " + dest.wstring() + L" (is OBS Studio still running?)";
		return false;
	}
	out.write(static_cast<const char *>(ptr), static_cast<std::streamsize>(sz));
	out.close();
	if (!out) {
		err = L"Failed writing " + dest.wstring();
		return false;
	}
	return true;
}

bool verify_embedded_resources()
{
	const int ids[] = {IDR_PLUGIN_DLL, IDR_FRAME_EFFECT, IDR_LOCALE_EN_US, IDR_LICENSE_TXT};
	for (int id : ids) {
		HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10));
		if (!res || SizeofResource(nullptr, res) == 0) {
			return false;
		}
	}
	return true;
}

void write_uninstall_registry(const std::filesystem::path &install_root, const std::filesystem::path &uninst_exe)
{
	HKEY key = nullptr;
	if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kUninstallRegKey, 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) !=
	    ERROR_SUCCESS) {
		return;
	}

	auto set_sz = [&](const wchar_t *name, const std::wstring &val) {
		RegSetValueExW(key, name, 0, REG_SZ,
			       reinterpret_cast<const BYTE *>(val.c_str()),
			       static_cast<DWORD>((val.size() + 1) * sizeof(wchar_t)));
	};
	auto set_dw = [&](const wchar_t *name, DWORD val) {
		RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE *>(&val), sizeof(val));
	};

	set_sz(L"DisplayName", std::wstring(kAppName) + L" " + kAppVersion);
	set_sz(L"DisplayVersion", kAppVersion);
	set_sz(L"Publisher", L"obs-airplay");
	set_sz(L"InstallLocation", install_root.wstring());
	set_sz(L"UninstallString", L"\"" + uninst_exe.wstring() + L"\" /uninstall");
	set_sz(L"QuietUninstallString", L"\"" + uninst_exe.wstring() + L"\" /uninstall /S");
	set_dw(L"EstimatedSize", 9200);
	set_dw(L"NoModify", 1);
	set_dw(L"NoRepair", 1);
	RegCloseKey(key);
}

bool perform_install(bool add_firewall, std::wstring &status_msg)
{
	if (is_obs_running()) {
		if (!close_obs_gracefully()) {
			status_msg = L"OBS Studio is still running. Please close OBS Studio and try again.";
			return false;
		}
	}

	const std::filesystem::path root(kInstallDir);
	std::wstring err;
	if (!extract_resource_to_file(IDR_PLUGIN_DLL, root / L"bin" / L"64bit" / L"obs-airplay.dll", err) ||
	    !extract_resource_to_file(IDR_FRAME_EFFECT, root / L"data" / L"airplay_frame.effect", err) ||
	    !extract_resource_to_file(IDR_LOCALE_EN_US, root / L"data" / L"locale" / L"en-US.ini", err) ||
	    !extract_resource_to_file(IDR_LICENSE_TXT, root / L"LICENSE.txt", err)) {
		status_msg = err;
		return false;
	}

	// If OBS Studio is detected in Portable Mode, also install into the portable plugins tree
	const std::filesystem::path obs_exe_path(detect_obs_exe());
	const std::filesystem::path obs_root = obs_exe_path.parent_path().parent_path().parent_path();
	if (!obs_root.empty() &&
	    (std::filesystem::exists(obs_root / L"portable_mode.txt") ||
	     std::filesystem::exists(obs_root / L"obs_portable_mode.txt"))) {
		std::wstring p_err;
		extract_resource_to_file(IDR_PLUGIN_DLL, obs_root / L"obs-plugins" / L"64bit" / L"obs-airplay.dll", p_err);
		extract_resource_to_file(IDR_FRAME_EFFECT, obs_root / L"data" / L"obs-plugins" / L"obs-airplay" / L"airplay_frame.effect", p_err);
		extract_resource_to_file(IDR_LOCALE_EN_US, obs_root / L"data" / L"obs-plugins" / L"obs-airplay" / L"locale" / L"en-US.ini", p_err);
	}

	// Copy self as uninstaller and register in Windows Installed Apps
	wchar_t self_path[MAX_PATH] = {};
	GetModuleFileNameW(nullptr, self_path, MAX_PATH);
	const std::filesystem::path uninst_exe = root / L"uninstall.exe";
	std::error_code ec;
	if (_wcsicmp(self_path, uninst_exe.c_str()) != 0) {
		std::filesystem::copy_file(self_path, uninst_exe, std::filesystem::copy_options::overwrite_existing, ec);
	}
	write_uninstall_registry(root, uninst_exe);

	if (add_firewall) {
		configure_firewall_rules(detect_obs_exe());
	}

	status_msg = L"Installed successfully! Open OBS Studio and add 'iPhone / iPad Screen Cast (AirPlay)'.";
	return true;
}

bool perform_uninstall(std::wstring &status_msg)
{
	if (is_obs_running()) {
		if (!close_obs_gracefully()) {
			status_msg = L"OBS Studio is currently running. Please close OBS Studio before uninstalling.";
			return false;
		}
	}

	remove_firewall_rules();
	RegDeleteKeyW(HKEY_LOCAL_MACHINE, kUninstallRegKey);

	const std::filesystem::path root(kInstallDir);
	std::error_code ec;
	std::filesystem::remove(root / L"bin" / L"64bit" / L"obs-airplay.dll", ec);
	std::filesystem::remove_all(root / L"data", ec);
	std::filesystem::remove(root / L"LICENSE.txt", ec);

	// Schedule cleanup of install directory (including uninstall.exe itself) after process exits
	std::wstring cleanup_cmd = L"cmd.exe /c ping -n 2 127.0.0.1 >nul & rmdir /s /q \"" + root.wstring() + L"\"";
	STARTUPINFOW si = {};
	si.cb = sizeof(si);
	si.dwFlags = STARTF_USESHOWWINDOW;
	si.wShowWindow = SW_HIDE;
	PROCESS_INFORMATION pi = {};
	if (CreateProcessW(nullptr, &cleanup_cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
			   nullptr, nullptr, &si, &pi)) {
		CloseHandle(pi.hThread);
		CloseHandle(pi.hProcess);
	}

	status_msg = L"OBS AirPlay Screen Cast has been uninstalled cleanly.";
	return true;
}

// ---------------------------------------------------------------------------
// OBS Yami Dark GUI Installer Window
// ---------------------------------------------------------------------------
enum class UiHit {
	None = 0,
	Close,
	CheckFirewall,
	CheckLaunch,
	PrimaryBtn,
	SecondaryBtn,
};

struct SetupUiState {
	bool uninstall_mode = false;
	bool opt_firewall = true;
	bool opt_launch_obs = true;
	bool done = false;
	bool succeeded = false;
	bool obs_running = false;
	std::wstring status_text;
	std::wstring obs_exe;

	float scale = 1.0f;
	UiHit hovered = UiHit::None;
	UiHit pressed = UiHit::None;
	bool tracking = false;

	Gdiplus::RectF rc_close;
	Gdiplus::RectF rc_chk_fw;
	Gdiplus::RectF rc_chk_launch;
	Gdiplus::RectF rc_primary;
	Gdiplus::RectF rc_secondary;
};

void add_rrect(Gdiplus::GraphicsPath &p, float x, float y, float w, float h, float r)
{
	float d = std::min(r * 2.0f, std::min(w, h));
	p.AddArc(x, y, d, d, 180.0f, 90.0f);
	p.AddArc(x + w - d, y, d, d, 270.0f, 90.0f);
	p.AddArc(x + w - d, y + h - d, d, d, 0.0f, 90.0f);
	p.AddArc(x, y + h - d, d, d, 90.0f, 90.0f);
	p.CloseFigure();
}

void fill_rrect(Gdiplus::Graphics &g, const Gdiplus::Brush &b, float x, float y, float w, float h, float r)
{
	Gdiplus::GraphicsPath p;
	add_rrect(p, x, y, w, h, r);
	g.FillPath(&b, &p);
}

void stroke_rrect(Gdiplus::Graphics &g, const Gdiplus::Pen &pen, float x, float y, float w, float h, float r)
{
	Gdiplus::GraphicsPath p;
	add_rrect(p, x, y, w, h, r);
	g.DrawPath(&pen, &p);
}

bool contains(const Gdiplus::RectF &rc, float x, float y)
{
	return x >= rc.X && x <= (rc.X + rc.Width) && y >= rc.Y && y <= (rc.Y + rc.Height);
}

void draw_checkbox(Gdiplus::Graphics &g, const Gdiplus::RectF &rc, bool checked, const wchar_t *label, float s)
{
	const float box = 18.0f * s;
	const float by = rc.Y + (rc.Height - box) * 0.5f;
	Gdiplus::SolidBrush bg(checked ? theme::Accent() : theme::BgPanel());
	Gdiplus::Pen edge(checked ? theme::AccentHover() : theme::BorderLight(), 1.2f);
	fill_rrect(g, bg, rc.X, by, box, box, 4.0f * s);
	stroke_rrect(g, edge, rc.X, by, box, box, 4.0f * s);

	if (checked) {
		Gdiplus::Pen ck(theme::TextPrimary(), 2.0f * s);
		ck.SetStartCap(Gdiplus::LineCapRound);
		ck.SetEndCap(Gdiplus::LineCapRound);
		ck.SetLineJoin(Gdiplus::LineJoinRound);
		Gdiplus::PointF pts[3] = {
			{rc.X + box * 0.24f, by + box * 0.53f},
			{rc.X + box * 0.44f, by + box * 0.72f},
			{rc.X + box * 0.77f, by + box * 0.30f},
		};
		g.DrawLines(&ck, pts, 3);
	}

	Gdiplus::Font font(L"Segoe UI", 13.0f * s, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
	Gdiplus::SolidBrush tb(theme::TextPrimary());
	Gdiplus::StringFormat sf;
	sf.SetAlignment(Gdiplus::StringAlignmentNear);
	sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
	g.DrawString(label, -1, &font,
		     Gdiplus::RectF(rc.X + box + 10.0f * s, rc.Y, rc.Width - box - 10.0f * s, rc.Height), &sf, &tb);
}

void draw_button(Gdiplus::Graphics &g, const Gdiplus::RectF &rc, const wchar_t *label,
		 const Gdiplus::Color &bg, const Gdiplus::Color &edge, float s)
{
	Gdiplus::SolidBrush b(bg);
	Gdiplus::Pen p(edge, 1.0f);
	fill_rrect(g, b, rc.X, rc.Y, rc.Width, rc.Height, 6.0f * s);
	stroke_rrect(g, p, rc.X, rc.Y, rc.Width, rc.Height, 6.0f * s);

	Gdiplus::Font font(L"Segoe UI Semibold", 13.0f * s, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
	Gdiplus::SolidBrush tb(theme::TextPrimary());
	Gdiplus::StringFormat sf;
	sf.SetAlignment(Gdiplus::StringAlignmentCenter);
	sf.SetLineAlignment(Gdiplus::StringAlignmentCenter);
	g.DrawString(label, -1, &font, rc, &sf, &tb);
}

void render_setup_ui(HWND hwnd, HDC hdc, SetupUiState *st)
{
	RECT cl;
	GetClientRect(hwnd, &cl);
	const int w = cl.right;
	const int h = cl.bottom;
	if (w <= 0 || h <= 0 || !st)
		return;

	HDC mem = CreateCompatibleDC(hdc);
	HBITMAP bmp = CreateCompatibleBitmap(hdc, w, h);
	HGDIOBJ old = SelectObject(mem, bmp);

	{
		Gdiplus::Graphics g(mem);
		g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
		g.SetTextRenderingHint(Gdiplus::TextRenderingHintClearTypeGridFit);

		const float s = st->scale;
		const float fw = static_cast<float>(w);
		const float fh = static_cast<float>(h);

		Gdiplus::SolidBrush bg(theme::BgWindow());
		g.FillRectangle(&bg, 0.0f, 0.0f, fw, fh);

		// Header strip
		const float hdr_h = 56.0f * s;
		Gdiplus::SolidBrush hdr_bg(theme::BgHeader());
		g.FillRectangle(&hdr_bg, 0.0f, 0.0f, fw, hdr_h);
		Gdiplus::Pen div(theme::Border(), 1.0f);
		g.DrawLine(&div, 0.0f, hdr_h, fw, hdr_h);

		// App icon badge (rounded phone glyph)
		Gdiplus::SolidBrush badge(theme::Accent());
		fill_rrect(g, badge, 20.0f * s, 12.0f * s, 32.0f * s, 32.0f * s, 7.0f * s);
		Gdiplus::Pen phone_pen(theme::TextPrimary(), 1.8f * s);
		stroke_rrect(g, phone_pen, 30.0f * s, 18.0f * s, 12.0f * s, 20.0f * s, 2.5f * s);

		Gdiplus::Font f_title(L"Segoe UI Semibold", 15.5f * s, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
		Gdiplus::Font f_sub(L"Segoe UI", 11.5f * s, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
		Gdiplus::SolidBrush tp(theme::TextPrimary());
		Gdiplus::SolidBrush tm(theme::TextMuted());
		Gdiplus::StringFormat sf_l;
		sf_l.SetAlignment(Gdiplus::StringAlignmentNear);
		sf_l.SetLineAlignment(Gdiplus::StringAlignmentCenter);

		std::wstring header_title = st->uninstall_mode
						    ? L"Uninstall OBS AirPlay Screen Cast"
						    : (std::wstring(kAppName) + L"  v" + kAppVersion);
		g.DrawString(header_title.c_str(), -1, &f_title,
			     Gdiplus::RectF(64.0f * s, 8.0f * s, fw - 110.0f * s, 24.0f * s), &sf_l, &tp);
		g.DrawString(L"Native Zero-App iPhone / iPad Screen Mirroring Plugin for OBS Studio 30-33+", -1, &f_sub,
			     Gdiplus::RectF(64.0f * s, 30.0f * s, fw - 110.0f * s, 18.0f * s), &sf_l, &tm);

		// Close X
		st->rc_close = Gdiplus::RectF(fw - 38.0f * s, 15.0f * s, 26.0f * s, 26.0f * s);
		if (st->hovered == UiHit::Close) {
			Gdiplus::SolidBrush ch(theme::BtnHover());
			fill_rrect(g, ch, st->rc_close.X, st->rc_close.Y, st->rc_close.Width, st->rc_close.Height,
				   4.0f * s);
		}
		Gdiplus::Pen xp(st->hovered == UiHit::Close ? theme::TextPrimary() : theme::TextMuted(), 1.7f * s);
		float cp = 8.0f * s;
		g.DrawLine(&xp, st->rc_close.X + cp, st->rc_close.Y + cp,
			   st->rc_close.X + st->rc_close.Width - cp, st->rc_close.Y + st->rc_close.Height - cp);
		g.DrawLine(&xp, st->rc_close.X + st->rc_close.Width - cp, st->rc_close.Y + cp,
			   st->rc_close.X + cp, st->rc_close.Y + st->rc_close.Height - cp);

		// Info Panel
		const float px = 22.0f * s;
		const float py = hdr_h + 18.0f * s;
		const float pw = fw - 44.0f * s;
		const float ph = 112.0f * s;
		Gdiplus::SolidBrush pbg(theme::BgPanel());
		fill_rrect(g, pbg, px, py, pw, ph, 6.0f * s);
		stroke_rrect(g, div, px, py, pw, ph, 6.0f * s);

		Gdiplus::Font f_body(L"Segoe UI", 12.5f * s, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
		Gdiplus::Font f_bold(L"Segoe UI Semibold", 13.0f * s, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);

		if (st->done) {
			Gdiplus::SolidBrush sc(st->succeeded ? theme::Success() : theme::Danger());
			g.DrawString(st->succeeded ? L"Complete!" : L"Action Required", -1, &f_bold,
				     Gdiplus::RectF(px + 16.0f * s, py + 14.0f * s, pw - 32.0f * s, 22.0f * s),
				     &sf_l, &sc);
			Gdiplus::StringFormat sf_wrap;
			sf_wrap.SetAlignment(Gdiplus::StringAlignmentNear);
			g.DrawString(st->status_text.c_str(), -1, &f_body,
				     Gdiplus::RectF(px + 16.0f * s, py + 42.0f * s, pw - 32.0f * s, 60.0f * s),
				     &sf_wrap, &tp);
		} else if (st->uninstall_mode) {
			g.DrawString(L"This will remove OBS AirPlay Screen Cast from OBS Studio:", -1, &f_bold,
				     Gdiplus::RectF(px + 16.0f * s, py + 16.0f * s, pw - 32.0f * s, 22.0f * s),
				     &sf_l, &tp);
			std::wstring info = L"• Plugin files in " + std::wstring(kInstallDir) +
					    L"\n• OBS AirPlay Windows Firewall rules\n• Installed Apps registry entry";
			Gdiplus::StringFormat sf_wrap;
			g.DrawString(info.c_str(), -1, &f_body,
				     Gdiplus::RectF(px + 16.0f * s, py + 42.0f * s, pw - 32.0f * s, 64.0f * s),
				     &sf_wrap, &tm);
		} else {
			g.DrawString(L"Included in this release:", -1, &f_bold,
				     Gdiplus::RectF(px + 16.0f * s, py + 12.0f * s, pw - 32.0f * s, 22.0f * s),
				     &sf_l, &tp);
			std::wstring features =
				L"• Native iOS AirPlay Screen & Audio Source (H.264 / HEVC + D3D11VA)\n"
				L"• GPU Rounded Corners, Border & Soft Back Shadow shader\n"
				L"• Stream-Safe 2FA PIN, PC Allow/Deny Gate & Trusted Device Allowlist\n"
				L"• Target: " + std::wstring(kInstallDir);
			Gdiplus::StringFormat sf_wrap;
			g.DrawString(features.c_str(), -1, &f_sub,
				     Gdiplus::RectF(px + 16.0f * s, py + 36.0f * s, pw - 32.0f * s, 72.0f * s),
				     &sf_wrap, &tm);
		}

		// Options checkboxes (only in install mode before completion)
		st->rc_chk_fw = Gdiplus::RectF(0, 0, 0, 0);
		st->rc_chk_launch = Gdiplus::RectF(0, 0, 0, 0);
		if (!st->done && !st->uninstall_mode) {
			st->rc_chk_fw = Gdiplus::RectF(24.0f * s, py + ph + 14.0f * s, fw - 48.0f * s, 28.0f * s);
			draw_checkbox(g, st->rc_chk_fw, st->opt_firewall,
				      L"Configure Windows Firewall for local Wi-Fi AirPlay (LocalSubnet only)", s);

			st->rc_chk_launch = Gdiplus::RectF(24.0f * s, py + ph + 46.0f * s, fw - 48.0f * s, 28.0f * s);
			draw_checkbox(g, st->rc_chk_launch, st->opt_launch_obs,
				      L"Launch OBS Studio when finished", s);

			if (st->obs_running) {
				Gdiplus::SolidBrush wb(theme::Warning());
				g.DrawString(L"Note: OBS Studio is running and will be closed automatically during install.",
					     -1, &f_sub,
					     Gdiplus::RectF(24.0f * s, py + ph + 78.0f * s, fw - 48.0f * s, 20.0f * s),
					     &sf_l, &wb);
			}
		}

		// Footer bar
		const float ftr_h = 62.0f * s;
		const float ftr_y = fh - ftr_h;
		Gdiplus::SolidBrush ftr_bg(theme::BgHeader());
		g.FillRectangle(&ftr_bg, 0.0f, ftr_y, fw, ftr_h);
		g.DrawLine(&div, 0.0f, ftr_y, fw, ftr_y);

		const float bh = 36.0f * s;
		const float by = ftr_y + (ftr_h - bh) * 0.5f;

		if (st->done) {
			st->rc_secondary = Gdiplus::RectF(0, 0, 0, 0);
			st->rc_primary = Gdiplus::RectF(fw - 140.0f * s, by, 118.0f * s, bh);
			Gdiplus::Color pc = (st->hovered == UiHit::PrimaryBtn) ? theme::AccentHover() : theme::Accent();
			draw_button(g, st->rc_primary, L"Finish", pc, theme::AccentHover(), s);
		} else {
			st->rc_secondary = Gdiplus::RectF(fw - 266.0f * s, by, 108.0f * s, bh);
			st->rc_primary = Gdiplus::RectF(fw - 148.0f * s, by, 126.0f * s, bh);

			Gdiplus::Color sc = (st->hovered == UiHit::SecondaryBtn) ? theme::BtnHover() : theme::BtnBg();
			draw_button(g, st->rc_secondary, L"Cancel", sc, theme::BorderLight(), s);

			if (st->uninstall_mode) {
				Gdiplus::Color pc = (st->hovered == UiHit::PrimaryBtn)
							    ? theme::DangerHover()
							    : theme::Danger();
				draw_button(g, st->rc_primary, L"Uninstall", pc, theme::DangerHover(), s);
			} else {
				Gdiplus::Color pc = (st->pressed == UiHit::PrimaryBtn)
							    ? theme::AccentPress()
							    : (st->hovered == UiHit::PrimaryBtn ? theme::AccentHover()
												: theme::Accent());
				draw_button(g, st->rc_primary, L"Install Now", pc, theme::AccentHover(), s);
			}
		}
	}

	BitBlt(hdc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
	SelectObject(mem, old);
	DeleteObject(bmp);
	DeleteDC(mem);
}

UiHit hit_test_setup(const SetupUiState *st, float x, float y)
{
	if (!st)
		return UiHit::None;
	if (contains(st->rc_close, x, y))
		return UiHit::Close;
	if (contains(st->rc_chk_fw, x, y))
		return UiHit::CheckFirewall;
	if (contains(st->rc_chk_launch, x, y))
		return UiHit::CheckLaunch;
	if (contains(st->rc_primary, x, y))
		return UiHit::PrimaryBtn;
	if (contains(st->rc_secondary, x, y))
		return UiHit::SecondaryBtn;
	return UiHit::None;
}

LRESULT CALLBACK setup_wnd_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
	auto *st = reinterpret_cast<SetupUiState *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
	switch (msg) {
	case WM_CREATE: {
		auto *cs = reinterpret_cast<CREATESTRUCTW *>(lparam);
		st = reinterpret_cast<SetupUiState *>(cs->lpCreateParams);
		SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
		return 0;
	}
	case WM_MOUSEMOVE:
		if (st) {
			if (!st->tracking) {
				TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, hwnd, 0};
				TrackMouseEvent(&tme);
				st->tracking = true;
			}
			float mx = static_cast<float>(static_cast<short>(LOWORD(lparam)));
			float my = static_cast<float>(static_cast<short>(HIWORD(lparam)));
			UiHit h = hit_test_setup(st, mx, my);
			if (h != st->hovered) {
				st->hovered = h;
				SetCursor(LoadCursorW(nullptr,
						      reinterpret_cast<LPCWSTR>(
							      h == UiHit::None ? IDC_ARROW : IDC_HAND)));
				InvalidateRect(hwnd, nullptr, FALSE);
			}
		}
		return 0;
	case WM_MOUSELEAVE:
		if (st) {
			st->tracking = false;
			st->hovered = UiHit::None;
			st->pressed = UiHit::None;
			InvalidateRect(hwnd, nullptr, FALSE);
		}
		return 0;
	case WM_LBUTTONDOWN:
		if (st) {
			float mx = static_cast<float>(static_cast<short>(LOWORD(lparam)));
			float my = static_cast<float>(static_cast<short>(HIWORD(lparam)));
			UiHit h = hit_test_setup(st, mx, my);
			if (h != UiHit::None) {
				st->pressed = h;
				SetCapture(hwnd);
				InvalidateRect(hwnd, nullptr, FALSE);
			} else {
				ReleaseCapture();
				SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
			}
		}
		return 0;
	case WM_LBUTTONUP:
		if (st && st->pressed != UiHit::None) {
			ReleaseCapture();
			float mx = static_cast<float>(static_cast<short>(LOWORD(lparam)));
			float my = static_cast<float>(static_cast<short>(HIWORD(lparam)));
			UiHit rel = hit_test_setup(st, mx, my);
			UiHit clicked = (rel == st->pressed) ? rel : UiHit::None;
			st->pressed = UiHit::None;

			if (clicked == UiHit::Close || clicked == UiHit::SecondaryBtn) {
				DestroyWindow(hwnd);
				return 0;
			} else if (clicked == UiHit::CheckFirewall) {
				st->opt_firewall = !st->opt_firewall;
			} else if (clicked == UiHit::CheckLaunch) {
				st->opt_launch_obs = !st->opt_launch_obs;
			} else if (clicked == UiHit::PrimaryBtn) {
				if (st->done) {
					if (st->succeeded && !st->uninstall_mode && st->opt_launch_obs) {
						std::wstring obs = st->obs_exe;
						std::filesystem::path p(obs);
						ShellExecuteW(nullptr, L"open", obs.c_str(), nullptr,
							      p.parent_path().c_str(), SW_SHOWNORMAL);
					}
					DestroyWindow(hwnd);
					return 0;
				}
				SetCursor(LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_WAIT)));
				if (st->uninstall_mode) {
					st->succeeded = perform_uninstall(st->status_text);
				} else {
					st->succeeded = perform_install(st->opt_firewall, st->status_text);
				}
				st->done = true;
				st->obs_running = is_obs_running();
			}
			InvalidateRect(hwnd, nullptr, FALSE);
		}
		return 0;
	case WM_ERASEBKGND:
		return 1;
	case WM_PAINT: {
		PAINTSTRUCT ps;
		HDC hdc = BeginPaint(hwnd, &ps);
		render_setup_ui(hwnd, hdc, st);
		EndPaint(hwnd, &ps);
		return 0;
	}
	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;
	default:
		break;
	}
	return DefWindowProcW(hwnd, msg, wparam, lparam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE hinst, HINSTANCE, LPWSTR cmdline, int)
{
	std::wstring args = cmdline ? cmdline : L"";
	bool silent = (args.find(L"/S") != std::wstring::npos || args.find(L"--silent") != std::wstring::npos);
	bool uninstall = (args.find(L"/uninstall") != std::wstring::npos ||
			  args.find(L"--uninstall") != std::wstring::npos);
	bool verify_only = (args.find(L"--verify-resources") != std::wstring::npos);

	if (verify_only) {
		return verify_embedded_resources() ? 0 : 2;
	}

	if (silent) {
		std::wstring msg;
		bool ok = uninstall ? perform_uninstall(msg) : perform_install(true, msg);
		return ok ? 0 : 1;
	}

	Gdiplus::GdiplusStartupInput gdip_in;
	ULONG_PTR gdip_tok = 0;
	Gdiplus::GdiplusStartup(&gdip_tok, &gdip_in, nullptr);

	WNDCLASSEXW wc = {};
	wc.cbSize = sizeof(wc);
	wc.lpfnWndProc = setup_wnd_proc;
	wc.hInstance = hinst;
	wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
	wc.lpszClassName = kSetupWindowClass;
	RegisterClassExW(&wc);

	SetupUiState st;
	st.uninstall_mode = uninstall;
	st.obs_exe = detect_obs_exe();
	st.obs_running = is_obs_running();

	const float base_w = 520.0f;
	const float base_h = uninstall ? 270.0f : 366.0f;

	HWND hwnd = CreateWindowExW(
		0, kSetupWindowClass, L"OBS AirPlay Screen Cast Setup",
		WS_POPUP, 0, 0, static_cast<int>(base_w), static_cast<int>(base_h),
		nullptr, nullptr, hinst, &st);

	if (hwnd) {
		using GetDpiForWindowFn = UINT(WINAPI *)(HWND);
		auto dpi_fn = reinterpret_cast<GetDpiForWindowFn>(
			reinterpret_cast<void *>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow")));
		if (dpi_fn) {
			UINT dpi = dpi_fn(hwnd);
			if (dpi >= 96)
				st.scale = static_cast<float>(dpi) / 96.0f;
		}
		const int w = static_cast<int>(std::round(base_w * st.scale));
		const int h = static_cast<int>(std::round(base_h * st.scale));
		RECT work = {};
		SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
		const int x = work.left + ((work.right - work.left) - w) / 2;
		const int y = work.top + ((work.bottom - work.top) - h) / 2;

		DWORD corner = DWMWCP_ROUND;
		DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
		COLORREF border_col = RGB(60, 64, 77);
		DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &border_col, sizeof(border_col));

		SetWindowPos(hwnd, HWND_TOP, x, y, w, h, SWP_SHOWWINDOW);
		UpdateWindow(hwnd);

		MSG msg;
		while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
			TranslateMessage(&msg);
			DispatchMessageW(&msg);
		}
	}

	Gdiplus::GdiplusShutdown(gdip_tok);
	return 0;
}
