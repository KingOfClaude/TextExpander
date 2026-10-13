// textexpander.cpp - a tiny tray text expander for Windows (pure Win32, no dependencies)
//
// Type a trigger such as ;date anywhere and it is replaced with its expansion.
// Left-click the tray icon to add/edit snippets. Right-click for the menu.
//
// Build (MinGW-w64):
//   g++ -std=c++17 -O2 -municode -mwindows -static textexpander.cpp -o textexpander.exe -lcomctl32 -lole32 -lshell32 -luser32 -lgdi32 -ladvapi32
// Build (MSVC, from a Developer Command Prompt):
//   cl /EHsc /O2 /std:c++17 textexpander.cpp /link /SUBSYSTEM:WINDOWS
//
// Placeholders usable in any expansion:
//   {date} {date+1} {date-7} {date:%A %d %B} {date+1:%Y-%m-%d}
//   {time} {now} {uuid} {branch} {clipboard}

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cwctype>
#include <ctime>
#include <string>
#include <vector>

#ifdef _MSC_VER
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

// --------------------------------------------------------------------------- //
// State
// --------------------------------------------------------------------------- //
struct Snippet { std::wstring trig, body; };

enum {
    WM_TRAYICON = WM_APP + 1,
    WM_EXPAND   = WM_APP + 2,
    ID_OPEN = 100, ID_TOGGLE, ID_AUTOSTART, ID_QUIT,
    ID_LIST = 200, ID_TRIG, ID_BODY, ID_SAVE, ID_NEW, ID_DELETE,
    ID_DATEFMT, ID_GITDIR, ID_BROWSE, ID_SAVESET,
    TIMER_PREVIEW = 1
};

struct App {
    HINSTANCE hInst = nullptr;
    HWND hwnd = nullptr;      // hidden window: tray messages + hook dispatch
    HWND editor = nullptr;    // snippet editor window (created lazily, then hidden/shown)
    HHOOK hook = nullptr;
    HICON icon = nullptr;     // small: tray + window title bar
    HICON iconBig = nullptr;  // large: taskbar button + Alt+Tab
    HFONT font = nullptr;
    UINT taskbarCreated = 0;
    int dpi = 96;
    bool enabled = true;

    std::vector<Snippet> snips;
    std::wstring dateFmt = L"%d/%m/%Y";   // UK style; editable in the GUI
    std::wstring gitDir;                  // folder used by {branch}

    std::wstring buf;        // recently typed characters
    std::wstring pending;    // trigger waiting to be expanded
    std::wstring selected;   // trigger currently loaded in the editor

    HWND list = nullptr, edTrig = nullptr, edBody = nullptr, stPreview = nullptr,
         edDate = nullptr, edGit = nullptr;
} g;

static int S(int x) { return MulDiv(x, g.dpi, 96); }

// --------------------------------------------------------------------------- //
// String helpers
// --------------------------------------------------------------------------- //
static std::wstring FromUtf8(const std::string& s) {
    size_t off = (s.size() >= 3 && (uint8_t)s[0] == 0xEF && (uint8_t)s[1] == 0xBB && (uint8_t)s[2] == 0xBF) ? 3 : 0;
    if (s.size() <= off) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data() + off, (int)(s.size() - off), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data() + off, (int)(s.size() - off), &w[0], n);
    return w;
}

static std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

static std::wstring Trim(const std::wstring& s) {
    size_t a = 0, b = s.size();
    while (a < b && iswspace(s[a])) a++;
    while (b > a && iswspace(s[b - 1])) b--;
    return s.substr(a, b - a);
}

static std::wstring ReplaceAll(std::wstring s, const std::wstring& from, const std::wstring& to) {
    size_t p = 0;
    while ((p = s.find(from, p)) != std::wstring::npos) { s.replace(p, from.size(), to); p += to.size(); }
    return s;
}

static std::wstring ToEdit(const std::wstring& s)   { return ReplaceAll(s, L"\n", L"\r\n"); }
static std::wstring FromEdit(const std::wstring& s) { return ReplaceAll(s, L"\r", L""); }

static std::wstring GetText(HWND h) {
    int n = GetWindowTextLengthW(h);
    std::wstring s(n, L'\0');
    if (n) GetWindowTextW(h, &s[0], n + 1);
    return s;
}

// --------------------------------------------------------------------------- //
// Placeholder rendering
// --------------------------------------------------------------------------- //
static std::wstring GitBranch() {
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return L"(git unavailable)";
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"git rev-parse --abbrev-ref HEAD";
    const wchar_t* dir = g.gitDir.empty() ? nullptr : g.gitDir.c_str();

    BOOL ok = CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                             nullptr, dir, &si, &pi);
    CloseHandle(wr);
    if (!ok) { CloseHandle(rd); return L"(git unavailable)"; }

    if (WaitForSingleObject(pi.hProcess, 2000) == WAIT_TIMEOUT) TerminateProcess(pi.hProcess, 1);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);

    std::string out;
    char tmp[256];
    DWORD got = 0, avail = 0;
    while (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) && avail &&
           ReadFile(rd, tmp, sizeof tmp, &got, nullptr) && got)
        out.append(tmp, got);

    CloseHandle(rd);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (code != 0) return L"(not a git repo)";
    return Trim(FromUtf8(out));
}

static std::wstring ClipboardText() {
    std::wstring r;
    if (!OpenClipboard(nullptr)) return r;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        if (auto* p = (const wchar_t*)GlobalLock(h)) { r = p; GlobalUnlock(h); }
    }
    CloseClipboard();
    return r;
}

static std::wstring NewUuid() {
    GUID id;
    CoCreateGuid(&id);
    wchar_t b[64];
    StringFromGUID2(id, b, 64);
    std::wstring s(b);
    s = s.substr(1, s.size() - 2);  // strip braces
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

static bool FormatDate(int days, const std::wstring& fmt, std::wstring& out) {
    time_t t = time(nullptr);
    tm lt{};
    localtime_s(&lt, &t);
    lt.tm_mday += days;
    lt.tm_isdst = -1;
    mktime(&lt);  // normalises e.g. 32 October -> 1 November
    wchar_t buf[256];
    size_t n = wcsftime(buf, 256, fmt.c_str(), &lt);
    if (n == 0) return false;
    out.assign(buf, n);
    return true;
}

// token is the text between { and }, e.g. "date+1:%A"
static bool ExpandToken(const std::wstring& tok, std::wstring& out) {
    size_t p = 0;
    while (p < tok.size() && iswalpha(tok[p])) p++;
    std::wstring name = tok.substr(0, p);
    for (auto& c : name) c = (wchar_t)towlower(c);

    int days = 0;
    if (p < tok.size() && (tok[p] == L'+' || tok[p] == L'-')) {
        int sign = tok[p] == L'-' ? -1 : 1;
        p++;
        size_t s = p;
        while (p < tok.size() && iswdigit(tok[p])) p++;
        if (p == s) return false;
        days = sign * _wtoi(tok.substr(s, p - s).c_str());
    }
    std::wstring fmt;
    bool hasFmt = false;
    if (p < tok.size() && tok[p] == L':') { fmt = tok.substr(p + 1); hasFmt = true; p = tok.size(); }
    if (p != tok.size()) return false;

    if (name == L"date" || name == L"now" || name == L"time") {
        if (!hasFmt) {
            fmt = name == L"date" ? g.dateFmt
                : name == L"time" ? L"%H:%M"
                                  : g.dateFmt + L" %H:%M";
        }
        return FormatDate(days, fmt, out);
    }
    if (name == L"uuid" && tok == L"uuid")           { out = NewUuid(); return true; }
    if (name == L"branch" && tok == L"branch")       { out = GitBranch(); return true; }
    if (name == L"clipboard" && tok == L"clipboard") { out = ClipboardText(); return true; }
    return false;
}

static std::wstring Render(const std::wstring& s) {
    std::wstring out;
    for (size_t i = 0; i < s.size();) {
        if (s[i] == L'{') {
            size_t j = s.find(L'}', i);
            std::wstring r;
            if (j != std::wstring::npos && ExpandToken(s.substr(i + 1, j - i - 1), r)) {
                out += r;
                i = j + 1;
                continue;
            }
        }
        out += s[i++];
    }
    return out;
}

// --------------------------------------------------------------------------- //
// Storage: %APPDATA%\TextExpander\snippets.txt
//   @key=value        settings
//   trigger<TAB>body  snippets (\n \t \\ escaped)
// --------------------------------------------------------------------------- //
static std::wstring ConfigPath() {
    const wchar_t* ap = _wgetenv(L"APPDATA");
    std::wstring dir = ap ? ap : L".";
    dir += L"\\TextExpander";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\snippets.txt";
}

static std::wstring Escape(const std::wstring& s) {
    std::wstring o;
    for (wchar_t c : s) {
        if (c == L'\\') o += L"\\\\";
        else if (c == L'\n') o += L"\\n";
        else if (c == L'\t') o += L"\\t";
        else if (c != L'\r') o += c;
    }
    return o;
}

static std::wstring Unescape(const std::wstring& s) {
    std::wstring o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == L'\\' && i + 1 < s.size()) {
            wchar_t n = s[++i];
            o += n == L'n' ? L'\n' : n == L't' ? L'\t' : n;
        } else o += s[i];
    }
    return o;
}

static bool ReadAll(const std::wstring& path, std::string& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    char tmp[4096];
    DWORD n = 0;
    while (ReadFile(h, tmp, sizeof tmp, &n, nullptr) && n) out.append(tmp, n);
    CloseHandle(h);
    return true;
}

static void SortSnips() {
    std::sort(g.snips.begin(), g.snips.end(),
              [](const Snippet& a, const Snippet& b) { return _wcsicmp(a.trig.c_str(), b.trig.c_str()) < 0; });
}

static int FindSnip(const std::wstring& trig) {
    for (size_t i = 0; i < g.snips.size(); i++)
        if (_wcsicmp(g.snips[i].trig.c_str(), trig.c_str()) == 0) return (int)i;
    return -1;
}

static void SaveConfig() {
    std::wstring all = L"# Text Expander snippets\n@date_format=" + Escape(g.dateFmt) +
                       L"\n@git_dir=" + Escape(g.gitDir) + L"\n";
    for (auto& s : g.snips) all += s.trig + L"\t" + Escape(s.body) + L"\n";

    std::wstring path = ConfigPath(), tmp = path + L".tmp";
    std::string bytes = ToUtf8(all);
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    WriteFile(h, bytes.data(), (DWORD)bytes.size(), &w, nullptr);
    CloseHandle(h);
    MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

static void LoadDefaults() {
    g.snips = {
        {L";sig", L"Your Name"},
        {L";email", L"you@example.com"},
        {L";phone", L"+44 7000 000000"},
        {L";addr", L"1 Example Street, Rushden, NN10 0AA"},
        {L";regards", L"Kind regards,\nYour Name"},
        {L";date", L"{date}"},
        {L";tomorrow", L"{date+1}"},
        {L";yesterday", L"{date-1}"},
        {L";nextweek", L"{date+7}"},
        {L";day", L"{date:%A}"},
        {L";iso", L"{date:%Y-%m-%d}"},
        {L";time", L"{time}"},
        {L";now", L"{now}"},
        {L";stamp", L"{date:%Y%m%d-%H%M%S}"},
        {L";uuid", L"{uuid}"},
        {L";branch", L"{branch}"},
        {L";shrug", L"\u00af\\_(\u30c4)_/\u00af"},
        {L";lorem", L"Lorem ipsum dolor sit amet, consectetur adipiscing elit, "
                    L"sed do eiusmod tempor incididunt ut labore et dolore magna aliqua."},
    };
}

// returns true if a config file already existed
static bool LoadConfig() {
    std::string raw;
    if (!ReadAll(ConfigPath(), raw)) { LoadDefaults(); SortSnips(); SaveConfig(); return false; }

    std::wstring all = FromUtf8(raw);
    g.snips.clear();
    size_t pos = 0;
    while (pos <= all.size()) {
        size_t e = all.find(L'\n', pos);
        if (e == std::wstring::npos) e = all.size();
        std::wstring line = all.substr(pos, e - pos);
        pos = e + 1;
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        if (line.empty() || line[0] == L'#') continue;
        if (line[0] == L'@') {
            size_t eq = line.find(L'=');
            if (eq == std::wstring::npos) continue;
            std::wstring key = line.substr(1, eq - 1), val = Unescape(line.substr(eq + 1));
            if (key == L"date_format" && !val.empty()) g.dateFmt = val;
            else if (key == L"git_dir") g.gitDir = val;
            continue;
        }
        size_t tab = line.find(L'\t');
        if (tab == std::wstring::npos || tab == 0) continue;
        g.snips.push_back({line.substr(0, tab), Unescape(line.substr(tab + 1))});
    }
    SortSnips();
    return true;
}

// --------------------------------------------------------------------------- //
// Keyboard hook
// --------------------------------------------------------------------------- //
static void SendExpansion(size_t backspaces, const std::wstring& text) {
    std::vector<INPUT> in;
    auto key = [&](WORD vk, WORD scan, DWORD flags) {
        INPUT i{};
        i.type = INPUT_KEYBOARD;
        i.ki.wVk = vk;
        i.ki.wScan = scan;
        i.ki.dwFlags = flags;
        in.push_back(i);
    };
    for (size_t i = 0; i < backspaces; i++) { key(VK_BACK, 0, 0); key(VK_BACK, 0, KEYEVENTF_KEYUP); }
    // Prefer genuine key presses (works in older apps like TextPad); fall back to
    // Unicode packets only for characters that aren't on the current keyboard layout.
    HKL layout = GetKeyboardLayout(GetWindowThreadProcessId(GetForegroundWindow(), nullptr));
    bool caps = (GetKeyState(VK_CAPITAL) & 1) != 0;
    WORD shiftScan = (WORD)MapVirtualKeyExW(VK_SHIFT, MAPVK_VK_TO_VSC, layout);
    for (wchar_t c : text) {
        if (c == L'\r') continue;
        if (c == L'\n')      { key(VK_RETURN, 0, 0); key(VK_RETURN, 0, KEYEVENTF_KEYUP); continue; }
        if (c == L'\t')      { key(VK_TAB, 0, 0);    key(VK_TAB, 0, KEYEVENTF_KEYUP);    continue; }

        SHORT r = VkKeyScanExW(c, layout);
        BYTE vk = LOBYTE(r), st = HIBYTE(r);
        if (r != -1 && vk != 0xFF && (st & ~1) == 0) {  // plain or Shift-only key
            bool shift = (st & 1) != 0;
            if (caps && iswalpha(c)) shift = !shift;    // Caps Lock inverts letters
            WORD scan = (WORD)MapVirtualKeyExW(vk, MAPVK_VK_TO_VSC, layout);
            if (shift) key(VK_SHIFT, shiftScan, 0);
            key(vk, scan, 0);
            key(vk, scan, KEYEVENTF_KEYUP);
            if (shift) key(VK_SHIFT, shiftScan, KEYEVENTF_KEYUP);
        } else {
            key(0, c, KEYEVENTF_UNICODE);
            key(0, c, KEYEVENTF_UNICODE | KEYEVENTF_KEYUP);
        }
    }
    if (!in.empty()) SendInput((UINT)in.size(), in.data(), sizeof(INPUT));
}

static bool Down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

static void OnKey(const KBDLLHOOKSTRUCT* k) {
    DWORD vk = k->vkCode;
    switch (vk) {  // pure modifiers neither add to nor reset the buffer
        case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT: case VK_CAPITAL:
        case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
        case VK_MENU: case VK_LMENU: case VK_RMENU:
        case VK_LWIN: case VK_RWIN:
            return;
    }

    DWORD pid = 0;
    HWND fg = GetForegroundWindow();
    DWORD tid = GetWindowThreadProcessId(fg, &pid);
    if (pid == GetCurrentProcessId()) { g.buf.clear(); return; }  // don't expand inside our own editor

    if (vk == VK_BACK) { if (!g.buf.empty()) g.buf.pop_back(); return; }

    bool ctrl = Down(VK_CONTROL), alt = Down(VK_MENU), win = Down(VK_LWIN) || Down(VK_RWIN);
    bool altgr = ctrl && alt;  // AltGr reports as Ctrl+Alt
    if (win || ((ctrl || alt) && !altgr)) { g.buf.clear(); return; }  // shortcuts, not typing

    BYTE ks[256] = {};
    if (Down(VK_SHIFT)) ks[VK_SHIFT] = 0x80;
    if (GetKeyState(VK_CAPITAL) & 1) ks[VK_CAPITAL] = 1;
    if (altgr) { ks[VK_CONTROL] = 0x80; ks[VK_MENU] = 0x80; }

    wchar_t out[8] = {};
    int n = ToUnicodeEx(vk, k->scanCode, ks, out, 8, 0x4 /* don't disturb dead-key state */,
                        GetKeyboardLayout(tid));
    if (n != 1 || iswspace(out[0]) || iswcntrl(out[0])) { g.buf.clear(); return; }

    g.buf += out[0];
    if (g.buf.size() > 64) g.buf.erase(0, g.buf.size() - 64);

    const Snippet* best = nullptr;
    for (auto& s : g.snips) {
        if (s.trig.size() > g.buf.size()) continue;
        if (_wcsicmp(g.buf.c_str() + g.buf.size() - s.trig.size(), s.trig.c_str()) != 0) continue;
        if (!best || s.trig.size() > best->trig.size()) best = &s;
    }
    if (best) {
        g.pending = best->trig;
        g.buf.clear();
        PostMessageW(g.hwnd, WM_EXPAND, 0, 0);  // do the work outside the hook callback
    }
}

static LRESULT CALLBACK KbProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && g.enabled) {
        auto* k = (const KBDLLHOOKSTRUCT*)lp;
        if (!(k->flags & LLKHF_INJECTED) &&  // ignore our own synthetic keystrokes
            (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN))
            OnKey(k);
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

static void DoExpand() {
    int i = FindSnip(g.pending);
    if (i < 0) return;
    std::wstring text = Render(g.snips[i].body);
    Sleep(15);  // let the final trigger keystroke reach the target app first
    SendExpansion(g.snips[i].trig.size(), text);
}

// --------------------------------------------------------------------------- //
// Tray / autostart
// --------------------------------------------------------------------------- //
static const wchar_t* RUNKEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

static bool IsAutostart() {
    return RegGetValueW(HKEY_CURRENT_USER, RUNKEY, L"TextExpander", RRF_RT_REG_SZ,
                        nullptr, nullptr, nullptr) == ERROR_SUCCESS;
}

static void SetAutostart(bool on) {
    if (on) {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring v = L"\"" + std::wstring(exe) + L"\"";
        RegSetKeyValueW(HKEY_CURRENT_USER, RUNKEY, L"TextExpander", REG_SZ, v.c_str(),
                        (DWORD)((v.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteKeyValueW(HKEY_CURRENT_USER, RUNKEY, L"TextExpander");
    }
}

static HICON MakeIcon(int sz) {
    HDC sdc = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(sdc);
    HBITMAP color = CreateCompatibleBitmap(sdc, sz, sz);
    HBITMAP mask = CreateBitmap(sz, sz, 1, 1, nullptr);

    SelectObject(dc, color);
    HBRUSH br = CreateSolidBrush(RGB(37, 99, 235));
    RECT rc{0, 0, sz, sz};
    FillRect(dc, &rc, br);
    DeleteObject(br);
    HFONT f = CreateFontW(-sz, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY,
                          0, L"Segoe UI");
    HGDIOBJ oldf = SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    DrawTextW(dc, L";", 1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, oldf);
    DeleteObject(f);

    SelectObject(dc, mask);
    PatBlt(dc, 0, 0, sz, sz, BLACKNESS);  // 0 = fully opaque

    ICONINFO ii{TRUE, 0, 0, mask, color};
    HICON ic = CreateIconIndirect(&ii);
    DeleteDC(dc);
    DeleteObject(color);
    DeleteObject(mask);
    ReleaseDC(nullptr, sdc);
    return ic;
}

static void AddTray() {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof nid;
    nid.hWnd = g.hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = g.icon;
    lstrcpynW(nid.szTip, L"Text Expander", ARRAYSIZE(nid.szTip));
    Shell_NotifyIconW(NIM_ADD, &nid);
}

static void RemoveTray() {
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof nid;
    nid.hWnd = g.hwnd;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

static void ShowTrayMenu() {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, ID_OPEN, L"Edit snippets");
    SetMenuDefaultItem(m, ID_OPEN, FALSE);
    AppendMenuW(m, MF_STRING | (g.enabled ? MF_CHECKED : 0), ID_TOGGLE, L"Enabled");
    AppendMenuW(m, MF_STRING | (IsAutostart() ? MF_CHECKED : 0), ID_AUTOSTART, L"Start with Windows");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_QUIT, L"Quit");
    POINT p;
    GetCursorPos(&p);
    SetForegroundWindow(g.hwnd);
    TrackPopupMenu(m, TPM_RIGHTBUTTON, p.x, p.y, 0, g.hwnd, nullptr);
    PostMessageW(g.hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
}

// --------------------------------------------------------------------------- //
// Editor window
// --------------------------------------------------------------------------- //
static void UpdatePreview() {
    std::wstring r = Render(FromEdit(GetText(g.edBody)));
    r = ReplaceAll(r, L"\n", L" \u21b5 ");
    if (r.size() > 300) r.resize(300);
    SetWindowTextW(g.stPreview, r.c_str());
}

static void SchedulePreview() { SetTimer(g.editor, TIMER_PREVIEW, 350, nullptr); }

static void RefreshList(const std::wstring& select = L"") {
    ListView_DeleteAllItems(g.list);
    int selIdx = -1;
    for (size_t i = 0; i < g.snips.size(); i++) {
        LVITEMW it{};
        it.mask = LVIF_TEXT;
        it.iItem = (int)i;
        it.pszText = const_cast<wchar_t*>(g.snips[i].trig.c_str());
        ListView_InsertItem(g.list, &it);
        std::wstring pv = ReplaceAll(g.snips[i].body, L"\n", L" \u21b5 ");
        ListView_SetItemText(g.list, (int)i, 1, const_cast<wchar_t*>(pv.c_str()));
        if (!select.empty() && _wcsicmp(select.c_str(), g.snips[i].trig.c_str()) == 0) selIdx = (int)i;
    }
    if (selIdx >= 0) {
        ListView_SetItemState(g.list, selIdx, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(g.list, selIdx, FALSE);
    }
}

static void OnSelect(int idx) {
    if (idx < 0 || idx >= (int)g.snips.size()) return;
    g.selected = g.snips[idx].trig;
    SetWindowTextW(g.edTrig, g.snips[idx].trig.c_str());
    SetWindowTextW(g.edBody, ToEdit(g.snips[idx].body).c_str());
}

static void NewSnippet() {
    g.selected.clear();
    ListView_SetItemState(g.list, -1, 0, LVIS_SELECTED);
    SetWindowTextW(g.edTrig, L";");
    SetWindowTextW(g.edBody, L"");
    SetFocus(g.edTrig);
    SendMessageW(g.edTrig, EM_SETSEL, 1, 1);
}

static void SaveSnippet() {
    std::wstring trig = Trim(GetText(g.edTrig));
    std::wstring body = FromEdit(GetText(g.edBody));
    if (trig.size() < 2 || trig.find_first_of(L" \t\r\n") != std::wstring::npos) {
        MessageBoxW(g.editor, L"Trigger must be 2+ characters with no spaces (e.g. ;sig).",
                    L"Trigger", MB_ICONWARNING);
        return;
    }
    if (body.empty()) {
        MessageBoxW(g.editor, L"Expansion can't be empty.", L"Expansion", MB_ICONWARNING);
        return;
    }

    int existing = FindSnip(trig);
    bool isSelf = existing >= 0 && !g.selected.empty() && _wcsicmp(g.snips[existing].trig.c_str(), g.selected.c_str()) == 0;
    if (existing >= 0 && !isSelf &&
        MessageBoxW(g.editor, (L"'" + trig + L"' already exists. Replace it?").c_str(), L"Replace",
                    MB_YESNO | MB_ICONQUESTION) != IDYES)
        return;

    // A trigger fires the instant it's typed, so one that starts another shadows it.
    std::wstring clashes;
    for (auto& s : g.snips) {
        if (_wcsicmp(s.trig.c_str(), trig.c_str()) == 0) continue;
        if (!g.selected.empty() && _wcsicmp(s.trig.c_str(), g.selected.c_str()) == 0) continue;
        size_t n = (std::min)(s.trig.size(), trig.size());
        if (_wcsnicmp(s.trig.c_str(), trig.c_str(), n) == 0) clashes += (clashes.empty() ? L"" : L", ") + s.trig;
    }
    if (!clashes.empty() &&
        MessageBoxW(g.editor,
                    (L"'" + trig + L"' overlaps with " + clashes +
                     L".\nThe shorter one fires first, so the longer one will never trigger.\n\nSave anyway?").c_str(),
                    L"Possible clash", MB_YESNO | MB_ICONWARNING) != IDYES)
        return;

    if (!g.selected.empty() && _wcsicmp(g.selected.c_str(), trig.c_str()) != 0) {  // renamed
        int old = FindSnip(g.selected);
        if (old >= 0) g.snips.erase(g.snips.begin() + old);
    }
    int idx = FindSnip(trig);
    if (idx >= 0) g.snips[idx] = {trig, body};
    else g.snips.push_back({trig, body});
    SortSnips();
    SaveConfig();
    g.selected = trig;
    RefreshList(trig);
}

static void DeleteSnippet() {
    std::wstring trig = g.selected.empty() ? Trim(GetText(g.edTrig)) : g.selected;
    int idx = FindSnip(trig);
    if (idx < 0) return;
    if (MessageBoxW(g.editor, (L"Delete " + trig + L"?").c_str(), L"Delete", MB_YESNO | MB_ICONQUESTION) != IDYES)
        return;
    g.snips.erase(g.snips.begin() + idx);
    SaveConfig();
    RefreshList();
    NewSnippet();
}

static void BrowseGit() {
    BROWSEINFOW bi{};
    bi.hwndOwner = g.editor;
    bi.lpszTitle = L"Folder used for {branch}";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    if (PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi)) {
        wchar_t p[MAX_PATH];
        if (SHGetPathFromIDListW(pidl, p)) SetWindowTextW(g.edGit, p);
        CoTaskMemFree(pidl);
    }
}

static void SaveSettings() {
    std::wstring fmt = Trim(GetText(g.edDate));
    g.dateFmt = fmt.empty() ? L"%d/%m/%Y" : fmt;
    g.gitDir = Trim(GetText(g.edGit));
    SaveConfig();
    UpdatePreview();
    MessageBoxW(g.editor, L"Settings saved.", L"Text Expander", MB_ICONINFORMATION);
}

static HWND Mk(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style,
               int x, int y, int w, int h, int id = 0, DWORD ex = 0) {
    HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h),
                             parent, (HMENU)(INT_PTR)id, g.hInst, nullptr);
    SendMessageW(c, WM_SETFONT, (WPARAM)g.font, TRUE);
    return c;
}

static void BuildEditor(HWND w) {
    const DWORD TAB = WS_TABSTOP;
    g.list = Mk(w, WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | TAB,
                10, 10, 740, 230, ID_LIST, WS_EX_CLIENTEDGE);
    ListView_SetExtendedListViewStyle(g.list, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
    LVCOLUMNW col{};
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    col.pszText = const_cast<wchar_t*>(L"Trigger");
    col.cx = S(140);
    ListView_InsertColumn(g.list, 0, &col);
    col.pszText = const_cast<wchar_t*>(L"Expands to");
    col.cx = S(570);
    ListView_InsertColumn(g.list, 1, &col);

    Mk(w, L"BUTTON", L"Snippet", BS_GROUPBOX, 10, 248, 740, 214);
    Mk(w, L"STATIC", L"Trigger", 0, 24, 276, 70, 20);
    g.edTrig = Mk(w, L"EDIT", L"", ES_AUTOHSCROLL | TAB, 100, 273, 220, 24, ID_TRIG, WS_EX_CLIENTEDGE);
    Mk(w, L"STATIC", L"Expansion", 0, 24, 308, 70, 20);
    g.edBody = Mk(w, L"EDIT", L"", ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL | TAB,
                  100, 305, 635, 90, ID_BODY, WS_EX_CLIENTEDGE);
    Mk(w, L"STATIC", L"Preview", 0, 24, 404, 70, 20);
    g.stPreview = Mk(w, L"STATIC", L"", SS_NOPREFIX, 100, 404, 635, 36);
    Mk(w, L"BUTTON", L"Save snippet", BS_PUSHBUTTON | TAB, 100, 424, 120, 28, ID_SAVE);
    Mk(w, L"BUTTON", L"New", BS_PUSHBUTTON | TAB, 228, 424, 80, 28, ID_NEW);
    Mk(w, L"BUTTON", L"Delete", BS_PUSHBUTTON | TAB, 316, 424, 80, 28, ID_DELETE);

    Mk(w, L"STATIC",
       L"Placeholders:  {date}  {date+1}  {date-7}  {date:%A %d %B}  {time}  {now}  {uuid}  {branch}  {clipboard}\n"
       L"Offsets are in days. Formats use strftime codes (%d %m %Y %H %M %S %A %B).",
       SS_NOPREFIX, 12, 468, 738, 36);

    Mk(w, L"BUTTON", L"Settings", BS_GROUPBOX, 10, 508, 740, 92);
    Mk(w, L"STATIC", L"Date format", 0, 24, 534, 110, 20);
    g.edDate = Mk(w, L"EDIT", g.dateFmt.c_str(), ES_AUTOHSCROLL | TAB, 150, 531, 150, 24, ID_DATEFMT, WS_EX_CLIENTEDGE);
    Mk(w, L"BUTTON", L"Save settings", BS_PUSHBUTTON | TAB, 320, 530, 120, 26, ID_SAVESET);
    Mk(w, L"STATIC", L"Git folder ({branch})", 0, 24, 566, 126, 20);
    g.edGit = Mk(w, L"EDIT", g.gitDir.c_str(), ES_AUTOHSCROLL | TAB, 150, 563, 480, 24, ID_GITDIR, WS_EX_CLIENTEDGE);
    Mk(w, L"BUTTON", L"Browse...", BS_PUSHBUTTON | TAB, 640, 562, 95, 26, ID_BROWSE);

    RefreshList();
}

static LRESULT CALLBACK EditorProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        g.editor = h;
        SendMessageW(h, WM_SETICON, ICON_SMALL, (LPARAM)g.icon);   // top-left of the window
        SendMessageW(h, WM_SETICON, ICON_BIG, (LPARAM)g.iconBig);  // taskbar / Alt+Tab
        BuildEditor(h);
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_SAVE:    SaveSnippet(); break;
        case ID_NEW:     NewSnippet(); break;
        case ID_DELETE:  DeleteSnippet(); break;
        case ID_BROWSE:  BrowseGit(); break;
        case ID_SAVESET: SaveSettings(); break;
        case IDCANCEL:   ShowWindow(h, SW_HIDE); break;
        case ID_BODY:    if (HIWORD(wp) == EN_CHANGE) SchedulePreview(); break;
        }
        return 0;
    case WM_NOTIFY: {
        auto* nm = (NMLISTVIEW*)lp;
        if (nm->hdr.idFrom == ID_LIST && nm->hdr.code == LVN_ITEMCHANGED &&
            (nm->uChanged & LVIF_STATE) && (nm->uNewState & LVIS_SELECTED))
            OnSelect(nm->iItem);
        return 0;
    }
    case WM_TIMER:
        if (wp == TIMER_PREVIEW) { KillTimer(h, TIMER_PREVIEW); UpdatePreview(); }
        return 0;
    case WM_CLOSE:
        ShowWindow(h, SW_HIDE);  // keep running in the tray
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void ShowEditor() {
    if (!g.editor) {
        RECT r{0, 0, S(760), S(610)};
        DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
        AdjustWindowRect(&r, style, FALSE);
        CreateWindowExW(0, L"TextExpanderEditor", L"Text Expander", style, CW_USEDEFAULT, CW_USEDEFAULT,
                        r.right - r.left, r.bottom - r.top, nullptr, nullptr, g.hInst, nullptr);
    }
    ShowWindow(g.editor, SW_SHOW);
    ShowWindow(g.editor, SW_RESTORE);
    SetForegroundWindow(g.editor);
}

// --------------------------------------------------------------------------- //
// Hidden main window + entry point
// --------------------------------------------------------------------------- //
static LRESULT CALLBACK MainProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == g.taskbarCreated && msg != 0) { AddTray(); return 0; }  // explorer restarted
    switch (msg) {
    case WM_EXPAND:
        DoExpand();
        return 0;
    case WM_TRAYICON:
        if (lp == WM_LBUTTONUP) ShowEditor();
        else if (lp == WM_RBUTTONUP) ShowTrayMenu();
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_OPEN:      ShowEditor(); break;
        case ID_TOGGLE:    g.enabled = !g.enabled; g.buf.clear(); break;
        case ID_AUTOSTART: SetAutostart(!IsAutostart()); break;
        case ID_QUIT:      DestroyWindow(h); break;
        }
        return 0;
    case WM_DESTROY:
        if (g.hook) UnhookWindowsHookEx(g.hook);
        RemoveTray();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static void NoopInvalidParam(const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) {}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\TextExpanderSingleton");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;  // already running

    _set_invalid_parameter_handler(NoopInvalidParam);  // bad strftime formats return 0 instead of crashing
    SetProcessDPIAware();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc{sizeof icc, ICC_LISTVIEW_CLASSES};
    InitCommonControlsEx(&icc);

    g.hInst = hInst;
    HDC dc = GetDC(nullptr);
    g.dpi = GetDeviceCaps(dc, LOGPIXELSX);
    ReleaseDC(nullptr, dc);

    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof ncm;
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
    g.font = CreateFontIndirectW(&ncm.lfMessageFont);

    bool hadConfig = LoadConfig();

    WNDCLASSW wc{};
    wc.hInstance = hInst;
    wc.lpfnWndProc = MainProc;
    wc.lpszClassName = L"TextExpanderMain";
    RegisterClassW(&wc);
    wc.lpfnWndProc = EditorProc;
    wc.lpszClassName = L"TextExpanderEditor";
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);

    g.hwnd = CreateWindowExW(0, L"TextExpanderMain", L"TextExpander", 0, 0, 0, 0, 0,
                             nullptr, nullptr, hInst, nullptr);
    g.taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    g.icon = MakeIcon(GetSystemMetrics(SM_CXSMICON));
    g.iconBig = MakeIcon(GetSystemMetrics(SM_CXICON));
    AddTray();
    g.hook = SetWindowsHookExW(WH_KEYBOARD_LL, KbProc, GetModuleHandleW(nullptr), 0);
    if (!g.hook) MessageBoxW(nullptr, L"Couldn't install the keyboard hook.", L"Text Expander", MB_ICONERROR);

    if (!hadConfig) ShowEditor();  // first run: show the defaults so they can be personalised

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        if (g.editor && IsDialogMessageW(g.editor, &m)) continue;  // Tab navigation etc.
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    ReleaseMutex(mutex);
    return 0;
}
