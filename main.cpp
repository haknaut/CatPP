#ifndef UNICODE
#define UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <windowsx.h>
#include <objidl.h>
#include <gdiplus.h>
#include <richedit.h>
#include <tom.h> // ITextDocument for undo-suspended syntax highlighting
#include <commdlg.h>
#include <string>
#include <fstream>
#include <vector>
#include <sstream>
#include <algorithm>
#include <unordered_set>
#include <map>
#include <memory>
#include <cmath>
#include <dwmapi.h>
#include <shlobj.h>
#include <shellapi.h>
#include <mmsystem.h>
#include <cwctype>
#include <climits>
#include <cstdio>
#include <iterator>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "winmm.lib")

using namespace Gdiplus;

// ============================================================================
// CatPP: Pixel-perfect C++ runner matching design mockups in ui images/
// Custom topbar with macOS-like buttons, smooth GDI+ vector strokes,
// exact Google Sans Code typography, monochrome aesthetic, capsule pills.
// ============================================================================

HWND hMain = NULL, hCode = NULL, hGutter = NULL, hOut = NULL, hSuggest = NULL;
// Declared with the popup HWND because zoom/layout code also needs this state
// before the completion implementation appears later in the translation unit.
bool suggestVisible = false;
// RichEdit keeps an internal vertical range for reliable programmatic scrolling;
// its native scrollbar is hidden so the panes stay visually clean.
// Editor fonts deliberately follow code zoom. UI fonts never do: Ctrl+wheel
// changes source readability, not modal dialogs, file pickers, menus, or chrome.
HFONT hFontCode = NULL, hFontPill = NULL, hFontBold = NULL, hFontHeavy = NULL;
HFONT hFontUI = NULL, hFontUIBold = NULL;
HBRUSH hPaperBrush = NULL;
// theme-tracked solid brushes — recreated on theme change, reused every paint
HBRUSH hBrBgWin = NULL, hBrBgCard = NULL, hBrBgOut = NULL;
HBITMAP hbmCaretInvis = NULL;
// One shared custom caret overlays code and live output input; RichEdit native carets stay invisible (see CaretProc/tickCaret)
HWND hCaret = NULL;
HBRUSH hCaretBrush = NULL;
int g_caretW = 2, g_caretH = 16, g_caretTopPad = 0;
int g_baseOff = 20; // line-top → text-baseline offset (RichEdit centers tm cell in exact line box)
int g_glyphH = 21;  // baseline + descent: the box DT_BOTTOM needs to plant a baseline
int g_caretAlpha = -1; // cached blink bucket, -1 forces brush rebuild (applyTheme resets it)
WNDPROC codeOrig = NULL, outOrig = NULL;
// metrics shared by gutter so line boxes match RichEdit exactly
int g_dpi = 96, g_fontH = 16, g_lineH = 22;
// word wrap is permanent: EM_SETTARGETDEVICE re-applied only when width changes
int gWrapApplied = -1;

// Smooth wheel state follows RichEdit's visual-line model.  There is
// intentionally no painted scrollbar or Windows scrollbar chrome.
struct SmartScroll {
    HWND editor = NULL;
    bool active = false;
    int totalLines = 1, visibleLines = 1, firstLine = 0, maxFirst = 0;
    float animLine = 0.0f, targetLine = 0.0f;
};
static SmartScroll smartCode, smartOut;

// ---- Custom scrollbars ------------------------------------------------------
// Windows' scrollbar chrome is drawn outside the app's palette, so CatPP paints
// its own rail + thumb into a strip it reserves in doLayout.  One geometry table
// feeds both the paint pass and the hit test, so they can never disagree.
static const int SB_W = 6;        // thumb width
static const int SB_INSET = 16;   // right edge of the rail, same inset as the panes
static const int SB_PAD = 12;     // gap to the card's top/bottom edges
static const int SB_MIN_THUMB = 24;
static const int SB_AIR = 8;      // breathing room between pane and rail
struct ScrollbarGeom {
    RECT track{ 0, 0, 0, 0 }, thumb{ 0, 0, 0, 0 };
    bool shown = false, hover = false, drag = false;
    int dragOff = 0;
};
static ScrollbarGeom sbCode, sbOut;
static bool gSbCodeOn = false, gSbOutOn = false; // strip currently reserved (hysteresis)
inline int sbStripW() { return SB_W + SB_AIR; }

static void sbGeom(const RECT& card, const SmartScroll& sc, ScrollbarGeom& g) {
    g.shown = sc.maxFirst > 0;
    if (!g.shown) return;
    const int top = card.top + SB_PAD, bot = card.bottom - SB_PAD;
    if (bot - top < 48) { g.shown = false; return; }
    g.track = { card.right - SB_INSET - SB_W, top, card.right - SB_INSET, bot };
    const int trackH = g.track.bottom - g.track.top;
    const int total = std::max(1, sc.totalLines);
    const int vis = std::max(1, std::min(sc.visibleLines, total));
    int thumbH = (int)lround((double)trackH * vis / total);
    thumbH = std::max(SB_MIN_THUMB, std::min(thumbH, trackH));
    const float pos = sc.active ? sc.animLine : (float)sc.firstLine;
    double frac = sc.maxFirst > 0 ? (double)pos / (double)sc.maxFirst : 0.0;
    frac = std::max(0.0, std::min(frac, 1.0));
    const int y = g.track.top + (int)lround((trackH - thumbH) * frac);
    g.thumb = { g.track.left, y, g.track.right, y + thumbH };
}

// Repaint just the two rails (plus slack for the hover/active glow) rather than
// the whole window: hover and drag invalidate at mouse rate, and a full GDI+
// pass there is what makes a hand-drawn scrollbar feel sticky.
static void sbInvalidate(HWND h) {
    if (!h) return;
    RECT a = sbCode.track, b = sbOut.track, inv{};
    InflateRect(&a, 8, 8);
    InflateRect(&b, 8, 8);
    UnionRect(&inv, &a, &b);
    InvalidateRect(h, &inv, FALSE);
}

// hCode uses RichEdit's own caret (exact position, native blink).
// Only the output pane needs its native caret hidden outside the live input zone.
void pinNativeCaret(HWND h) {
    if (!h) return;
    if (!hbmCaretInvis) {
        DWORD zero = 0;
        hbmCaretInvis = CreateBitmap(1, 1, 1, 1, &zero);
    }
    DestroyCaret();
    // CreateCaret starts hidden. HideCaret also cancels a pending RichEdit
    // ShowCaret so its default white caret cannot flash below our custom bar.
    if (CreateCaret(h, hbmCaretInvis, 1, 1)) HideCaret(h);
}

// One Dark Pro Theme Palette
struct ThemeColors {
    COLORREF bgWindow;      // #21252b (RGB 33, 37, 43)
    COLORREF bgCard;        // #282c34 (RGB 40, 44, 52)
    COLORREF bgOutput;      // #21252b (RGB 33, 37, 43)
    COLORREF border;        // #3a3f4b (RGB 58, 63, 75)
    COLORREF text;          // #abb2bf (RGB 171, 178, 191)
    COLORREF gutterText;    // #5c6370 (RGB 92, 99, 112)
    COLORREF keyword;       // #c678dd (RGB 198, 120, 221)
    COLORREF type;          // #e5c07b (RGB 229, 192, 123)
    COLORREF preproc;       // #e06c75 (RGB 224, 108, 117)
    COLORREF string;        // #98c379 (RGB 152, 195, 121)
    COLORREF number;        // #d19a66 (RGB 209, 154, 102)
    COLORREF function;      // #61afef (RGB 97, 175, 239)
    COLORREF comment;       // #5c6370 (RGB 92, 99, 112)
    COLORREF op;            // #56b6c2 (RGB 86, 182, 194)
    COLORREF runBtnBg;      // #2c313a (RGB 44, 49, 58)
    COLORREF runBtnText;    // #98c379 (RGB 152, 195, 121)
    COLORREF stopBtnText;   // #e06c75 (RGB 224, 108, 117)
};

const ThemeColors ONE_DARK = {
    RGB(30, 34, 40),    // bgWindow #1e2228 (refined darker outer chrome)
    RGB(40, 44, 52),    // bgCard #282c34 (One Dark editor card)
    RGB(40, 44, 52),    // bgOutput #282c34 (matching card background)
    RGB(58, 63, 75),    // border #3a3f4b
    RGB(171, 178, 191), // text #abb2bf
    RGB(92, 99, 112),   // gutterText #5c6370
    RGB(198, 120, 221), // keyword #c678dd
    RGB(229, 192, 123), // type #e5c07b
    RGB(224, 108, 117), // preproc #e06c75
    RGB(152, 195, 121), // string #98c379
    RGB(209, 154, 102), // number #d19a66
    RGB(97, 175, 239),  // function #61afef
    RGB(92, 99, 112),   // comment #5c6370
    RGB(86, 182, 194),  // op #56b6c2
    RGB(44, 49, 58),    // runBtnBg #2c313a
    RGB(152, 195, 121), // runBtnText #98c379
    RGB(224, 108, 117)  // stopBtnText #e06c75
};

const ThemeColors ONE_LIGHT = {
    RGB(245, 246, 248), // bgWindow
    RGB(255, 255, 255), // bgCard
    RGB(255, 255, 255), // bgOutput
    RGB(215, 218, 224), // border
    RGB(56, 58, 66),    // text
    RGB(160, 161, 167), // gutterText
    RGB(166, 38, 164),  // keyword
    RGB(193, 132, 1),   // type
    RGB(228, 86, 73),   // preproc
    RGB(80, 161, 79),   // string
    RGB(152, 104, 1),   // number
    RGB(64, 120, 242),  // function
    RGB(160, 161, 167), // comment
    RGB(1, 132, 188),   // op
    RGB(235, 237, 240), // runBtnBg
    RGB(46, 160, 67),   // runBtnText
    RGB(207, 34, 46)    // stopBtnText
};

// CatPP Void — original signature syntax palette on pure black.
// Tuned for contrast/harmony: violet control flow, amber types, coral
// preproc, mint strings, orange numbers, sky functions, ice operators.
const ThemeColors VOID_THEME = {
    RGB(0, 0, 0),       // bgWindow
    RGB(0, 0, 0),       // bgCard
    RGB(0, 0, 0),       // bgOutput
    RGB(42, 42, 46),    // border
    RGB(228, 228, 231), // text
    RGB(98, 98, 108),   // gutterText
    RGB(210, 166, 255), // keyword  soft violet
    RGB(255, 198, 109), // type     amber
    RGB(255, 123, 134), // preproc  coral
    RGB(126, 231, 135), // string   mint
    RGB(255, 171, 112), // number   orange
    RGB(121, 192, 255), // function sky
    RGB(110, 118, 129), // comment  slate
    RGB(165, 214, 250), // op       ice
    RGB(18, 18, 20),    // runBtnBg
    RGB(126, 231, 135), // runBtnText
    RGB(255, 123, 134)  // stopBtnText
};

enum ThemeMode { THEME_VOID = 0, THEME_ONE_DARK = 1, THEME_ONE_LIGHT = 2 };
ThemeMode currentTheme = THEME_VOID;
bool darkTheme = true;
ThemeColors th = VOID_THEME;
// Theme cross-fade state (interpolated in tickAnimations over ~0.2 s)
ThemeColors themeFrom = VOID_THEME, themeTo = VOID_THEME;
float animThemeT = 1.0f;
bool themeAnimating = false;

int codeFontSize = 11; // Default editor font size (compact, crisp)

const int TOPBAR_H = 38; // custom topbar height (centered traffic lights / run / badge)
int hoveredBtn = -1;     // -1: none, 0: close (red), 1: min (yellow), 2: max (green)
bool hoveredRunBtn = false;
bool pressedRunBtn = false;
int pressedMacBtn = -1;
bool trackingMouse = false;

// 60 FPS Smooth UI Transitions & Animations
float animRunHover = 0.0f;
float animRunPress = 0.0f;
float animRunState = 0.0f; // 0.0 = Idle/Run, 1.0 = Running/Stop
float animMacHover[3] = { 0.0f, 0.0f, 0.0f };
float animLeftCardFocus = 1.0f;
float animRightCardFocus = 0.0f;
float animSuggestSelY = 0.0f;
bool animSuggestSelInit = false;
// Popup fade + scale (0..1) on show/hide; the window is WS_EX_LAYERED so alpha
// is applied with SetLayeredWindowAttributes while GDI keeps painting normally.
float animSuggestPop = 1.0f;
bool suggestFading = false;    // window still on screen fading out (suggestVisible already false)
int suggestBaseX = 0, suggestBaseY = 0, suggestBaseW = 0, suggestBaseH = 0;

inline Color lerpColor(const Color& c1, const Color& c2, float t) {
    if (t <= 0.0f) return c1;
    if (t >= 1.0f) return c2;
    BYTE a = (BYTE)(c1.GetA() + (c2.GetA() - c1.GetA()) * t);
    BYTE r = (BYTE)(c1.GetR() + (c2.GetR() - c1.GetR()) * t);
    BYTE g = (BYTE)(c1.GetG() + (c2.GetG() - c1.GetG()) * t);
    BYTE b = (BYTE)(c1.GetB() + (c2.GetB() - c1.GetB()) * t);
    return Color(a, r, g, b);
}

inline COLORREF lerpColorRef(COLORREF c1, COLORREF c2, float t) {
    if (t <= 0.0f) return c1;
    if (t >= 1.0f) return c2;
    int r = (int)(GetRValue(c1) + (GetRValue(c2) - GetRValue(c1)) * t);
    int g = (int)(GetGValue(c1) + (GetGValue(c2) - GetGValue(c1)) * t);
    int b = (int)(GetBValue(c1) + (GetBValue(c2) - GetBValue(c1)) * t);
    return RGB(r, g, b);
}

static ThemeColors lerpTheme(const ThemeColors& a, const ThemeColors& b, float t) {
    ThemeColors r;
    r.bgWindow    = lerpColorRef(a.bgWindow,    b.bgWindow,    t);
    r.bgCard      = lerpColorRef(a.bgCard,      b.bgCard,      t);
    r.bgOutput    = lerpColorRef(a.bgOutput,    b.bgOutput,    t);
    r.border      = lerpColorRef(a.border,      b.border,      t);
    r.text        = lerpColorRef(a.text,        b.text,        t);
    r.gutterText  = lerpColorRef(a.gutterText,  b.gutterText,  t);
    r.keyword     = lerpColorRef(a.keyword,     b.keyword,     t);
    r.type        = lerpColorRef(a.type,        b.type,        t);
    r.preproc     = lerpColorRef(a.preproc,     b.preproc,     t);
    r.string      = lerpColorRef(a.string,      b.string,      t);
    r.number      = lerpColorRef(a.number,      b.number,      t);
    r.function    = lerpColorRef(a.function,    b.function,    t);
    r.comment     = lerpColorRef(a.comment,     b.comment,     t);
    r.op          = lerpColorRef(a.op,          b.op,          t);
    r.runBtnBg    = lerpColorRef(a.runBtnBg,    b.runBtnBg,    t);
    r.runBtnText  = lerpColorRef(a.runBtnText,  b.runBtnText,  t);
    r.stopBtnText = lerpColorRef(a.stopBtnText, b.stopBtnText, t);
    return r;
}

const int M = 14;      // outer margin
const int GAP = 14;    // gap between cards
const int PRAD = 20;   // play button radius

// Window controls: one geometry table for the paint pass and the hit test.
// 20px squares on a 24px pitch, with marks that actually read at 120%+ DPI.
const int MAC_BTN_R = 10;                    // half the square
const int MAC_BTN_CX[3] = { 24, 48, 72 };    // close, minimize, maximize
inline int macBtnCY() { return TOPBAR_H / 2; }
int getMacBtnHit(int x, int y) {
    if (y < 2 || y > MAC_BTN_R * 2 + 2) return -1;
    int cy = macBtnCY();
    for (int i = 0; i < 3; i++) {
        int dx = x - MAC_BTN_CX[i];
        int dy = y - cy;
        if (dx <= MAC_BTN_R && dx >= -MAC_BTN_R && dy <= MAC_BTN_R && dy >= -MAC_BTN_R)
            return i;
    }
    return -1;
}

// Shared geometry so hit-test, hover-invalidate and paint can never drift apart.
const int RUN_BTN_W = 86, RUN_BTN_H = 26; // same height as the output pill
void runBtnRect(int winW, RECT* r) {
    int x = winW - RUN_BTN_W - 14;
    int y = (TOPBAR_H - RUN_BTN_H) / 2; // exact vertical center of the topbar
    r->left = x; r->top = y; r->right = x + RUN_BTN_W; r->bottom = y + RUN_BTN_H;
}
bool isOverRunBtn(int x, int y, int winW) {
    RECT r; runBtnRect(winW, &r);
    return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom;
}

std::wstring tmpDir;
static bool ownsTmpDir = false;
static unsigned long runArtifactSerial = 0;
static std::wstring runSourcePath, runExePath;
std::wstring fontFace = L"Google Sans Code Monospaced"; // real family name inside the shipped TTF
std::wstring spaceGroteskFamily = L"Space Grotesk";     // real family inside SpaceGrotesk-VariableFont_wght.ttf (RCDATA 103)
static bool spaceGroteskFamilyOk = false;                // GDI resolved that family - safe to use
static std::wstring g_codeFontFile;                      // resolved path of the shipped editor font
static std::wstring g_uiFontFile;                        // resolved path of the shipped chrome font
int g_uiCapH = 0;                                        // chrome cap height in px, from the font's OS/2 table
int g_uiEmPx = 0;                                        // chrome font em in px (the size g_uiCapH scales with)

// Family for all app text: Space Grotesk when it is installed, else the embedded
// Google Sans Code Monospaced.
static const wchar_t* uiFontFamily() {
    return spaceGroteskFamilyOk ? spaceGroteskFamily.c_str() : fontFace.c_str();
}

// decls used by getGppPath helpers (defined below in Forward declarations block)
std::string w2u(const std::wstring& w);
std::wstring u2w(const std::string& s);
int uiMessageBox(const wchar_t* title, const wchar_t* text,
                 const wchar_t* b0, const wchar_t* b1 = nullptr, const wchar_t* b2 = nullptr);
bool uiInputBox(const wchar_t* title, const wchar_t* msg, std::wstring* out,
                const wchar_t* initial = NULL);
static bool readFileBytes(const std::wstring& path, std::string& out);
static bool writeFileBytes(const std::wstring& path, const std::string& data);

static int CALLBACK fontFamilyFound(const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM p) {
    *(bool*)p = true;
    return 0;
}

// Is this family really installed for the process (private AddFontResourceEx or
// system)? EnumFontFamiliesExW matches the family name, which is how a real face
// is told apart from the font mapper silently substituting something else.
static bool FontFamilyAvailable(const wchar_t* family) {
    HDC hdc = GetDC(NULL);
    if (!hdc) return false;
    LOGFONTW lf{};
    lf.lfCharSet = DEFAULT_CHARSET;
    lstrcpynW(lf.lfFaceName, family, LF_FACESIZE);
    bool found = false;
    EnumFontFamiliesExW(hdc, &lf, (FONTENUMPROCW)fontFamilyFound, (LPARAM)&found, 0);
    ReleaseDC(NULL, hdc);
    return found;
}

static std::wstring gppCached;
static bool gppTriedConfig = false;

static std::wstring exeDirW() {
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(NULL, exe, MAX_PATH);
    std::wstring d = exe;
    size_t s = d.find_last_of(L"\\/");
    return s == std::wstring::npos ? d : d.substr(0, s);
}

static bool isGppFile(const std::wstring& p) {
    if (p.empty()) return false;
    DWORD a = GetFileAttributesW(p.c_str());
    if (a == INVALID_FILE_ATTRIBUTES || (a & FILE_ATTRIBUTE_DIRECTORY)) return false;
    std::wstring b = p;
    size_t s = b.find_last_of(L"\\/");
    b = (s == std::wstring::npos) ? b : b.substr(s + 1);
    return _wcsicmp(b.c_str(), L"g++.exe") == 0;
}

// Accept a pasted path: folder, full path to g++.exe, or bare name (search PATH later).
static std::wstring normalizeGppInput(std::wstring in) {
    while (!in.empty() && (in.front() == L'"' || in.front() == L' ' || in.front() == L'\t')) in.erase(in.begin());
    while (!in.empty() && (in.back() == L'"' || in.back() == L' ' || in.back() == L'\t' || in.back() == L'\\' || in.back() == L'/')) in.pop_back();
    if (in.empty()) return L"";
    DWORD a = GetFileAttributesW(in.c_str());
    if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) {
        std::wstring p = in + L"\\g++.exe";
        return isGppFile(p) ? p : L"";
    }
    if (isGppFile(in)) return in;
    // maybe user pasted ...\bin without g++.exe — try appending
    if (a == INVALID_FILE_ATTRIBUTES) {
        std::wstring p = in + L"\\g++.exe";
        if (isGppFile(p)) return p;
    }
    return L"";
}

static std::wstring gppConfigPath() {
    // Prefer next to exe; fall back to %TEMP% if that dir isn't writable
    std::wstring beside = exeDirW() + L"\\gpp_path.txt";
    HANDLE h = CreateFileW(beside.c_str(), GENERIC_WRITE, 0, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); return beside; }
    wchar_t t[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, t);
    return std::wstring(t) + L"CatPP_gpp_path.txt";
}

static std::wstring loadGppConfig() {
    std::string raw;
    if (!readFileBytes(gppConfigPath(), raw)) return L"";
    return normalizeGppInput(u2w(raw));
}

static void saveGppConfig(const std::wstring& gpp) {
    if (!gpp.empty()) writeFileBytes(gppConfigPath(), w2u(gpp));
}

static void scanForGpp(const std::wstring& root, int depth, std::wstring& found) {
    if (!found.empty() || depth <= 0 || root.empty()) return;
    std::wstring pat = root;
    if (!pat.empty() && pat.back() != L'\\' && pat.back() != L'/') pat += L'\\';
    pat += L"*";
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(pat.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        std::wstring child = root + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            // Do not recurse into junctions/symlinks: an install can contain a
            // loop back to an ancestor and used to make compiler discovery crawl.
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
            // common layout: ...\bin\g++.exe or ...\mingw64\bin\g++.exe
            std::wstring g = child + L"\\bin\\g++.exe";
            if (isGppFile(g)) { found = g; break; }
            g = child + L"\\g++.exe";
            if (isGppFile(g)) { found = g; break; }
            scanForGpp(child, depth - 1, found);
            if (!found.empty()) break;
        } else if (_wcsicmp(fd.cFileName, L"g++.exe") == 0) {
            found = child;
            break;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static std::wstring findGppOnPath() {
    wchar_t buf[32767] = {};
    DWORD n = GetEnvironmentVariableW(L"PATH", buf, 32767);
    if (n == 0 || n >= 32767) return L"";
    std::wstring path(buf, n);
    size_t start = 0;
    while (start <= path.size()) {
        size_t end = path.find(L';', start);
        if (end == std::wstring::npos) end = path.size();
        std::wstring dir = path.substr(start, end - start);
        start = end + 1;
        if (dir.empty()) continue;
        if (dir.front() == L'"' && dir.back() == L'"' && dir.size() >= 2)
            dir = dir.substr(1, dir.size() - 2);
        std::wstring g = dir + L"\\g++.exe";
        if (isGppFile(g)) return g;
        if (start > path.size()) break;
    }
    return L"";
}

std::wstring getGppPath() {
    if (!gppCached.empty()) return gppCached;

    if (!gppTriedConfig) {
        gppTriedConfig = true;
        gppCached = loadGppConfig();
        if (!gppCached.empty()) return gppCached;
    }

    const wchar_t* candidates[] = {
        L"C:\\msys64\\mingw64\\bin\\g++.exe",
        L"C:\\msys64\\ucrt64\\bin\\g++.exe",
        L"C:\\msys64\\clang64\\bin\\g++.exe",
        L"C:\\msys64\\mingw32\\bin\\g++.exe",
        L"C:\\msys32\\mingw32\\bin\\g++.exe",
        L"C:\\msys32\\mingw64\\bin\\g++.exe",
        L"C:\\mingw64\\bin\\g++.exe",
        L"C:\\mingw32\\bin\\g++.exe",
        L"C:\\MinGW\\bin\\g++.exe",
        L"C:\\MinGW64\\bin\\g++.exe",
        L"C:\\TDM-GCC-64\\bin\\g++.exe",
        L"C:\\Strawberry\\c\\bin\\g++.exe",
        L"C:\\Program Files\\mingw-w64\\bin\\g++.exe",
        L"C:\\Program Files (x86)\\mingw-w64\\bin\\g++.exe",
        L"C:\\ProgramData\\chocolatey\\bin\\g++.exe",
        L"C:\\Program Files\\Git\\mingw64\\bin\\g++.exe",
        L"C:\\Program Files\\Git\\usr\\bin\\g++.exe",
        L"C:\\Program Files\\LLVM\\bin\\g++.exe",
        L"C:\\tools\\mingw64\\bin\\g++.exe",
        L"C:\\Program Files\\CodeBlocks\\MinGW\\bin\\g++.exe",
    };
    for (auto* p : candidates) {
        if (isGppFile(p)) {
            gppCached = p;
            return gppCached;
        }
    }

    gppCached = findGppOnPath();
    if (!gppCached.empty()) return gppCached;

    // scoop / local installs
    wchar_t home[MAX_PATH] = {};
    SHGetFolderPathW(NULL, CSIDL_PROFILE, NULL, SHGFP_TYPE_CURRENT, home);
    std::wstring scoop = std::wstring(home) + L"\\scoop\\apps";
    scanForGpp(scoop, 3, gppCached);
    if (!gppCached.empty()) return gppCached;

    scanForGpp(L"C:\\Program Files\\mingw-w64", 4, gppCached);
    if (!gppCached.empty()) return gppCached;
    scanForGpp(L"C:\\Program Files (x86)\\mingw-w64", 4, gppCached);
    if (!gppCached.empty()) return gppCached;
    scanForGpp(L"C:\\ProgramData\\chocolatey\\lib", 4, gppCached);
    if (!gppCached.empty()) return gppCached;

    wchar_t local[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, SHGFP_TYPE_CURRENT, local))) {
        scanForGpp(std::wstring(local) + L"\\Microsoft\\WinGet\\Packages", 4, gppCached);
        if (!gppCached.empty()) return gppCached;
    }

    return L"";
}

// The executable itself starts from the compiler directory, but a dynamically
// linked program needs MinGW's runtime DLLs on PATH.  Add that directory once,
// not once per Run click (the old code steadily grew PATH until launches failed).
static void addCompilerBinToPath(const std::wstring& gpp) {
    size_t slash = gpp.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return;
    std::wstring bin = gpp.substr(0, slash);
    if (bin.empty()) return;

    wchar_t oldBuf[32767] = {};
    DWORD n = GetEnvironmentVariableW(L"PATH", oldBuf, 32767);
    std::wstring oldPath = (n > 0 && n < 32767) ? std::wstring(oldBuf, n) : L"";
    size_t pos = 0;
    while (pos <= oldPath.size()) {
        size_t end = oldPath.find(L';', pos);
        if (end == std::wstring::npos) end = oldPath.size();
        std::wstring item = oldPath.substr(pos, end - pos);
        if (item.size() >= 2 && item.front() == L'"' && item.back() == L'"')
            item = item.substr(1, item.size() - 2);
        if (_wcsicmp(item.c_str(), bin.c_str()) == 0) return;
        if (end == oldPath.size()) break;
        pos = end + 1;
    }
    std::wstring updated = bin + (oldPath.empty() ? L"" : L";" + oldPath);
    if (updated.size() < 32767) SetEnvironmentVariableW(L"PATH", updated.c_str());
}

// Resolve compiler: search, else ask user to paste MinGW / g++ path. false = cancel.
bool ensureGpp(std::wstring& out) {
    out = getGppPath();
    while (out.empty()) {
        std::wstring in;
        if (!uiInputBox(L"",
                        L"g++ / MinGW not found.\n"
                        L"Paste the full path to g++.exe or the MinGW bin folder:",
                        &in))
            return false;
        out = normalizeGppInput(in);
        if (out.empty()) {
            uiMessageBox(L"", L"Path doesn't contain g++.exe.\nCheck the path and try again.", L"OK");
            continue;
        }
        gppCached = out;
        saveGppConfig(out);
    }
    addCompilerBinToPath(out);
    return true;
}

#define WM_APPEND (WM_APP + 1)
#define WM_DONE   (WM_APP + 2)

HANDLE sProc = NULL, sInW = NULL;
int sGen = 0, progLen = 0;
bool sAlive = false;

// Auto syntax-check state (startCheck/checkTick): the last buffer handed to
// g++, and whether a program's output currently owns the output pane.
static std::wstring gLastChecked;
static bool gOutHasProg = false;
static bool gOutIsCheck = false; // pane currently holds a syntax-check report, not program output
static std::wstring gOutNotePath; // document the "no syntax check" notice was written for

// RichEdit internal length (CR-only) — must match EM_GETSEL. GetWindowTextLength /
// WM_GETTEXTLENGTH report CRLF form and over-count, which hid the caret and
// blocked backspace (compared against an inflated progLen).
static int outDocLen(HWND h) {
    if (!h) return 0;
    GETTEXTLENGTHEX gtl{};
    gtl.flags = GTL_NUMCHARS; // no GTL_USECRLF → \r-only, same space as EM_GETSEL
    gtl.codepage = CP_ACP;
    LRESULT n = SendMessage(h, EM_GETTEXTLENGTHEX, (WPARAM)&gtl, 0);
    if (n < 0) n = GetWindowTextLengthW(h);
    return (int)n;
}

// Document model — one buffer at a time (no tabs; old multi-doc stack orphaned files)
struct Doc {
    std::wstring path;
    std::wstring text;
    bool dirty = false;
};
std::vector<Doc> docs;
int cur = -1;

// File-type dispatch: .h/.c/.cpp share the C++ lexer, .md/.glsl/.hlsl get their
// own pass, .txt gets none. Extension-less / unknown = C++ (historical default).
enum DocLang { LANG_CPP, LANG_MD, LANG_SHADER, LANG_BAT, LANG_TXT };
// Language follows the extension, and the DEFAULT for a name we do not recognise
// is plain text — never C++.  Treating every unknown file as C++ is what made the
// app report phantom compile errors ("expected unqualified-id") for .rc, .json,
// .log and anything else the compiler was never meant to see.
static DocLang docLang(const std::wstring& p) {
    if (p.empty()) return LANG_CPP; // untitled buffer: new docs are C++ by default
    size_t slash = p.find_last_of(L"\\/");
    size_t dot = p.find_last_of(L'.');
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash)) return LANG_TXT;
    std::wstring e = p.substr(dot + 1);
    for (auto& c : e) if (c >= L'A' && c <= L'Z') c += 32;
    if (e == L"md" || e == L"markdown") return LANG_MD;
    if (e == L"glsl" || e == L"vert" || e == L"frag" || e == L"hlsl") return LANG_SHADER;
    if (e == L"bat" || e == L"cmd") return LANG_BAT;
    if (e == L"c" || e == L"cc" || e == L"cpp" || e == L"cxx" || e == L"c++" ||
        e == L"h" || e == L"hh" || e == L"hpp" || e == L"hxx" || e == L"h++" ||
        e == L"inl" || e == L"ipp" || e == L"tpp" || e == L"ino" || e == L"m" || e == L"mm")
        return LANG_CPP;
    return LANG_TXT;
}
static DocLang curLang() {
    return (cur >= 0 && cur < (int)docs.size()) ? docLang(docs[cur].path) : LANG_CPP;
}

// Files panel — one folder open at a time; clicking a file loads it into the single editor
static const int EXP_W = 216;      // min width / default
static const int EXP_W_MAX = 432;  // 2x
int expWCur = EXP_W;               // ponytail: one int, drag clamps it
// Slide open/close: the DISPLAYED width eases toward expWCur (open) or 0 (closed);
// panels() and hFiles' show/hide key off this, so the panel animates instead of snapping.
float animExpWCur = (float)EXP_W;
bool expSliding = false;
bool expHidden = false;            // Ctrl+E toggle
bool expWDirty = false;            // explorer splitter changed; committed on a visual frame
bool expDrag = false;
bool gWindowSizing = false;        // defer expensive rewrap/highlight until resize ends
bool gLayoutDirty = false;         // coalesce live WM_SIZE bursts to display cadence
static const int EXP_HEAD_H = 42;
static const int EXP_ROW_H = 24;
HWND hFiles = NULL;
std::wstring expRoot;
struct TreeNode {
    std::wstring path, name;
    bool dir = false, open = false, scanned = false;
    std::vector<TreeNode> kids;
};
std::vector<TreeNode> expRoots;
struct TreeRow { TreeNode* node; int depth; };
std::vector<TreeRow> expRows;
int expScroll = 0, expHover = -1;
// Explorer hover band glides between rows on the main animation clock instead
// of snapping; alpha fades it in on enter and out on leave.
float animExpHoverY = 0.0f, animExpHoverA = 0.0f;
bool animExpHoverInit = false;

std::wstring baseName(const std::wstring& p);

std::wstring mainTitle() {
    if (cur < 0 || cur >= (int)docs.size() || docs[cur].path.empty())
        return L"CatPP";
    std::wstring t = baseName(docs[cur].path);
    if (docs[cur].dirty) t += L" *";
    t += L" — CatPP";
    return t;
}

// Forward declarations
void doLayout();
void startRun();
void stopSession(bool kill);
void insertProg(const std::wstring& norm);
void syncOutCaret();
void updateFonts();
void updateControlFormatting();
static void paraBounds(HWND h, long idx, long& ps, long& pe);
static long charOfPara(long paraIdx);
void highlightCode();
// Hard-line paragraph map validity (see ensureParaCache). Declared before
// showDoc so buffer-replacement paths can invalidate it.
bool g_paraCacheValid = false;

// Syntax highlight recolors via EM_SETCHARFORMAT — undoable units that would
// sit on top of typing, so the first Ctrl+Z would revert invisible
// formatting (selecting a span) instead of text. Suspend collection around
// the format passes; text edits elsewhere keep normal undo.
static void setHlUndo(HWND h, bool suspend) {
    if (!h) return;
    IUnknown* unk = NULL;
    if (!SendMessage(h, EM_GETOLEINTERFACE, 0, (LPARAM)&unk) || !unk) return;
    // IID_ITextDocument {8CC497C0-A1DF-11CE-8098-00AA0047BE5D} — declared
    // locally: mingw's libuuid doesn't export the tom.h symbol.
    static const GUID IID_TxtDoc =
        { 0x8CC497C0, 0xA1DF, 0x11CE, { 0x80, 0x98, 0x00, 0xAA, 0x00, 0x47, 0xBE, 0x5D } };
    ITextDocument* doc = NULL;
    if (unk->QueryInterface(IID_TxtDoc, (void**)&doc) == S_OK && doc) {
        doc->Undo(suspend ? tomSuspend : tomResume, NULL);
        doc->Release();
    }
    unk->Release();
}
void setZoom(int newSize);
void setTheme(ThemeMode mode);
void refreshChrome();
void hideSuggestions();
void updateSuggestions();
void applySelectedSnippet();
void tickAnimations();
void tickCaret();
static bool outCaretClientPoint(int caretIdx, POINTL& pt);
void smartRefresh(SmartScroll& sc);
void smartWheel(SmartScroll& sc, WPARAM w);
void tickSmartScrolls(double dt);
bool loadPath(const std::wstring& p);
void openFolder();
void findUI();
void findNext();
void findReplace();
LRESULT CALLBACK TreeProc(HWND h, UINT m, WPARAM w, LPARAM l);
LRESULT CALLBACK SuggestProc(HWND h, UINT m, WPARAM w, LPARAM l);
LRESULT CALLBACK UiMsgProc(HWND h, UINT m, WPARAM w, LPARAM l);
LRESULT CALLBACK UiFileProc(HWND h, UINT m, WPARAM w, LPARAM l);
LRESULT CALLBACK UiMenuProc(HWND h, UINT m, WPARAM w, LPARAM l);
bool uiFileDialog(bool save, const std::wstring& initial, std::wstring* out, bool folderMode = false);
void uiShowMenu(int x, int y);

// Helper: UTF conversions (explicit length — the old -1/+1 pattern wrote one past the buffer)
std::string w2u(const std::wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), NULL, 0, NULL, NULL);
    if (n <= 0) return "";
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, NULL, NULL);
    return s;
}

std::wstring u2w(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), NULL, 0);
    UINT cp = CP_UTF8;
    DWORD flags = 0;
    if (n <= 0) {
        cp = CP_ACP;
        n = MultiByteToWideChar(cp, 0, s.data(), (int)s.size(), NULL, 0);
        flags = 0;
    }
    if (n <= 0) return L"";
    std::wstring w(n, L'\0');
    MultiByteToWideChar(cp, flags, s.data(), (int)s.size(), w.data(), n);
    return w;
}

// Read/write bytes through Win32 so non-ASCII Windows paths work on every
// supported MinGW libstdc++ version.  The old std::ifstream/ofstream(wchar_t*)
// calls are not portable and fail to compile with several MinGW distributions.
static bool readFileBytes(const std::wstring& path, std::string& out) {
    out.clear();
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(f, &size) != FALSE && size.QuadPart >= 0 &&
              (unsigned long long)size.QuadPart <= (unsigned long long)(size_t)-1;
    if (!ok) { CloseHandle(f); return false; }
    out.resize((size_t)size.QuadPart);
    size_t done = 0;
    while (done < out.size()) {
        DWORD want = (DWORD)std::min<size_t>(out.size() - done, 1u << 20);
        DWORD got = 0;
        if (!ReadFile(f, out.data() + done, want, &got, NULL) || got == 0) {
            out.clear();
            ok = false;
            break;
        }
        done += got;
    }
    CloseHandle(f);
    return ok;
}

static bool writeFileBytes(const std::wstring& path, const std::string& data) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, NULL,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return false;
    size_t done = 0;
    bool ok = true;
    while (done < data.size()) {
        DWORD want = (DWORD)std::min<size_t>(data.size() - done, 1u << 20);
        DWORD wrote = 0;
        if (!WriteFile(f, data.data() + done, want, &wrote, NULL) || wrote == 0) {
            ok = false;
            break;
        }
        done += wrote;
    }
    if (!FlushFileBuffers(f)) ok = false;
    CloseHandle(f);
    return ok;
}

std::wstring getCodeW();

// ---- Crash-safe session autosave -------------------------------------------
// ponytail: plain UTF-8 lines in %TEMP%, rewritten on a timer and on a clean
// exit. Restored on the next launch, so closing the window by mistake (or a
// crash) never loses the buffer. Format:
//   CatPP <ts> <path or "-"> <caret>
//   <text, CRLF preserved>
static std::wstring sessionPath() {
    wchar_t t[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, t);
    return std::wstring(t) + L"Catpp_session.txt";
}

static void saveSession() {
    if (!hCode) return;
    std::wstring text = getCodeW();
    // The recovery file only exists to hold work that is NOT on disk. A clean
    // buffer has nothing to lose, so never write it — and drop one left behind
    // by an older build or by a buffer that has since been saved.
    bool dirty = (cur >= 0 && cur < (int)docs.size()) ? docs[cur].dirty : false;
    if (text.empty() || !dirty) { DeleteFileW(sessionPath().c_str()); return; }
    long caret = 0;
    CHARRANGE cr{};
    SendMessage(hCode, EM_EXGETSEL, 0, (LPARAM)&cr);
    caret = cr.cpMin > cr.cpMax ? cr.cpMin : cr.cpMax;
    // The path is quoted: it routinely contains spaces ("Documents\\cpp com"),
    // and a whitespace-delimited header silently truncated it to its first
    // segment, so the buffer came back attached to a path that never existed.
    std::wstring head = L"CatPP 1 \"" + (docs[cur].path.empty() ? std::wstring(L"-") : docs[cur].path) +
           L"\" " + std::to_wstring(caret);
    std::string blob = w2u(head + L"\r\n" + text);
    writeFileBytes(sessionPath(), blob);
}

// The editor buffer is always CRLF (RichEdit normalizes), while a file on disk
// may hold any mix of CR, LF and CRLF. Fold both sides to LF before comparing.
static void foldEol(std::wstring& s) {
    std::wstring o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] != L'\r') { o += s[i]; }
        else {
            o += L'\n';
            if (i + 1 < s.size() && s[i + 1] == L'\n') i++;
        }
    }
    s.swap(o);
}

static bool loadSession(std::wstring& text, std::wstring& path, long& caret) {
    std::string raw;
    if (!readFileBytes(sessionPath(), raw) || raw.size() < 12) return false;
    std::wstring all = u2w(raw);
    size_t nl = all.find(L'\n');
    if (nl == std::wstring::npos) return false;
    std::wstring head = all.substr(0, nl);
    if (head.rfind(L"CatPP ", 0) != 0) return false;   // not ours / truncated header
    text = all.substr(nl + 1);
    if (text.empty()) return false;
    std::wstringstream ss(head.substr(6));
    int ver = 0;
    std::wstring rest;
    if (!(ss >> ver) || !std::getline(ss, rest)) return false;
    size_t q = rest.find(L'"');
    if (q != std::wstring::npos) {
        size_t q2 = rest.find(L'"', q + 1);
        if (q2 == std::wstring::npos) return false;
        path = rest.substr(q + 1, q2 - q - 1);
        rest = rest.substr(q2 + 1);
    } else {
        // Older sessions wrote a bare token; keep reading them (minus paths with spaces).
        std::wstringstream rs(rest);
        rs >> path;
        std::getline(rs, rest);
    }
    if (path == L"-") path.clear();
    std::wstringstream ms(rest);
    if (!(ms >> caret)) caret = 0;
    return true;
}

// Session autosave: crash safety is the point, so it must not depend on the
// app closing cleanly — 1.2 s after the last edit.
static void sessionTick() {
    static ULONGLONG last = 0;
    ULONGLONG t = GetTickCount64();
    if (last && t - last < 1200) return;
    last = t;
    saveSession();
}

std::wstring getCodeW() {
    int n = GetWindowTextLengthW(hCode);
    if (n <= 0) return L"";
    // GetWindowTextW always writes a terminating NUL.  The old n-character
    // allocation passed n + 1 and wrote one wchar_t past std::wstring storage.
    std::wstring s((size_t)n + 1, L'\0');
    int got = GetWindowTextW(hCode, s.data(), n + 1);
    if (got < 0) return L"";
    if (got > n) got = n;
    s.resize((size_t)got);
    return s;
}

std::wstring baseName(const std::wstring& p) {
    auto i = p.find_last_of(L"\\/");
    return i == std::wstring::npos ? p : p.substr(i + 1);
}

std::wstring docTitle(int i) {
    if (i < 0 || i >= (int)docs.size()) return L"";
    return docs[i].path.empty() ? L"" : baseName(docs[i].path);
}

bool hasFilePill() {
    if (cur < 0 || cur >= (int)docs.size()) return false;
    return !docs[cur].path.empty();
}

// Each app process gets a private temp directory and every run gets fresh names.
// That avoids two CatPP instances overwriting each other's source/exe and avoids
// a just-terminated child keeping the next run's executable locked.
static void makeRunArtifactPaths() {
    ++runArtifactSerial;
    std::wstring stem = L"CatPP_run_" + std::to_wstring(GetCurrentProcessId()) +
                        L"_" + std::to_wstring(runArtifactSerial);
    runSourcePath = tmpDir + L"\\" + stem + L".cpp";
    runExePath = tmpDir + L"\\" + stem + L".exe";
}

// Geometry helper: calculate Left and Right card rectangles (+ explorer when open)
void panels(RECT* L, RECT* Rr, RECT* E = NULL) {
    RECT c;
    GetClientRect(hMain, &c);
    int topY = TOPBAR_H;
    const int clientW = c.right - c.left;
    // Keep usable minimum widths for the editor/output cards while the explorer
    // is open.  The stored drag width remains intact and expands again later.
    int expCardW = 0;
    if (!expRoot.empty() && animExpWCur > 0.5f) {
        const int minBothCards = 520;
        int maxExplorer = clientW - 2 * M - 2 * GAP - minBothCards;
        if (maxExplorer > 0) expCardW = std::min((int)animExpWCur, std::min(EXP_W_MAX, maxExplorer));
    }
    int expW = expCardW ? expCardW + GAP : 0;
    int tot = clientW - 2 * M - GAP - expW;
    if (tot < 200) tot = 200;
    // Exactly 60.78% split matching 1920x1080 mockup (1119px left card / 1841px total)
    int pw = (int)(tot * (1119.0 / 1841.0));
    int x0 = M + expW;
    *L  = { x0, topY, x0 + pw, c.bottom - M };
    *Rr = { x0 + pw + GAP, topY, c.right - M, c.bottom - M };
    if (E) {
        if (expCardW) *E = { M, topY, M + expCardW, c.bottom - M };
        else *E = { 0, 0, 0, 0 };
    }
}

// GDI+ GraphicsPath Helpers
// Minimalist pass: every surface is a hard-edged rectangle. Cards, pills, buttons,
// row highlights and scrollbar thumbs all come through this one square builder.
void addRect(GraphicsPath& path, float x, float y, float w, float h) {
    path.AddRectangle(RectF(x, y, w, h));
}

// Document Management
void showDoc(int i) {
    hideSuggestions();
    // Only snapshot the live buffer when switching AWAY — showDoc(i) after
    // externally replacing docs[i].text must not clobber it (open/new bug).
    if (cur >= 0 && cur < (int)docs.size() && cur != i) {
        docs[cur].text = getCodeW();
    }
    cur = i;
    bool d = docs[i].dirty;
    SetWindowTextW(hCode, docs[i].text.c_str());
    SendMessage(hCode, EM_EMPTYUNDOBUFFER, 0, 0);
    g_paraCacheValid = false; // whole buffer replaced — paragraph map is stale
    docs[i].dirty = d; // SetWindowText fires EN_CHANGE — don't mark a clean load dirty
    updateControlFormatting();
    highlightCode();
    // EN_CHANGE for the programmatic load can arrive (sync or pumped) during the
    // apply loop's SendMessages — undo its dirty star / re-highlight timer here.
    KillTimer(hMain, 101);
    docs[i].dirty = d;
    gLastChecked.clear();    // fresh buffer — syntax-check it once it settles
    SetTimer(hMain, 103, 1000, NULL);
    SetWindowTextW(hMain, mainTitle().c_str());
    refreshChrome();
}

bool saveDoc(bool saveAs);

// Discard buffer? Yes=discard, No=save first, Cancel=abort. false=abort.
bool confirmDiscard() {
    if (cur < 0 || cur >= (int)docs.size() || !docs[cur].dirty) return true;
    int r = uiMessageBox(L"", L"Discard unsaved changes?", L"Yes", L"No", L"Cancel");
    if (r == 0) return true;
    if (r == 1) return saveDoc(false);
    return false;
}

void newDoc() {
    if (!confirmDiscard()) return;
    hideSuggestions();
    if (docs.empty()) docs.push_back({});
    docs[0].path.clear();
    docs[0].text.clear();
    docs[0].dirty = false;
    showDoc(0);
}

bool saveDoc(bool saveAs) {
    if (cur < 0 || cur >= (int)docs.size()) return false;
    std::wstring p = docs[cur].path;
    if (saveAs || p.empty()) {
        std::wstring chosen;
        if (!uiFileDialog(true, p, &chosen)) return false;
        p = chosen;
    }
    docs[cur].text = getCodeW();
    std::string u = w2u(docs[cur].text);
    if (!writeFileBytes(p, u)) {
        uiMessageBox(L"", L"Save failed.", L"OK");
        return false;
    }
    DocLang before = docLang(docs[cur].path);
    docs[cur].path = p;
    docs[cur].dirty = false;
    if (docLang(p) != before) highlightCode(); // save-as crossed languages — re-lex
    refreshChrome();
    return true;
}

void openDoc() {
    std::wstring chosen;
    std::wstring initial;
    if (cur >= 0 && cur < (int)docs.size()) initial = docs[cur].path;
    if (!uiFileDialog(false, initial, &chosen)) return;
    loadPath(chosen);
}

void refreshChrome() {
    if (hMain) SetWindowTextW(hMain, mainTitle().c_str());
    InvalidateRect(hMain, NULL, FALSE);
}

// ============================================================================
// Files panel — folder tree; click a file → load into the single editor
// ============================================================================
static void expScan(TreeNode& n) {
    n.scanned = true;
    n.kids.clear();
    std::wstring pat = n.path + L"\\*";
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(pat.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            bool d = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            n.kids.push_back({ n.path + L"\\" + fd.cFileName, fd.cFileName, d, false, false, {} });
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(n.kids.begin(), n.kids.end(), [](const TreeNode& a, const TreeNode& b) {
        if (a.dir != b.dir) return a.dir;
        return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
    });
}

static void expFlatten(TreeNode* n, int depth) {
    for (auto& k : n->kids) {
        expRows.push_back({ &k, depth });
        if (k.dir && k.open) expFlatten(&k, depth + 1);
    }
}

static void expRebuild() {
    expRows.clear();
    for (auto& r : expRoots) {
        expRows.push_back({ &r, 0 });
        if (r.dir && r.open) expFlatten(&r, 1);
    }
}

// Loads p into the editor. false = aborted (dirty + user cancel) or unreadable.
bool loadPath(const std::wstring& p) {
    if (p.empty()) return false;
    if (!confirmDiscard()) return false;
    std::string raw;
    if (!readFileBytes(p, raw)) return false;
    if (docs.empty()) docs.push_back({});
    docs[0].path = p;
    docs[0].text = u2w(raw);
    docs[0].dirty = false;
    showDoc(0);
    if (hFiles) InvalidateRect(hFiles, NULL, FALSE);
    return true;
}

static void expSetRoot(const std::wstring& folderIn) {
    std::wstring folder = folderIn;
    DWORD a = GetFileAttributesW(folder.c_str());
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) return;
    while (folder.size() > 3 && (folder.back() == L'\\' || folder.back() == L'/')) folder.pop_back();
    expRoot = folder;
    expRoots.clear();
    TreeNode root{ folder, baseName(folder), true, true, false, {} };
    expScan(root);
    expRoots.push_back(std::move(root));
    expRebuild();
    expScroll = 0;
    expHover = -1;
    expHidden = false;         // opening a folder reveals the panel
    animExpWCur = 0.0f;        // slide it in from the edge
    expSliding = true;
    doLayout();
    if (hFiles) InvalidateRect(hFiles, NULL, FALSE);
}

void openFolder() {
    std::wstring chosen;
    std::wstring initial;
    if (cur >= 0 && cur < (int)docs.size()) initial = docs[cur].path;
    if (!uiFileDialog(false, initial, &chosen, true)) return;
    expSetRoot(chosen);
}

void toggleExplorer() {
    if (expRoot.empty()) return;
    expHidden = !expHidden;
    expSliding = true;
    doLayout();
    if (hMain) InvalidateRect(hMain, NULL, FALSE);
}

// ponytail: drag zone on the explorer card's right edge (7px band, outside hFiles' 14px inset)
static bool expEdgeHit(int x, int y) {
    if (expRoot.empty() || expHidden) return false;
    RECT L, Rr, E;
    panels(&L, &Rr, &E);
    return y >= E.top && y <= E.bottom && abs(x - E.right) <= 7;
}

// ============================================================================
// Themed modal UI — message box, file open/save, context menu (no OS dialogs)
// ============================================================================
static bool uiDone = false;
static int uiVal = 0;
static std::wstring uiPath;
static HWND hUi = NULL;

// shared chrome metrics for dialogs
static const int UI_PAD = 18;
static const int UI_BTN_W = 88;
static const int UI_BTN_H = 30;
static const int UI_BTN_GAP = 10;
static const int UI_ROW_H = 26;

static std::wstring uiTitle, uiMsg;
static std::vector<std::wstring> uiBtnLabels;
// Shortcuts Dialog — two-column layout (key | description)
static bool uiShortcutMode = false;
static const wchar_t* uiKeys[32];
static const wchar_t* uiDescs[32];
static int uiKeyCount = 0;
static RECT uiBtnRc[6];
static int uiBtnN = 0;
static int uiHoverBtn = -1;
static int uiTitleH() { return uiTitle.empty() ? 0 : 44; }

static bool uiSaveMode = false;
static bool uiFolderMode = false; // file dialog in "pick a folder" mode (explorer)
static std::wstring uiSaveDefExt = L".cpp"; // appended when a saved name has none
static std::wstring uiDir, uiName;
struct UiEntry { bool dir; std::wstring name; };
static std::vector<UiEntry> uiEntries;
static int uiSel = -1, uiScroll = 0, uiHoverRow = -1;
static bool uiNameFocus = false;
static RECT uiListRc{}, uiPathRc{}, uiNameRc{}, uiUpRc{};

static void uiEnd(int v) {
    uiVal = v;
    uiDone = true;
}

static bool uiIsDriveRoot(const std::wstring& dir) {
    return dir.size() == 3 && dir[1] == L':' && (dir[2] == L'\\' || dir[2] == L'/');
}

static std::wstring uiParentDir(const std::wstring& dir) {
    if (uiIsDriveRoot(dir)) return dir;
    size_t slash = dir.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return L"C:\\";
    if (slash == 2 && dir.size() >= 2 && dir[1] == L':') return dir.substr(0, 3);
    std::wstring parent = dir.substr(0, slash);
    return parent.empty() ? L"C:\\" : parent;
}

static void uiLoadDir(const std::wstring& dir) {
    uiDir = dir;
    // Preserve C:\ as an absolute root.  The old code turned it into C:,
    // which is drive-relative and can point at a different working directory.
    if (uiDir.size() > 3 && (uiDir.back() == L'\\' || uiDir.back() == L'/')) uiDir.pop_back();
    uiEntries.clear();
    uiSel = -1;
    uiScroll = 0;
    uiHoverRow = -1;
    if (uiDir.size() > 3) uiEntries.push_back({ true, L".." });

    std::wstring pat = uiDir + L"\\*";
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(pat.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            bool d = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            // Folder picking still needs to show the complete project view.
            // Files are displayed in a muted style but only directories can
            // be entered/selected as the target folder.
            uiEntries.push_back({ d, fd.cFileName });
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(uiEntries.begin(), uiEntries.end(), [](const UiEntry& a, const UiEntry& b) {
        if (a.dir != b.dir) return a.dir;
        return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
    });
}

static void uiPaintBtn(HDC dc, int i) {
    if (i < 0 || i >= uiBtnN) return;
    RECT r = uiBtnRc[i];
    Graphics g(dc);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
    bool primary = (i == uiBtnN - 1) && uiBtnLabels.size() > 1;
    COLORREF bg = primary ? th.runBtnText : th.bgCard;
    if (i == uiHoverBtn) {
        bg = primary
            ? lerpColorRef(bg, RGB(255, 255, 255), darkTheme ? 0.10f : 0.15f)
            : lerpColorRef(bg, th.border, 0.85f);
    }
    SolidBrush br(Color(255, GetRValue(bg), GetGValue(bg), GetBValue(bg)));
    GraphicsPath p;
    addRect(p, (float)r.left, (float)r.top, (float)(r.right - r.left), (float)(r.bottom - r.top));
    g.FillPath(&br, &p);
    Color bc = primary
        ? Color(255, GetRValue(th.runBtnText), GetGValue(th.runBtnText), GetBValue(th.runBtnText))
        : Color(255, GetRValue(th.border), GetGValue(th.border), GetBValue(th.border));
    Pen pen(bc, 1.0f);
    g.DrawPath(&pen, &p);

    SetBkMode(dc, TRANSPARENT);
    SetBkColor(dc, bg);
    SetTextColor(dc, primary ? (darkTheme ? RGB(14, 18, 14) : RGB(255, 255, 255)) : th.text);
    HFONT old = (HFONT)SelectObject(dc, hFontPill);
    RECT tr = r;
    DrawTextW(dc, uiBtnLabels[i].c_str(), -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, old);
}

static void uiPaintCommon(HDC dc, RECT rc) {
    // GDI clear first so a failed/partial GDI+ pass never leaves stock white bits
    FillRect(dc, &rc, hBrBgWin ? hBrBgWin : (HBRUSH)GetStockObject(BLACK_BRUSH));

    Graphics g(dc);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

    Color bg(255, GetRValue(th.bgWindow), GetGValue(th.bgWindow), GetBValue(th.bgWindow));
    Color card(255, GetRValue(th.bgCard), GetGValue(th.bgCard), GetBValue(th.bgCard));
    Color border(255, GetRValue(th.border), GetGValue(th.border), GetBValue(th.border));
    SolidBrush bgB(bg), cardB(card);
    g.FillRectangle(&bgB, 0.0f, 0.0f, (float)rc.right, (float)rc.bottom);

    int th44 = uiTitleH();
    if (th44) {
        g.FillRectangle(&cardB, 0.0f, 0.0f, (float)rc.right, (float)th44);
        Pen bp(border, 1.0f);
        g.DrawLine(&bp, 0.0f, (float)th44, (float)rc.right, (float)th44);
        SetBkMode(dc, TRANSPARENT);
        SetBkColor(dc, th.bgCard);
        SetTextColor(dc, th.text);
        HFONT oldF = (HFONT)SelectObject(dc, hFontPill);
        RECT tr = { UI_PAD, 0, rc.right - UI_PAD, th44 };
        DrawTextW(dc, uiTitle.c_str(), -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, oldF);
    }
    // 2px edge — square, sits flush with the (unrounded) window corner
    Pen edge(border, 1.0f);
    g.DrawRectangle(&edge, 1.0f, 1.0f, (float)rc.right - 3.0f, (float)rc.bottom - 3.0f);
}

static int uiHitBtn(int x, int y) {
    for (int i = 0; i < uiBtnN; i++) {
        if (x >= uiBtnRc[i].left && x < uiBtnRc[i].right &&
            y >= uiBtnRc[i].top && y < uiBtnRc[i].bottom) return i;
    }
    return -1;
}

// WM_SETCURSOR helper: hand over clickable controls, I-beam over text, arrow elsewhere
static bool uiCursor(LPCWSTR id) { SetCursor(LoadCursor(NULL, id)); return TRUE; }

// ---------- Message box ----------
LRESULT CALLBACK UiMsgProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_ERASEBKGND) return 1;
    if (m == WM_SETCURSOR) {
        POINT pt; GetCursorPos(&pt); ScreenToClient(h, &pt);
        return uiCursor(uiHitBtn(pt.x, pt.y) >= 0 ? IDC_HAND : IDC_ARROW);
    }
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HGDIOBJ old = SelectObject(mem, bmp);
        uiPaintCommon(mem, rc);

        // Body: explicit transparent bk — white boxes were DrawText OPAQUE leaking through
        SetBkMode(mem, TRANSPARENT);
        SetBkColor(mem, th.bgWindow);
        SetTextColor(mem, th.text);
        int titleH = uiTitleH();
        int bodyTop = titleH ? titleH + 16 : UI_PAD;
        int bodyBot = rc.bottom - UI_PAD - UI_BTN_H - 14;
        if (uiShortcutMode) {
            // Two columns: fixed key width, description fills the rest
            const int keyW = 132;
            const int rowH = 22;
            int y = bodyTop;
            // Shortcut rows are application chrome. They must stay compact at
            // every source zoom level, so never use the editor's hFontCode.
            HFONT oldK = (HFONT)SelectObject(mem, hFontUIBold);
            for (int i = 0; i < uiKeyCount && y + rowH <= bodyBot; i++) {
                RECT rk = { UI_PAD, y, UI_PAD + keyW, y + rowH };
                RECT rd = { UI_PAD + keyW + 8, y, rc.right - UI_PAD, y + rowH };
                SetTextColor(mem, th.function);
                DrawTextW(mem, uiKeys[i], -1, &rk, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                SelectObject(mem, hFontUI);
                SetTextColor(mem, th.text);
                DrawTextW(mem, uiDescs[i], -1, &rd, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
                SelectObject(mem, hFontUIBold);
                y += rowH;
            }
            SelectObject(mem, oldK);
        } else {
            HFONT oldF = (HFONT)SelectObject(mem, hFontPill);
            RECT mr = { UI_PAD, bodyTop, rc.right - UI_PAD, bodyBot };
            DrawTextW(mem, uiMsg.c_str(), -1, &mr, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
            SelectObject(mem, oldF);
        }

        for (int i = 0; i < uiBtnN; i++) uiPaintBtn(mem, i);

        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(h, &ps);
        return 0;
    }
    if (m == WM_MOUSEMOVE) {
        int hit = uiHitBtn(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        if (hit != uiHoverBtn) {
            uiHoverBtn = hit;
            InvalidateRect(h, NULL, FALSE);
        }
        return 0;
    }
    if (m == WM_LBUTTONDOWN) {
        int hit = uiHitBtn(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        if (hit >= 0) uiEnd(hit);
        else uiEnd(-1); // click outside buttons = cancel
        return 0;
    }
    if (m == WM_KEYDOWN) {
        if (w == VK_ESCAPE) { uiEnd(-1); return 0; }
        if (w == VK_RETURN) { uiEnd(uiBtnN > 0 ? uiBtnN - 1 : 0); return 0; }
        if (w == VK_LEFT || w == VK_TAB) {
            uiHoverBtn = (uiHoverBtn <= 0) ? uiBtnN - 1 : uiHoverBtn - 1;
            InvalidateRect(h, NULL, FALSE);
            return 0;
        }
        if (w == VK_RIGHT) {
            uiHoverBtn = (uiHoverBtn + 1) % uiBtnN;
            InvalidateRect(h, NULL, FALSE);
            return 0;
        }
    }
    if (m == WM_CLOSE) { uiEnd(-1); return 0; }
    if (m == WM_DESTROY) { uiDone = true; return 0; }
    return DefWindowProc(h, m, w, l);
}

// ---------- Path paste dialog (MinGW / g++) ----------
static HWND hUiEdit = NULL;
static RECT uiEditRc{};
static std::wstring uiInputVal;
static WNDPROC uiEditOrig = NULL;

static LRESULT CALLBACK UiEditSub(HWND h, UINT m, WPARAM w, LPARAM l) {
    // Enter on KEYDOWN still yields WM_CHAR — swallow it or the path field gets a newline
    static bool swallowChar = false;
    if (m == WM_NCDESTROY) swallowChar = false; // uiEnd may destroy before WM_CHAR arrives
    if (m == WM_CHAR && swallowChar) {
        swallowChar = false;
        return 0;
    }
    if (m == WM_KEYDOWN && w == VK_RETURN) {
        wchar_t b[1024] = {};
        GetWindowTextW(h, b, 1024);
        uiInputVal = b;
        swallowChar = true;
        uiEnd(1);
        return 0;
    }
    if (m == WM_KEYDOWN && w == VK_ESCAPE) { uiEnd(-1); return 0; }
    return CallWindowProc(uiEditOrig, h, m, w, l);
}

LRESULT CALLBACK UiInputProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_ERASEBKGND) return 1;
    if (m == WM_SETCURSOR) {
        POINT pt; GetCursorPos(&pt); ScreenToClient(h, &pt);
        return uiCursor(uiHitBtn(pt.x, pt.y) >= 0 ? IDC_HAND : IDC_ARROW);
    }
    if (m == WM_SETFOCUS) { if (hUiEdit) SetFocus(hUiEdit); return 0; }
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HGDIOBJ old = SelectObject(mem, bmp);
        uiPaintCommon(mem, rc);
        SetBkMode(mem, TRANSPARENT);
        SetBkColor(mem, th.bgWindow);
        SetTextColor(mem, th.text);
        HFONT oldF = (HFONT)SelectObject(mem, hFontPill);
        int titleH = uiTitleH();
        int bodyTop = titleH ? titleH + 14 : UI_PAD;
        RECT mr = { UI_PAD, bodyTop, rc.right - UI_PAD, uiEditRc.top - 8 };
        DrawTextW(mem, uiMsg.c_str(), -1, &mr, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
        SelectObject(mem, oldF);
        for (int i = 0; i < uiBtnN; i++) uiPaintBtn(mem, i);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(h, &ps);
        return 0;
    }
    if (m == WM_MOUSEMOVE) {
        int hit = uiHitBtn(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        if (hit != uiHoverBtn) { uiHoverBtn = hit; InvalidateRect(h, NULL, FALSE); }
        return 0;
    }
    if (m == WM_LBUTTONDOWN) {
        int hit = uiHitBtn(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        if (hit >= 0) {
            if (hit == uiBtnN - 1) {
                wchar_t b[1024] = {};
                if (hUiEdit) GetWindowTextW(hUiEdit, b, 1024);
                uiInputVal = b;
                uiEnd(1);
            } else uiEnd(-1);
        }
        return 0;
    }
    if (m == WM_KEYDOWN) {
        if (w == VK_ESCAPE) { uiEnd(-1); return 0; }
        if (w == VK_RETURN) {
            wchar_t b[1024] = {};
            if (hUiEdit) GetWindowTextW(hUiEdit, b, 1024);
            uiInputVal = b;
            uiEnd(1);
            return 0;
        }
        return 0;
    }
    if (m == WM_CLOSE) { uiEnd(-1); return 0; }
    if (m == WM_DESTROY) {
        if (hUiEdit && IsWindow(hUiEdit)) DestroyWindow(hUiEdit);
        hUiEdit = NULL;
        uiEditOrig = NULL;
        uiDone = true;
        return 0;
    }
    return DefWindowProc(h, m, w, l);
}

// Pump until dialog closes. Disables owner so clicks can't fall through.
static int uiModalDepth = 0;
static int uiRunModal(HWND hDlg, HWND owner) {
    uiDone = false;
    uiVal = -1;
    uiHoverBtn = (int)uiBtnLabels.size() - 1;
    if (owner && uiModalDepth == 0) EnableWindow(owner, FALSE);
    uiModalDepth++;
    ShowWindow(hDlg, SW_SHOW);
    SetForegroundWindow(hDlg);
    SetFocus(hDlg);
    bool quit = false;
    MSG msg;
    while (!uiDone && GetMessageW(&msg, NULL, 0, 0)) {
        if (msg.message == WM_QUIT) {
            uiVal = -1;
            uiDone = true;
            quit = true;
            break;
        }
        if (!IsWindow(hDlg)) { uiDone = true; break; }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    int result = uiVal;
    uiModalDepth--;
    if (owner && uiModalDepth == 0 && IsWindow(owner)) {
        EnableWindow(owner, TRUE);
        SetActiveWindow(owner);
    }
    if (IsWindow(hDlg)) DestroyWindow(hDlg);
    if (quit) PostQuitMessage(0);
    else { uiDone = false; uiVal = -1; } // outer modal keeps pumping
    return result;
}

// Shared paint state lives in globals — save/restore around nested modals
// (file dialog → overwrite confirm reuses the same slots).
struct UiSave {
    std::wstring title, msg;
    std::vector<std::wstring> btns;
    RECT rc[6];
    int n, hv;
};
static UiSave uiSaveState() {
    UiSave s{ uiTitle, uiMsg, uiBtnLabels, {}, uiBtnN, uiHoverBtn };
    for (int i = 0; i < 6; i++) s.rc[i] = uiBtnRc[i];
    return s;
}
static void uiRestoreState(const UiSave& s) {
    uiTitle = s.title;
    uiMsg = s.msg;
    uiBtnLabels = s.btns;
    uiBtnN = s.n;
    uiHoverBtn = s.hv;
    for (int i = 0; i < 6; i++) uiBtnRc[i] = s.rc[i];
}

// buttons: up to 3 labels. returns 0-based index, or -1 cancel.
int uiMessageBox(const wchar_t* title, const wchar_t* text,
                 const wchar_t* b0, const wchar_t* b1, const wchar_t* b2) {
    UiSave st = uiSaveState();
    HWND prevUi = hUi;
    hideSuggestions();
    uiTitle = title ? title : L"";
    uiMsg = text ? text : L"";
    uiBtnLabels.clear();
    if (b0) uiBtnLabels.push_back(b0);
    if (b1) uiBtnLabels.push_back(b1);
    if (b2) uiBtnLabels.push_back(b2);
    if (uiBtnLabels.empty()) uiBtnLabels.push_back(L"OK");
    uiBtnN = (int)uiBtnLabels.size();

    HDC screen = GetDC(NULL);
    HFONT tf = (HFONT)SelectObject(screen, hFontPill);
    RECT measure = { 0, 0, 400, 200 };
    DrawTextW(screen, uiMsg.c_str(), -1, &measure, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(screen, tf);
    ReleaseDC(NULL, screen);

    int w = measure.right + UI_PAD * 2;
    if (uiShortcutMode) w = 520; // fixed width for two-column key list
    if (w < 360) w = 360;
    if (w > 560) w = 560;
    int textH = measure.bottom + 8;
    if (uiShortcutMode) textH = uiKeyCount * 22 + 8;
    if (textH < 36) textH = 36;
    int titleH = uiTitleH();
    int boxH = titleH + (titleH ? 16 : UI_PAD) + textH + 16 + UI_BTN_H + UI_PAD;

    int x = 0, y = 0;
    if (hMain) {
        RECT wr; GetWindowRect(hMain, &wr);
        // Centred on the owning window, both axes (was one third down).
        x = wr.left + ((wr.right - wr.left) - w) / 2;
        y = wr.top + ((wr.bottom - wr.top) - boxH) / 2;
    } else {
        RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        x = wa.left + (wa.right - wa.left - w) / 2;
        y = wa.top + (wa.bottom - wa.top - boxH) / 3;
    }

    HWND hDlg = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"CatPP_Dlg", uiTitle.c_str(),
        WS_POPUP, x, y, w, boxH, hMain, NULL, GetModuleHandleW(NULL), NULL);
    if (!hDlg) { hUi = prevUi; uiRestoreState(st); return -1; }
    hUi = hDlg;
    {
        // Square corners, no region: the painted border is the window edge.
        // attr33 is a harmless E_INVALIDARG on Windows 10 and squares the window
        // on Windows 11, where DWM would otherwise round the corners itself.
        typedef HRESULT (WINAPI *SetAttrFn)(HWND, DWORD, LPCVOID, DWORD);
        static SetAttrFn setAttr = (SetAttrFn)GetProcAddress(
            GetModuleHandleW(L"dwmapi.dll"), "DwmSetWindowAttribute");
        if (setAttr) {
            COLORREF noBorder = 0xFFFFFFFE;
            setAttr(hDlg, 34, &noBorder, sizeof(noBorder));
            COLORREF cap = th.bgCard;
            setAttr(hDlg, 35, &cap, sizeof(cap));
            int cornerPref = 1; // DWMWCP_DONOTROUND
            setAttr(hDlg, 33, &cornerPref, sizeof(cornerPref));
        }
    }

    int totalW = uiBtnN * UI_BTN_W + (uiBtnN - 1) * UI_BTN_GAP;
    int bx = w - UI_PAD - totalW;
    int by = boxH - UI_PAD - UI_BTN_H;
    for (int i = 0; i < uiBtnN; i++) {
        uiBtnRc[i] = { bx, by, bx + UI_BTN_W, by + UI_BTN_H };
        bx += UI_BTN_W + UI_BTN_GAP;
    }
    int r = uiRunModal(hDlg, hMain);
    hUi = prevUi;
    uiRestoreState(st);
    return r;
}

// Paste MinGW / g++.exe path when auto-detect fails.
bool uiInputBox(const wchar_t* title, const wchar_t* msg, std::wstring* out, const wchar_t* initial) {
    UiSave st = uiSaveState();
    HWND prevUi = hUi;
    hideSuggestions();
    uiTitle = title ? title : L"";
    uiMsg = msg ? msg : L"";
    uiBtnLabels = { L"Cancel", L"OK" };
    uiBtnN = 2;
    uiInputVal = initial ? initial : L"";

    HDC screen = GetDC(NULL);
    HFONT tf = (HFONT)SelectObject(screen, hFontPill);
    RECT measure = { 0, 0, 440, 200 };
    DrawTextW(screen, uiMsg.c_str(), -1, &measure, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(screen, tf);
    ReleaseDC(NULL, screen);

    int titleH = uiTitleH();
    int w = measure.right + UI_PAD * 2;
    if (w < 420) w = 420;
    if (w > 560) w = 560;
    int textH = measure.bottom + 8;
    if (textH < 36) textH = 36;
    int editH = 32;
    int boxH = titleH + (titleH ? 14 : UI_PAD) + textH + 10 + editH + 14 + UI_BTN_H + UI_PAD;

    int x = 0, y = 0;
    if (hMain) {
        RECT wr; GetWindowRect(hMain, &wr);
        // Centred on the owning window, both axes (was one third down).
        x = wr.left + ((wr.right - wr.left) - w) / 2;
        y = wr.top + ((wr.bottom - wr.top) - boxH) / 2;
    } else {
        RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        x = wa.left + (wa.right - wa.left - w) / 2;
        y = wa.top + (wa.bottom - wa.top - boxH) / 3;
    }

    HWND hDlg = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"CatPP_Input", uiTitle.c_str(),
        WS_POPUP, x, y, w, boxH, hMain, NULL, GetModuleHandleW(NULL), NULL);
    if (!hDlg) { hUi = prevUi; uiRestoreState(st); return false; }
    hUi = hDlg;
    {
        typedef HRESULT (WINAPI *SetAttrFn)(HWND, DWORD, LPCVOID, DWORD);
        static SetAttrFn setAttr = (SetAttrFn)GetProcAddress(
            GetModuleHandleW(L"dwmapi.dll"), "DwmSetWindowAttribute");
        if (setAttr) {
            COLORREF noBorder = 0xFFFFFFFE;
            setAttr(hDlg, 34, &noBorder, sizeof(noBorder));
            COLORREF cap = th.bgCard;
            setAttr(hDlg, 35, &cap, sizeof(cap));
            int cornerPref = 1; // DWMWCP_DONOTROUND
            setAttr(hDlg, 33, &cornerPref, sizeof(cornerPref));
        }
    }

    int bodyTop = titleH ? titleH + 14 : UI_PAD;
    int editY = bodyTop + textH + 10;
    uiEditRc = { UI_PAD, editY, w - UI_PAD, editY + editH };

    int totalW = uiBtnN * UI_BTN_W + (uiBtnN - 1) * UI_BTN_GAP;
    int bx = w - UI_PAD - totalW;
    int by = boxH - UI_PAD - UI_BTN_H;
    for (int i = 0; i < uiBtnN; i++) {
        uiBtnRc[i] = { bx, by, bx + UI_BTN_W, by + UI_BTN_H };
        bx += UI_BTN_W + UI_BTN_GAP;
    }

    hUiEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        uiEditRc.left, uiEditRc.top,
        uiEditRc.right - uiEditRc.left, uiEditRc.bottom - uiEditRc.top,
        hDlg, NULL, GetModuleHandleW(NULL), NULL);
    if (hUiEdit) {
        SendMessageW(hUiEdit, WM_SETFONT, (WPARAM)hFontPill, TRUE);
        if (initial && *initial) {
            SetWindowTextW(hUiEdit, initial);
            SendMessageW(hUiEdit, EM_SETSEL, 0, -1);
        }
        uiEditOrig = (WNDPROC)SetWindowLongPtrW(hUiEdit, GWLP_WNDPROC, (LONG_PTR)UiEditSub);
        SetFocus(hUiEdit);
    }

    int r = uiRunModal(hDlg, hMain);
    hUi = prevUi;
    hUiEdit = NULL;
    uiEditOrig = NULL;
    uiRestoreState(st);
    if (r > 0) {
        if (out) *out = uiInputVal;
        return true;
    }
    return false;
}

// ---------- File open/save ----------
static void uiPaintFile(HDC dc, RECT rc) {
    FillRect(dc, &rc, hBrBgWin ? hBrBgWin : (HBRUSH)GetStockObject(BLACK_BRUSH));
    uiPaintCommon(dc, rc);
    Graphics g(dc);
    g.SetSmoothingMode(SmoothingModeAntiAlias);

    // path row
    {
        SolidBrush card(Color(255, GetRValue(th.bgOutput), GetGValue(th.bgOutput), GetBValue(th.bgOutput)));
        GraphicsPath p; addRect(p, (float)uiPathRc.left, (float)uiPathRc.top,
            (float)(uiPathRc.right - uiPathRc.left), (float)(uiPathRc.bottom - uiPathRc.top));
        g.FillPath(&card, &p);
        Pen bp(Color(255, GetRValue(th.border), GetGValue(th.border), GetBValue(th.border)), 1);
        g.DrawPath(&bp, &p);
    }
    // up button
    {
        COLORREF bg = th.runBtnBg;
        SolidBrush br(Color(255, GetRValue(bg), GetGValue(bg), GetBValue(bg)));
        GraphicsPath p; addRect(p, (float)uiUpRc.left, (float)uiUpRc.top,
            (float)(uiUpRc.right - uiUpRc.left), (float)(uiUpRc.bottom - uiUpRc.top));
        g.FillPath(&br, &p);
        Pen bp(Color(255, GetRValue(th.border), GetGValue(th.border), GetBValue(th.border)), 1);
        g.DrawPath(&bp, &p);
    }

    SetBkMode(dc, TRANSPARENT);
    SetBkColor(dc, th.bgOutput);
    SetTextColor(dc, th.gutterText);
    HFONT oldPathF = (HFONT)SelectObject(dc, hFontPill);
    RECT pr = uiPathRc;
    pr.left += 10;
    DrawTextW(dc, uiDir.c_str(), -1, &pr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, oldPathF);

    SetTextColor(dc, th.text);
    HFONT oldF = (HFONT)SelectObject(dc, hFontPill);
    RECT ur = uiUpRc;
    DrawTextW(dc, L"..", -1, &ur, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, oldF);

    // list background
    {
        SolidBrush card(Color(255, GetRValue(th.bgOutput), GetGValue(th.bgOutput), GetBValue(th.bgOutput)));
        GraphicsPath p; addRect(p, (float)uiListRc.left, (float)uiListRc.top,
            (float)(uiListRc.right - uiListRc.left), (float)(uiListRc.bottom - uiListRc.top));
        g.FillPath(&card, &p);
        Pen bp(Color(255, GetRValue(th.border), GetGValue(th.border), GetBValue(th.border)), 1);
        g.DrawPath(&bp, &p);
    }

    int rowH = UI_ROW_H;
    int visRows = (uiListRc.bottom - uiListRc.top - 8) / rowH;
    if (visRows < 1) visRows = 1;
    int maxScroll = (int)uiEntries.size() - visRows;
    if (maxScroll < 0) maxScroll = 0;
    if (uiScroll > maxScroll) uiScroll = maxScroll;
    if (uiScroll < 0) uiScroll = 0;

    // File picker text is UI, not source text: it remains at the fixed UI
    // scale even when the currently open document is zoomed.
    oldF = (HFONT)SelectObject(dc, hFontUI);
    for (int i = 0; i < visRows; i++) {
        int idx = uiScroll + i;
        if (idx >= (int)uiEntries.size()) break;
        int ry = uiListRc.top + 4 + i * rowH;
        RECT row = { uiListRc.left + 4, ry, uiListRc.right - 4, ry + rowH };
        if (idx == uiSel || idx == uiHoverRow) {
            COLORREF sel = th.runBtnBg;
            SolidBrush sb(Color(255, GetRValue(sel), GetGValue(sel), GetBValue(sel)));
            GraphicsPath sp; addRect(sp, (float)row.left, (float)row.top,
                (float)(row.right - row.left), (float)(row.bottom - row.top));
            g.FillPath(&sb, &sp);
            SetBkColor(dc, sel);
        } else {
            SetBkColor(dc, th.bgOutput);
        }
        const UiEntry& e = uiEntries[idx];
        SetTextColor(dc, e.dir ? th.function : (uiFolderMode ? th.gutterText : th.text));
        std::wstring label = (e.dir ? L"[+] " : L"    ") + e.name;
        RECT tr = row; tr.left += 8;
        DrawTextW(dc, label.c_str(), -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
    SelectObject(dc, oldF);

    // name row (not in folder mode)
    if (!uiFolderMode) {
        SolidBrush card(Color(255, GetRValue(th.bgOutput), GetGValue(th.bgOutput), GetBValue(th.bgOutput)));
        GraphicsPath p; addRect(p, (float)uiNameRc.left, (float)uiNameRc.top,
            (float)(uiNameRc.right - uiNameRc.left), (float)(uiNameRc.bottom - uiNameRc.top));
        g.FillPath(&card, &p);
        COLORREF bc = uiNameFocus ? th.runBtnBg : th.border;
        Pen bp(Color(255, GetRValue(bc), GetGValue(bc), GetBValue(bc)), 1.0f);
        g.DrawPath(&bp, &p);

        SetBkMode(dc, TRANSPARENT);
        SetBkColor(dc, th.bgOutput);
        SetTextColor(dc, th.text);
        oldF = (HFONT)SelectObject(dc, hFontUI);
        RECT nr = uiNameRc; nr.left += 10;
        std::wstring nameShow = uiName;
        if (uiNameFocus && GetTickCount64() / 500 % 2) nameShow += L"|";
        DrawTextW(dc, nameShow.c_str(), -1, &nr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        SelectObject(dc, oldF);
    }

    for (int i = 0; i < uiBtnN; i++) uiPaintBtn(dc, i);
}

static void uiActivateFileRow(int idx) {
    if (idx < 0 || idx >= (int)uiEntries.size()) return;
    const UiEntry& e = uiEntries[idx];
    if (e.dir) {
        if (e.name == L"..") {
            uiLoadDir(uiParentDir(uiDir));
        } else {
            uiLoadDir(uiDir + L"\\" + e.name);
        }
        InvalidateRect(hUi, NULL, FALSE);
        return;
    }
    if (uiFolderMode) return; // visible for context; folders are the only target
    uiSel = idx;
    uiName = e.name;
    uiNameFocus = true;
    InvalidateRect(hUi, NULL, FALSE);
}

static void uiAcceptFile(bool button = false) {
    if (uiFolderMode) {
        // Enter on a selected folder browses into it; Open button / bare Enter accepts uiDir
        if (!button && uiSel >= 0 && uiSel < (int)uiEntries.size() && uiEntries[uiSel].dir) {
            uiActivateFileRow(uiSel);
            return;
        }
        uiPath = uiDir;
        uiEnd(1);
        return;
    }
    std::wstring name = uiName;
    while (!name.empty() && iswspace(name.front())) name.erase(name.begin());
    while (!name.empty() && iswspace(name.back())) name.pop_back();
    if (name.empty() && uiSel >= 0 && uiSel < (int)uiEntries.size() && !uiEntries[uiSel].dir)
        name = uiEntries[uiSel].name;
    if (name.empty()) return;

    // Enter on a selected folder browses into it
    if (uiSel >= 0 && uiSel < (int)uiEntries.size() && uiEntries[uiSel].dir) {
        if (name.empty() || name == uiEntries[uiSel].name) {
            uiActivateFileRow(uiSel);
            return;
        }
    }

    std::wstring full = name;
    bool abs = (full.size() >= 2 && full[1] == L':') ||
               (!full.empty() && (full[0] == L'\\' || full[0] == L'/'));
    if (!abs) full = uiDir + L"\\" + name;
    if (uiSaveMode) {
        // keep the extension the user typed (.md/.txt/.glsl...); default to
        // uiSaveDefExt only when the name has none
        size_t slash = full.find_last_of(L"\\/");
        size_t dot = full.find_last_of(L'.');
        bool hasExt = dot != std::wstring::npos && (slash == std::wstring::npos || dot > slash) && dot + 1 < full.size();
        if (!hasExt) {
            if (dot != std::wstring::npos && dot + 1 == full.size()) full.erase(dot); // "foo." → "foo" + def ext
            full += uiSaveDefExt;
        }
        uiName = full.substr(full.find_last_of(L"\\/") + 1);
    }

    DWORD attr = GetFileAttributesW(full.c_str());
    if (!uiSaveMode) {
        if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY)) return;
    } else {
        if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
            int r = uiMessageBox(L"Confirm overwrite", (L"Overwrite?\n" + full).c_str(), L"Yes", L"No");
            if (r != 0) return;
        }
    }
    uiPath = full;
    uiEnd(1);
}

LRESULT CALLBACK UiFileProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_ERASEBKGND) return 1;
    if (m == WM_SETCURSOR) {
        POINT pt; GetCursorPos(&pt); ScreenToClient(h, &pt);
        int x = pt.x, y = pt.y;
        // Hand only over buttons, the up pill and rows that hold an entry —
        // empty space below the list stays an arrow.
        bool inList = x >= uiListRc.left && x < uiListRc.right && y >= uiListRc.top && y < uiListRc.bottom;
        int row = inList ? uiScroll + (y - uiListRc.top - 4) / UI_ROW_H : -1;
        bool rowAction = inList && row >= 0 && row < (int)uiEntries.size() &&
            (!uiFolderMode || uiEntries[row].dir);
        bool act = uiHitBtn(x, y) >= 0
            || (x >= uiUpRc.left && x < uiUpRc.right && y >= uiUpRc.top && y < uiUpRc.bottom)
            || rowAction;
        bool name = !uiFolderMode && x >= uiNameRc.left && x < uiNameRc.right
            && y >= uiNameRc.top && y < uiNameRc.bottom;
        return uiCursor(act ? IDC_HAND : name ? IDC_IBEAM : IDC_ARROW);
    }
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HGDIOBJ old = SelectObject(mem, bmp);
        uiPaintFile(mem, rc);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(h, &ps);
        return 0;
    }
    if (m == WM_MOUSEWHEEL) {
        int dir = GET_WHEEL_DELTA_WPARAM(w) > 0 ? -3 : 3;
        uiScroll += dir;
        if (uiScroll < 0) uiScroll = 0;
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }
    if (m == WM_MOUSEMOVE) {
        int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
        int hb = uiHitBtn(x, y);
        int hr = -1;
        if (x >= uiListRc.left && x < uiListRc.right && y >= uiListRc.top && y < uiListRc.bottom) {
            hr = uiScroll + (y - uiListRc.top - 4) / UI_ROW_H;
            if (hr < 0 || hr >= (int)uiEntries.size()) hr = -1;
        }
        if (hb != uiHoverBtn || hr != uiHoverRow) {
            uiHoverBtn = hb;
            uiHoverRow = hr;
            InvalidateRect(h, NULL, FALSE);
        }
        return 0;
    }
    if (m == WM_LBUTTONDOWN) {
        int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
        int hb = uiHitBtn(x, y);
        if (hb >= 0) {
            // 0 = cancel (first), last = accept — depends on label order
            if (uiBtnN >= 2 && hb == 0) uiEnd(-1);
            else uiAcceptFile(true);
            return 0;
        }
        if (x >= uiUpRc.left && x < uiUpRc.right && y >= uiUpRc.top && y < uiUpRc.bottom) {
            uiLoadDir(uiParentDir(uiDir));
            InvalidateRect(h, NULL, FALSE);
            return 0;
        }
        if (!uiFolderMode && x >= uiNameRc.left && x < uiNameRc.right && y >= uiNameRc.top && y < uiNameRc.bottom) {
            uiNameFocus = true;
            InvalidateRect(h, NULL, FALSE);
            return 0;
        }
        if (x >= uiListRc.left && x < uiListRc.right && y >= uiListRc.top && y < uiListRc.bottom) {
            uiNameFocus = false;
            int idx = uiScroll + (y - uiListRc.top - 4) / UI_ROW_H;
            if (idx >= 0 && idx < (int)uiEntries.size()) {
                // Keep files visible in a folder picker but do not treat one
                // as a folder target when it is clicked.
                if (uiFolderMode && !uiEntries[idx].dir) return 0;
                uiSel = idx;
                if (uiEntries[idx].dir) uiName.clear();
                else uiName = uiEntries[idx].name;
                InvalidateRect(h, NULL, FALSE);
            }
            return 0;
        }
        return 0;
    }
    if (m == WM_LBUTTONDBLCLK) {
        int x = GET_X_LPARAM(l), y = GET_Y_LPARAM(l);
        if (x >= uiListRc.left && x < uiListRc.right && y >= uiListRc.top && y < uiListRc.bottom) {
            int idx = uiScroll + (y - uiListRc.top - 4) / UI_ROW_H;
            uiActivateFileRow(idx);
            return 0;
        }
        return 0;
    }
    if (m == WM_KEYDOWN) {
        if (w == VK_ESCAPE) { uiEnd(-1); return 0; }
        if (w == VK_RETURN) { uiAcceptFile(); return 0; }
        if (!uiNameFocus && (w == VK_UP || w == VK_DOWN)) {
            int step = (w == VK_UP) ? -1 : 1;
            int n = uiSel + step;
            if (n < 0) n = 0;
            if (n >= (int)uiEntries.size()) n = (int)uiEntries.size() - 1;
            uiSel = n;
            if (!uiEntries.empty() && n >= 0) {
                if (uiEntries[n].dir) uiName.clear();
                else uiName = uiEntries[n].name;
            }
            int visRows = (uiListRc.bottom - uiListRc.top - 8) / UI_ROW_H;
            if (uiSel >= 0) {
                if (uiSel < uiScroll) uiScroll = uiSel;
                if (uiSel >= uiScroll + visRows) uiScroll = uiSel - visRows + 1;
            }
            InvalidateRect(h, NULL, FALSE);
            return 0;
        }
        if (w == VK_BACK && !uiNameFocus) {
            if (!uiIsDriveRoot(uiDir)) {
                uiLoadDir(uiParentDir(uiDir));
                InvalidateRect(h, NULL, FALSE);
            }
            return 0;
        }
        return 0;
    }
    if (m == WM_CHAR) {
        if (w == VK_RETURN || w == VK_ESCAPE) return 0;
        if (uiFolderMode) return 0; // no name field in folder mode — typing does nothing
        if (w == VK_BACK) {
            if (uiNameFocus && !uiName.empty()) {
                uiName.pop_back();
                InvalidateRect(h, NULL, FALSE);
            }
            return 0;
        }
        if (w >= 32) {
            if (!uiNameFocus) { uiNameFocus = true; uiName.clear(); }
            uiSel = -1;
            uiName += (wchar_t)w; // allow Unicode filenames, not ASCII only
            InvalidateRect(h, NULL, FALSE);
            return 0;
        }
        return 0;
    }
    if (m == WM_PASTE) {
        if (uiFolderMode) return 0;
        if (OpenClipboard(h)) {
            HANDLE clip = GetClipboardData(CF_UNICODETEXT);
            const wchar_t* text = clip ? (const wchar_t*)GlobalLock(clip) : NULL;
            if (text) {
                std::wstring pasted(text);
                GlobalUnlock(clip);
                pasted.erase(std::remove(pasted.begin(), pasted.end(), L'\r'), pasted.end());
                pasted.erase(std::remove(pasted.begin(), pasted.end(), L'\n'), pasted.end());
                if (!uiNameFocus) { uiNameFocus = true; uiName.clear(); }
                uiSel = -1;
                uiName += pasted;
                InvalidateRect(h, NULL, FALSE);
            }
            CloseClipboard();
        }
        return 0;
    }
    if (m == WM_TIMER) {
        if (w == 99 && uiNameFocus) InvalidateRect(h, &uiNameRc, FALSE);
        return 0;
    }
    if (m == WM_CLOSE) { uiEnd(-1); return 0; }
    if (m == WM_DESTROY) { uiDone = true; return 0; }
    return DefWindowProc(h, m, w, l);
}

// Returns full path in *out, true on accept.
bool uiFileDialog(bool save, const std::wstring& initial, std::wstring* out, bool folderMode) {
    UiSave st = uiSaveState();
    HWND prevUi = hUi;
    hideSuggestions();
    uiSaveMode = save;
    uiFolderMode = folderMode;
    uiTitle = folderMode ? L"Open Folder" : (save ? L"Save File" : L"Open File");
    uiBtnLabels = { L"Cancel", save ? L"Save" : L"Open" };
    uiBtnN = 2;

    std::wstring start = initial;
    if (start.empty() && cur >= 0 && cur < (int)docs.size() && !docs[cur].path.empty())
        start = docs[cur].path;
    // split dir / name — never seed from the module path (that yields the exe name)
    size_t s = start.find_last_of(L"\\/");
    if (s != std::wstring::npos && s > 0) {
        std::wstring initialDir = start.substr(0, s);
        if (s == 2 && start.size() >= 2 && start[1] == L':') initialDir = start.substr(0, 3);
        uiLoadDir(initialDir);
        uiName = save ? start.substr(s + 1) : L"";
    } else {
        wchar_t cwd[MAX_PATH]; GetCurrentDirectoryW(MAX_PATH, cwd);
        uiLoadDir(cwd);
        uiName = save ? (start.empty() ? L"untitled.cpp" : start) : start;
    }
    if (folderMode) uiName.clear(); // folder pick — no name field
    uiNameFocus = save && !uiName.empty();
    if (!save && !folderMode) {
        // preselect if initial pointed at existing file
        DWORD a = GetFileAttributesW(initial.c_str());
        if (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY) && s != std::wstring::npos) {
            std::wstring base = initial.substr(s + 1);
            for (int i = 0; i < (int)uiEntries.size(); i++) {
                if (!uiEntries[i].dir && _wcsicmp(uiEntries[i].name.c_str(), base.c_str()) == 0) {
                    uiSel = i;
                    uiName = uiEntries[i].name;
                    break;
                }
            }
        }
    }

    int winW = 560, winH = 440;
    int x = 0, y = 0;
    if (hMain) {
        RECT wr; GetWindowRect(hMain, &wr);
        x = wr.left + ((wr.right - wr.left) - winW) / 2;
        y = wr.top + ((wr.bottom - wr.top) - winH) / 2;
    } else {
        RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        x = wa.left + (wa.right - wa.left - winW) / 2;
        y = wa.top + (wa.bottom - wa.top - winH) / 2;
    }

    HWND hDlg = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"CatPP_File", uiTitle.c_str(),
        WS_POPUP, x, y, winW, winH, hMain, NULL, GetModuleHandleW(NULL), NULL);
    if (!hDlg) { hUi = prevUi; uiRestoreState(st); return false; }
    hUi = hDlg;
    {
        typedef HRESULT (WINAPI *SetAttrFn)(HWND, DWORD, LPCVOID, DWORD);
        static SetAttrFn setAttr = (SetAttrFn)GetProcAddress(
            GetModuleHandleW(L"dwmapi.dll"), "DwmSetWindowAttribute");
        if (setAttr) {
            COLORREF noBorder = 0xFFFFFFFE;
            setAttr(hDlg, 34, &noBorder, sizeof(noBorder));
            COLORREF cap = th.bgCard;
            setAttr(hDlg, 35, &cap, sizeof(cap));
            int cornerPref = 1; // DWMWCP_DONOTROUND
            setAttr(hDlg, 33, &cornerPref, sizeof(cornerPref));
        }
    }

    // layout
    int top = 52;
    uiPathRc = { UI_PAD, top, winW - UI_PAD - 44, top + 28 };
    uiUpRc = { winW - UI_PAD - 40, top, winW - UI_PAD, top + 28 };
    top += 38;
    int btnY = winH - UI_PAD - UI_BTN_H;
    int nameY = btnY - 36;
    uiListRc = { UI_PAD, top, winW - UI_PAD, (folderMode ? btnY : nameY) - 10 };
    uiNameRc = { UI_PAD, nameY, winW - UI_PAD, nameY + 28 };

    int totalW = uiBtnN * UI_BTN_W + (uiBtnN - 1) * UI_BTN_GAP;
    int bx = winW - UI_PAD - totalW;
    for (int i = 0; i < uiBtnN; i++) {
        uiBtnRc[i] = { bx, btnY, bx + UI_BTN_W, btnY + UI_BTN_H };
        bx += UI_BTN_W + UI_BTN_GAP;
    }

    uiPath = L"";
    SetTimer(hDlg, 99, 400, NULL); // name-field caret blink
    int r = uiRunModal(hDlg, hMain);
    if (IsWindow(hDlg)) KillTimer(hDlg, 99);
    hUi = prevUi;
    uiRestoreState(st);
    if (r > 0 && out) { *out = uiPath; return true; }
    return false;
}

// ---------- Themed context menu ----------
static std::vector<std::pair<int, std::wstring>> uiMenu; // id, label; id -1 = sep
static int uiMenuHover = -1;
static float animMenuHoverY = 0.0f, animMenuHoverA = 0.0f;
static const UINT_PTR MENU_ANIM_TIMER = 1;
static RECT uiMenuRc{};
static int uiMenuH = 0;
static const int UI_MENU_W = 280;
static const int UI_MENU_ITEM_H = 30;
static const int UI_MENU_SEP_H = 8;

static float uiMenuRowY(int idx) {
    int yy = 4;
    for (int i = 0; i < idx && i < (int)uiMenu.size(); i++)
        yy += (uiMenu[i].first < 0) ? UI_MENU_SEP_H : UI_MENU_ITEM_H;
    return (float)yy;
}
static void kickMenuAnim(HWND h) { SetTimer(h, MENU_ANIM_TIMER, 16, NULL); }

static int uiMenuHit(int x, int y) {
    int yy = 4;
    for (int i = 0; i < (int)uiMenu.size(); i++) {
        int h = (uiMenu[i].first < 0) ? UI_MENU_SEP_H : UI_MENU_ITEM_H;
        if (x >= 0 && x < UI_MENU_W && y >= yy && y < yy + h) {
            return uiMenu[i].first < 0 ? -1 : i;
        }
        yy += h;
    }
    return -1;
}

// Menu open/close fade via AnimateWindow (AW_BLEND). Falls back to an instant
// show/hide if the API declines (e.g. animations disabled system-wide).
static void menuShowAnimated(HWND h) {
    if (IsWindow(h) && !AnimateWindow(h, 130, AW_BLEND)) ShowWindow(h, SW_SHOW);
}
static void menuCloseAnimated(HWND h) {
    if (h && IsWindow(h)) AnimateWindow(h, 100, AW_BLEND | AW_HIDE);
    if (h && IsWindow(h)) DestroyWindow(h);
}

LRESULT CALLBACK UiMenuProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_SETCURSOR) {
        POINT pt; GetCursorPos(&pt); ScreenToClient(h, &pt);
        return uiCursor(uiMenuHit(pt.x, pt.y) >= 0 ? IDC_HAND : IDC_ARROW);
    }
    if (m == WM_ERASEBKGND) return 1;
    if (m == WM_TIMER && w == MENU_ANIM_TIMER) {
        float tY = (uiMenuHover >= 0) ? uiMenuRowY(uiMenuHover) : animMenuHoverY;
        float tA = (uiMenuHover >= 0) ? 1.0f : 0.0f;
        bool eY = fabsf(tY - animMenuHoverY) > 0.5f;
        bool eA = fabsf(tA - animMenuHoverA) > 0.01f;
        if (eY) animMenuHoverY += (tY - animMenuHoverY) * 0.30f;
        if (eA) animMenuHoverA += (tA - animMenuHoverA) * 0.30f;
        if (!eY && !eA) { animMenuHoverY = tY; animMenuHoverA = tA; KillTimer(h, MENU_ANIM_TIMER); }
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }
    if (m == WM_MOUSELEAVE) {
        if (uiMenuHover != -1) { uiMenuHover = -1; kickMenuAnim(h); }
        return 0;
    }
    if (m == WM_ACTIVATE) {
        if (LOWORD(w) == WA_INACTIVE) {
            // click outside — dismiss
            menuCloseAnimated(h);
            return 0;
        }
    }
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HGDIOBJ old = SelectObject(mem, bmp);

        HBRUSH clear = hBrBgCard ? hBrBgCard : (HBRUSH)GetStockObject(BLACK_BRUSH);
        FillRect(mem, &rc, clear);

        Graphics g(mem);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
        Color bg(255, GetRValue(th.bgCard), GetGValue(th.bgCard), GetBValue(th.bgCard));
        Color border(255, GetRValue(th.border), GetGValue(th.border), GetBValue(th.border));
        SolidBrush bgB(bg);
        g.FillRectangle(&bgB, 1.0f, 1.0f, (float)rc.right - 3, (float)rc.bottom - 3);
        Pen bp(border, 1.0f);
        g.DrawRectangle(&bp, 1.0f, 1.0f, (float)rc.right - 3, (float)rc.bottom - 3);

        SetBkMode(mem, TRANSPARENT);
        SetBkColor(mem, th.bgCard);
        int yy = 4;
        // Gliding hover band (under the labels)
        if (animMenuHoverA > 0.01f) {
            BYTE a = (BYTE)std::lround(std::min(1.0f, animMenuHoverA) * 255.0f);
            SolidBrush hs(Color(a, GetRValue(th.runBtnBg), GetGValue(th.runBtnBg), GetBValue(th.runBtnBg)));
            GraphicsPath hp;
            addRect(hp, 6.0f, animMenuHoverY, (float)(rc.right - 12), (float)UI_MENU_ITEM_H);
            g.FillPath(&hs, &hp);
        }
        for (int i = 0; i < (int)uiMenu.size(); i++) {
            if (uiMenu[i].first < 0) {
                Pen sep(Color(255, GetRValue(th.border), GetGValue(th.border), GetBValue(th.border)), 1);
                g.DrawLine(&sep, 10.0f, (float)(yy + UI_MENU_SEP_H / 2), (float)(rc.right - 10), (float)(yy + UI_MENU_SEP_H / 2));
                yy += UI_MENU_SEP_H;
                continue;
            }
            RECT row = { 6, yy, rc.right - 6, yy + UI_MENU_ITEM_H };
            SetTextColor(mem, th.text);
            HFONT oldF = (HFONT)SelectObject(mem, hFontPill);
            RECT tr = row; tr.left += 12;
            DrawTextW(mem, uiMenu[i].second.c_str(), -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            SelectObject(mem, oldF);
            yy += UI_MENU_ITEM_H;
        }

        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(h, &ps);
        return 0;
    }
    if (m == WM_MOUSEMOVE) {
        TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, h, 0 };
        TrackMouseEvent(&tme);
        int hit = uiMenuHit(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        if (hit != uiMenuHover) {
            uiMenuHover = hit;
            kickMenuAnim(h);
        }
        return 0;
    }
    if (m == WM_LBUTTONDOWN) {
        int hit = uiMenuHit(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        if (hit >= 0) {
            int id = uiMenu[hit].first;
            menuCloseAnimated(h);
            if (hMain) PostMessageW(hMain, WM_COMMAND, MAKEWPARAM(id, 0), 0);
        } else {
            menuCloseAnimated(h);
        }
        return 0;
    }
    if (m == WM_KEYDOWN && w == VK_ESCAPE) { menuCloseAnimated(h); return 0; }
    if (m == WM_DESTROY) { if (hUi == h) hUi = NULL; return 0; }
    return DefWindowProc(h, m, w, l);
}

void uiShowMenu(int x, int y) {
    hideSuggestions();
    if (hUi && IsWindow(hUi)) DestroyWindow(hUi);

    uiMenu = {
        { 11, L"Run" },
        { -1, L"" },
        { 13, L"New" },
        { 14, L"Open..." },
        { 23, L"Open Folder..." },
        { 15, L"Save" },
        { 16, L"Save As..." },
        { -1, L"" },
        { 25, L"Find..." },
        { 26, L"Replace..." },
        { 28, L"Format Code" },
        { 29, L"Check Errors" },
        { -1, L"" },
        { 17, L"Toggle Theme" },
        { 19, L"Keyboard Shortcuts" },
    };
    uiMenuHover = -1;
    animMenuHoverA = 0.0f;
    animMenuHoverY = uiMenuRowY(0);
    uiMenuH = 4;
    for (auto& it : uiMenu) uiMenuH += (it.first < 0) ? UI_MENU_SEP_H : UI_MENU_ITEM_H;
    uiMenuH += 4;
    // The themed app menu is centred on the owning window (it used to open at the
    // click point, which read as a stray popup), always on top, and stays clamped
    // inside the monitor work area.
    HMONITOR mon = MonitorFromPoint({ x, y }, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(mon, &mi);
    RECT owner = mi.rcWork;
    if (hMain) GetWindowRect(hMain, &owner);
    x = owner.left + ((owner.right - owner.left) - UI_MENU_W) / 2;
    y = owner.top + ((owner.bottom - owner.top) - uiMenuH) / 2;
    if (x + UI_MENU_W > mi.rcWork.right) x = mi.rcWork.right - UI_MENU_W;
    if (y + uiMenuH > mi.rcWork.bottom) y = mi.rcWork.bottom - uiMenuH;
    if (x < mi.rcWork.left) x = mi.rcWork.left;
    if (y < mi.rcWork.top) y = mi.rcWork.top;

    HWND h = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"CatPP_Menu", L"",
        WS_POPUP, x, y, UI_MENU_W, uiMenuH, hMain, NULL, GetModuleHandleW(NULL), NULL);
    if (!h) return;
    hUi = h;
    menuShowAnimated(h);
    SetForegroundWindow(h);
    // No SetCapture here: while capture is held Windows suppresses WM_SETCURSOR
    // and the pointer keeps the editor's I-beam. Without it, normal hover
    // tracking drives the cursor. Set it once for the opening position.
    POINT cpt; GetCursorPos(&cpt); ScreenToClient(h, &cpt);
    SetCursor(LoadCursor(NULL, uiMenuHit(cpt.x, cpt.y) >= 0 ? IDC_HAND : IDC_ARROW));
}

// Process Runner — pumps UI messages while waiting so compile doesn't freeze the app
static volatile bool sInRunCap = false;

// Quote one Windows command-line argument for CreateProcessW (including paths
// with spaces, quotes, or a trailing backslash such as C:\).  Concatenating
// bare quotes around a root path leaves the closing quote escaped.
static std::wstring quoteCommandArg(const std::wstring& arg) {
    std::wstring q = L"\"";
    size_t slashCount = 0;
    for (wchar_t ch : arg) {
        if (ch == L'\\') {
            ++slashCount;
        } else if (ch == L'\"') {
            q.append(slashCount * 2 + 1, L'\\');
            q += ch;
            slashCount = 0;
        } else {
            q.append(slashCount, L'\\');
            q += ch;
            slashCount = 0;
        }
    }
    q.append(slashCount * 2, L'\\');
    q += L'\"';
    return q;
}

static std::wstring directoryOf(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return L"";
    // C:\file.cpp has C:\ as its parent, never drive-relative C:.
    if (slash == 2 && path.size() >= 2 && path[1] == L':') return path.substr(0, 3);
    return path.substr(0, slash);
}

static std::wstring runWorkingDirectory() {
    if (cur >= 0 && cur < (int)docs.size()) {
        std::wstring dir = directoryOf(docs[cur].path);
        if (!dir.empty()) return dir;
    }
    wchar_t cwd[MAX_PATH] = {};
    DWORD n = GetCurrentDirectoryW(MAX_PATH, cwd);
    return n > 0 && n < MAX_PATH ? std::wstring(cwd, n) : exeDirW();
}

bool runCap(std::wstring cmd, const std::string& in, std::string& out, DWORD timeout,
            DWORD& exitCode, const std::wstring& workingDir = L"") {
    HANDLE ro = NULL, wo = NULL, ri = NULL, wi = NULL;
    SECURITY_ATTRIBUTES sa{sizeof(sa), NULL, TRUE};
    if (!CreatePipe(&ro, &wo, &sa, 0) || !CreatePipe(&ri, &wi, &sa, 0)) {
        if (ro) CloseHandle(ro);
        if (wo) CloseHandle(wo);
        if (ri) CloseHandle(ri);
        if (wi) CloseHandle(wi);
        return false;
    }
    SetHandleInformation(ro, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(wi, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wo;
    si.hStdError = wo;
    si.hStdInput = ri;
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    bool ok = CreateProcessW(NULL, buf.data(), NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL,
        workingDir.empty() ? NULL : workingDir.c_str(), &si, &pi);
    CloseHandle(wo);
    CloseHandle(ri);
    if (!ok) {
        CloseHandle(ro);
        CloseHandle(wi);
        return false;
    }
    if (!in.empty()) {
        DWORD w;
        WriteFile(wi, in.c_str(), (DWORD)in.size(), &w, NULL);
    }
    CloseHandle(wi);
    char tmp[4096];
    DWORD r;
    out.clear();
    DWORD waited = 0;
    bool quit = false;
    sInRunCap = true;
    while (true) {
        // Keep the UI alive during a multi-second compile
        MSG mm;
        while (PeekMessageW(&mm, NULL, 0, 0, PM_REMOVE)) {
            if (mm.message == WM_QUIT) {
                quit = true;
                break;
            }
            TranslateMessage(&mm);
            DispatchMessageW(&mm);
        }
        if (quit) break;

        DWORD wr = WaitForSingleObject(pi.hProcess, 50);
        while (PeekNamedPipe(ro, NULL, 0, NULL, &r, NULL) && r) {
            if (ReadFile(ro, tmp, sizeof(tmp), &r, NULL) && r) out.append(tmp, r);
            else break;
        }
        if (wr == WAIT_OBJECT_0) break;
        waited += 50;
        if (waited > timeout) {
            TerminateProcess(pi.hProcess, 124);
            out += "\n[TIMEOUT: killed after " + std::to_string(timeout / 1000) + "s]\n";
            break;
        }
    }
    sInRunCap = false;
    if (quit) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(ro);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        PostQuitMessage(0);
        return false;
    }
    while (PeekNamedPipe(ro, NULL, 0, NULL, &r, NULL) && r) {
        if (ReadFile(ro, tmp, sizeof(tmp), &r, NULL) && r) out.append(tmp, r);
        else break;
    }
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(ro);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return true;
}

void stopSession(bool kill) {
    if (!sAlive) return;
    sAlive = false;
    sGen++;
    if (kill && sProc) TerminateProcess(sProc, 1);
    if (sInW) { CloseHandle(sInW); sInW = NULL; }
    if (sProc) {
        WaitForSingleObject(sProc, 3000);
        CloseHandle(sProc);
        sProc = NULL;
    }
    if (hOut) {
        SendMessage(hOut, EM_SETREADONLY, FALSE, 0);
        insertProg(L"\r\n[Stopped]\r\n");
        SendMessage(hOut, EM_SETREADONLY, TRUE, 0);
        syncOutCaret();
    }
    if (hMain) {
        // repaint Run button + output pill (running... → output) + card focus
        RECT rcClient;
        GetClientRect(hMain, &rcClient);
        RECT rcRun;
        runBtnRect(rcClient.right - rcClient.left, &rcRun);
        InvalidateRect(hMain, &rcRun, FALSE);
        RECT L, Rr; panels(&L, &Rr);
        RECT rcPill = { Rr.left, 0, Rr.right, Rr.top + 24 };
        InvalidateRect(hMain, &rcPill, FALSE);
        InvalidateRect(hMain, NULL, FALSE);
    }
}

void insertProg(const std::wstring& norm) {
    DWORD a = 0, b = 0;
    SendMessage(hOut, EM_GETSEL, (WPARAM)&a, (LPARAM)&b);
    if (a > b) std::swap(a, b);
    int before = outDocLen(hOut);
    SendMessage(hOut, EM_SETSEL, progLen, progLen);
    SendMessage(hOut, EM_REPLACESEL, FALSE, (LPARAM)norm.c_str());
    int after = outDocLen(hOut);
    int wasLen = progLen;
    progLen += (after - before); // internal delta — \r\n collapses to \r in the doc
    if ((int)a >= wasLen) {
        a += (DWORD)(after - before);
        b += (DWORD)(after - before);
    } else {
        a = b = (DWORD)progLen;
    }
    SendMessage(hOut, EM_SETSEL, a, b);
    syncOutCaret();
}

void uiAppend(const std::string& raw) {
    if (!sAlive) return;
    if (progLen > 400000) {
        insertProg(L"\r\n[output truncated]\r\n");
        stopSession(true);
        return;
    }
    int n = MultiByteToWideChar(CP_UTF8, 0, raw.c_str(), (int)raw.size(), NULL, 0);
    std::wstring w(n ? n : (int)raw.size(), L'\0');
    if (n) MultiByteToWideChar(CP_UTF8, 0, raw.c_str(), (int)raw.size(), w.data(), n);
    else for (size_t i = 0; i < raw.size(); i++) w[i] = (wchar_t)(unsigned char)raw[i];
    std::wstring norm;
    norm.reserve(w.size() + 8);
    for (size_t i = 0; i < w.size(); i++) {
        wchar_t c = w[i];
        if (c == L'\r') {
            norm += L"\r\n";
            if (i + 1 < w.size() && w[i + 1] == L'\n') i++; // already a CRLF pair
        } else if (c == L'\n') {
            norm += L"\r\n";
        } else {
            norm += c;
        }
    }
    insertProg(norm);
}

struct Rdr { HANDLE h; int gen; };
DWORD WINAPI readThread(LPVOID p) {
    Rdr* r = (Rdr*)p;
    char buf[4096];
    DWORD n;
    while (ReadFile(r->h, buf, sizeof(buf), &n, NULL) && n) {
        std::string* chunk = new std::string(buf, n);
        if (!hMain || !PostMessageW(hMain, WM_APPEND, r->gen, (LPARAM)chunk)) delete chunk;
    }
    CloseHandle(r->h);
    if (hMain) PostMessageW(hMain, WM_DONE, r->gen, 0);
    delete r;
    return 0;
}

// g++ diagnostics carry the absolute temp source path — drop it for display.
// "C:\...\CatPP_xxx\run.cpp:5:10: error" → "5:10: error"; include-chain lines
// keep their "In file included from" prefix.
static void fmtDiagLine(std::wstring& s) {
    size_t cut = s.find(L".cpp:");
    if (cut != std::wstring::npos) {
        size_t ps = 0;
        size_t from = s.find(L"from ");
        if (from != std::wstring::npos && from < cut) ps = from + 5;
        s.erase(ps, cut + 5 - ps);
        // "5:10: error..." → "5:10  error..." — still parses as line:col at start
        size_t c1 = 0;
        while (c1 < s.size() && iswdigit(s[c1])) c1++;
        if (c1 > 0 && c1 + 1 < s.size() && s[c1] == L':') {
            size_t c2 = c1 + 1;
            while (c2 < s.size() && iswdigit(s[c2])) c2++;
            if (c2 > c1 + 1 && c2 < s.size() && s[c2] == L':') s[c2] = L' ';
        }
    }
}

// g++ source echo ("  5 |   cin >> ;") and caret rail ("      |   ^") lines —
// the editor shows the same code and double-click jumps to the column.
static bool isDiagQuoteLine(const std::wstring& s) {
    size_t i = 0;
    while (i < s.size() && (s[i] == L' ' || s[i] == L'\t')) i++;
    if (i < s.size() && s[i] == L'|') return true;
    size_t j = i;
    while (j < s.size() && iswdigit(s[j])) j++;
    return j > i && j + 2 < s.size() && s[j] == L' ' && s[j + 1] == L'|' && s[j + 2] == L' ';
}

static std::wstring fmtDiagLog(const std::string& raw) {
    std::wstring in = u2w(raw), out, line;
    auto flush = [&] {
        fmtDiagLine(line);
        if (!isDiagQuoteLine(line)) { out += line; out += L"\r\n"; }
        line.clear();
    };
    for (size_t i = 0; i < in.size(); i++) {
        wchar_t c = in[i];
        if (c == L'\r' || c == L'\n') {
            flush();
            if (c == L'\r' && i + 1 < in.size() && in[i + 1] == L'\n') i++;
        } else {
            line += c;
        }
    }
    if (!line.empty()) flush();
    return out;
}

// Colorize check/compile output in the output pane: counts header (green/red/
// amber), diagnostics red/amber, notes and include-chain lines dim.  Stateless —
// safe to re-run after any SCF_ALL theme reset.  Bails out if the first line
// isn't a diagnostic (program output, status messages stay plain).
static void colorDiagOut() {
    if (!hOut) return;
    long len = outDocLen(hOut);
    if (len <= 0 || len > 1000000) return;
    std::wstring t((size_t)len + 1, 0);
    TEXTRANGEW tr{{0, len}, t.data()};
    SendMessageW(hOut, EM_GETTEXTRANGE, 0, (LPARAM)&tr);
    t.resize((size_t)len);
    size_t eol = t.find_first_of(L"\r\n");
    std::wstring head = t.substr(0, eol == std::wstring::npos ? t.size() : eol);
    COLORREF cHead;
    const bool clean = head.find(L"No errors") != std::wstring::npos;
    const bool isErr = head.find(L"error") != std::wstring::npos ||
                       head.find(L"ERROR") != std::wstring::npos;
    if (clean)       cHead = darkTheme ? RGB(110, 235, 130) : RGB(0, 140, 60);
    else if (isErr)  cHead = darkTheme ? RGB(255, 80, 80)   : RGB(200, 30, 30);
    else if (head.find(L"warning") != std::wstring::npos)
                     cHead = darkTheme ? RGB(255, 190, 90)  : RGB(170, 110, 0);
    else return;
    const COLORREF cErr  = darkTheme ? RGB(255, 80, 80)   : RGB(200, 30, 30);
    const COLORREF cWarn = darkTheme ? RGB(255, 190, 90)  : RGB(170, 110, 0);
    const COLORREF cDim  = th.gutterText;
    DWORD selA = 0, selB = 0;
    SendMessageW(hOut, EM_GETSEL, (WPARAM)&selA, (LPARAM)&selB);
    setHlUndo(hOut, true);
    auto style = [&](long from, long to, COLORREF col, bool bold) {
        if (to <= from) return;
        SendMessageW(hOut, EM_SETSEL, (WPARAM)from, (LPARAM)to);
        CHARFORMAT2W cf{};
        cf.cbSize = sizeof(cf);
        cf.dwMask = CFM_COLOR | (bold ? CFM_BOLD : 0);
        cf.crTextColor = col;
        if (bold) cf.dwEffects = CFE_BOLD;
        SendMessageW(hOut, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cf);
    };
    long headEnd = (long)SendMessageW(hOut, EM_LINEINDEX, 1, 0);
    if (headEnd <= 0) headEnd = (long)head.size();
    style(0, headEnd, cHead, true);
    // EM_LINEINDEX is VISUAL under wrap — walk rows but style whole paragraphs
    // (deduped), or a wrapped message only gets its first row colored.
    int lines = (int)SendMessageW(hOut, EM_GETLINECOUNT, 0, 0);
    long lastPara = -1;
    for (int i = 1; i < lines; i++) {
        long s = (long)SendMessageW(hOut, EM_LINEINDEX, i, 0);
        if (s < 0 || s >= len) break;
        // Paragraph bounds straight from `t` (already fetched above). paraBounds()
        // re-reads the whole control for controls without a cache, so calling it
        // once per line is O(lines * len) — 75k rows x 780KB never returns.
        long ps = s;
        while (ps > 0 && t[(size_t)ps - 1] != L'\r') --ps;
        long pe = s;
        while (pe < len && t[(size_t)pe] != L'\r') ++pe;
        s = ps;
        long e = pe;
        if (s == lastPara) continue; // wrapped continuation of the previous row
        lastPara = s;
        if (e > len) e = len;
        long p = s;
        while (p < e && (t[(size_t)p] == L' ' || t[(size_t)p] == L'\t')) p++;
        if (t.compare((size_t)p, 3, L"In ") == 0 ||
            t.compare((size_t)p, 14, L"required from") == 0) {
            style(p, e, cDim, false);
            continue;
        }
        // location prefix "5:10" dim, keyword colored, message left plain
        if (p < e && iswdigit(t[(size_t)p])) {
            long q = p;
            while (q < e && iswdigit(t[(size_t)q])) q++;
            if (q < e && t[(size_t)q] == L':') {
                q++;
                while (q < e && iswdigit(t[(size_t)q])) q++;
                style(p, q, cDim, false);
            }
        }
        for (long k = s; k < e; k++) {
            if (t.compare((size_t)k, 6, L"error:") == 0) { style(k, k + 6, cErr, true); break; }
            if (t.compare((size_t)k, 8, L"warning:") == 0) { style(k, k + 8, cWarn, true); break; }
            if (t.compare((size_t)k, 5, L"note:") == 0) { style(k, k + 5, cDim, false); break; }
        }
    }
    SendMessageW(hOut, EM_SETSEL, (WPARAM)selA, (LPARAM)selB);
    setHlUndo(hOut, false);
}

// Batch files do not need the compiler — Windows runs them directly through
// cmd.exe.  The saved file is executed in place; an unsaved buffer is written to
// the per-process temp directory first so Run still does something useful.
static void runBatchFile() {
    std::wstring script = (cur >= 0 && cur < (int)docs.size()) ? docs[cur].path : L"";
    std::wstring e = script;
    for (auto& c : e) if (c >= L'A' && c <= L'Z') c += 32;
    bool isBatName = e.size() > 4 && (e.rfind(L".bat") == e.size() - 4 || e.rfind(L".cmd") == e.size() - 4);
    if (!isBatName || GetFileAttributesW(script.c_str()) == INVALID_FILE_ATTRIBUTES) {
        makeRunArtifactPaths();
        runSourcePath = runSourcePath.substr(0, runSourcePath.find_last_of(L'.')) + L".bat";
        if (!writeFileBytes(runSourcePath, w2u(getCodeW()))) {
            SetWindowTextW(hOut, L"Could not create the temporary batch file.");
            progLen = outDocLen(hOut);
            return;
        }
        script = runSourcePath;
    }
    std::wstring workDir = runWorkingDirectory();
    SetWindowTextW(hOut, (L"Running " + baseName(script) + L"...\r\n\r\n").c_str());
    UpdateWindow(hOut);
    progLen = outDocLen(hOut);
    std::string log;
    DWORD ec = 0;
    std::wstring cmd = L"cmd.exe /c " + quoteCommandArg(script);
    if (!runCap(cmd, std::string(), log, 120000, ec, workDir))
        SetWindowTextW(hOut, (L"Could not start cmd.exe for " + baseName(script) + L".").c_str());
    else
        SetWindowTextW(hOut, (u2w(log) + L"[exit code " + std::to_wstring(ec) + L"]").c_str());
    progLen = outDocLen(hOut);
    gOutHasProg = true; // real program output — the auto-check must not wipe it
    gOutIsCheck = false;
    colorDiagOut();
}

void startRun() {
    if (cur < 0) return;
    // An auto syntax check may be mid-flight; its runCap pump re-enters this
    // WndProc, so spin (pumping) until it finishes rather than dropping Run.
    for (int i = 0; i < 200 && sInRunCap; i++) {
        MSG wm;
        bool q = false;
        while (PeekMessageW(&wm, NULL, 0, 0, PM_REMOVE)) {
            if (wm.message == WM_QUIT) { q = true; break; }
            TranslateMessage(&wm);
            DispatchMessageW(&wm);
        }
        if (q) { PostQuitMessage(0); return; }
        Sleep(50);
    }
    if (sInRunCap) return;
    if (curLang() == LANG_BAT) { runBatchFile(); return; }
    if (curLang() != LANG_CPP) { // .md/.txt/.glsl are not compilable sources
        SetWindowTextW(hOut, L"Not a C/C++ file — nothing to run.");
        progLen = outDocLen(hOut);
        return;
    }
    if (sAlive) stopSession(true);

    std::wstring gpp;
    if (!ensureGpp(gpp)) {
        SetWindowTextW(hOut, L"Compiler not found. Run again after setting the MinGW path.");
        progLen = outDocLen(hOut);
        return;
    }

    makeRunArtifactPaths();
    std::string code = w2u(getCodeW());
    if (!writeFileBytes(runSourcePath, code)) {
        SetWindowTextW(hOut, L"Could not create the temporary source file.");
        progLen = outDocLen(hOut);
        return;
    }
    SetWindowTextW(hOut, L"Compiling...\r\n");
    UpdateWindow(hOut);
    progLen = outDocLen(hOut);
    gOutHasProg = false;  // program output no longer owns the pane
    gOutIsCheck = false;
    gLastChecked.clear(); // buffer may have changed since the last check

    std::string log;
    DWORD ec = 0;
    std::wstring workDir = runWorkingDirectory();
    // Do not force -static: standard MSYS2/MinGW installations often omit one
    // of the static runtime archives.  ensureGpp adds its bin folder to PATH so
    // the normal MinGW runtime DLLs are available to the launched program.
    std::wstring compile = quoteCommandArg(gpp) + L" -std=c++17 -O0 -o " +
        quoteCommandArg(runExePath) + L" " + quoteCommandArg(runSourcePath) +
        L" -I " + quoteCommandArg(workDir);
    if (!runCap(compile, std::string(), log, 20000, ec, workDir)) {
        SetWindowTextW(hOut, L"Could not execute g++ compiler.\nCheck the MinGW path (Run again to update it).");
        progLen = outDocLen(hOut);
        gppCached.clear();
        gppTriedConfig = false;
        return;
    }
    if (ec != 0) {
        SetWindowTextW(hOut, (L"COMPILE ERROR\r\n\r\n" + fmtDiagLog(log)).c_str());
        progLen = outDocLen(hOut);
        colorDiagOut();
        return;
    }

    SECURITY_ATTRIBUTES sa{sizeof(sa), NULL, TRUE};
    HANDLE ro = NULL, wo = NULL, ri = NULL, wi = NULL;
    if (!CreatePipe(&ro, &wo, &sa, 0) || !CreatePipe(&ri, &wi, &sa, 0)) {
        if (ro) CloseHandle(ro);
        if (wo) CloseHandle(wo);
        if (ri) CloseHandle(ri);
        if (wi) CloseHandle(wi);
        SetWindowTextW(hOut, L"Failed to launch compiled program.");
        progLen = outDocLen(hOut);
        return;
    }
    SetHandleInformation(ro, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(wi, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wo;
    si.hStdError = wo;
    si.hStdInput = ri;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = quoteCommandArg(runExePath);
    std::vector<wchar_t> b(cmd.begin(), cmd.end());
    b.push_back(0);
    if (!CreateProcessW(NULL, b.data(), NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL,
                        workDir.empty() ? NULL : workDir.c_str(), &si, &pi)) {
        CloseHandle(ro); CloseHandle(wo); CloseHandle(ri); CloseHandle(wi);
        SetWindowTextW(hOut, L"Failed to launch compiled program.");
        progLen = outDocLen(hOut);
        return;
    }
    CloseHandle(wo);
    CloseHandle(ri);
    CloseHandle(pi.hThread);
    sProc = pi.hProcess;
    sInW = wi;
    sAlive = true;
    sGen++;

    SendMessage(hOut, EM_SETREADONLY, FALSE, 0);
    SetWindowTextW(hOut, L""); // ponytail: no [Running...] banner, errors still print
    UpdateWindow(hOut);
    progLen = outDocLen(hOut);
    SendMessage(hOut, EM_SETSEL, progLen, progLen);
    SetFocus(hOut); // caret lands in the input zone where the program will read
    syncOutCaret();
    Rdr* rdr = new Rdr{ro, sGen};
    HANDLE hThread = CreateThread(NULL, 0, readThread, rdr, 0, NULL);
    if (!hThread) {
        // no reader → child fills the pipe and hangs; tear down like stopSession
        delete rdr;
        CloseHandle(ro);
        stopSession(true);
        SetWindowTextW(hOut, L"Failed to launch output reader.");
        progLen = outDocLen(hOut);
        return;
    }
    CloseHandle(hThread);

    if (hMain) {
        RECT rcClient;
        GetClientRect(hMain, &rcClient);
        RECT rcRun;
        runBtnRect(rcClient.right - rcClient.left, &rcRun);
        InvalidateRect(hMain, &rcRun, FALSE);
        RECT L, Rr; panels(&L, &Rr);
        RECT rcPill = { Rr.left, 0, Rr.right, Rr.top + 24 };
        InvalidateRect(hMain, &rcPill, FALSE);
    }
}

// Syntax check (Ctrl+B / menu / auto ~1 s after typing stops): g++ -fsyntax-only
// on the buffer.  The compiler IS the error detector — undeclared names, missing
// semicolons, type mismatches, bad arguments — every C++ compile error reported
// with file:line:col and the compiler's own explanation.  Results replace the
// output pane, where double-click already jumps to the offending line.
static void startCheck(bool force) {
    if (!hOut || !hCode) return;
    if (curLang() != LANG_CPP) {
        if (force) {
            SetWindowTextW(hOut, L"Not a C/C++ file — nothing to check.");
            progLen = outDocLen(hOut);
        }
        return;
    }
    if (sAlive || sInRunCap) return; // program owns the pane / compile in flight
    std::wstring gpp = getGppPath();
    if (gpp.empty()) {
        if (!force) return; // auto-check never prompts for the compiler path
        if (!ensureGpp(gpp)) return;
    }
    addCompilerBinToPath(gpp);
    std::wstring code = getCodeW();
    if (code.empty()) return;
    gLastChecked = code;
    makeRunArtifactPaths();
    if (!writeFileBytes(runSourcePath, w2u(code))) {
        SetWindowTextW(hOut, L"Could not create the temporary source file.");
        progLen = outDocLen(hOut);
        return;
    }
    SetWindowTextW(hOut, L"Checking...\r\n");
    UpdateWindow(hOut);
    progLen = outDocLen(hOut);
    std::string log;
    DWORD ec = 0;
    std::wstring workDir = runWorkingDirectory();
    std::wstring cmd = quoteCommandArg(gpp) + L" -std=c++17 -fsyntax-only " +
        quoteCommandArg(runSourcePath) + L" -I " + quoteCommandArg(workDir);
    if (!runCap(cmd, std::string(), log, 15000, ec, workDir)) {
        SetWindowTextW(hOut, L"Could not execute g++ for the syntax check.");
        progLen = outDocLen(hOut);
        gppCached.clear();
        gppTriedConfig = false;
        return;
    }
    int errs = 0, warns = 0;
    for (size_t p = 0; (p = log.find(": error:", p)) != std::string::npos; p += 8) errs++;
    for (size_t p = 0; (p = log.find(": warning:", p)) != std::string::npos; p += 10) warns++;
    std::wstring out;
    if (ec == 0 && log.empty()) {
        out = L"No errors found.\r\n";
    } else {
        wchar_t hdr[96];
        if (errs > 0 && warns > 0)
            wsprintfW(hdr, L"%d error%s, %d warning%s\r\n\r\n", errs, errs == 1 ? L"" : L"s",
                      warns, warns == 1 ? L"" : L"s");
        else if (errs > 0)
            wsprintfW(hdr, L"%d error%s\r\n\r\n", errs, errs == 1 ? L"" : L"s");
        else
            wsprintfW(hdr, L"%d warning%s\r\n\r\n", warns, warns == 1 ? L"" : L"s");
        out = hdr + fmtDiagLog(log);
    }
    SetWindowTextW(hOut, out.c_str());
    progLen = outDocLen(hOut);
    gOutHasProg = false;
    gOutIsCheck = true; // this pane now holds a compiler report
    gOutNotePath.clear();
    colorDiagOut();
}

// Timer 103: fires ~1 s after the last real edit (re-armed in EN_CHANGE) or one
// second after a document is shown.  One check per settled buffer.
static void checkTick() {
    if (!hOut || !hCode) return;
    if (sInRunCap) { SetTimer(hMain, 103, 800, NULL); return; } // in flight — retry
    if (curLang() != LANG_CPP) {
        // Nothing here is compiled, so a compiler report would be a lie.  Say so
        // once per document (and retire any report left over from the previous
        // one) rather than pretending the file has errors — or that it was
        // checked at all.
        if (!sAlive && !gOutHasProg) {
            const std::wstring path = (cur >= 0 && cur < (int)docs.size()) ? docs[cur].path : std::wstring();
            if (gOutNotePath != path) {
                gOutNotePath = path;
                gOutIsCheck = false;
                std::wstring nm = path.empty() ? std::wstring(L"this file") : baseName(path);
                SetWindowTextW(hOut, (L"No syntax check for " + nm + L".").c_str());
                progLen = outDocLen(hOut);
                colorDiagOut();
            }
        }
        return;
    }
    gOutNotePath.clear();
    if (sAlive) return;
    if (gOutHasProg) return; // the program's output owns the pane
    std::wstring code = getCodeW();
    if (code.empty() || code == gLastChecked) return;
    startCheck(false);
}

// Pixel-centered pill text. TextOut plants the BASELINE at y + tmAscent, so
// centring the font's whole cell (ascent+descent, its internal leading included)
// always rides low - visible on "output" and "Run". Centre the cap box instead:
// baseline = rect centre + capHeight/2, with capHeight = ascent - internal leading.
void pillText(HDC dc, RECT r, const wchar_t* s, HFONT f = NULL) {
    HFONT use = f ? f : hFontPill;
    HFONT prev = (HFONT)SelectObject(dc, use);
    SetBkMode(dc, TRANSPARENT);
    SIZE sz{0, 0};
    GetTextExtentPoint32W(dc, s, lstrlenW(s), &sz);
    TEXTMETRICW tm{};
    GetTextMetricsW(dc, &tm);
    int cy = (r.top + r.bottom + 1) / 2;
    // Prefer the font's declared cap height (g_uiCapH): it is the box a designer
    // centres against, while ascent-internal-leading is GDI's approximation and
    // comes out short whenever a font's win ascent overshoots its hhea ascent.
    int capH = g_uiCapH > 0 ? g_uiCapH : std::max(1, (int)(tm.tmAscent - tm.tmInternalLeading));
    int y = cy + capH / 2 - tm.tmAscent;
    int x = r.left + (r.right - r.left - sz.cx) / 2;
    TextOutW(dc, x, y, s, lstrlenW(s));
    SelectObject(dc, prev);
}

// --- icons/*.svg renderer ---------------------------------------------------
// House SVGs (Tabler-style): path-only, viewBox 0 0 24 24, absolute M/L/C/H/V/Z,
// stroke 1.5 round/round. One GraphicsPath per polarity because a single path
// can be filled, stroked, or both (run.svg) — mixing them would fill outlines.
struct SvgIcon { GraphicsPath fill, stroke; bool hasFill = false, hasStroke = false; };
static std::map<std::wstring, std::unique_ptr<SvgIcon>> g_svgCache;
// Resource builds generate this table from app.rc.  Keep the source standalone
// too: absent optional SVG assets simply fall back to the native controls/icons.
#if defined(__has_include)
#  if __has_include("svg_res_table.inc")
#    include "svg_res_table.inc"
#    define CATPP_HAS_SVG_RES 1
#  endif
#endif
#ifndef CATPP_HAS_SVG_RES
struct CatPPSvgResource { const wchar_t* name; int id; };
static const std::vector<CatPPSvgResource> SVG_RES;
#endif

static void svgParseD(const std::wstring& ds, GraphicsPath& gp) {
    const wchar_t* p = ds.c_str();
    auto skip = [&] { while (*p == L' ' || *p == L'\t' || *p == L',' || *p == L'\r' || *p == L'\n') ++p; };
    auto num = [&]() -> float {
        skip();
        wchar_t* e = nullptr;
        float v = (float)wcstod(p, &e);
        if (e == p) return 0.0f;
        p = e;
        return v;
    };
    float x = 0, y = 0, sx = 0, sy = 0;
    wchar_t cmd = L'M';
    bool rel = false;
    for (;;) {
        skip();
        if (!*p) break;
        bool explicitCmd = iswalpha(*p) != 0;
        if (explicitCmd) {
            cmd = *p++;
            rel = iswlower(cmd) != 0;
        } else if (cmd == L'M' || cmd == L'm') {
            cmd = rel ? L'l' : L'L'; // implicit pairs after moveto = lineto
        }
        wchar_t C = (wchar_t)towlower(cmd);
        float x1, y1, x2, y2, nx, ny;
        switch (C) {
        case L'm':
            nx = num(); ny = num();
            if (rel) { nx += x; ny += y; }
            gp.StartFigure();
            x = sx = nx; y = sy = ny;
            break;
        case L'l':
            nx = num(); ny = num();
            if (rel) { nx += x; ny += y; }
            gp.AddLine(x, y, nx, ny); x = nx; y = ny;
            break;
        case L'h':
            nx = num(); if (rel) nx += x;
            gp.AddLine(x, y, nx, y); x = nx;
            break;
        case L'v':
            ny = num(); if (rel) ny += y;
            gp.AddLine(x, y, x, ny); y = ny;
            break;
        case L'c':
            x1 = num(); y1 = num(); x2 = num(); y2 = num(); nx = num(); ny = num();
            if (rel) { x1 += x; y1 += y; x2 += x; y2 += y; nx += x; ny += y; }
            gp.AddBezier(x, y, x1, y1, x2, y2, nx, ny);
            x = nx; y = ny;
            break;
        case L'z':
            gp.CloseFigure();
            x = sx; y = sy;
            skip();
            if (!*p) break;
            if (iswalpha(*p)) { cmd = *p++; rel = iswlower(cmd) != 0; continue; }
            return; // garbage after Z without a letter — bail safely
        default:
            return; // unsupported command (Q/S/T/A) — icons don't use them
        }
        skip();
        if (!*p || iswalpha(*p)) continue; // next explicit command (loop top reads it)
    }
}

static SvgIcon* svgIcon(const wchar_t* name) {
    auto it = g_svgCache.find(name);
    if (it != g_svgCache.end()) return it->second.get();
    auto ic = std::make_unique<SvgIcon>();
    HRSRC hr = NULL;
    for (auto& r : SVG_RES)
        if (wcscmp(r.name, name) == 0) { hr = FindResourceW(NULL, MAKEINTRESOURCEW(r.id), RT_RCDATA); break; }
    if (hr) {
        DWORD sz = SizeofResource(GetModuleHandleW(NULL), hr);
        HGLOBAL hg = LoadResource(GetModuleHandleW(NULL), hr);
        const char* bytes = hg ? (const char*)LockResource(hg) : NULL;
        if (bytes && sz) {
            std::wstring s = u2w(std::string(bytes, sz));
            size_t pos = 0;
        while ((pos = s.find(L"<path", pos)) != std::wstring::npos) {
            size_t end = s.find(L'>', pos);
            if (end == std::wstring::npos) break;
            std::wstring tag = s.substr(pos, end - pos);
            auto attr = [&](const wchar_t* k) -> std::wstring {
                std::wstring key = std::wstring(k) + L"=\"";
                size_t a = tag.find(key);
                if (a == std::wstring::npos) return L"";
                a += key.size();
                size_t b = tag.find(L'"', a);
                return b == std::wstring::npos ? L"" : tag.substr(a, b - a);
            };
            std::wstring d = attr(L"d");
            if (!d.empty()) {
                std::wstring fill = attr(L"fill");   // absent → root fill="none"
                std::wstring stroke = attr(L"stroke"); // absent → SVG default none
                if (!fill.empty() && fill != L"none") { svgParseD(d, ic->fill); ic->hasFill = true; }
                if (!stroke.empty() && stroke != L"none") { svgParseD(d, ic->stroke); ic->hasStroke = true; }
            }
            pos = end + 1;
        }
        }
    }
    SvgIcon* raw = ic.get();
    g_svgCache[name] = std::move(ic); // cache misses too — no per-frame loads
    return raw;
}

static int dpx(int px) { return MulDiv(px, g_dpi, 96); } // icon px design-96 → device

static void drawSvgIcon(Graphics& g, const wchar_t* name, float x, float y, float px, const Color& col) {
    SvgIcon* ic = svgIcon(name);
    if (!ic || (!ic->hasFill && !ic->hasStroke)) return;
    GraphicsState st = g.Save();
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.TranslateTransform(x, y);
    g.ScaleTransform(px / 24.0f, px / 24.0f);
    if (ic->hasFill) { SolidBrush b(col); g.FillPath(&b, &ic->fill); }
    if (ic->hasStroke) {
        Pen p(col, 1.5f);
        p.SetLineCap(LineCapRound, LineCapRound, DashCapRound);
        p.SetLineJoin(LineJoinRound);
        g.DrawPath(&p, &ic->stroke);
    }
    g.Restore(st);
}

// File saved/unsaved badge slot: 16px icon, 8px left of the Run button, vertically centered.
static void saveBadgeRect(int winW, RECT* r) {
    RECT rr;
    runBtnRect(winW, &rr);
    int s = dpx(16);
    r->left = rr.left - s - 8; r->right = rr.left - 8;
    r->top = (TOPBAR_H - s) / 2; r->bottom = r->top + s;
}

// Main Window Vector Painting (Double-Buffered GDI+)
void paintAll(HDC hdc) {
    RECT c;
    GetClientRect(hMain, &c);
    int w = c.right - c.left, h = c.bottom - c.top;
    if (w <= 0 || h <= 0) return;

    // Cache the backbuffer across paints (Create/Delete every frame was pure waste)
    static HDC memDC = NULL;
    static HBITMAP memBmp = NULL;
    static HGDIOBJ oldBmp = NULL;
    static int bw = 0, bh = 0;
    if (!memDC || w != bw || h != bh) {
        if (memBmp) {
            if (oldBmp) SelectObject(memDC, oldBmp);
            DeleteObject(memBmp);
            memBmp = NULL;
        }
        if (memDC) DeleteDC(memDC);
        memDC = CreateCompatibleDC(hdc);
        memBmp = CreateCompatibleBitmap(hdc, w, h);
        oldBmp = SelectObject(memDC, memBmp);
        bw = w; bh = h;
    }

    {
        Graphics g(memDC);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(PixelOffsetModeHalf);
        g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

        Color winBgCol(255, GetRValue(th.bgWindow), GetGValue(th.bgWindow), GetBValue(th.bgWindow));
        Color cardBgCol(255, GetRValue(th.bgCard), GetGValue(th.bgCard), GetBValue(th.bgCard));
        Color outBgCol(255, GetRValue(th.bgOutput), GetGValue(th.bgOutput), GetBValue(th.bgOutput));
        Color borderCol(255, GetRValue(th.border), GetGValue(th.border), GetBValue(th.border));

        SolidBrush winBrush(winBgCol);
        SolidBrush cardBrush(cardBgCol);
        SolidBrush outBrush(outBgCol);
        Pen strokePen(borderCol, 1.0f); // hairline border

        // Canvas background
        g.FillRectangle(&winBrush, 0, 0, w, h);

        // 0. Window controls (Close, Minimize, Maximize) - flat monochrome squares.
        // The colored traffic-light discs are gone: a hairline box plus a glyph that
        // brightens on hover keeps the bar quiet without hiding the controls.
        struct MacBtn {
            float x, y;
        };
        float btnCenterY = (float)macBtnCY();
        MacBtn macBtns[3] = {
            { (float)MAC_BTN_CX[0], btnCenterY },
            { (float)MAC_BTN_CX[1], btnCenterY },
            { (float)MAC_BTN_CX[2], btnCenterY }
        };

        const float rBtn = (float)MAC_BTN_R;
        const float markR = rBtn - 4.0f; // mark reach: 6px inside a 20px square
        Color macMarkRest(255, GetRValue(th.gutterText), GetGValue(th.gutterText), GetBValue(th.gutterText));
        Color macMarkHot(255, GetRValue(th.text), GetGValue(th.text), GetBValue(th.text));
        for (int i = 0; i < 3; i++) {
            float bd = 2.0f * rBtn;
            float bx = macBtns[i].x - rBtn;
            float by = macBtns[i].y - rBtn;
            if (animMacHover[i] > 0.01f) {
                SolidBrush hoverBr(lerpColor(cardBgCol, borderCol, animMacHover[i] * 0.55f));
                g.FillRectangle(&hoverBr, bx, by, bd, bd);
            }
            Pen bPen(borderCol, 1.0f);
            g.DrawRectangle(&bPen, bx, by, bd, bd);
            // Native marks instead of the shipped svg glyphs: those icons are filled
            // discs, so they would put a circle back inside the square control.
            float gx = macBtns[i].x, gy = macBtns[i].y;
            Pen markPen(lerpColor(macMarkRest, macMarkHot, animMacHover[i]), 1.6f);
            if (i == 0) { // close: X
                g.DrawLine(&markPen, gx - markR, gy - markR, gx + markR, gy + markR);
                g.DrawLine(&markPen, gx + markR, gy - markR, gx - markR, gy + markR);
            } else if (i == 1) { // minimize: dash
                g.DrawLine(&markPen, gx - markR, gy, gx + markR, gy);
            } else { // maximize: square frame
                g.DrawRectangle(&markPen, gx - markR, gy - markR, markR * 2.0f, markR * 2.0f);
            }
        }

        // Topbar Run Button (smooth animated hover, press feedback & state transitions)
        RECT rcRun;
        runBtnRect(w, &rcRun);
        float pressY = animRunPress * 1.0f;
        GraphicsPath pathRun;
        addRect(pathRun, (float)rcRun.left, (float)rcRun.top + pressY, (float)RUN_BTN_W, (float)RUN_BTN_H);

        Color runRestBg(255, GetRValue(th.runBtnBg), GetGValue(th.runBtnBg), GetBValue(th.runBtnBg));
        Color runHoverBg(255, darkTheme ? 52 : 225, darkTheme ? 58 : 230, darkTheme ? 69 : 240);
        Color runPressBg(255, darkTheme ? 34 : 205, darkTheme ? 38 : 210, darkTheme ? 46 : 220);

        Color runBg = lerpColor(runRestBg, runHoverBg, animRunHover);
        runBg = lerpColor(runBg, runPressBg, animRunPress);

        SolidBrush runBrush(runBg);
        g.FillPath(&runBrush, &pathRun);

        // static neutral border — no blue/red glow
        Pen runBorder(Color(255, GetRValue(th.border), GetGValue(th.border), GetBValue(th.border)), 1.0f);
        g.DrawPath(&runBorder, &pathRun);

        COLORREF runTextColor = lerpColorRef(th.runBtnText, th.stopBtnText, animRunState);
        if (animRunHover > 0.01f) {
            runTextColor = lerpColorRef(runTextColor, RGB(255, 255, 255), animRunHover * 0.25f);
        }
        // icon + label group centered in the pill: run.svg play / native stop square
        {
            HFONT mf = (HFONT)SelectObject(memDC, hFontHeavy); // kept selected through the draw
            SetBkMode(memDC, TRANSPARENT);
            const wchar_t* runLabel = sAlive ? L"Stop" : L"Run";
            SIZE runSz{0, 0};
            GetTextExtentPoint32W(memDC, runLabel, lstrlenW(runLabel), &runSz);
            const int runIcon = dpx(13), runGap = dpx(5);
            int groupW = runIcon + runGap + runSz.cx;
            int gx = rcRun.left + (RUN_BTN_W - groupW) / 2;
            int gy = rcRun.top + (RUN_BTN_H - runIcon) / 2 + (int)pressY;
            SetTextColor(memDC, runTextColor);
            // Same optical centring as the pills: the label rect is exactly the
            // measured text box, so the glyph baseline is centred, not the cell.
            RECT rcRunLabel = { gx + runIcon + runGap, rcRun.top + (int)pressY,
                                gx + runIcon + runGap + runSz.cx,
                                rcRun.top + RUN_BTN_H + (int)pressY };
            pillText(memDC, rcRunLabel, runLabel, hFontHeavy);
            SelectObject(memDC, hFontHeavy);
            SelectObject(memDC, mf);
            Color runIconCol(255, GetRValue(runTextColor), GetGValue(runTextColor), GetBValue(runTextColor));
            if (sAlive) {
                SolidBrush stopBr(runIconCol); // no stop.svg in the set — native square
                g.FillRectangle(&stopBr, (float)gx + 2.0f, (float)gy + 2.0f,
                                (float)(runIcon - 4), (float)(runIcon - 4));
            } else {
                drawSvgIcon(g, L"run", (float)gx, (float)gy, (float)runIcon, runIconCol);
            }
        }

        // File saved/unsaved badge — 8px left of Run
        if (cur >= 0 && cur < (int)docs.size()) {
            RECT rb;
            saveBadgeRect(w, &rb);
            COLORREF bc = docs[cur].dirty ? th.stopBtnText : th.runBtnText;
            drawSvgIcon(g, docs[cur].dirty ? L"file-unsaved" : L"file-saved",
                        (float)rb.left, (float)rb.top, (float)(rb.bottom - rb.top),
                        Color(255, GetRValue(bc), GetGValue(bc), GetBValue(bc)));
        }

        RECT L, Rr, E;
        panels(&L, &Rr, &E);

        // 0. Files panel card (left of the editor card, when a folder is open)
        if (!expRoot.empty() && animExpWCur > 0.5f && E.right > E.left) {
            GraphicsPath pathE;
            addRect(pathE, (float)E.left, (float)E.top, (float)(E.right - E.left), (float)(E.bottom - E.top));
            g.FillPath(&cardBrush, &pathE);
            Pen strokePenE(borderCol, 1.0f);
            g.DrawPath(&strokePenE, &pathE);
        }

        // 1. Draw rounded card backgrounds and smoothly focused borders
        GraphicsPath pathL, pathR;
        addRect(pathL, (float)L.left, (float)L.top, (float)(L.right - L.left), (float)(L.bottom - L.top));
        addRect(pathR, (float)Rr.left, (float)Rr.top, (float)(Rr.right - Rr.left), (float)(Rr.bottom - Rr.top));
        g.FillPath(&cardBrush, &pathL);

        Color borderRest(255, GetRValue(th.border), GetGValue(th.border), GetBValue(th.border));
        Color borderFocusL = lerpColor(borderRest, Color(255, darkTheme ? 78 : 180, darkTheme ? 86 : 185, darkTheme ? 102 : 195), animLeftCardFocus);
        Pen strokePenL(borderFocusL, 1.0f);
        g.DrawPath(&strokePenL, &pathL);

        g.FillPath(&outBrush, &pathR);
        Color borderFocusR = lerpColor(borderRest, Color(255, darkTheme ? 78 : 180, darkTheme ? 86 : 185, darkTheme ? 102 : 195), animRightCardFocus);
        Pen strokePenR(borderFocusR, 1.0f);
        g.DrawPath(&strokePenR, &pathR);

        // 2. Output pill (centered on top edge of right card, width fits label)
        float pillH = 26.0f;
        const wchar_t* outLabel = sAlive ? L"running..." : L"output";
        HFONT measureF = (HFONT)SelectObject(memDC, hFontHeavy);
        SIZE outSz{0, 0};
        GetTextExtentPoint32W(memDC, outLabel, lstrlenW(outLabel), &outSz);
        SelectObject(memDC, measureF);
        // Even, integer width: an odd or fractional pill puts its vertical edges
        // on a half-pixel phase, and GDI+ then rasterises them twice as bright as
        // the card hairlines the pill sits on.  Even width keeps left and right
        // edges on whole pixels for any centred origin, so pill and card borders
        // come out of the same pipeline looking identical.
        int pillW_Out = outSz.cx + 28;
        if (pillW_Out < 76) pillW_Out = 76;
        if (pillW_Out & 1) pillW_Out++;
        int pillX_Out = (Rr.left + Rr.right - pillW_Out) / 2;
        int pillY_Out = Rr.top - (int)(pillH / 2.0f);

        GraphicsPath pathPillOut;
        addRect(pathPillOut, (float)pillX_Out, (float)pillY_Out, (float)pillW_Out, pillH);
        g.FillPath(&outBrush, &pathPillOut); // occludes top card border

        Pen outPillPen(borderRest, 1.0f);
        g.DrawPath(&outPillPen, &pathPillOut);

        RECT rcTextOut = { pillX_Out, pillY_Out, pillX_Out + pillW_Out, pillY_Out + (int)pillH };
        SetTextColor(memDC, th.text);
        pillText(memDC, rcTextOut, outLabel, hFontHeavy);

        // 3. File pill on left card (if a file is opened/saved, matching ui images/with file.png)
        if (hasFilePill()) {
            std::wstring fName = docTitle(cur);
            SIZE ts;
            SelectObject(memDC, hFontPill);
            GetTextExtentPoint32W(memDC, fName.c_str(), (int)fName.size(), &ts);
            // Same even/integer rule as the output pill so both read as hairlines.
            int pillW_File = ts.cx + 28;
            if (pillW_File < 76) pillW_File = 76;
            if (pillW_File & 1) pillW_File++;

            // In with file.png: pill is near the top right of the left card, right edge ~38px before card right
            int pillRight = L.right - 38;
            int pillX_File = pillRight - pillW_File;
            int pillY_File = L.top - (int)(pillH / 2.0f);

            GraphicsPath pathPillFile;
            addRect(pathPillFile, (float)pillX_File, (float)pillY_File, (float)pillW_File, pillH);
            g.FillPath(&cardBrush, &pathPillFile); // occludes card border
            g.DrawPath(&strokePen, &pathPillFile);

            RECT rcTextFile = { pillX_File, pillY_File, pillX_File + pillW_File, pillY_File + (int)pillH };
            pillText(memDC, rcTextFile, fName.c_str());
        }

        // 4. Custom scrollbars.  Off by default (the two-card look stays clean for
        //    short files); once a pane is taller than its viewport it gets a soft
        //    rail and a thumb that reacts to hover and drag.  Drawn last so it sits
        //    above the card, and its strip is reserved in doLayout so the wrap width
        //    already accounts for it.
        smartRefresh(smartCode);
        smartRefresh(smartOut);
        sbGeom(L, smartCode, sbCode);
        sbGeom(Rr, smartOut, sbOut);
        Color gutterCol(255, GetRValue(th.gutterText), GetGValue(th.gutterText), GetBValue(th.gutterText));
        Color textCol(255, GetRValue(th.text), GetGValue(th.text), GetBValue(th.text));
        for (int i = 0; i < 2; i++) {
            ScrollbarGeom& sg = i ? sbOut : sbCode;
            if (!sg.shown) continue;
            const int tw = sg.thumb.right - sg.thumb.left;
            const int railW = std::max(2, tw - 4);
            SolidBrush railBr(lerpColor(cardBgCol, borderCol, 0.75f));
            g.FillRectangle(&railBr, (float)(sg.track.left + (tw - railW) / 2), (float)sg.track.top,
                            (float)railW, (float)(sg.track.bottom - sg.track.top));
            Color thumbCol = sg.drag ? lerpColor(gutterCol, textCol, 0.85f)
                                     : (sg.hover ? lerpColor(gutterCol, textCol, 0.45f) : gutterCol);
            SolidBrush thumbBr(thumbCol);
            g.FillRectangle(&thumbBr, (float)sg.thumb.left, (float)sg.thumb.top,
                            (float)tw, (float)(sg.thumb.bottom - sg.thumb.top));
        }
    }

    BitBlt(hdc, 0, 0, w, h, memDC, 0, 0, SRCCOPY);
}

bool inHighlight = false;
bool inApply = false; // applyViewportSpans is running — its EM_SETCHARFORMAT fires a nested
                      // EN_CHANGE that must not invalidate the cache or arm timer101

// ponytail: cached lexer spans — scroll/zoom recolors the viewport from these
// directly; the old path re-armed timer101 (180ms, re-reset every frame while
// scrolling) so freshly-revealed lines sat base-gray for the whole scroll.
struct HlSpan { int start, end; COLORREF color; };
static std::vector<HlSpan> g_hlSpans;
static bool g_hlValid = false;
// Hard-line starts are cached for the gutter.  Re-reading/scanning a multi-MB
// buffer on every paint was the main source of scroll hitching on large files.
static std::vector<long> g_paraStarts;

void applyViewportSpans() {
    if (!hCode) return;
    if (!g_hlValid) { highlightCode(); return; } // text changed but not yet re-lexed → full pass now
    inApply = true;
    CHARRANGE prevSel;
    SendMessage(hCode, EM_EXGETSEL, 0, (LPARAM)&prevSel);
    POINT ptScroll = {0, 0};
    SendMessage(hCode, EM_GETSCROLLPOS, 0, (LPARAM)&ptScroll);
    SendMessage(hCode, WM_SETREDRAW, FALSE, 0);

    RECT vrc{};
    GetClientRect(hCode, &vrc);
    int visLines = (g_lineH > 0 ? vrc.bottom / g_lineH : 40) + 8;
    int firstLine = (int)SendMessage(hCode, EM_GETFIRSTVISIBLELINE, 0, 0);
    long vStart = (long)SendMessage(hCode, EM_LINEINDEX, firstLine, 0);
    long vEnd = (long)SendMessage(hCode, EM_LINEINDEX, firstLine + visLines, 0);
    if (vEnd <= vStart) {
        // EM_LINEINDEX fails either past the end of a short doc or before the
        // control has a laid-out viewport (0-height during load). Color a whole
        // short doc, but never expand to the whole of a large one: the fallback
        // would turn "viewport" into "entire document" and format every span —
        // seconds of dead stall on a big file. Layout re-runs this with a real
        // viewport once the control has a size.
        long total = GetWindowTextLengthW(hCode);
        if (total - vStart > 30000) return;
        vEnd = total;
    }
    CHARFORMAT2W cfSpan{};
    cfSpan.cbSize = sizeof(cfSpan);
    cfSpan.dwMask = CFM_COLOR;
    // Reset base color for the VISIBLE region only, then layer span colors on top.
    // The old path restyled the whole document on every re-lex (O(doc) and the
    // dominant hitch on 20k+ line files); outside the viewport the base reset
    // happens lazily when those lines scroll in.
    cfSpan.crTextColor = th.text;
    SendMessage(hCode, EM_SETSEL, vStart, vEnd);
    SendMessage(hCode, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cfSpan);
    // g_hlSpans is sorted by start: jump straight to the visible slice instead
    // of testing every span in the document (that scan grew with file size and
    // was the remaining per-frame cost on 20k+ line files). Step back over any
    // span straddling vStart (a multi-line string/comment entering the view).
    size_t lo = (size_t)(std::lower_bound(g_hlSpans.begin(), g_hlSpans.end(), vStart,
                     [](const HlSpan& s, long v) { return s.start < v; }) - g_hlSpans.begin());
    while (lo > 0 && g_hlSpans[lo - 1].end > vStart) lo--;
    for (size_t i = lo; i < g_hlSpans.size() && g_hlSpans[i].start < vEnd; i++) {
        cfSpan.crTextColor = g_hlSpans[i].color;
        SendMessage(hCode, EM_SETSEL, g_hlSpans[i].start, g_hlSpans[i].end);
        SendMessage(hCode, EM_SETCHARFORMAT, SCF_SELECTION, (LPARAM)&cfSpan);
    }

    SendMessage(hCode, EM_SETSEL, prevSel.cpMin, prevSel.cpMax);
    SendMessage(hCode, EM_SETSCROLLPOS, 0, (LPARAM)&ptScroll);
    SendMessage(hCode, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(hCode, NULL, FALSE);
    inApply = false;
}

void highlightCode() {
    if (!hCode || inHighlight) return;
    inHighlight = true;

    CHARRANGE prevSel;
    SendMessage(hCode, EM_EXGETSEL, 0, (LPARAM)&prevSel);
    POINT ptScroll = {0, 0};
    SendMessage(hCode, EM_GETSCROLLPOS, 0, (LPARAM)&ptScroll);

    int textLen = GetWindowTextLengthW(hCode);
    if (textLen <= 0) {
        g_hlSpans.clear();
        g_hlValid = true; // empty doc = parsed-empty; EN_CHANGE invalidates once text arrives
        inHighlight = false;
        return;
    }
    // .txt and other unrecognised types: plain buffer, no spans — showDoc already
    // painted it th.text.  Better no colour than wrong colour.
    if (curLang() == LANG_TXT) {
        g_hlSpans.clear();
        g_hlValid = true;
        inHighlight = false;
        return;
    }

    setHlUndo(hCode, true); // formatting must not pollute Ctrl+Z
    SendMessage(hCode, WM_SETREDRAW, FALSE, 0);

    // Lexer with exact RichEdit index mapping to eliminate newline drift
    static std::vector<wchar_t> text;
    text.assign((size_t)textLen + 1, 0);
    GetWindowTextW(hCode, text.data(), textLen + 1);

    static std::vector<int> toRichEdit;
    toRichEdit.resize((size_t)textLen + 1);
    int richPos = 0;
    for (int k = 0; k < textLen; k++) {
        toRichEdit[k] = richPos;
        if (text[k] == L'\r' && k + 1 < textLen && text[k + 1] == L'\n') {
            richPos++;
            k++; // skip \n
            toRichEdit[k] = richPos;
            continue;
        }
        richPos++;
    }
    toRichEdit[textLen] = richPos;

    std::vector<HlSpan> spans;
    spans.reserve(512);

    auto addSpan = [&](int s, int e, COLORREF col) {
        if (s >= 0 && s < (int)toRichEdit.size() && e >= 0 && e <= (int)toRichEdit.size() && s < e) {
            spans.push_back({ toRichEdit[s], toRichEdit[e], col });
        }
    };

    DocLang lang = curLang();

    if (lang == LANG_BAT) {
        // Batch: line-oriented.  REM / :: comment out a whole line, :label names a
        // jump target, %VAR% and !VAR! are expansions, the rest is commands and
        // arguments, so keywords are matched only at the start of a command word.
        static const std::unordered_set<std::wstring> batKw = {
            L"assoc", L"attrib", L"break", L"call", L"cd", L"chcp", L"chdir", L"cls",
            L"color", L"copy", L"date", L"del", L"dir", L"do", L"echo", L"else",
            L"endlocal", L"erase", L"errorlevel", L"exist", L"exit", L"for", L"if",
            L"in", L"md", L"mkdir", L"move", L"not", L"pause", L"popd", L"powershell",
            L"pushd", L"rd", L"ren", L"rename", L"rmdir", L"robocopy", L"set", L"setlocal",
            L"shift", L"start", L"time", L"title", L"type", L"where", L"xcopy"
        };
        int p = 0;
        while (p < textLen) {
            int ls = p, le = p;
            while (le < textLen && text[le] != L'\r' && text[le] != L'\n') le++;
            int k = ls;
            while (k < le && (text[k] == L' ' || text[k] == L'\t')) k++;
            bool at = (k < le && text[k] == L'@');
            if (at) k++;
            bool remLine = (k + 3 <= le) && towlower(text[k]) == L'r' &&
                           towlower(text[k + 1]) == L'e' && towlower(text[k + 2]) == L'm' &&
                           (k + 3 == le || !iswalnum(text[k + 3]));
            bool colonComment = !at && k + 1 < le && text[k] == L':' && text[k + 1] == L':';
            if (remLine || colonComment) {
                addSpan(ls, le, th.comment);
            } else if (!at && k < le && text[k] == L':') {
                int j = k + 1;
                while (j < le && !iswspace(text[j])) j++;
                addSpan(k, j, th.function); // :label
                k = j;
            }
            while (k < le) {
                wchar_t c = text[k];
                if (c == L'"') {
                    int s = k++;
                    while (k < le && text[k] != L'"') k++;
                    if (k < le) k++;
                    addSpan(s, k, th.string);
                } else if (c == L'%' || c == L'!') {
                    int s = k++;
                    while (k < le && text[k] != c) k++;
                    if (k < le) k++;
                    addSpan(s, k, th.type); // %VAR% / !VAR!
                } else if (iswalpha(c)) {
                    int s = k;
                    while (k < le && (iswalnum(text[k]) || text[k] == L'_')) k++;
                    std::wstring w(text.data() + s, (size_t)(k - s));
                    if (batKw.count(w)) addSpan(s, k, th.keyword);
                } else if (iswdigit(c)) {
                    int s = k;
                    while (k < le && (iswdigit(text[k]) || text[k] == L'.')) k++;
                    addSpan(s, k, th.number);
                } else {
                    k++;
                }
            }
            p = le;
            while (p < textLen && (text[p] == L'\r' || text[p] == L'\n')) p++;
        }
    } else if (lang == LANG_MD) {
        // line-based: fences toggle, headings/links/markers, inline code & emphasis
        bool inFence = false;
        int p = 0;
        while (p < textLen) {
            int ls = p, le = p;
            while (le < textLen && text[le] != L'\r' && text[le] != L'\n') le++;

            bool fenceLine = (le - ls >= 3 && text[ls] == L'`' && text[ls + 1] == L'`' && text[ls + 2] == L'`');
            if (fenceLine) {
                addSpan(ls, le, th.comment);
                inFence = !inFence;
            } else if (inFence) {
                addSpan(ls, le, th.string);
            } else {
                int k = ls;
                while (k < le && text[k] == L'#') k++;
                if (k > ls && k < le && text[k] == L' ') {
                    addSpan(ls, le, th.function); // ATX heading
                } else {
                    if (text[ls] == L'>') addSpan(ls, le, th.comment);
                    if (le - ls >= 2 && (text[ls] == L'-' || text[ls] == L'*' || text[ls] == L'+') && text[ls + 1] == L' ')
                        addSpan(ls, ls + 2, th.op);
                    else if (iswdigit(text[ls])) {
                        int c2 = ls;
                        while (c2 < le && iswdigit(text[c2])) c2++;
                        if (c2 + 1 < le && text[c2] == L'.' && text[c2 + 1] == L' ')
                            addSpan(ls, c2 + 2, th.op);
                    }
                    int q = ls;
                    while (q < le) {
                        if (text[q] == L'`') {
                            int s = q++;
                            while (q < le && text[q] != L'`') q++;
                            if (q < le) q++;
                            addSpan(s, q, th.string);
                        } else if (q + 1 < le && text[q] == L'*' && text[q + 1] == L'*') {
                            int s = q;
                            q += 2;
                            while (q + 1 < le && !(text[q] == L'*' && text[q + 1] == L'*')) q++;
                            if (q + 1 < le) q += 2;
                            addSpan(s, q, th.type);
                        } else if (text[q] == L'*') {
                            int s = q++;
                            while (q < le && text[q] != L'*') q++;
                            if (q < le) q++;
                            addSpan(s, q, th.type);
                        } else if (text[q] == L'[') {
                            int s = q, j = q;
                            while (j < le && text[j] != L']') j++;
                            if (j < le && j + 1 < le && text[j] == L']' && text[j + 1] == L'(') {
                                int u = j + 2;
                                while (u < le && text[u] != L')') u++;
                                if (u < le) {
                                    addSpan(s, u + 1, th.function);
                                    q = u + 1;
                                    continue;
                                }
                            }
                            q++;
                        } else {
                            q++;
                        }
                    }
                }
            }
            p = le;
            while (p < textLen && (text[p] == L'\r' || text[p] == L'\n')) p++;
        }
    } else {
        static const std::unordered_set<std::wstring> keywords = {
            L"alignas", L"alignof", L"and", L"and_eq", L"asm", L"atomic_cancel",
            L"atomic_commit", L"atomic_noexcept", L"bitand", L"bitor", L"break",
            L"case", L"catch", L"class", L"compl", L"concept", L"const", L"consteval",
            L"constexpr", L"constinit", L"const_cast", L"continue", L"co_await",
            L"co_return", L"co_yield", L"decltype", L"default", L"delete", L"do",
            L"dynamic_cast", L"else", L"enum", L"explicit", L"export", L"extern",
            L"false", L"for", L"friend", L"goto", L"if", L"inline", L"mutable",
            L"namespace", L"new", L"noexcept", L"not", L"not_eq", L"nullptr",
            L"operator", L"or", L"or_eq", L"override", L"private", L"protected",
            L"public", L"reflexpr", L"register", L"reinterpret_cast", L"requires",
            L"return", L"sizeof", L"static", L"static_assert", L"static_cast",
            L"struct", L"switch", L"synchronized", L"template", L"this", L"thread_local",
            L"throw", L"true", L"try", L"typedef", L"typeid", L"typename", L"union",
            L"using", L"virtual", L"volatile", L"while", L"xor", L"xor_eq", L"final"
        };

        static const std::unordered_set<std::wstring> types = {
            L"bool", L"char", L"char8_t", L"char16_t", L"char32_t", L"wchar_t",
            L"short", L"int", L"long", L"signed", L"unsigned", L"float", L"double",
            L"void", L"auto", L"size_t", L"ptrdiff_t", L"int8_t", L"int16_t",
            L"int32_t", L"int64_t", L"uint8_t", L"uint16_t", L"uint32_t", L"uint64_t",
            L"string", L"wstring", L"vector", L"map", L"set", L"pair", L"queue",
            L"stack", L"deque", L"list", L"unordered_map", L"unordered_set",
            L"shared_ptr", L"unique_ptr", L"weak_ptr", L"cout", L"cin", L"cerr", L"endl",
            L"std", L"ifstream", L"ofstream", L"stringstream", L"istringstream", L"ostringstream"
        };

        // GLSL/HLSL extras — only consulted when the buffer is a shader
        static const std::unordered_set<std::wstring> shaderKw = {
            L"uniform", L"varying", L"attribute", L"in", L"out", L"inout",
            L"layout", L"precision", L"highp", L"mediump", L"lowp", L"discard",
            L"buffer", L"shared", L"coherent", L"volatile", L"restrict",
            L"readonly", L"writeonly", L"cbuffer", L"packoffset", L"patch"
        };
        static const std::unordered_set<std::wstring> shaderTy = {
            L"vec2", L"vec3", L"vec4", L"ivec2", L"ivec3", L"ivec4",
            L"uvec2", L"uvec3", L"uvec4", L"bvec2", L"bvec3", L"bvec4",
            L"mat2", L"mat3", L"mat4", L"mat2x2", L"mat3x3", L"mat4x4",
            L"dmat2", L"dmat3", L"dmat4", L"sampler2D", L"sampler3D",
            L"samplerCube", L"sampler2DShadow", L"isampler2D", L"usampler2D",
            L"Texture2D", L"Texture3D", L"TextureCube", L"SamplerState",
            L"SamplerComparisonState", L"RWTexture2D", L"RWTexture3D",
            L"StructuredBuffer", L"RWStructuredBuffer", L"Buffer",
            L"float2", L"float3", L"float4", L"float2x2", L"float3x3",
            L"float4x4", L"int2", L"int3", L"int4", L"uint2", L"uint3",
            L"uint4", L"half", L"half2", L"half3", L"half4",
            L"min16float", L"min16int"
        };

        int i = 0;
        while (i < textLen) {
            wchar_t c = text[i];

            // Whitespace
            if (c == L' ' || c == L'\t' || c == L'\r' || c == L'\n') {
                i++;
                continue;
            }

            // Line comment
            if (c == L'/' && i + 1 < textLen && text[i + 1] == L'/') {
                int start = i;
                i += 2;
                while (i < textLen && text[i] != L'\r' && text[i] != L'\n') i++;
                addSpan(start, i, th.comment);
                continue;
            }

            // Block comment
            if (c == L'/' && i + 1 < textLen && text[i + 1] == L'*') {
                int start = i;
                i += 2;
                while (i + 1 < textLen && !(text[i] == L'*' && text[i + 1] == L'/')) i++;
                if (i + 1 < textLen) i += 2;
                else i = textLen;
                addSpan(start, i, th.comment);
                continue;
            }

            // Preprocessor (#...)
            if (c == L'#') {
                int start = i++;
                while (i < textLen && (iswalnum(text[i]) || text[i] == L'_')) i++;
                addSpan(start, i, th.keyword);

                // Scan rest of preprocessor line for <header> or "header"
                while (i < textLen && text[i] != L'\r' && text[i] != L'\n') {
                    if (text[i] == L'<' || text[i] == L'"') {
                        wchar_t closeQuote = (text[i] == L'<') ? L'>' : L'"';
                        int qStart = i++;
                        while (i < textLen && text[i] != closeQuote && text[i] != L'\r' && text[i] != L'\n') i++;
                        if (i < textLen && text[i] == closeQuote) i++;
                        addSpan(qStart, i, th.string);
                    } else {
                        i++;
                    }
                }
                continue;
            }

            // String literal "..."
            if (c == L'"' || c == L'“' || c == L'”') {
                wchar_t endQuote = (c == L'“') ? L'”' : (c == L'”' ? L'”' : L'"');
                int start = i++;
                while (i < textLen && text[i] != endQuote && text[i] != L'"') {
                    if (text[i] == L'\\' && i + 1 < textLen) i += 2;
                    else if (text[i] == L'\r' || text[i] == L'\n') break;
                    else i++;
                }
                if (i < textLen && (text[i] == endQuote || text[i] == L'"')) i++;
                addSpan(start, i, th.string);
                continue;
            }

            // Char literal '...'
            if (c == L'\'') {
                int start = i++;
                while (i < textLen && text[i] != L'\'') {
                    if (text[i] == L'\\' && i + 1 < textLen) i += 2;
                    else if (text[i] == L'\r' || text[i] == L'\n') break;
                    else i++;
                }
                if (i < textLen && text[i] == L'\'') i++;
                addSpan(start, i, th.string);
                continue;
            }

            // Number literal
            if ((c >= L'0' && c <= L'9') || (c == L'.' && i + 1 < textLen && text[i + 1] >= L'0' && text[i + 1] <= L'9')) {
                int start = i++;
                while (i < textLen && (iswalnum(text[i]) || text[i] == L'.' || text[i] == L'\'')) i++;
                addSpan(start, i, th.number);
                continue;
            }

            // Identifiers and keywords
            if (iswalpha(c) || c == L'_') {
                int start = i++;
                while (i < textLen && (iswalnum(text[i]) || text[i] == L'_')) i++;
                std::wstring word(&text[start], i - start);

                if (keywords.count(word) || (lang == LANG_SHADER && shaderKw.count(word))) {
                    addSpan(start, i, th.keyword);
                } else if (types.count(word) || (lang == LANG_SHADER && shaderTy.count(word))) {
                    if (word == L"cout" || word == L"cin" || word == L"cerr" || word == L"endl") {
                        addSpan(start, i, th.op); // stream objects in cyan #56b6c2
                    } else {
                        addSpan(start, i, th.type);
                    }
                } else {
                    // Check if followed by '(' -> function call
                    int peek = i;
                    while (peek < textLen && (text[peek] == L' ' || text[peek] == L'\t')) peek++;
                    if (peek < textLen && text[peek] == L'(') {
                        addSpan(start, i, th.function);
                    }
                }
                continue;
            }

            // Operators
            if (wcschr(L"+-*/%=<>!&|^~?:", c)) {
                int start = i++;
                while (i < textLen && wcschr(L"+-*/%=<>!&|^~?:", text[i])) i++;
                addSpan(start, i, th.op);
                continue;
            }

            i++;
        }
    } // end lang != LANG_MD

    // Apply spans — visible viewport only (per-span cost grows with applied
    // runs: ~7s for 15k spans on 220KB files). Cache the parse so scroll can
    // recolor newly-revealed lines instantly instead of waiting on timer101.
    g_hlSpans = std::move(spans);
    // Sorted by start so applyViewportSpans can binary-search the visible slice
    // instead of scanning every span in the document on each repaint.
    std::sort(g_hlSpans.begin(), g_hlSpans.end(),
              [](const HlSpan& a, const HlSpan& b) { return a.start < b.start; });
    g_hlValid = true;
    // put the view back before the viewport apply so it covers what the user sees
    SendMessage(hCode, EM_SETSCROLLPOS, 0, (LPARAM)&ptScroll);
    applyViewportSpans();

    // Restore selection & scroll
    SendMessage(hCode, EM_SETSEL, prevSel.cpMin, prevSel.cpMax);
    SendMessage(hCode, EM_SETSCROLLPOS, 0, (LPARAM)&ptScroll);
    SendMessage(hCode, WM_SETREDRAW, TRUE, 0);
    setHlUndo(hCode, false);
    InvalidateRect(hCode, NULL, FALSE);

    inHighlight = false;
}

// ============================================================================
// Smooth invisible scrolling — RichEdit keeps the reliable visual-line model
// and native range internally, while CatPP hides all scrollbar chrome and
// animates wheel/touchpad input toward a line target.
// ============================================================================
static void smartSetFirstLine(SmartScroll& sc, int wanted) {
    if (!sc.editor || !IsWindow(sc.editor)) return;
    wanted = std::max(0, std::min(wanted, sc.maxFirst));
    // smartRefresh() snapshots this whenever layout/a wheel gesture begins.
    // Do not ask RichEdit for the same line again every animation frame: that
    // synchronous cross-control query was the expensive part of a long scroll.
    if (wanted != sc.firstLine) {
        SendMessageW(sc.editor, EM_LINESCROLL, 0, wanted - sc.firstLine);
        sc.firstLine = wanted;
    }
}

void smartRefresh(SmartScroll& sc) {
    if (!sc.editor || !IsWindow(sc.editor)) return;
    int oldFirst = sc.firstLine;
    RECT rc{};
    GetClientRect(sc.editor, &rc);
    const int height = std::max<int>(1, (int)(rc.bottom - rc.top));
    sc.totalLines = std::max(1, (int)SendMessageW(sc.editor, EM_GETLINECOUNT, 0, 0));
    sc.visibleLines = std::max(1, (height + std::max(1, g_lineH) - 1) / std::max(1, g_lineH));
    sc.maxFirst = std::max(0, sc.totalLines - sc.visibleLines);
    sc.firstLine = (int)SendMessageW(sc.editor, EM_GETFIRSTVISIBLELINE, 0, 0);
    if (sc.firstLine < 0) sc.firstLine = 0;
    if (sc.firstLine > sc.maxFirst) sc.firstLine = sc.maxFirst;

    if (oldFirst != sc.firstLine && &sc == &smartCode && hGutter)
        InvalidateRect(hGutter, NULL, FALSE);
}

void smartWheel(SmartScroll& sc, WPARAM w) {
    smartRefresh(sc);
    if (sc.maxFirst <= 0) return;
    short delta = GET_WHEEL_DELTA_WPARAM(w);
    if (!delta) return;

    UINT configured = 3;
    SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &configured, 0);
    int lines = configured == WHEEL_PAGESCROLL
        ? std::max(1, sc.visibleLines - 1)
        : std::max(1, (int)configured);

    if (!sc.active) {
        sc.animLine = (float)sc.firstLine;
        sc.targetLine = sc.animLine;
    }
    // Wheel down has a negative delta and advances toward later lines.
    sc.targetLine -= ((float)delta / (float)WHEEL_DELTA) * lines;
    sc.targetLine = std::max(0.0f, std::min(sc.targetLine, (float)sc.maxFirst));
    sc.active = true;
}

void tickSmartScrolls(double dt) {
    if (dt < 0.001) dt = 0.001;
    if (dt > 0.1) dt = 0.1;
    for (SmartScroll* sc : { &smartCode, &smartOut }) {
        // Idle panes deliberately do no RichEdit queries. While active, the
        // cached visual-line range remains valid until layout or the next wheel
        // event refreshes it, so the animation path is pure math + a scroll
        // commit only when the rounded target actually crosses a line.
        if (!sc->active || !sc->editor || !IsWindow(sc->editor)) continue;
        if (sc->maxFirst <= 0) { sc->active = false; continue; }
        sc->targetLine = std::max(0.0f, std::min(sc->targetLine, (float)sc->maxFirst));
        const float k = 1.0f - expf(-(float)dt / 0.085f);
        sc->animLine += (sc->targetLine - sc->animLine) * k;
        int wanted = (int)lroundf(sc->animLine);
        if (fabsf(sc->targetLine - sc->animLine) < 0.03f) {
            sc->animLine = sc->targetLine;
            wanted = (int)lroundf(sc->targetLine);
            sc->active = false;
        }
        const int before = sc->firstLine;
        smartSetFirstLine(*sc, wanted);
        if (sc == &smartCode && before != sc->firstLine && hGutter)
            InvalidateRect(hGutter, NULL, FALSE);
    }
}

// Layout Manager
void doLayout() {
    RECT L, Rr, E;
    panels(&L, &Rr, &E);

    // Padding inside cards
    int topOffset = L.top + 20;
    int edH = (L.bottom - 16) - topOffset;

    // Gutter: tight box measured to widest line number, right-aligned inside.
    // 14px inset from the card edge, slack keeps AA off the border.
    int gutterX = L.left + 14; // 14px breathing room before "1."
    int gutterW = (codeFontSize >= 18) ? 48 : 42;
    // A still-zero-width RichEdit wraps every character, so EM_GETLINECOUNT on
    // a large file never returns. Position hCode at its final-ish width (default
    // gutter, no scrollbar strip yet) before the first line-count query below.
    if (hCode) {
        RECT r{};
        bool sized = GetClientRect(hCode, &r) && r.right >= 16;
        if (!sized) {
            const int pX = (int)gutterX + 42 + 14;
            const int pW = std::max(1, (int)(L.right - 16) - pX);
            MoveWindow(hCode, pX, topOffset - 8, pW, edH + 8, FALSE);
        }
    }
    if (hCode && hFontBold) { // measure "N." in bold (active-line) font: widest case
        int tl = (int)SendMessage(hCode, EM_GETLINECOUNT, 0, 0);
        if (tl < 10) tl = 10; // ponytail: floor at "10." so short docs keep the wide gutter
        wchar_t nb[16];
        wsprintfW(nb, L"%d.", tl);
        HDC dc = GetDC(hCode);
        if (dc) {
            HGDIOBJ old = SelectObject(dc, hFontBold);
            SIZE sz{};
            if (GetTextExtentPoint32W(dc, nb, lstrlenW(nb), &sz) && sz.cx > 0)
                gutterW = sz.cx + 10; // 8px right pad (GutterProc gw-8) + 2 slack
            SelectObject(dc, old);
            ReleaseDC(hCode, dc);
        }
    } else { // no fonts yet — digit estimate: 42px fits "99." + 12 per extra digit
        int tl = hCode ? (int)SendMessage(hCode, EM_GETLINECOUNT, 0, 0) : 99, dg = 2;
        for (int t = 100; t <= tl && dg < 6; t *= 10) dg++;
        if (dg > 2) gutterW += (dg - 2) * 12;
    }
    if (gutterW < 28) gutterW = 28;
    int codeX = gutterX + gutterW + 14; // 14px clean gap between line numbers and code
    int codeW = (L.right - 16) - codeX;
    // Reserve the scrollbar strip BEFORE wrapping, so the wrap width and the
    // control width always agree and no glyph ever runs under the drawn bar.
    // Hysteresis on both edges: the strip itself eats text width, which can add a
    // wrapped line and tip the count back under the threshold, and that would
    // flicker the bar on and off while typing.
    {
        int lines = hCode ? (int)SendMessage(hCode, EM_GETLINECOUNT, 0, 0) : 0;
        int visible = std::max(1, edH / std::max(1, g_lineH));
        if (lines > visible) gSbCodeOn = true;
        else if (lines + 2 <= visible) gSbCodeOn = false;
        if (gSbCodeOn) codeW -= sbStripW();
    }

    // Keep the source grid a touch higher in the card. Move the editor and
    // gutter together so the text, custom caret, and hard-line numbers retain
    // their shared vertical coordinate system instead of drifting apart.
    const int editorLift = 8;
    const int editorTop = topOffset - editorLift;
    const int editorH = edH + editorLift;
    MoveWindow(hGutter, gutterX, editorTop, gutterW, editorH, TRUE);
    // The gutter consumes EM_POSFROMCHAR coordinates from hCode. Both windows
    // must therefore share exactly the same top origin: this is the only
    // geometry that makes source glyphs, custom caret and hard-line numbers
    // mathematically coincide at every font size and DPI.
    const int codeTop = editorTop;

    // Word wrap: msftedit treats EM_SETTARGETDEVICE width as TWIPS (not HIMETRIC).
    // Target BEFORE the size change below — MoveWindow is then the reflow trigger,
    // no jiggle needed when width actually moves. Skipped mid-drag (re-applied on
    // release): a full re-line-break per mousemove is what made dragging janky.
    int wantWrap = MulDiv(codeW, 1440, g_dpi);
    bool wrapChanged = hCode && wantWrap != gWrapApplied && !expDrag && !gWindowSizing;
    RECT prc{};
    if (hCode) GetClientRect(hCode, &prc);
    if (wrapChanged) {
        HDC dc = GetDC(hCode);
        SendMessage(hCode, WM_SETREDRAW, FALSE, 0);
        SendMessage(hCode, EM_SETTARGETDEVICE, (WPARAM)dc, wantWrap);
        SendMessage(hCode, WM_SETREDRAW, TRUE, 0);
        ReleaseDC(hCode, dc);
        gWrapApplied = wantWrap;
    }

    MoveWindow(hCode, codeX, codeTop, codeW, editorH, TRUE);
    // Native chrome stays off everywhere: CatPP paints its own rail + thumb into
    // the reserved strip (see sbGeom / paintAll), which is also why the pane got
    // narrower above instead of having a system scrollbar bolted onto its edge.
    if (hCode) ShowScrollBar(hCode, SB_VERT, FALSE);
    if (wrapChanged) {
        InvalidateRect(hGutter, NULL, FALSE);
        if (prc.right == codeW) {
            // wrap-only change (width unchanged) — jiggle so msftedit reflows display;
            // sync redraw: async invalidate alone leaves stale wrap pixels
            SetWindowPos(hCode, NULL, 0, 0, prc.right + 1, prc.bottom, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            SetWindowPos(hCode, NULL, 0, 0, prc.right, prc.bottom, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            RedrawWindow(hCode, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW | RDW_ALLCHILDREN);
        }
    }

    // Output console: inside right card, with the same reserved strip
    int outX = Rr.left + 16;
    int outW = (Rr.right - 16) - outX;
    {
        int lines = hOut ? (int)SendMessage(hOut, EM_GETLINECOUNT, 0, 0) : 0;
        int visible = std::max(1, edH / std::max(1, g_lineH));
        if (lines > visible) gSbOutOn = true;
        else if (lines + 2 <= visible) gSbOutOn = false;
        if (gSbOutOn) outW -= sbStripW();
    }
    MoveWindow(hOut, outX, topOffset, outW, edH, TRUE);
    ShowScrollBar(hOut, SB_VERT, FALSE);
    smartRefresh(smartCode);
    smartRefresh(smartOut);

    // Files panel: inside explorer card
    if (hFiles) {
        if (!expRoot.empty() && animExpWCur > 0.5f && E.right - E.left > 32) {
            MoveWindow(hFiles, E.left + 16, topOffset, (E.right - E.left) - 32, edH, TRUE);
            InvalidateRect(hFiles, NULL, FALSE); // resize leaves its update region empty (verified: hover-repaint is the only thing that refreshed it)
            if (!IsWindowVisible(hFiles)) ShowWindow(hFiles, SW_SHOW);
        } else if (IsWindowVisible(hFiles)) {
            ShowWindow(hFiles, SW_HIDE);
        }
    }

    InvalidateRect(hMain, NULL, FALSE);
    // Skip the RichEdit reformat while a resize or the explorer slide is still
    // moving: the reflow already repaints each frame, and the final layout applies
    // the colors once the motion settles (same deal as the resize path).
    if (!gWindowSizing && !expSliding) {
        applyViewportSpans(); // zoom/resize reveals new lines — color them now, not after debounce
        SetTimer(hMain, 101, 180, NULL); // viewport changed → colors re-cover it
    }
}

// Vertical alignment helper -----------------------------------------------------------------
// RichEdit lays the source out with the font's OWN typographic descent (hhea /
// OS-2 sTypoDescender) and bottom-aligns that cell inside the exact line box.
// GDI's GetOutlineTextMetrics reports a smaller, hinted descent for the shipped
// variable font (0.215em vs the file's 0.286em), so trusting it plants the line
// numbers about a pixel per 11pt of zoom off the source.  Reading head/hhea from
// the very file we loaded is the only version of that number both engines agree on.
// Ratios are relative to the em square, so they stay valid at any zoom/DPI.
struct FontTableMetrics {
    double descent = 0.0;   // hhea descender, positive
    double ascent = 0.0;    // hhea ascender
    double capHeight = 0.0; // OS/2 sCapHeight (0 when the table is too old)
};
static FontTableMetrics fontTableMetrics(const std::wstring& path) {
    FontTableMetrics m;
    std::ifstream f(path.c_str(), std::ios::binary);
    if (!f) return m;
    std::string d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (d.size() < 64) return m;
    auto u16 = [&](size_t o) -> unsigned {
        return ((unsigned)(unsigned char)d[o] << 8) | (unsigned char)d[o + 1];
    };
    auto i16 = [&](size_t o) -> int {
        return (int)(short)u16(o);
    };
    auto u32 = [&](size_t o) -> unsigned {
        return (u16(o) << 16) | u16(o + 2);
    };
    if (u32(0) != 0x00010000u) return m; // sfnt / TrueType outlines only
    const unsigned n = u16(4);
    size_t head = 0, hhea = 0, os2 = 0;
    unsigned os2Len = 0;
    for (unsigned i = 0; i < n; i++) {
        const size_t rec = 12 + 16 * (size_t)i;
        if (rec + 16 > d.size()) return m;
        const char* tag = d.data() + rec;
        const unsigned off = u32(rec + 8);
        const unsigned len = u32(rec + 12);
        if (std::string(tag, 4) == "head") head = off;
        else if (std::string(tag, 4) == "hhea") hhea = off;
        else if (std::string(tag, 4) == "OS/2") { os2 = off; os2Len = len; }
    }
    if (!head || !hhea || head + 20 > d.size() || hhea + 8 > d.size()) return m;
    const int unitsPerEm = (int)u16(head + 18);
    if (unitsPerEm <= 0) return m;
    const int ascender = i16(hhea + 4);
    const int descender = i16(hhea + 6); // negative, y-down
    if (ascender > 0) m.ascent = (double)ascender / unitsPerEm;
    if (descender < 0) m.descent = (double)(-descender) / unitsPerEm;
    // sCapHeight only exists from OS/2 version 2 onward; it is the number that
    // actually matters for optically centring a label in a pill or button.
    if (os2 && os2Len >= 90 && os2 + 90 <= d.size() && (int)u16(os2) >= 2) {
        const int cap = i16(os2 + 88);
        if (cap > 0) m.capHeight = (double)cap / unitsPerEm;
    }
    return m;
}

void updateFonts() {
    if (hFontCode) DeleteObject(hFontCode);
    if (hFontPill) DeleteObject(hFontPill);
    if (hFontBold) DeleteObject(hFontBold);
    if (hFontHeavy) DeleteObject(hFontHeavy);
    if (hFontUI) DeleteObject(hFontUI);
    if (hFontUIBold) DeleteObject(hFontUIBold);
    hFontCode = hFontPill = hFontBold = hFontHeavy = hFontUI = hFontUIBold = NULL;

    int dpi = g_dpi;
    HMODULE hUser = GetModuleHandleW(L"user32.dll");
    if (hUser && hMain) {
        typedef UINT (WINAPI *GetDpiForWindowFn)(HWND);
        GetDpiForWindowFn fn = (GetDpiForWindowFn)GetProcAddress(hUser, "GetDpiForWindow");
        if (fn) dpi = (int)fn(hMain);
    }
    if (!hMain) {
        // startup path (no window yet): use real system DPI, not hardcoded 96
        HDC screen = GetDC(NULL);
        if (screen) {
            dpi = GetDeviceCaps(screen, LOGPIXELSX);
            ReleaseDC(NULL, screen);
        }
    }
    if (dpi <= 0) dpi = 96;
    g_dpi = dpi;
    int fontH = -MulDiv(codeFontSize, dpi, 72);
    int pillH = -MulDiv(11, dpi, 72);

    // Chrome (pills, dialogs, buttons) takes Space Grotesk when it is installed.
    // The EDITOR and its gutter must use exactly the family the RichEdit content
    // is set to (fontFace), because g_fontH / g_lineH, the caret height, the
    // gutter digits and the text baselines are all derived from hFontCode's
    // metrics. A different family here is what pushed the line numbers off the
    // code lines: the two fonts have different ascents at the same point size.
    const wchar_t* uiFamily = uiFontFamily();
    const wchar_t* editorFamily = fontFace.c_str();
    hFontCode = CreateFontW(fontH, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_NATURAL_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, editorFamily);
    hFontBold = CreateFontW(fontH, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_NATURAL_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, editorFamily);
    hFontPill = CreateFontW(pillH, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_NATURAL_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, uiFamily);
    hFontHeavy = CreateFontW(pillH, 0, 0, 0, FW_HEAVY, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_NATURAL_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, uiFamily);
    // Stable UI typography: these two are intentionally based on the fixed
    // UI point size, never on codeFontSize.
    hFontUI = CreateFontW(pillH, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_NATURAL_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, uiFamily);
    hFontUIBold = CreateFontW(pillH, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_NATURAL_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, uiFamily);
    if (!hFontCode) hFontCode = CreateFontW(fontH, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_NATURAL_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Consolas");
    if (!hFontBold) hFontBold = CreateFontW(fontH, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_NATURAL_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Consolas Bold");
    if (!hFontPill) hFontPill = CreateFontW(pillH, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_NATURAL_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    if (!hFontHeavy) hFontHeavy = CreateFontW(pillH, 0, 0, 0, FW_EXTRABOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_NATURAL_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Semibold");
    if (!hFontUI) hFontUI = CreateFontW(pillH, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_NATURAL_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    if (!hFontUIBold) hFontUIBold = CreateFontW(pillH, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_NATURAL_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Semibold");

    // Theme-tracked solid brushes — one alloc per theme, reused every frame
    if (hBrBgWin) DeleteObject(hBrBgWin);
    if (hBrBgCard) DeleteObject(hBrBgCard);
    if (hBrBgOut) DeleteObject(hBrBgOut);
    hBrBgWin = CreateSolidBrush(th.bgWindow);
    hBrBgCard = CreateSolidBrush(th.bgCard);
    hBrBgOut = CreateSolidBrush(th.bgOutput);

    // Measure the real cell height so the caret is font-height (not line-height).
    HDC hdc = CreateCompatibleDC(NULL);
    HFONT old = (HFONT)SelectObject(hdc, hFontCode);
    TEXTMETRICW tm{};
    GetTextMetricsW(hdc, &tm);
    SelectObject(hdc, old);
    DeleteDC(hdc);
    g_fontH = tm.tmHeight > 0 ? tm.tmHeight : MulDiv(codeFontSize, dpi, 72) * 4 / 3;
    const int requestedLineH = MulDiv((int)(codeFontSize * 1.5f), dpi, 72);
    // Variable/fallback fonts may report a taller cell at large zoom levels.
    // Keep paragraph spacing at least one full glyph cell plus a small breath.
    g_lineH = std::max(requestedLineH, g_fontH + std::max(2, dpi / 48));
    g_caretW = 2; // fixed 2 physical px — DPI scaling rounded it up to 3 at 120%
    // Caret spans the full ascent, bottom planted on the shared baseline
    // (same baseOff the gutter digits use) — tall like every editor, and it
    // cannot float above the text at any zoom/DPI.
    //
    // Where does RichEdit actually draw the baseline inside its exact line box?
    // Not where GDI would.  RichEdit lays text out with the font's TYPOGRAPHIC
    // cell (the ascender/descender a designer drew, e.g. 1932/-572 units) and
    // bottom-aligns it in the box, so slack from an exact dyLineSpacing piles up
    // ABOVE the glyphs.  GDI's TEXTMETRIC cell is far taller — it uses the font's
    // win ascent/descent (2377/930 units here), i.e. a whole 6px of internal
    // leading at 11pt — so DT_BOTTOM with the GDI cell plants the baseline 6px
    // away from RichEdit's.  Deriving the offset from the typographic descent
    // keeps the digits on the source at every zoom and DPI instead of only at
    // the one size a hand-tuned pixel nudge happened to fit:
    //     baseline = lineTop + lineH - typoDescent
    int typoDesc = 0, otmEm = 0, otmDesc = 0, emPx = 0;
    const int emPxSize = std::abs(-MulDiv(codeFontSize, dpi, 72));
    {
        // Preferred source: the real font tables (see fontTableMetrics).
        if (!g_codeFontFile.empty())
            typoDesc = (int)std::lround(fontTableMetrics(g_codeFontFile).descent * emPxSize);
        // Fallback: GDI's outline metrics — same quantity, just quantised
        // differently, so it is a usable second best when the file is absent.
        if (typoDesc <= 0) {
            OUTLINETEXTMETRICW otm{};
            if (GetOutlineTextMetricsW(hdc, sizeof(otm), &otm) && otm.otmEMSquare > 0) {
                emPx = emPxSize;
                otmEm = (int)otm.otmEMSquare;
                otmDesc = (int)otm.otmDescent;
                typoDesc = MulDiv(std::abs(otmDesc), emPx, otmEm);
            }
        }
        // Reject nonsense (missing table, unusual metric) and fall back to half
        // the GDI descent, which sits between the two engines rather than far
        // off either one.
        if (typoDesc <= 0 || typoDesc >= g_lineH / 2) typoDesc = (int)tm.tmDescent / 2;
    }
    g_baseOff = g_lineH - typoDesc;
    g_glyphH = g_baseOff + (int)tm.tmDescent; // DT_BOTTOM box height for that baseline
    g_caretH = std::max(10, (int)tm.tmAscent + 1);
    // Seat it lower: half a descent below the baseline anchor so the bar's
    // weight sits on the text instead of towering over it. Scales with font.
    g_caretTopPad = std::max(1, g_baseOff - 1 - g_caretH + (int)tm.tmDescent / 2);

    // Cap height for the chrome font: pill and button labels are centred on the
    // cap box, and GDI's ascent-minus-internal-leading is not that box for these
    // fonts (Space Grotesk's win ascent is 1.15em, so the derived "cap height"
    // comes out ~10px instead of the real 0.7em = 12.6px, riding every label a
    // pixel and a half high).
    {
        const int uiEm = std::abs(-MulDiv(11, dpi, 72));
        g_uiEmPx = uiEm;
        double capRatio = g_uiFontFile.empty() ? 0.0 : fontTableMetrics(g_uiFontFile).capHeight;
        if (capRatio > 0.4 && capRatio < 1.0)
            g_uiCapH = (int)std::lround(capRatio * uiEm);
        else {
            HDC hc = CreateCompatibleDC(NULL);
            HFONT prev = (HFONT)SelectObject(hc, hFontHeavy ? hFontHeavy : hFontPill);
            TEXTMETRICW tmUi{};
            GetTextMetricsW(hc, &tmUi);
            SelectObject(hc, prev);
            DeleteDC(hc);
            g_uiCapH = std::max(1, (int)tmUi.tmAscent - (int)tmUi.tmInternalLeading);
        }
    }
}

void setZoom(int newSize) {
    if (newSize < 11) newSize = 11;
    if (newSize > 28) newSize = 28;
    if (codeFontSize == newSize) return;
    codeFontSize = newSize;
    updateFonts();
    updateControlFormatting();
    highlightCode();
    doLayout();
    // Ctrl+wheel can be used while completion is open. Rebuild/reposition the
    // popup immediately so it never keeps the former font's row geometry.
    if (suggestVisible) updateSuggestions();
}

// Update font, exact line height (~1.5x), and theme colors on RichEdit
void updateControlFormatting() {
    // g_fontH / g_lineH come from updateFonts() — same numbers the caret and gutter use.
    if (g_lineH <= 0) g_lineH = MulDiv((int)(codeFontSize * 1.5f), g_dpi, 72);

    CHARFORMAT2W cfCode{};
    cfCode.cbSize = sizeof(cfCode);
    cfCode.dwMask = CFM_FACE | CFM_SIZE | CFM_COLOR | CFM_WEIGHT;
    cfCode.crTextColor = th.text;
    cfCode.wWeight = FW_NORMAL;
    cfCode.yHeight = codeFontSize * 20; // twips (1 pt = 20 twips)
    wcscpy_s(cfCode.szFaceName, fontFace.c_str());

    PARAFORMAT2 pf{};
    pf.cbSize = sizeof(pf);
    pf.dwMask = PFM_LINESPACING;
    pf.bLineSpacingRule = 4; // exact line height in twips
    pf.dyLineSpacing = MulDiv(g_lineH, 1440, std::max(1, g_dpi));

    if (hCode) {
        // EM_SETCHARFORMAT/SETPARAFORMAT set the modify flag then raise EN_CHANGE —
        // without this guard every theme/font churn would dirty the document.
        inApply = true;
        setHlUndo(hCode, true); // full-font reapply must not pollute Ctrl+Z either
        SendMessage(hCode, EM_SETBKGNDCOLOR, 0, th.bgCard);
        SendMessage(hCode, EM_SETCHARFORMAT, SCF_ALL, (LPARAM)&cfCode);
        SendMessage(hCode, EM_SETCHARFORMAT, SCF_DEFAULT, (LPARAM)&cfCode);
        SendMessage(hCode, EM_SETPARAFORMAT, SCF_ALL, (LPARAM)&pf);
        SendMessage(hCode, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(0, 0));
        setHlUndo(hCode, false);
        inApply = false;
    }

    CHARFORMAT2W cfOut = cfCode;
    cfOut.crTextColor = th.text;
    // The console shows diagnostics, logs and previews - prose, not aligned
    // source - so it uses the UI family (Space Grotesk when installed).
    wcscpy_s(cfOut.szFaceName, uiFontFamily());
    if (hOut) {
        setHlUndo(hOut, true);
        SendMessage(hOut, EM_SETBKGNDCOLOR, 0, th.bgOutput);
        SendMessage(hOut, EM_SETCHARFORMAT, SCF_ALL, (LPARAM)&cfOut);
        SendMessage(hOut, EM_SETCHARFORMAT, SCF_DEFAULT, (LPARAM)&cfOut);
        SendMessage(hOut, EM_SETPARAFORMAT, SCF_ALL, (LPARAM)&pf);
        SendMessage(hOut, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(0, 0));
        setHlUndo(hOut, false);
        colorDiagOut(); // SCF_ALL just wiped diagnostic colors — put them back
    }

    if (hCode) InvalidateRect(hCode, NULL, TRUE);
    if (hGutter) InvalidateRect(hGutter, NULL, TRUE);
    if (hOut) InvalidateRect(hOut, NULL, TRUE);
}

// Structural theme bits that must be applied as a discrete step (DWM caption
// mode, dark scrollbars) rather than interpolated.
static void applyThemeStructural() {
    if (!hMain) return;
    COLORREF noBorder = 0xFFFFFFFE; // DWMWA_COLOR_NONE (disables borders on Windows 11)
    DwmSetWindowAttribute(hMain, 34 /* DWMWA_BORDER_COLOR */, &noBorder, sizeof(noBorder));
    COLORREF capColor = th.bgWindow;
    DwmSetWindowAttribute(hMain, 35 /* DWMWA_CAPTION_COLOR */, &capColor, sizeof(capColor));
    int cornerPref = 1; // DWMWCP_DONOTROUND - square window corners on Windows 11
    DwmSetWindowAttribute(hMain, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &cornerPref, sizeof(cornerPref));
    // Dark scrollbars on dark themes (no blue accent bars), classic on light.
    typedef HRESULT (WINAPI *SetThemeFn)(HWND, LPCWSTR, LPCWSTR);
    static SetThemeFn setThemeFn = (SetThemeFn)GetProcAddress(GetModuleHandleW(L"uxtheme.dll"), "SetWindowTheme");
    if (setThemeFn) {
        LPCWSTR mode = darkTheme ? L"DarkMode_Explorer" : L"Explorer";
        if (hCode) setThemeFn(hCode, mode, NULL);
        if (hOut) setThemeFn(hOut, mode, NULL);
        if (hGutter) setThemeFn(hGutter, mode, NULL);
    }
}

// Repaint chrome (brushes, editor background) from the current `th` WITHOUT the
// expensive text re-lex — used every frame of the theme cross-fade.
static void applyThemeChrome() {
    DeleteObject(hPaperBrush);
    hPaperBrush = CreateSolidBrush(th.bgWindow);
    if (hBrBgWin) DeleteObject(hBrBgWin);
    if (hBrBgCard) DeleteObject(hBrBgCard);
    if (hBrBgOut) DeleteObject(hBrBgOut);
    hBrBgWin = CreateSolidBrush(th.bgWindow);
    hBrBgCard = CreateSolidBrush(th.bgCard);
    hBrBgOut = CreateSolidBrush(th.bgOutput);
    g_caretAlpha = -1; // theme colors changed — rebuild caret brush on next tick
    if (hCode) SendMessage(hCode, EM_SETBKGNDCOLOR, 0, th.bgCard);
    if (hOut) SendMessage(hOut, EM_SETBKGNDCOLOR, 0, th.bgOutput);
    for (HWND h : {hCode, hGutter, hOut, hMain, hFiles, hCaret}) {
        if (h) InvalidateRect(h, NULL, TRUE);
    }
}

void setTheme(ThemeMode mode) {
    hideSuggestions();
    ThemeColors target = (mode == THEME_ONE_DARK) ? ONE_DARK
                       : (mode == THEME_ONE_LIGHT) ? ONE_LIGHT : VOID_THEME;
    themeFrom = th;          // start from whatever is on screen right now
    themeTo = target;
    animThemeT = 0.0f;
    themeAnimating = true;
    currentTheme = mode;
    darkTheme = (mode != THEME_ONE_LIGHT);
    applyThemeStructural();  // scrollbar/caption mode switch is discrete
    applyThemeChrome();      // first fade frame
}

void toggleTheme() {
    if (currentTheme == THEME_VOID) setTheme(THEME_ONE_LIGHT);
    else if (currentTheme == THEME_ONE_LIGHT) setTheme(THEME_ONE_DARK);
    else setTheme(THEME_VOID);
}

// Toggle comment on selected lines (Ctrl+/)
void toggleComment() {
    if (cur < 0 || !hCode) return;
    CHARRANGE cr{};
    SendMessage(hCode, EM_EXGETSEL, 0, (LPARAM)&cr);
    // RichEdit can report reverse (bottom-to-top) selections as cpMin > cpMax
    if (cr.cpMin > cr.cpMax) std::swap(cr.cpMin, cr.cpMax);

    // Paragraph bounds — visual rows under wrap, whole hard lines for the op
    long ps, pe, es, ee;
    paraBounds(hCode, cr.cpMin, ps, pe);
    paraBounds(hCode, cr.cpMax > cr.cpMin ? cr.cpMax - 1 : cr.cpMin, es, ee);
    if (ee < ps) ee = ps;
    int selStartChar = (int)ps;
    int selEndChar = (int)ee;

    SendMessage(hCode, EM_SETSEL, selStartChar, selEndChar);
    int selLen = selEndChar - selStartChar;
    if (selLen < 0) selLen = 0;
    std::vector<wchar_t> buf(selLen + 8, 0);
    SendMessage(hCode, EM_GETSELTEXT, 0, (LPARAM)buf.data());
    std::wstring selText(buf.data());

    // Split selection into lines
    std::vector<std::wstring> lines;
    size_t pos = 0;
    while (pos < selText.size()) {
        size_t next = selText.find_first_of(L"\r\n", pos);
        if (next == std::wstring::npos) {
            lines.push_back(selText.substr(pos));
            break;
        }
        lines.push_back(selText.substr(pos, next - pos));
        if (next + 1 < selText.size() && selText[next] == L'\r' && selText[next + 1] == L'\n') next++;
        pos = next + 1;
    }
    if (lines.empty()) return;

    // Check if all non-empty lines currently start with "//"
    bool allCommented = true;
    bool hasNonEmpty = false;
    for (auto& s : lines) {
        size_t nonWs = s.find_first_not_of(L" \t");
        if (nonWs == std::wstring::npos) continue;
        hasNonEmpty = true;
        if (s.substr(nonWs, 2) != L"//") {
            allCommented = false;
            break;
        }
    }
    if (!hasNonEmpty) return;

    std::wstring res;
    for (size_t i = 0; i < lines.size(); i++) {
        std::wstring s = lines[i];
        size_t nonWs = s.find_first_not_of(L" \t");
        if (nonWs != std::wstring::npos) {
            if (allCommented) {
                if (s.substr(nonWs, 3) == L"// ") s.erase(nonWs, 3);
                else if (s.substr(nonWs, 2) == L"//") s.erase(nonWs, 2);
            } else {
                s.insert(nonWs, L"// ");
            }
        }
        res += s;
        if (i + 1 < lines.size()) res += L"\r\n";
    }

    SendMessage(hCode, EM_REPLACESEL, TRUE, (LPARAM)res.c_str());
    // \r\n in res becomes \r in the doc — size must match CR-space
    long resCR = 0;
    for (size_t i = 0; i < res.size(); i++) if (res[i] != L'\n') resCR++;
    long caretEnd = selStartChar + resCR; // collapse: never leave the block selected
    SendMessage(hCode, EM_SETSEL, caretEnd, caretEnd);
}

// Format Code (Ctrl+Shift+F, menu): brace-aware re-indent with a tiny
// string/comment-aware lexer — leading indent becomes depth*4 spaces,
// trailing whitespace trimmed, everything else byte-identical. Line count
// never changes, so the caret is restored by line + clamped column.
static void formatCode() {
    if (!hCode) return;
    int textLen = GetWindowTextLengthW(hCode);
    if (textLen <= 0) return;
    std::vector<wchar_t> buf((size_t)textLen + 1, 0);
    GetWindowTextW(hCode, buf.data(), textLen + 1);
    std::wstring doc(buf.data());

    CHARRANGE cr{};
    SendMessage(hCode, EM_EXGETSEL, 0, (LPARAM)&cr);
    long caretLine = (long)SendMessage(hCode, EM_LINEFROMCHAR, (WPARAM)cr.cpMin, 0);
    long caretCol = cr.cpMin - (long)SendMessage(hCode, EM_LINEINDEX, (WPARAM)caretLine, 0);

    // Split on \r\n (GetWindowText space), keep line count identical.
    std::vector<std::wstring> lines;
    size_t pos = 0;
    while (true) {
        size_t next = doc.find(L"\r\n", pos);
        if (next == std::wstring::npos) { lines.push_back(doc.substr(pos)); break; }
        lines.push_back(doc.substr(pos, next - pos));
        pos = next + 2;
    }

    bool inBlock = false;
    int depth = 0;
    for (auto& ln : lines) {
        // trim trailing whitespace (also kills stray spaces on blank lines)
        while (!ln.empty() && (ln.back() == L' ' || ln.back() == L'\t')) ln.pop_back();
        size_t ns = ln.find_first_not_of(L" \t");
        if (ns == std::wstring::npos) { ln.clear(); continue; } // blank stays blank
        // lex the line for net braces + leading closers (strings/comments/chars skipped)
        int opens = 0, closes = 0, leadClose = 0;
        bool lead = true, inStr = false, inChr = false;
        for (size_t k = 0; k < ln.size(); k++) {
            wchar_t c = ln[k];
            if (inBlock) {
                if (c == L'*' && k + 1 < ln.size() && ln[k + 1] == L'/') { inBlock = false; k++; }
                continue;
            }
            if (inStr) {
                if (c == L'\\') k++;
                else if (c == L'"') inStr = false;
                continue;
            }
            if (inChr) {
                if (c == L'\\') k++;
                else if (c == L'\'') inChr = false;
                continue;
            }
            if (c == L'"') { inStr = true; lead = false; continue; }
            if (c == L'\'') { inChr = true; lead = false; continue; }
            if (c == L'/' && k + 1 < ln.size() && ln[k + 1] == L'/') break; // rest is comment
            if (c == L'/' && k + 1 < ln.size() && ln[k + 1] == L'*') { inBlock = true; k++; continue; }
            if (c == L' ' || c == L'\t') continue;
            if (c == L'{') { opens++; lead = false; continue; }
            if (c == L'}') { closes++; if (lead) leadClose++; continue; }
            lead = false;
        }
        int d = depth - leadClose;
        if (d < 0) d = 0;
        // preprocessor directives stay at column 0
        if (ln[ns] == L'#') d = 0;
        ln = std::wstring((size_t)d * 4, L' ') + ln.substr(ns);
        depth += opens - closes;
        if (depth < 0) depth = 0;
    }

    std::wstring out;
    for (size_t i = 0; i < lines.size(); i++) {
        out += lines[i];
        if (i + 1 < lines.size()) out += L"\r\n";
    }
    SendMessage(hCode, EM_SETSEL, 0, -1);
    SendMessage(hCode, EM_REPLACESEL, TRUE, (LPARAM)out.c_str());
    long nStart = (long)SendMessage(hCode, EM_LINEINDEX, (WPARAM)caretLine, 0);
    long nLen = (long)SendMessage(hCode, EM_LINELENGTH, (WPARAM)nStart, 0);
    long nCaret = nStart + std::min(caretCol, nLen);
    SendMessage(hCode, EM_SETSEL, nCaret, nCaret);
    SendMessage(hCode, EM_SCROLLCARET, 0, 0);
    SetFocus(hCode);
}

// Shortcuts Dialog
void showShortcuts() {
    static const wchar_t* keys[] = {
        L"Ctrl+R", L"Ctrl+S", L"Ctrl+O", L"Ctrl+N",
        L"Ctrl+T", L"Ctrl+K", L"Ctrl+/", L"Ctrl+Z", L"Ctrl+Y",
        L"Ctrl+D", L"Ctrl+Shift+K", L"Ctrl+Left", L"Ctrl+Right",
        L"Ctrl+Home", L"Ctrl+End", L"Tab", L"Shift+Tab",
        L"Ctrl++ / -", L"Ctrl+0",
        L"Ctrl+Shift+O", L"Ctrl+F", L"Ctrl+H", L"F3",
        L"Ctrl+B"
    };
    static const wchar_t* descs[] = {
        L"Run or rerun code",
        L"Save as .cpp file",
        L"Open .cpp file",
        L"New file",
        L"Cycle theme (Void / Light / One Dark)",
        L"Open keyboard shortcuts",
        L"Comment / uncomment line or selection",
        L"Undo",
        L"Redo",
        L"Duplicate line or selection",
        L"Delete line",
        L"Go to start of line",
        L"Go to end of line",
        L"Go to start of document",
        L"Go to end of document",
        L"Insert 4 spaces",
        L"Remove one indent level",
        L"Zoom in / out",
        L"Reset zoom",
        L"Open folder in files panel",
        L"Find in file",
        L"Replace all in file",
        L"Find next",
        L"Check errors (g++ syntax check)"
    };
    uiKeyCount = 24;
    for (int i = 0; i < uiKeyCount; i++) { uiKeys[i] = keys[i]; uiDescs[i] = descs[i]; }
    uiShortcutMode = true;
    uiMessageBox(L"Keyboard Shortcuts", L"", L"OK");
    uiShortcutMode = false;
}

// Right-click Context Menu (themed popup)
void contextMenu(int x, int y) {
    uiShowMenu(x, y);
}

// ============================================================================
// Find / Replace — search the whole doc in RichEdit CR-space, wrap, case-insensitive
// ============================================================================
static std::wstring gFind, gRepl;

static void readCodeCR(std::wstring& s) {
    s.clear();
    long len = outDocLen(hCode);
    if (len <= 0) return;
    s.resize((size_t)len + 1, 0);
    TEXTRANGEW tr{ { 0, len }, s.data() };
    SendMessage(hCode, EM_GETTEXTRANGE, 0, (LPARAM)&tr);
    s.resize((size_t)len);
}

static void ensureParaCache() {
    if (g_paraCacheValid) return;
    std::wstring text;
    readCodeCR(text);
    g_paraStarts.clear();
    g_paraStarts.push_back(0);
    for (long i = 0; i < (long)text.size(); ++i)
        if (text[i] == L'\r' && i + 1 <= (long)text.size()) g_paraStarts.push_back(i + 1);
    g_paraCacheValid = true;
}

static int paraIndexAt(long pos) {
    ensureParaCache();
    if (g_paraStarts.empty()) return 0;
    auto it = std::upper_bound(g_paraStarts.begin(), g_paraStarts.end(), pos);
    return std::max(0, (int)(it - g_paraStarts.begin()) - 1);
}

// RichEdit EM_LINE* messages are VISUAL rows once word wrap is on — ops that
// mean "the whole hard line" must scan for \r instead. CR-space, both bounds.
static void paraBounds(HWND h, long idx, long& ps, long& pe) {
    long n = outDocLen(h);
    if (idx < 0) idx = 0;
    if (idx > n) idx = n;
    // Code's hard-paragraph map is already maintained for the gutter. Reusing
    // it makes Tab, Enter, } and line commands O(log n), even in large files.
    if (h == hCode) {
        ensureParaCache();
        if (!g_paraStarts.empty()) {
            auto it = std::upper_bound(g_paraStarts.begin(), g_paraStarts.end(), idx);
            size_t line = it == g_paraStarts.begin() ? 0 : (size_t)(it - g_paraStarts.begin() - 1);
            ps = g_paraStarts[line];
            pe = line + 1 < g_paraStarts.size() ? g_paraStarts[line + 1] - 1 : n;
            if (pe < ps) pe = ps;
            return;
        }
    }
    // Output/fallback controls have no code cache; obtain only their document.
    std::wstring text((size_t)n + 1, L'\0');
    if (n) {
        TEXTRANGEW tr{ { 0, n }, text.data() };
        SendMessageW(h, EM_GETTEXTRANGE, 0, (LPARAM)&tr);
    }
    ps = idx;
    while (ps > 0 && text[(size_t)ps - 1] != L'\r') --ps;
    pe = idx;
    while (pe < n && text[(size_t)pe] != L'\r') ++pe;
}

// Start char of the 0-based hard-line index (logical line → CR-space char).
static long charOfPara(long paraIdx) {
    ensureParaCache();
    if (paraIdx <= 0 || g_paraStarts.empty()) return 0;
    if (paraIdx >= (long)g_paraStarts.size()) return outDocLen(hCode);
    return g_paraStarts[(size_t)paraIdx];
}

static bool eqNoCase(const wchar_t* a, const wchar_t* b, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (towlower(a[i]) != towlower(b[i])) return false;
    return true;
}

static long findPos(const std::wstring& doc, const std::wstring& nd, long from, bool reverse) {
    long n = (long)doc.size(), m = (long)nd.size();
    if (m == 0 || m > n) return -1;
    long limit = n - m;
    if (!reverse) {
        for (long i = from; i <= limit; i++)
            if (eqNoCase(doc.c_str() + i, nd.c_str(), (size_t)m)) return i;
        for (long i = 0; i < from && i <= limit; i++)
            if (eqNoCase(doc.c_str() + i, nd.c_str(), (size_t)m)) return i;
    } else {
        long start = from - m;
        if (start > limit) start = limit;
        for (long i = start; i >= 0; i--)
            if (eqNoCase(doc.c_str() + i, nd.c_str(), (size_t)m)) return i;
        for (long i = limit; i > start; i--)
            if (eqNoCase(doc.c_str() + i, nd.c_str(), (size_t)m)) return i;
    }
    return -1;
}

static void findGo(const std::wstring& nd, bool reverse) {
    std::wstring doc;
    readCodeCR(doc);
    DWORD a = 0, b = 0;
    SendMessage(hCode, EM_GETSEL, (WPARAM)&a, (LPARAM)&b);
    long from = reverse ? (long)a : (long)b;
    long p = findPos(doc, nd, from, reverse);
    if (p < 0) {
        uiMessageBox(L"Find", (L"Cannot find \"" + nd + L"\"").c_str(), L"OK");
        return;
    }
    SendMessage(hCode, EM_SETSEL, p, p + (long)nd.size());
    SendMessage(hCode, EM_SCROLLCARET, 0, 0);
    SetFocus(hCode);
}

void findNext() {
    if (gFind.empty()) { findUI(); return; }
    findGo(gFind, false);
}

void findUI() {
    std::wstring q;
    if (uiInputBox(L"Find", L"Find what:", &q, gFind.empty() ? NULL : gFind.c_str()) && !q.empty()) {
        gFind = q;
        findGo(gFind, false);
    }
}

void findReplace() {
    std::wstring q;
    if (!uiInputBox(L"Replace All", L"Find what:", &q, gFind.empty() ? NULL : gFind.c_str())) return;
    if (q.empty()) return;
    gFind = q;
    std::wstring r;
    if (!uiInputBox(L"Replace All", L"Replace with:", &r, gRepl.empty() ? NULL : gRepl.c_str())) return;
    gRepl = r;

    std::wstring doc;
    readCodeCR(doc);
    long m = (long)gFind.size();
    if (m == 0 || m > (long)doc.size()) return;
    std::vector<long> hits;
    for (long i = 0; i + m <= (long)doc.size(); ) {
        if (eqNoCase(doc.c_str() + i, gFind.c_str(), (size_t)m)) { hits.push_back(i); i += m; }
        else i++;
    }
    if (hits.empty()) {
        uiMessageBox(L"Replace All", L"Cannot find.", L"OK");
        return;
    }
    SendMessage(hCode, WM_SETREDRAW, FALSE, 0);
    for (int k = (int)hits.size() - 1; k >= 0; k--) { // back-to-front — earlier indices stay valid
        SendMessage(hCode, EM_SETSEL, hits[k], hits[k] + m);
        SendMessage(hCode, EM_REPLACESEL, TRUE, (LPARAM)gRepl.c_str());
    }
    SendMessage(hCode, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(hCode, NULL, TRUE);
    SetTimer(hMain, 101, 180, NULL); // re-highlight after the bulk edit
    // ponytail: replace-all = one undo step per hit; single-shot undo if Ctrl+Z spam ever annoys
}

// ============================================================================
// Autocomplete Suggestions & Snippets System (VS Code Style)
// ============================================================================

struct Snippet {
    std::wstring trigger;
    std::wstring label;
    std::wstring desc;
    std::wstring templateText; // contains '|' for cursor placement
};

const std::vector<Snippet>& getBuiltinSnippets() {
    static const std::vector<Snippet> v = {
        // --- structure / preprocessor ---
        { L"cpp", L"cpp", L"Basic C++ template",
          L"#include <iostream>\nusing namespace std;\n\nint main () {\n  |\n  return 0;\n}" },
        { L"main", L"main", L"main function", L"int main(int argc, char* argv[]) {\n    |\n    return 0;}" },
        { L"inc", L"inc", L"#include <...>", L"#include <|>" },
        { L"incq", L"incq", L"#include \"...\"", L"#include \"|.h\"" },
        { L"def", L"def", L"#define macro", L"#define |" },
        { L"once", L"once", L"#pragma once", L"#pragma once" },
        { L"ifdef", L"ifdef", L"include guard", L"#ifndef FILE_H\n#define FILE_H\n\n#endif // FILE_H" },
        { L"assert", L"assert", L"runtime assert", L"assert(|);" },
        { L"sassert", L"sassert", L"compile-time assert", L"static_assert(|, \"msg\");" },
        { L"uns", L"uns", L"using namespace std;", L"using namespace std;" },
        { L"alias", L"alias", L"type alias", L"using | = int;" },

        // --- i/o ---
        { L"cin", L"cin", L"cin >> |;", L"cin >> |;" },
        { L"cout", L"cout", L"cout << |;", L"cout << |;" },
        { L"cendl", L"cendl", L"cout << | << endl;", L"cout << | << endl;" },
        { L"printf", L"printf", L"printf with newline", L"printf(\"%|\\n\");" },
        { L"scanf", L"scanf", L"scanf %d into var", L"scanf(\"%d\", &|);" },
        { L"getline", L"getline", L"read whole line", L"getline(cin, |);" },
        { L"fin", L"fin", L"ifstream (<fstream>)", L"ifstream in(\"|\");" },
        { L"fout", L"fout", L"ofstream (<fstream>)", L"ofstream out(\"|\");" },
        { L"fastio", L"fastio", L"speed up cin / cout", L"ios::sync_with_stdio(false);\ncin.tie(nullptr);" },

        // --- control flow ---
        { L"if", L"if", L"if statement", L"if (|) {\n    \n}" },
        { L"ifelse", L"ifelse", L"if-else statement", L"if (|) {\n    \n} else {\n    \n}" },
        { L"elif", L"elif", L"if-else if-else", L"if (|) {\n    \n} else if () {\n    \n} else {\n    \n}" },
        { L"for", L"for", L"for loop (i)", L"for (int i = 0; i < |; i++) {\n    \n}" },
        { L"forj", L"forj", L"for loop (j)", L"for (int j = 0; j < |; j++) {\n    \n}" },
        { L"forr", L"forr", L"range-based for", L"for (const auto& | : container) {\n    \n}" },
        { L"while", L"while", L"while loop", L"while (|) {\n    \n}" },
        { L"wtrue", L"wtrue", L"while (true) loop", L"while (true) {\n    |\n}" },
        { L"dowhile", L"dowhile", L"do-while loop", L"do {\n    |\n} while ();" },
        { L"switch", L"switch", L"switch statement", L"switch (|) {\n    case :\n        break;\n    default:\n        break;\n}" },

        // --- functions ---
        { L"func", L"func", L"void function", L"void |() {\n    \n}" },
        { L"lambda", L"lambda", L"capture-by-ref lambda", L"auto f = [&](|) {\n    \n};" },
        { L"ctor", L"ctor", L"constructor + init list", L"|()\n    : member()\n{\n}" },
        { L"dtor", L"dtor", L"destructor", L"~|() {\n    \n}" },

        // --- classes / types ---
        { L"class", L"class", L"class definition", L"class | {\npublic:\n    \nprivate:\n    \n};" },
        { L"struct", L"struct", L"struct definition", L"struct | {\n    \n};" },
        { L"inherit", L"inherit", L"class : public Base", L"class | : public Base {\npublic:\n    \n};" },
        { L"virtual", L"virtual", L"virtual method", L"virtual void |() {\n    \n}" },
        { L"purevirt", L"purevirt", L"pure virtual = 0", L"virtual void |() = 0;" },
        { L"enum", L"enum", L"enum class", L"enum class | {\n    \n};" },
        { L"union", L"union", L"union", L"union | {\n    \n};" },
        { L"namespace", L"namespace", L"namespace block", L"namespace | {\n\n}" },
        { L"tmpl", L"tmpl", L"function template", L"template <typename T>\nvoid |() {\n    \n}" },
        { L"tcls", L"tcls", L"class template", L"template <typename T>\nclass | {\n    \n};" },

        // --- exceptions ---
        { L"try", L"try", L"try-catch block", L"try {\n    |\n} catch (const std::exception& e) {\n    \n}" },
        { L"catch", L"catch", L"catch exception", L"catch (const std::exception& |) {\n    \n}" },
        { L"catchall", L"catchall", L"catch anything", L"catch (...) {\n    \n}" },
        { L"throw", L"throw", L"throw runtime_error", L"throw std::runtime_error(\"|\");" },

        // --- containers ---
        { L"vec", L"vec", L"std::vector", L"vector<|> " },
        { L"vint", L"vint", L"vector<int>", L"vector<int> |;" },
        { L"arr", L"arr", L"std::array", L"array<int, |> a;" },
        { L"map", L"map", L"map", L"map<int, int> |;" },
        { L"umap", L"umap", L"unordered_map", L"unordered_map<int, int> |;" },
        { L"set", L"set", L"set", L"set<int> |;" },
        { L"uset", L"uset", L"unordered_set", L"unordered_set<int> |;" },
        { L"stack", L"stack", L"stack", L"stack<int> |;" },
        { L"queue", L"queue", L"queue", L"queue<int> |;" },
        { L"deque", L"deque", L"deque", L"deque<int> |;" },
        { L"list", L"list", L"list", L"list<int> |;" },
        { L"pair", L"pair", L"pair", L"pair<int, int> |;" },
        { L"str", L"str", L"std::string", L"string |;" },

        // --- algorithms (container named a) ---
        { L"sort", L"sort", L"sort a ascending", L"sort(a.begin(), a.end());" },
        { L"sortd", L"sortd", L"sort a descending", L"sort(a.rbegin(), a.rend());" },
        { L"rev", L"rev", L"reverse a", L"reverse(a.begin(), a.end());" },
        { L"uniq", L"uniq", L"dedupe sorted a", L"a.erase(unique(a.begin(), a.end()), a.end());" },
        { L"lb", L"lb", L"lower_bound index in a", L"lower_bound(a.begin(), a.end(), |) - a.begin();" },
        { L"accum", L"accum", L"sum of a (<numeric>)", L"accumulate(a.begin(), a.end(), 0LL);" },
        { L"maxel", L"maxel", L"max element of a", L"*max_element(a.begin(), a.end());" },
        { L"minel", L"minel", L"min element of a", L"*min_element(a.begin(), a.end());" },
        { L"itr", L"itr", L"iterator to a.begin()", L"auto | = a.begin();" },

        // --- pointers / memory ---
        { L"uptr", L"uptr", L"unique_ptr (<memory>)", L"std::unique_ptr<int> | = std::make_unique<int>();" },
        { L"sptr", L"sptr", L"shared_ptr (<memory>)", L"std::shared_ptr<int> | = std::make_shared<int>();" },

        // --- threads ---
        { L"thread", L"thread", L"std::thread (<thread>)", L"std::thread t([&]() {\n    |\n});\nt.join();" },
        { L"mutex", L"mutex", L"std::mutex (<mutex>)", L"std::mutex |;" },
        { L"lock", L"lock", L"lock_guard on a mutex", L"std::lock_guard<std::mutex> lock(|);" },
    };
    return v;
}

// Common member/type names offered after . / -> / ::
const std::vector<Snippet>& getMemberList() {
    static const std::vector<Snippet> v = [] {
        static const wchar_t* names[] = {
            L"size", L"empty", L"clear", L"resize", L"reserve", L"capacity",
            L"push_back", L"pop_back", L"emplace_back", L"push_front", L"pop_front",
            L"insert", L"erase", L"find", L"count", L"lower_bound", L"upper_bound",
            L"begin", L"end", L"rbegin", L"rend", L"front", L"back", L"at",
            L"data", L"swap", L"sort", L"reverse", L"substr", L"length",
            L"find_first_of", L"to_string", L"stoi", L"cout", L"cin", L"endl",
            L"string", L"vector", L"map", L"set", L"unordered_map", L"pair",
            L"first", L"second", L"make_pair", L"get", L"push", L"pop", L"top",
            L"close", L"open", L"good", L"abs", L"max", L"min", L"nullptr",
            L"this", L"return", L"printf", L"scanf"
        };
        std::vector<Snippet> out;
        for (const wchar_t* n : names) out.push_back({ n, n, L"member", n });
        return out;
    }();
    return v;
}

std::vector<Snippet> getMatchingMembers(const std::wstring& prefix) {
    std::vector<Snippet> res;
    std::wstring lo = prefix;
    for (auto& c : lo) c = towlower(c);
    for (const auto& s : getMemberList()) {
        std::wstring t = s.trigger;
        for (auto& c : t) c = towlower(c);
        if (t.rfind(lo, 0) == 0) res.push_back(s);
    }
    return res;
}

Snippet makeElifSnippet(int count) {
    if (count < 1) count = 1;
    std::wstring trigger = L"elif" + (count > 1 ? std::to_wstring(count) : L"");
    std::wstring desc = L"if with " + std::to_wstring(count) + L" else if";
    std::wstring tpl = L"if (|) {\n    \n}";
    for (int i = 0; i < count; i++) {
        tpl += L" else if () {\n    \n}";
    }
    tpl += L" else {\n    \n}";
    return { trigger, trigger, desc, tpl };
}

std::vector<Snippet> curSnippets;
int suggestSel = 0;
int suggestScroll = 0;
int suggestWordStart = 0;

// The completion popup used to have a fixed 28px row while drawing hFontCode.
// At 18–28 pt zoom that made labels collide vertically and run through the
// description column.  These metrics are rebuilt whenever candidates/fonts
// change, keeping the popup's geometry coupled to the editor's real font cell.
struct SuggestLayout {
    int itemH = 28;
    int maxVisible = 7;
    int pad = 6;
    int footerH = 22;
    int width = 420;
    int badgeL = 14;
    int badgeW = 26;
    int labelL = 42;
    int descL = 134;
};
static SuggestLayout gSuggestLayout;

static int suggestionVisibleRows() {
    return std::min((int)curSnippets.size(), gSuggestLayout.maxVisible);
}

static void rebuildSuggestionLayout() {
    SuggestLayout lay{};
    const int dpi = std::max(96, g_dpi);
    const int px6 = std::max(1, MulDiv(6, dpi, 96));
    const int px8 = std::max(1, MulDiv(8, dpi, 96));
    const int px10 = std::max(1, MulDiv(10, dpi, 96));
    const int px22 = std::max(1, MulDiv(22, dpi, 96));
    const int px26 = std::max(1, MulDiv(26, dpi, 96));
    const int px28 = std::max(1, MulDiv(28, dpi, 96));
    const int px88 = std::max(1, MulDiv(88, dpi, 96));
    const int px230 = std::max(1, MulDiv(230, dpi, 96));
    const int px420 = std::max(1, MulDiv(420, dpi, 96));
    const int px820 = std::max(1, MulDiv(820, dpi, 96));

    // Label rows follow the actual code glyph height, with room for accents
    // and a little air.  The 28px base preserves the compact default UI.
    lay.itemH = std::max(px28, g_fontH + std::max(px8, g_fontH / 4));
    lay.pad = std::max(px6, lay.itemH / 6);
    lay.maxVisible = 7;
    lay.badgeL = lay.pad + std::max(px8, lay.itemH / 4);
    lay.badgeW = px26;
    lay.labelL = lay.badgeL + lay.badgeW + std::max(2, dpi / 96);

    int hintH = 0, maxLabelW = 0;
    HDC dc = GetDC(hMain ? hMain : NULL);
    if (dc) {
        HFONT old = (HFONT)SelectObject(dc, hFontPill ? hFontPill : (HFONT)GetStockObject(DEFAULT_GUI_FONT));
        TEXTMETRICW hintTm{};
        GetTextMetricsW(dc, &hintTm);
        hintH = hintTm.tmHeight;
        SelectObject(dc, hFontCode ? hFontCode : (HFONT)GetStockObject(DEFAULT_GUI_FONT));
        for (const Snippet& snip : curSnippets) {
            SIZE extent{};
            GetTextExtentPoint32W(dc, snip.label.c_str(), (int)snip.label.size(), &extent);
            maxLabelW = std::max<int>(maxLabelW, (int)extent.cx);
        }
        SelectObject(dc, old);
        ReleaseDC(hMain ? hMain : NULL, dc);
    }
    const int labelW = std::max(px88, maxLabelW + std::max(4, dpi / 48));
    lay.descL = lay.labelL + labelW + px10;
    lay.width = std::min(px820, std::max(px420, lay.descL + px230));
    lay.footerH = std::max(px22, hintH + px6);
    gSuggestLayout = lay;
}

// Enter/Tab handled on KEYDOWN still get a WM_CHAR from TranslateMessage — swallow it
bool swallowNextChar = false;

std::vector<Snippet> getMatchingSnippets(const std::wstring& prefix) {
    std::vector<Snippet> res;
    if (prefix.empty()) return res;

    std::wstring lowerPrefix = prefix;
    for (auto& c : lowerPrefix) c = towlower(c);

    // Check if prefix is "elif" with trailing numbers e.g. "elif2", "elif3"
    if (lowerPrefix.rfind(L"elif", 0) == 0) {
        std::wstring digits = lowerPrefix.substr(4);
        bool allDigits = true;
        for (auto c : digits) if (!iswdigit(c)) allDigits = false;
        if (!digits.empty() && digits.size() <= 2 && allDigits) {
            int n = std::stoi(digits);
            if (n >= 1 && n <= 20) {
                res.push_back(makeElifSnippet(n));
            }
        }
    }

    const std::vector<Snippet>& list = getBuiltinSnippets();
    // If prefix is "elif" without number, offer elif2, elif3, elif4, elif5
    std::vector<Snippet> withElif;
    if (lowerPrefix == L"elif") {
        withElif = list;
        for (int k = 2; k <= 5; k++) {
            withElif.push_back(makeElifSnippet(k));
        }
    }
    const std::vector<Snippet>& list2 = lowerPrefix == L"elif" ? withElif : list;

    for (const auto& snip : list2) {
        std::wstring lowTrig = snip.trigger;
        for (auto& c : lowTrig) c = towlower(c);
        if (lowTrig.rfind(lowerPrefix, 0) == 0) {
            bool exists = false;
            for (const auto& r : res) {
                if (r.trigger == snip.trigger) { exists = true; break; }
            }
            if (!exists) res.push_back(snip);
        }
    }
    return res;
}

// Apply the suggestion popup's current fade/scale: global alpha plus a small
// grow anchored at the popup's top-left (its layout is fixed, so shrinking the
// window clips rather than reflowing the rows).
static void applySuggestPop() {
    if (!hSuggest || !IsWindow(hSuggest)) return;
    float p = animSuggestPop;
    if (p < 0.0f) p = 0.0f; else if (p > 1.0f) p = 1.0f;
    SetLayeredWindowAttributes(hSuggest, 0, (BYTE)std::lround(p * 255.0f), LWA_ALPHA);
    if (suggestBaseW > 0 && suggestBaseH > 0) {
        float s = 0.94f + 0.06f * p;
        int w = std::max(1, (int)std::lround(suggestBaseW * s));
        int h = std::max(1, (int)std::lround(suggestBaseH * s));
        SetWindowPos(hSuggest, HWND_TOPMOST, suggestBaseX, suggestBaseY, w, h,
                     SWP_NOACTIVATE | SWP_NOZORDER);
    }
}

void hideSuggestions() {
    if (suggestVisible) {
        suggestVisible = false;
        animSuggestSelInit = false;
        if (hSuggest && IsWindow(hSuggest) && IsWindowVisible(hSuggest))
            suggestFading = true; // tickAnimations fades it out, then hides
        else if (hSuggest)
            ShowWindow(hSuggest, SW_HIDE);
    }
}

void applySelectedSnippet() {
    if (!suggestVisible || curSnippets.empty() || suggestSel < 0 || suggestSel >= (int)curSnippets.size()) {
        hideSuggestions();
        return;
    }

    const Snippet snip = curSnippets[suggestSel];
    std::wstring tpl = snip.templateText;

    // RichEdit stores one \r per line break (GetWindowText reports \r\n).
    // Keep indices in that space so EM_SETSEL targets stay correct mid-document.
    std::wstring normTpl;
    normTpl.reserve(tpl.size() + 32);
    for (size_t k = 0; k < tpl.size(); k++) {
        if (tpl[k] == L'\n' && (k == 0 || tpl[k - 1] != L'\r')) {
            normTpl += L'\r';
        } else {
            normTpl += tpl[k];
        }
    }

    // First '|' only sets the caret position — every later '|' is stripped too
    // so a template never leaves literal '|' characters in the buffer
    // (dowhile's `while (|)` used to).
    size_t barPos = normTpl.find(L'|');
    std::wstring toInsert;
    toInsert.reserve(normTpl.size());
    for (wchar_t ch : normTpl) {
        if (ch != L'|') toInsert += ch;
    }

    // Replace typed prefix only (normalize reverse range)
    CHARRANGE cr;
    SendMessage(hCode, EM_EXGETSEL, 0, (LPARAM)&cr);
    long a = cr.cpMin, b = cr.cpMax;
    if (a > b) std::swap(a, b);
    if (b < suggestWordStart) b = suggestWordStart;
    SendMessage(hCode, EM_SETSEL, suggestWordStart, b);
    SendMessage(hCode, EM_REPLACESEL, TRUE, (LPARAM)toInsert.c_str());

    long targetCaret = suggestWordStart +
        (barPos != std::wstring::npos ? (long)barPos : (long)toInsert.size());
    SendMessage(hCode, EM_SETSEL, targetCaret, targetCaret);
    SendMessage(hCode, EM_SCROLLCARET, 0, 0);

    hideSuggestions();

    SetFocus(hCode);

    highlightCode();
    // Re-assert after highlight in case a nested change moved the selection
    SendMessage(hCode, EM_SETSEL, targetCaret, targetCaret);
}

void updateSuggestions() {
    if (!hCode || !hSuggest) return;
    if (curLang() != LANG_CPP) { hideSuggestions(); return; } // snippets are C++-only

    CHARRANGE cr;
    SendMessage(hCode, EM_EXGETSEL, 0, (LPARAM)&cr);
    if (cr.cpMin != cr.cpMax) {
        hideSuggestions();
        return;
    }

    long caretPos = (long)cr.cpMin;
    if (caretPos <= 0) {
        hideSuggestions();
        return;
    }

    // Read in RichEdit index space (EM_GETTEXTRANGE) — GetWindowText uses \r\n
    // and diverges from EM_SETSEL once the doc has line breaks (snippets in main).
    long lineIdx = (long)SendMessage(hCode, EM_LINEFROMCHAR, (WPARAM)caretPos, 0);
    long lineStart = (long)SendMessage(hCode, EM_LINEINDEX, (WPARAM)lineIdx, 0);
    if (lineStart < 0 || caretPos <= lineStart) {
        hideSuggestions();
        return;
    }

    int span = (int)(caretPos - lineStart);
    std::vector<wchar_t> text((size_t)span + 1, 0);
    TEXTRANGEW trg{};
    trg.chrg.cpMin = lineStart;
    trg.chrg.cpMax = caretPos;
    trg.lpstrText = text.data();
    SendMessage(hCode, EM_GETTEXTRANGE, 0, (LPARAM)&trg);
    int len = span;
    if (len <= 0) {
        hideSuggestions();
        return;
    }

    // Find start of current word (relative to lineStart)
    int start = len;
    while (start > 0 && (iswalnum(text[start - 1]) || text[start - 1] == L'_')) {
        start--;
    }

    // Member-access context: obj.pu|  ptr->siz|  ns::vec| → member list, not snippets
    bool memberCtx = false;
    if (start > 0) {
        wchar_t p1 = text[start - 1];
        if (p1 == L'.' && !(start >= 2 && iswdigit(text[start - 2]))) memberCtx = true;
        else if (p1 == L':' && start >= 2 && text[start - 2] == L':') memberCtx = true;
        else if (p1 == L'>' && start >= 2 && text[start - 2] == L'-') memberCtx = true;
    }

    if (start >= len && !memberCtx) {
        hideSuggestions();
        return;
    }

    std::wstring word(&text[start], len - start);
    curSnippets = memberCtx ? getMatchingMembers(word) : getMatchingSnippets(word);
    if (curSnippets.empty()) {
        hideSuggestions();
        return;
    }

    rebuildSuggestionLayout();
    suggestWordStart = (int)(lineStart + start);
    // Keep keyboard selection across keystrokes when the same trigger still matches
    int keepSel = 0;
    if (suggestVisible && suggestSel >= 0 && suggestSel < (int)curSnippets.size()) {
        std::wstring prev = curSnippets[suggestSel].trigger;
        for (int i = 0; i < (int)curSnippets.size(); i++) {
            if (curSnippets[i].trigger == prev) { keepSel = i; break; }
        }
    }
    suggestSel = keepSel;
    if (suggestSel >= (int)curSnippets.size()) suggestSel = 0;
    suggestScroll = 0;
    if (suggestSel >= gSuggestLayout.maxVisible)
        suggestScroll = suggestSel - gSuggestLayout.maxVisible + 1;
    animSuggestSelInit = false;

    // Get caret coordinate in hCode client space
    POINTL ptChar = {0, 0};
    SendMessage(hCode, EM_POSFROMCHAR, (WPARAM)&ptChar, caretPos);

    POINT ptScreen = { (LONG)ptChar.x, (LONG)ptChar.y };
    ClientToScreen(hCode, &ptScreen);

    // The popup sits under the editor's actual paragraph grid, not an old
    // font-height estimate, so it cannot cover the enlarged current line.
    const int lineH = std::max(1, g_lineH);
    const int vis = suggestionVisibleRows();
    int popH = gSuggestLayout.pad + vis * gSuggestLayout.itemH + gSuggestLayout.footerH + 2;
    int popW = gSuggestLayout.width;

    int popX = ptScreen.x;
    int popY = ptScreen.y + lineH;

    HMONITOR hMon = MonitorFromPoint(ptScreen, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(hMon, &mi);
    const int workW = std::max<int>(1, (int)(mi.rcWork.right - mi.rcWork.left));
    const int workH = std::max<int>(1, (int)(mi.rcWork.bottom - mi.rcWork.top));
    popW = std::min(popW, workW);
    popH = std::min(popH, workH);

    if (popX + popW > mi.rcWork.right) popX = mi.rcWork.right - popW;
    if (popX < mi.rcWork.left) popX = mi.rcWork.left;
    if (popY + popH > mi.rcWork.bottom) popY = ptScreen.y - popH - 2;
    if (popY < mi.rcWork.top) popY = mi.rcWork.top;

    suggestBaseX = popX; suggestBaseY = popY; suggestBaseW = popW; suggestBaseH = popH;
    bool wasHidden = !IsWindowVisible(hSuggest);
    SetWindowPos(hSuggest, HWND_TOPMOST, popX, popY, popW, popH, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    if (wasHidden) animSuggestPop = 0.0f; // fresh pop-in (repositioning mid-typing keeps it open)
    suggestVisible = true;
    suggestFading = false;
    applySuggestPop();
    InvalidateRect(hSuggest, NULL, FALSE);
}

// Suggestion Window Proc
LRESULT CALLBACK SuggestProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_ERASEBKGND) return 1;
    if (m == WM_SETCURSOR) {
        POINT pt; GetCursorPos(&pt); ScreenToClient(h, &pt);
        const int rows = suggestionVisibleRows();
        int idx = suggestScroll + (pt.y - gSuggestLayout.pad) / gSuggestLayout.itemH;
        bool item = pt.y >= gSuggestLayout.pad
            && pt.y < gSuggestLayout.pad + rows * gSuggestLayout.itemH
            && idx >= 0 && idx < (int)curSnippets.size();
        return uiCursor(item ? IDC_HAND : IDC_ARROW);
    }
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT r;
        GetClientRect(h, &r);
        int winW = r.right - r.left;
        int winH = r.bottom - r.top;

        HDC memDC = CreateCompatibleDC(dc);
        HBITMAP memBmp = CreateCompatibleBitmap(dc, winW, winH);
        HGDIOBJ oldBmp = SelectObject(memDC, memBmp);

        {
            Graphics g(memDC);
            g.SetSmoothingMode(SmoothingModeAntiAlias);
            g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

            COLORREF bgRef = darkTheme ? th.bgCard : RGB(255, 255, 255);
            COLORREF borderRef = th.border;
            COLORREF selBg = darkTheme ? th.runBtnBg : RGB(232, 234, 239);
            COLORREF accent = th.function;
            Color bgCol(255, GetRValue(bgRef), GetGValue(bgRef), GetBValue(bgRef));
            Color borderCol(255, GetRValue(borderRef), GetGValue(borderRef), GetBValue(borderRef));
            Color selBgCol(255, GetRValue(selBg), GetGValue(selBg), GetBValue(selBg));
            Color accentCol(255, GetRValue(accent), GetGValue(accent), GetBValue(accent));
            COLORREF selTextCol = darkTheme ? RGB(255, 255, 255) : RGB(24, 26, 31);
            COLORREF selDescCol = darkTheme ? RGB(171, 178, 191) : RGB(100, 105, 115);

            // Inset by 1 so the 2px border sits flush with the window's square corner
            SolidBrush bgBrush(bgCol);
            g.FillRectangle(&bgBrush, 1.0f, 1.0f, (float)winW - 3.0f, (float)winH - 3.0f);
            Pen borderPen(borderCol, 1.0f);
            g.DrawRectangle(&borderPen, 1.0f, 1.0f, (float)winW - 3.0f, (float)winH - 3.0f);

            int total = (int)curSnippets.size();
            int visibleCount = suggestionVisibleRows();
            int listH = visibleCount * gSuggestLayout.itemH;

            // Selection geometry is advanced by tickAnimations() on the
            // display-paced QPC clock. WM_PAINT only renders a snapshot.
            int selRow = suggestSel - suggestScroll;
            if (selRow >= 0 && selRow < visibleCount) {
                const float targetY = (float)(gSuggestLayout.pad + selRow * gSuggestLayout.itemH);
                const float drawY = animSuggestSelInit ? animSuggestSelY : targetY;
                SolidBrush selBrush(selBgCol);
                g.FillRectangle(&selBrush, 5.0f, drawY + 1.0f,
                                (float)(winW - 10), (float)(gSuggestLayout.itemH - 2));
                // Accent bar on the left of the selected row
                SolidBrush accBrush(accentCol);
                g.FillRectangle(&accBrush, 5.0f, drawY + 5.0f,
                                3.0f, (float)std::max(4, gSuggestLayout.itemH - 12));
            }

            for (int i = 0; i < visibleCount; i++) {
                int itemIdx = suggestScroll + i;
                if (itemIdx >= total) break;

                int itemY = gSuggestLayout.pad + i * gSuggestLayout.itemH;
                bool isSel = (itemIdx == suggestSel);

                // Snippet badge
                SelectObject(memDC, hFontPill);
                SetBkMode(memDC, TRANSPARENT);
                SetTextColor(memDC, isSel ? accent : th.type);
                RECT rcBadge = { gSuggestLayout.badgeL, itemY,
                                 gSuggestLayout.badgeL + gSuggestLayout.badgeW,
                                 itemY + gSuggestLayout.itemH };
                DrawTextW(memDC, L"⌘", -1, &rcBadge, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

                // Trigger label (slightly brighter when selected)
                SelectObject(memDC, hFontCode);
                SetTextColor(memDC, isSel ? selTextCol : th.text);
                RECT rcLabel = { gSuggestLayout.labelL, itemY,
                                 gSuggestLayout.descL - std::max(4, g_dpi / 48),
                                 itemY + gSuggestLayout.itemH };
                DrawTextW(memDC, curSnippets[itemIdx].label.c_str(), -1, &rcLabel,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

                // Description / preview
                SetTextColor(memDC, isSel ? selDescCol : th.gutterText);
                SelectObject(memDC, hFontPill);
                RECT rcDesc = { gSuggestLayout.descL, itemY, winW - gSuggestLayout.pad - 8,
                                itemY + gSuggestLayout.itemH };
                DrawTextW(memDC, curSnippets[itemIdx].desc.c_str(), -1, &rcDesc,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            }

            // Footer hint
            RECT rcFoot = { gSuggestLayout.badgeL, gSuggestLayout.pad + listH,
                            winW - gSuggestLayout.pad - 8, winH - 4 };
            SelectObject(memDC, hFontPill);
            SetTextColor(memDC, th.gutterText);
            DrawTextW(memDC, L"↑↓ navigate    Tab / Enter insert    Esc dismiss", -1,
                      &rcFoot, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

            // Slim scrollbar
            if (total > visibleCount) {
                float trackH = (float)listH;
                float thumbH = trackH * ((float)visibleCount / (float)total);
                float thumbY = (float)gSuggestLayout.pad + (trackH - thumbH) *
                    ((float)suggestScroll / (float)(total - visibleCount));
                SolidBrush thumbBrush(Color(200, GetRValue(th.gutterText), GetGValue(th.gutterText), GetBValue(th.gutterText)));
                GraphicsPath thumbPath;
                addRect(thumbPath, (float)winW - 6.0f, thumbY + 2.0f, 3.0f, thumbH - 4.0f);
                g.FillPath(&thumbBrush, &thumbPath);
            }
        }

        BitBlt(dc, 0, 0, winW, winH, memDC, 0, 0, SRCCOPY);
        SelectObject(memDC, oldBmp);
        DeleteObject(memBmp);
        DeleteDC(memDC);

        EndPaint(h, &ps);
        return 0;
    }
    else if (m == WM_MOUSEMOVE) {
        int y = GET_Y_LPARAM(l);
        const int rows = suggestionVisibleRows();
        if (y < gSuggestLayout.pad || y >= gSuggestLayout.pad + rows * gSuggestLayout.itemH)
            return 0; // footer / border: never select a hidden row
        int idx = suggestScroll + (y - gSuggestLayout.pad) / gSuggestLayout.itemH;
        if (idx >= 0 && idx < (int)curSnippets.size() && idx != suggestSel) {
            suggestSel = idx;
            InvalidateRect(h, NULL, FALSE);
        }
        return 0;
    }
    else if (m == WM_LBUTTONDOWN) {
        int y = GET_Y_LPARAM(l);
        const int rows = suggestionVisibleRows();
        if (y < gSuggestLayout.pad || y >= gSuggestLayout.pad + rows * gSuggestLayout.itemH)
            return 0; // footer / border — ignore
        int idx = suggestScroll + (y - gSuggestLayout.pad) / gSuggestLayout.itemH;
        if (idx >= 0 && idx < (int)curSnippets.size()) {
            suggestSel = idx;
            applySelectedSnippet();
        }
        return 0;
    }
    return DefWindowProc(h, m, w, l);
}

// ============================================================================
// Indentation — all editor paths use one four-space logical indent.  Every
// operation works in RichEdit CR-space / hard paragraphs, never visual wraps.
// ============================================================================
static const int INDENT_WIDTH = 4;
static const wchar_t INDENT_TEXT[] = L"    ";

static std::wstring richTextRange(HWND h, long start, long end) {
    if (end <= start) return L"";
    std::wstring text((size_t)(end - start) + 1, L'\0');
    TEXTRANGEW tr{ { start, end }, text.data() };
    SendMessageW(h, EM_GETTEXTRANGE, 0, (LPARAM)&tr);
    text.resize((size_t)(end - start));
    return text;
}

static long removeLeadingIndent(std::wstring& line) {
    if (line.empty()) return 0;
    if (line[0] == L'\t') { line.erase(0, 1); return 1; }
    long count = 0;
    while (count < (long)line.size() && count < INDENT_WIDTH && line[count] == L' ') ++count;
    if (count) line.erase(0, (size_t)count);
    return count;
}

// Tab/Shift+Tab: a collapsed Tab inserts spaces; an active selection is shifted
// as complete hard lines and committed as one RichEdit undo operation.
static void changeIndentation(HWND h, bool outdent) {
    CHARRANGE cr{};
    SendMessageW(h, EM_EXGETSEL, 0, (LPARAM)&cr);
    long a = cr.cpMin, b = cr.cpMax;
    if (a > b) std::swap(a, b);

    if (a == b) {
        if (!outdent) {
            SendMessageW(h, EM_REPLACESEL, TRUE, (LPARAM)INDENT_TEXT);
            return;
        }
        long lineStart = 0, lineEnd = 0;
        paraBounds(h, a, lineStart, lineEnd);
        std::wstring line = richTextRange(h, lineStart, lineEnd);
        long removed = removeLeadingIndent(line);
        if (!removed) return;
        SendMessageW(h, EM_SETSEL, lineStart, lineStart + removed);
        SendMessageW(h, EM_REPLACESEL, TRUE, (LPARAM)L"");
        long newCaret = a >= lineStart + removed ? a - removed : lineStart;
        SendMessageW(h, EM_SETSEL, newCaret, newCaret);
        return;
    }

    long firstStart = 0, firstEnd = 0, lastStart = 0, lastEnd = 0;
    paraBounds(h, a, firstStart, firstEnd);
    // An end exactly after a CR belongs to the preceding selected line.
    paraBounds(h, b - 1, lastStart, lastEnd);
    std::wstring selected = richTextRange(h, firstStart, lastEnd);
    std::wstring changed;
    changed.reserve(selected.size() + INDENT_WIDTH * 4);
    for (size_t pos = 0; pos <= selected.size();) {
        size_t br = selected.find(L'\r', pos);
        bool hasBreak = br != std::wstring::npos;
        size_t end = hasBreak ? br : selected.size();
        std::wstring line = selected.substr(pos, end - pos);
        if (outdent) removeLeadingIndent(line);
        else line.insert(0, INDENT_TEXT);
        changed += line;
        if (hasBreak) {
            changed += L'\r';
            pos = br + 1;
        } else {
            break;
        }
    }
    SendMessageW(h, EM_SETSEL, firstStart, lastEnd);
    SendMessageW(h, EM_REPLACESEL, TRUE, (LPARAM)changed.c_str());
    // Selecting the transformed block makes repeated Tab / Shift+Tab natural.
    SendMessageW(h, EM_SETSEL, firstStart, firstStart + (long)changed.size());
}

// VS Code-style Enter: inherit indentation, add one level after an opening
// brace, and expand a nearby paired closing brace onto its own aligned line.
static void smartNewline(HWND h) {
    CHARRANGE cr{};
    SendMessageW(h, EM_EXGETSEL, 0, (LPARAM)&cr);
    long a = cr.cpMin, b = cr.cpMax;
    if (a > b) std::swap(a, b);

    long lineStart = 0, lineEnd = 0;
    paraBounds(h, a, lineStart, lineEnd);
    std::wstring line = richTextRange(h, lineStart, lineEnd);
    long toCaret = std::max(0L, std::min(a - lineStart, (long)line.size()));

    std::wstring indent;
    for (wchar_t c : line) {
        if (c == L' ') indent += L' ';
        else if (c == L'\t') indent += INDENT_TEXT; // normalize legacy tabs to one 4-space level
        else break;
    }

    bool selection = a != b;
    std::wstring before = line.substr(0, (size_t)toCaret);
    while (!before.empty() && (before.back() == L' ' || before.back() == L'\t')) before.pop_back();
    bool afterOpen = !selection && !before.empty() && before.back() == L'{';
    bool closeOnRight = false;
    if (afterOpen) {
        std::wstring after = line.substr((size_t)toCaret);
        size_t n = after.find_first_not_of(L" \t");
        closeOnRight = n != std::wstring::npos && after[n] == L'}';
    }

    std::wstring insert = L"\r" + indent;
    long targetCaret = a + 1 + (long)indent.size();
    if (afterOpen) {
        insert += INDENT_TEXT;
        targetCaret += INDENT_WIDTH;
        if (closeOnRight) insert += L"\r" + indent;
    }
    SendMessageW(h, EM_SETSEL, a, b);
    SendMessageW(h, EM_REPLACESEL, TRUE, (LPARAM)insert.c_str());
    SendMessageW(h, EM_SETSEL, targetCaret, targetCaret);
}

// Typing '}' on a whitespace-only line pulls back one complete indent level.
// The native handler then inserts '}' at the new (dedented) column.
static void outdentForClose(HWND h) {
    CHARRANGE cr{};
    SendMessageW(h, EM_EXGETSEL, 0, (LPARAM)&cr);
    if (cr.cpMin != cr.cpMax || cr.cpMin <= 0) return;
    long caret = cr.cpMin, lineStart = 0, lineEnd = 0;
    paraBounds(h, caret, lineStart, lineEnd);
    std::wstring prefix = richTextRange(h, lineStart, caret);
    if (prefix.empty()) return;
    for (wchar_t c : prefix)
        if (c != L' ' && c != L'\t') return;

    long remove = 0;
    if (prefix.back() == L'\t') remove = 1;
    else {
        while (remove < (long)prefix.size() && remove < INDENT_WIDTH &&
               prefix[prefix.size() - 1 - (size_t)remove] == L' ') ++remove;
    }
    if (!remove) return;
    long newCaret = caret - remove;
    SendMessageW(h, EM_SETSEL, caret - remove, caret);
    SendMessageW(h, EM_REPLACESEL, TRUE, (LPARAM)L"");
    // EM_REPLACESEL parks the caret after the deleted run (cpMin collapses),
    // so restore it to the dedented column or '}' lands one level too deep.
    SendMessageW(h, EM_SETSEL, newCaret, newCaret);
}


// Ctrl+Left/Right → start/end of line (VS Code-style).
// With extend (Ctrl+Shift+Left/Right) the anchor stays and the active end
// moves — standard shift-selection.
static void moveLineEdge(HWND h, bool toEnd, bool extend = false) {
    CHARRANGE cr;
    SendMessage(h, EM_EXGETSEL, 0, (LPARAM)&cr);
    long pos = cr.cpMin;
    long lineIdx = (long)SendMessage(h, EM_LINEFROMCHAR, (WPARAM)pos, 0);
    long lineStart = (long)SendMessage(h, EM_LINEINDEX, (WPARAM)lineIdx, 0);
    long lineLen = (long)SendMessage(h, EM_LINELENGTH, (WPARAM)lineStart, 0);
    long target = toEnd ? (lineStart + lineLen) : lineStart;
    if (extend) {
        // RichEdit always collapses an arrow to cpMin (verified: (0,6)+Left→0,
        // (6,2)+Left→2), so the caret must always be the SMALLER end:
        // target=lineStart → order (target, anchor); lineEnd → (anchor, target).
        long anchor = (cr.cpMin <= cr.cpMax) ? (toEnd ? cr.cpMin : cr.cpMax)
                                             : (toEnd ? cr.cpMax : cr.cpMin);
        if (toEnd) SendMessage(h, EM_SETSEL, anchor, target);
        else SendMessage(h, EM_SETSEL, target, anchor);
    } else {
        SendMessage(h, EM_SETSEL, target, target);
    }
    SendMessage(h, EM_SCROLLCARET, 0, 0);
}

// Ctrl+D — duplicate line or selection
static void duplicateLine() {
    CHARRANGE cr;
    SendMessage(hCode, EM_EXGETSEL, 0, (LPARAM)&cr);
    if (cr.cpMin > cr.cpMax) std::swap(cr.cpMin, cr.cpMax);
    if (cr.cpMin == cr.cpMax) {
        long ps, pe;
        paraBounds(hCode, cr.cpMin, ps, pe);
        long lineStart = ps;
        long lineLen = pe - ps;
        long lineEnd = pe;
        // Insert "\r"+line at end of content (before trailing \r) — never after it,
        // or mid-file duplicates get a blank line. WM_GETTEXTLENGTH is CRLF-space
        // and must not be mixed with EM_LINE* / EM_SETSEL (CR-space).
        std::vector<wchar_t> buf((size_t)lineLen + 2, 0);
        TEXTRANGEW tr{ { lineStart, lineEnd }, buf.data() };
        SendMessage(hCode, EM_GETTEXTRANGE, 0, (LPARAM)&tr);
        std::wstring line(buf.data(), (size_t)lineLen);
        std::wstring ins = L"\r" + line;
        SendMessage(hCode, EM_SETSEL, lineEnd, lineEnd);
        SendMessage(hCode, EM_REPLACESEL, TRUE, (LPARAM)ins.c_str());
        long newCaret = lineEnd + 1 + (long)line.size();
        SendMessage(hCode, EM_SETSEL, newCaret, newCaret);
    } else {
        std::vector<wchar_t> buf((size_t)(cr.cpMax - cr.cpMin) + 1, 0);
        SendMessage(hCode, EM_GETSELTEXT, 0, (LPARAM)buf.data());
        std::wstring sel(buf.data());
        SendMessage(hCode, EM_SETSEL, cr.cpMax, cr.cpMax);
        SendMessage(hCode, EM_REPLACESEL, TRUE, (LPARAM)sel.c_str());
        SendMessage(hCode, EM_SETSEL, cr.cpMax, cr.cpMax + (long)sel.size());
    }
}

// Ctrl+Shift+K — delete current line (or selection lines)
static void deleteLine() {
    CHARRANGE cr;
    SendMessage(hCode, EM_EXGETSEL, 0, (LPARAM)&cr);
    if (cr.cpMin > cr.cpMax) std::swap(cr.cpMin, cr.cpMax);
    long ps, pe, es, ee;
    paraBounds(hCode, cr.cpMin, ps, pe);
    paraBounds(hCode, cr.cpMax > cr.cpMin ? cr.cpMax - 1 : cr.cpMax, es, ee);
    long lineStart = ps;
    long delEnd = ee;
    long docLen = outDocLen(hCode); // CR-space — WM_GETTEXTLENGTH (CRLF) breaks last-line check
    if (delEnd < docLen) delEnd++; // include newline
    if (lineStart > 0 && delEnd >= docLen) {
        // last line — remove the preceding newline instead
        lineStart--;
    }
    SendMessage(hCode, EM_SETSEL, lineStart, delEnd);
    SendMessage(hCode, EM_REPLACESEL, TRUE, (LPARAM)L"");
}

// Code Editor Subclass
// ============================================================================
// RichEdit owns ordinary wheel and scrollbar scrolling.  This deliberately
// avoids a timer-only custom wheel path: it had swallowed WM_MOUSEWHEEL without
// reliably committing a scroll on several RichEdit versions.
static bool g_qpcInit = false;
static LARGE_INTEGER g_qpcFreq{};
static double nowSec() {
    if (!g_qpcInit) {
        QueryPerformanceFrequency(&g_qpcFreq);
        g_qpcInit = true;
    }
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)g_qpcFreq.QuadPart;
}

// The timer is only a wake-up source.  Visual work is frame-paced to the
// monitor's reported refresh rate, preventing 200+ Hz RichEdit/GDI work on a
// 60 Hz display while still allowing 120/144/165/240 Hz panels to feel native.
static double g_displayFrameSec = 1.0 / 60.0;
static double g_nextVisualFrame = 0.0;
static HMONITOR g_pacingMonitor = NULL;

static void refreshVisualPacing(HWND hwnd) {
    HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    if (monitor && monitor == g_pacingMonitor && g_nextVisualFrame > 0.0) return;
    g_pacingMonitor = monitor;
    int hz = 0;
    HDC dc = GetDC(hwnd);
    if (dc) {
        hz = GetDeviceCaps(dc, VREFRESH);
        ReleaseDC(hwnd, dc);
    }
    // Drivers sometimes report 0 or 1 for a virtual monitor.  Use a safe
    // desktop baseline in that case, and reject implausible values.
    if (hz < 30 || hz > 360) hz = 60;
    g_displayFrameSec = 1.0 / (double)hz;
    g_nextVisualFrame = 0.0; // apply the new monitor rate immediately
}

static bool takeVisualFrame(double now) {
    const double step = std::max(1.0 / 360.0, std::min(g_displayFrameSec, 1.0 / 30.0));
    if (g_nextVisualFrame <= 0.0) {
        g_nextVisualFrame = now + step;
        return true;
    }
    // Tiny tolerance avoids a needless extra wait from QPC/timer rounding.
    if (now + 0.00020 < g_nextVisualFrame) return false;
    if (now - g_nextVisualFrame > step * 4.0) {
        // A modal dialog, sleep/resume, or debugger pause occurred: restart
        // the cadence rather than replaying a backlog of frames.
        g_nextVisualFrame = now + step;
    } else {
        do { g_nextVisualFrame += step; } while (g_nextVisualFrame <= now);
    }
    return true;
}


// Text-only paste shared by editor + output: RichEdit would embed
// images/OLE objects — take CF_UNICODETEXT (else CF_TEXT) via
// EM_REPLACESEL, swallow the rest. Returns true when handled.
static bool pastePlainText(HWND h, bool callOrig, WNDPROC orig) {
    if (!OpenClipboard(h)) return false; // locked — caller falls through to native
    std::wstring pasted;
    HANDLE clip = GetClipboardData(CF_UNICODETEXT);
    if (clip) {
        if (const wchar_t* t = (const wchar_t*)GlobalLock(clip)) {
            pasted = t;
            GlobalUnlock(clip);
        }
    } else if ((clip = GetClipboardData(CF_TEXT))) {
        if (const char* t = (const char*)GlobalLock(clip)) {
            int n = MultiByteToWideChar(CP_ACP, 0, t, -1, NULL, 0);
            if (n > 1) {
                pasted.resize((size_t)n - 1);
                MultiByteToWideChar(CP_ACP, 0, t, -1, pasted.data(), n);
            }
            GlobalUnlock(clip);
        }
    }
    CloseClipboard();
    if (pasted.empty()) return true; // image-only clipboard: swallow
    if (callOrig) CallWindowProc(orig, h, EM_REPLACESEL, TRUE, (LPARAM)pasted.c_str());
    else SendMessage(h, EM_REPLACESEL, TRUE, (LPARAM)pasted.c_str());
    return true;
}

LRESULT CALLBACK CodeProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_GETDLGCODE) return DLGC_WANTALLKEYS;
    // Text-only paste shared by editor + output: RichEdit would embed
    // images/OLE objects — take CF_UNICODETEXT (else CF_TEXT) via
    // EM_REPLACESEL, swallow the rest. Returns true when handled.
    // Keyboard paste (Ctrl+V) enters via WM_CHAR, never as WM_PASTE,
    // so both entry points route here.
    if (m == WM_PASTE) {
        if (pastePlainText(h, false, NULL)) return 0;
        // clipboard locked — fall through to native rather than lose the paste
    }
    // Swallow the WM_CHAR that TranslateMessage posts after we handled Enter/Tab on KEYDOWN
    if (m == WM_CHAR) {
        if (swallowNextChar) {
            swallowNextChar = false;
            return 0;
        }

        wchar_t ch = (wchar_t)w;
        // Control chars (Ctrl+A=1, etc.) must not fall through as inserts;
        // \r/\t still pass for the non-swallowed fallback path
        if (w < 32 && w != L'\r' && w != L'\t') return 0;
        if (ch != L'\r' && ch != L'\t') {
            CHARRANGE cr{};
            SendMessage(h, EM_EXGETSEL, 0, (LPARAM)&cr);
            if (cr.cpMin > cr.cpMax) std::swap(cr.cpMin, cr.cpMax);
            bool collapsed = (cr.cpMin == cr.cpMax);

            auto charAt = [&](long idx) -> wchar_t {
                wchar_t c[2] = { 0, 0 }; // EM_GETTEXTRANGE writes the char + a NUL
                TEXTRANGEW tr{ { idx, idx + 1 }, c };
                SendMessage(h, EM_GETTEXTRANGE, 0, (LPARAM)&tr);
                return c[0];
            };

            // Type-over: closer already to the right — caret hops past it
            if (collapsed && (ch == L')' || ch == L']' || ch == L'}' || ch == L'"' || ch == L'\'')) {
                if (charAt(cr.cpMin) == ch) {
                    SendMessage(h, EM_SETSEL, cr.cpMin + 1, cr.cpMin + 1);
                    return 0;
                }
            }

            if (ch == L'}') outdentForClose(h);

            if (ch == L'(' || ch == L'[' || ch == L'{' || ch == L'"' || ch == L'\'') {
                wchar_t closeCh = (ch == L'(') ? L')'
                                : (ch == L'[') ? L']'
                                : (ch == L'{') ? L'}'
                                : ch;

                // Quotes inside a word: bare insert (don't pair)
                if ((ch == L'"' || ch == L'\'') && collapsed) {
                    wchar_t prev = (cr.cpMin > 0) ? charAt(cr.cpMin - 1) : 0;
                    wchar_t next = charAt(cr.cpMin);
                    if (iswalnum(prev) || iswalnum(next)) {
                        // fall through to native insert
                    } else {
                        std::wstring s;
                        s += ch; s += closeCh;
                        SendMessage(h, EM_REPLACESEL, TRUE, (LPARAM)s.c_str());
                        SendMessage(h, EM_SETSEL, cr.cpMin + 1, cr.cpMin + 1);
                            return 0;
                    }
                } else if (collapsed) {
                    std::wstring s;
                    s += ch; s += closeCh;
                    SendMessage(h, EM_REPLACESEL, TRUE, (LPARAM)s.c_str());
                    SendMessage(h, EM_SETSEL, cr.cpMin + 1, cr.cpMin + 1);
                    return 0;
                } else {
                    // Wrap selection: open + sel + close, selection stays selected
                    long selLen = cr.cpMax - cr.cpMin;
                    std::vector<wchar_t> sel((size_t)selLen + 1, 0);
                    SendMessage(h, EM_GETSELTEXT, 0, (LPARAM)sel.data());
                    std::wstring s;
                    s += ch;
                    s += sel.data();
                    s += closeCh;
                    SendMessage(h, EM_REPLACESEL, TRUE, (LPARAM)s.c_str());
                    SendMessage(h, EM_SETSEL, cr.cpMin + 1, cr.cpMin + 1 + selLen);
                    return 0;
                }
            }
        }
    }

    if (m == WM_MOUSEWHEEL) {
        if ((GetKeyState(VK_CONTROL) & 0x8000) != 0) {
            short delta = GET_WHEEL_DELTA_WPARAM(w);
            if (delta > 0) setZoom(codeFontSize + 1);
            else if (delta < 0) setZoom(codeFontSize - 1);
            return 0;
        }
        hideSuggestions();
        smartWheel(smartCode, w);
        InvalidateRect(hGutter, NULL, FALSE);
        return 0;
    }
    if (m == WM_KEYDOWN) {
        if (w != VK_RETURN && w != VK_TAB) swallowNextChar = false;
        // A keyboard viewport command takes ownership immediately; never let
        // a residual wheel easing target pull the editor back afterwards.
        if (!suggestVisible && (w == VK_UP || w == VK_DOWN || w == VK_PRIOR ||
            w == VK_NEXT || w == VK_HOME || w == VK_END)) {
            smartCode.active = false;
        }

        if (suggestVisible) {
            if (w == VK_UP) {
                int total = (int)curSnippets.size();
                if (total > 0) {
                    suggestSel = (suggestSel - 1 + total) % total;
                    if (suggestSel < suggestScroll) suggestScroll = suggestSel;
                    if (suggestSel >= suggestScroll + gSuggestLayout.maxVisible)
                        suggestScroll = suggestSel - gSuggestLayout.maxVisible + 1;
                    InvalidateRect(hSuggest, NULL, FALSE);
                }
                return 0;
            }
            if (w == VK_DOWN) {
                int total = (int)curSnippets.size();
                if (total > 0) {
                    suggestSel = (suggestSel + 1) % total;
                    if (suggestSel < suggestScroll) suggestScroll = suggestSel;
                    if (suggestSel >= suggestScroll + gSuggestLayout.maxVisible)
                        suggestScroll = suggestSel - gSuggestLayout.maxVisible + 1;
                    InvalidateRect(hSuggest, NULL, FALSE);
                }
                return 0;
            }
            if (w == VK_RETURN || w == VK_TAB) {
                swallowNextChar = true;
                applySelectedSnippet();
                return 0;
            }
            if (w == VK_ESCAPE) {
                hideSuggestions();
                return 0;
            }
            if (w == VK_LEFT || w == VK_RIGHT || w == VK_HOME || w == VK_END || w == VK_PRIOR || w == VK_NEXT) {
                hideSuggestions();
            }
        }

        if (w == VK_RETURN && !suggestVisible) {
            swallowNextChar = true;
            smartNewline(h);
            return 0;
        }

        // Ctrl+Left/Right → line start/end
        if ((GetKeyState(VK_CONTROL) & 0x8000) && (w == VK_LEFT || w == VK_RIGHT)) {
            hideSuggestions();
            moveLineEdge(h, w == VK_RIGHT,
                (GetKeyState(VK_SHIFT) & 0x8000) != 0); // +Shift selects (VS Code-style)
            return 0;
        }
        // Ctrl+D duplicate, Ctrl+Shift+K delete line
        if ((GetKeyState(VK_CONTROL) & 0x8000) &&
            (GetKeyState(VK_SHIFT) & 0x8000) && w == 'K') {
            deleteLine();
            return 0;
        }
        if ((GetKeyState(VK_CONTROL) & 0x8000) &&
            !(GetKeyState(VK_SHIFT) & 0x8000) && w == 'D') {
            duplicateLine();
            return 0;
        }
        // Text-only paste: keyboard paste enters via WM_CHAR, never as
        // WM_PASTE — intercept Ctrl+V / Shift+Insert so images can't embed.
        if (((GetKeyState(VK_CONTROL) & 0x8000) && w == 'V') ||
            ((GetKeyState(VK_SHIFT) & 0x8000) && !(GetKeyState(VK_CONTROL) & 0x8000) && w == VK_INSERT)) {
            if (pastePlainText(h, false, NULL)) { hideSuggestions(); return 0; }
        }

        // Explicit history shortcuts: Ctrl+Z undo, Ctrl+Shift+Z (and Ctrl+Y)
        // redo.  We handle them here so every RichEdit version behaves alike.
        if ((GetKeyState(VK_CONTROL) & 0x8000) && !(GetKeyState(VK_MENU) & 0x8000) &&
            (w == 'Z' || w == 'Y')) {
            const bool redo = w == 'Y' || (w == 'Z' && (GetKeyState(VK_SHIFT) & 0x8000));
            SendMessageW(h, redo ? EM_REDO : EM_UNDO, 0, 0);
            hideSuggestions();
            return 0;
        }

        if (w == VK_TAB) {
            swallowNextChar = true;
            changeIndentation(h, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
            return 0;
        }
    }
    LRESULT r = CallWindowProc(codeOrig, h, m, w, l);
    if (m == WM_KEYUP) {
        if (w != VK_UP && w != VK_DOWN && w != VK_RETURN && w != VK_TAB && w != VK_ESCAPE &&
            w != VK_LEFT && w != VK_RIGHT && w != VK_HOME && w != VK_END && w != VK_PRIOR && w != VK_NEXT) {
            updateSuggestions();
        }
    }
    if (m == WM_LBUTTONDOWN || m == WM_KILLFOCUS) {
        hideSuggestions();
    }
    if (m == WM_SETFOCUS) {
        InvalidateRect(hGutter, NULL, FALSE);
    }
    if (m == WM_VSCROLL || m == WM_HSCROLL || m == WM_MOUSEWHEEL || m == WM_KEYUP || m == WM_LBUTTONUP ||
        m == WM_KEYDOWN || m == WM_CHAR || m == WM_LBUTTONDOWN || m == WM_LBUTTONDBLCLK ||
        m == WM_PASTE || m == WM_CUT || m == WM_CLEAR || m == WM_UNDO) {
        if (m == WM_VSCROLL || m == WM_HSCROLL || m == WM_MOUSEWHEEL || m == WM_KEYUP || m == WM_LBUTTONUP)
            InvalidateRect(hGutter, NULL, FALSE);
    }
    // RichEdit may recreate its default caret while processing an edit/mouse
    // message. Re-pin it before returning so only the custom glide caret shows.
    if (GetFocus() == h) pinNativeCaret(h);
    return r;
}

// Current-line highlight band removed — active line is marked by the bold gutter number

// Time-based animation updater.  The master timer deliberately runs faster
// than 60 Hz for high-refresh caret motion, so UI easing must be scaled by QPC
// time rather than treating every callback as a 60 Hz frame.
void tickAnimations() {
    if (!hMain) return;
    static double lastAnimationTick = 0.0;
    const double now = nowSec();
    double dt = lastAnimationTick > 0.0 ? now - lastAnimationTick : 1.0 / 60.0;
    lastAnimationTick = now;
    dt = std::max(0.0, std::min(dt, 0.050));
    const float frames60 = (float)(dt * 60.0);
    const auto easedStep = [frames60](float per60Frame) {
        return 1.0f - powf(1.0f - per60Frame, frames60);
    };
    bool needRepaintMain = false;
    RECT rcClient;
    GetClientRect(hMain, &rcClient);
    int winW = rcClient.right - rcClient.left;

    // The completion selection used to advance from WM_PAINT, which makes it
    // run at an arbitrary rate and can form a paint/invalidate loop. It now
    // shares the QPC/display-paced animation clock with the main chrome.
    bool haveSuggestTarget = false;
    float suggestTargetY = 0.0f;
    if (suggestVisible && hSuggest && IsWindow(hSuggest) && !curSnippets.empty()) {
        const int visibleRows = suggestionVisibleRows();
        const int row = suggestSel - suggestScroll;
        if (row >= 0 && row < visibleRows) {
            suggestTargetY = (float)(gSuggestLayout.pad + row * gSuggestLayout.itemH);
            haveSuggestTarget = true;
        }
    }

    // Idle short-circuit: nothing animating and no hover → skip the whole tick
    bool busy = (haveSuggestTarget && (!animSuggestSelInit ||
                 fabsf(suggestTargetY - animSuggestSelY) > 0.15f)) ||
                fabs(animRunHover - (hoveredRunBtn ? 1.0f : 0.0f)) > 0.005f ||
                fabs(animRunPress - ((pressedRunBtn && hoveredRunBtn) ? 1.0f : 0.0f)) > 0.005f ||
                fabs(animRunHover) > 0.005f || fabs(animRunPress) > 0.005f ||
                fabs(animRunState - (sAlive ? 1.0f : 0.0f)) > 0.005f ||
                fabs(animLeftCardFocus - ((GetFocus() == hCode) ? 1.0f : 0.0f)) > 0.005f ||
                fabs(animRightCardFocus - ((GetFocus() == hOut || sAlive) ? 1.0f : 0.0f)) > 0.005f;
    for (int i = 0; i < 3; i++)
        if (fabs(animMacHover[i] - (hoveredBtn == i ? 1.0f : 0.0f)) > 0.005f) busy = true;
    // popup fade/scale still running?
    if ((suggestVisible || suggestFading) &&
        fabsf(animSuggestPop - (suggestVisible ? 1.0f : 0.0f)) > 0.005f) busy = true;
    // explorer hover glide still running?
    {
        float tY = (expHover >= 0) ? (float)(EXP_HEAD_H + (expHover - expScroll) * EXP_ROW_H) : animExpHoverY;
        float tA = (expHover >= 0) ? 1.0f : 0.0f;
        if (!animExpHoverInit || fabsf(tY - animExpHoverY) > 0.5f || fabsf(tA - animExpHoverA) > 0.01f)
            busy = true;
    }
    if (themeAnimating) busy = true;
    if (expSliding || fabsf((expHidden ? 0.0f : (float)expWCur) - animExpWCur) > 0.5f) busy = true;
    if (!busy) return;

    // 1. Run button hover animation
    float targetRunHover = hoveredRunBtn ? 1.0f : 0.0f;
    if (fabs(targetRunHover - animRunHover) > 0.005f) {
        animRunHover += (targetRunHover - animRunHover) * easedStep(0.22f);
        RECT rcRun; runBtnRect(winW, &rcRun);
        InvalidateRect(hMain, &rcRun, FALSE);
    } else {
        animRunHover = targetRunHover;
    }

    // 2. Run button press animation
    float targetRunPress = (pressedRunBtn && hoveredRunBtn) ? 1.0f : 0.0f;
    if (fabs(targetRunPress - animRunPress) > 0.005f) {
        animRunPress += (targetRunPress - animRunPress) * easedStep(0.30f);
        RECT rcRun; runBtnRect(winW, &rcRun);
        InvalidateRect(hMain, &rcRun, FALSE);
    } else {
        animRunPress = targetRunPress;
    }

    // 3. Run button state transition (Run vs Stop)
    float targetRunState = sAlive ? 1.0f : 0.0f;
    if (fabs(targetRunState - animRunState) > 0.005f) {
        animRunState += (targetRunState - animRunState) * easedStep(0.18f);
        RECT rcRun; runBtnRect(winW, &rcRun);
        InvalidateRect(hMain, &rcRun, FALSE);
    } else {
        animRunState = targetRunState;
    }

    // 4. macOS traffic lights hover transitions
    for (int i = 0; i < 3; i++) {
        float targetHover = (hoveredBtn == i) ? 1.0f : 0.0f;
        if (fabs(targetHover - animMacHover[i]) > 0.005f) {
            animMacHover[i] += (targetHover - animMacHover[i]) * easedStep(0.25f);
            RECT rcMac = { 0, 0, 84, TOPBAR_H };
            InvalidateRect(hMain, &rcMac, FALSE);
        } else {
            animMacHover[i] = targetHover;
        }
    }

    // 5. Card focus border animations
    HWND focused = GetFocus();
    float targetLeftFocus = (focused == hCode) ? 1.0f : 0.0f;
    float targetRightFocus = (focused == hOut || sAlive) ? 1.0f : 0.0f;

    if (fabs(targetLeftFocus - animLeftCardFocus) > 0.005f || fabs(targetRightFocus - animRightCardFocus) > 0.005f) {
        animLeftCardFocus += (targetLeftFocus - animLeftCardFocus) * easedStep(0.15f);
        animRightCardFocus += (targetRightFocus - animRightCardFocus) * easedStep(0.15f);
        needRepaintMain = true;
    }

    if (needRepaintMain) {
        InvalidateRect(hMain, NULL, FALSE);
    }

    if (haveSuggestTarget) {
        if (!animSuggestSelInit) {
            animSuggestSelY = suggestTargetY;
            animSuggestSelInit = true;
            InvalidateRect(hSuggest, NULL, FALSE);
        } else if (fabsf(suggestTargetY - animSuggestSelY) > 0.15f) {
            animSuggestSelY += (suggestTargetY - animSuggestSelY) * easedStep(0.28f);
            InvalidateRect(hSuggest, NULL, FALSE);
        } else {
            animSuggestSelY = suggestTargetY;
        }
    }

    // 6. Suggestion popup fade + scale (in on show, out on hide)
    if (suggestVisible || suggestFading) {
        const float popTarget = suggestVisible ? 1.0f : 0.0f;
        if (fabsf(animSuggestPop - popTarget) > 0.01f) {
            animSuggestPop += (popTarget - animSuggestPop) * easedStep(0.30f);
            applySuggestPop();
            InvalidateRect(hSuggest, NULL, FALSE);
        } else {
            animSuggestPop = popTarget;
            applySuggestPop();
            if (!suggestVisible) { // fade-out finished — take the window off screen
                ShowWindow(hSuggest, SW_HIDE);
                suggestFading = false;
            }
        }
    }

    // 7. Explorer row hover glide
    {
        float tY = (expHover >= 0) ? (float)(EXP_HEAD_H + (expHover - expScroll) * EXP_ROW_H) : animExpHoverY;
        float tA = (expHover >= 0) ? 1.0f : 0.0f;
        if (!animExpHoverInit) {
            animExpHoverY = tY; animExpHoverA = tA; animExpHoverInit = true;
        } else if (fabsf(tY - animExpHoverY) > 0.5f || fabsf(tA - animExpHoverA) > 0.01f) {
            animExpHoverY += (tY - animExpHoverY) * easedStep(0.30f);
            animExpHoverA += (tA - animExpHoverA) * easedStep(0.30f);
            if (hFiles) InvalidateRect(hFiles, NULL, FALSE);
        }
    }

    // 8. Theme cross-fade — interpolate chrome each frame; re-lex text once at the end
    if (themeAnimating) {
        animThemeT += (float)(dt / 0.20);
        if (animThemeT >= 1.0f) {
            animThemeT = 1.0f;
            themeAnimating = false;
            th = themeTo;
            applyThemeChrome();
            updateControlFormatting(); // final text/font colors
            highlightCode();
            applyThemeStructural();
        } else {
            float e = animThemeT * animThemeT * (3.0f - 2.0f * animThemeT); // smoothstep
            th = lerpTheme(themeFrom, themeTo, e);
            applyThemeChrome();
        }
    }

    // 9. Explorer panel slide open/close
    {
        float tgt = expHidden ? 0.0f : (float)expWCur;
        float d = tgt - animExpWCur;
        if (fabsf(d) > 0.5f) {
            animExpWCur += d * easedStep(0.30f);
            expWDirty = true; // committed by the 90 Hz layout deferral
            if (hMain) InvalidateRect(hMain, NULL, FALSE);
            if (hFiles) InvalidateRect(hFiles, NULL, FALSE);
        } else if (animExpWCur != tgt) {
            animExpWCur = tgt;
            expSliding = false;
            expWDirty = true;
            if (hMain) InvalidateRect(hMain, NULL, FALSE);
        }
    }
}

// Smooth glide caret — draws its own bar over hCode; native caret pinned invisible.
// State machine lives entirely in tickCaret. Its blink phase is absolute (not
// tied to typing or glide movement), and failed position samples keep the last
// target instead of making the overlay disappear.
static float caretX = -1, caretY = -1;   // rendered position, hCode client coords
static float caretTX = -1, caretTY = -1; // target from EM_POSFROMCHAR
static int caretIdxLast = -1;
static bool caretVis = false;
// scroll coupling — the bar rides the text while the view scrolls, so it
// stays glued to its character instead of lagging against a moving target
static int caretScrollBase = 0;      // scroll pos when the ride base was taken
static bool caretHasBase = false;    // ride base valid (bar glued to its char)
static bool caretHasTarget = false;  // a sampled EM_POSFROMCHAR target exists
static int caretDrawX = INT_MIN, caretDrawY = INT_MIN;
static int caretDrawW = 0, caretDrawH = 0;
static HWND caretHost = NULL;        // hCode or hOut — one shared overlay
static bool caretOutputHost = false;

LRESULT CALLBACK CaretProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_ERASEBKGND) return 1;
    if (m == WM_NCHITTEST) return HTTRANSPARENT; // overlay passes through — editor keeps its I-beam
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT r;
        GetClientRect(h, &r);
        FillRect(dc, &r, hCaretBrush ? hCaretBrush : (HBRUSH)GetStockObject(WHITE_BRUSH));
        EndPaint(h, &ps);
        return 0;
    }
    return DefWindowProc(h, m, w, l);
}

void tickCaret() {
    if (!hCode || !hOut || !hCaret) return;

    // No glide: the caret snaps to the target every display frame (the
    // 102-tick is already paced to the monitor), so motion tracks input 1:1.
    // Blink keeps its own absolute clock below, unaffected by movement.
    const double now = nowSec();

    HWND focus = GetFocus();
    const bool outputMode = focus == hOut && sAlive;
    HWND host = focus == hCode ? hCode : (outputMode ? hOut : NULL);
    if (!host) {
        if (caretVis) { ShowWindow(hCaret, SW_HIDE); caretVis = false; }
        caretIdxLast = -1;
        caretHasBase = false;
        caretHasTarget = false;
        caretHost = NULL;
        caretDrawX = caretDrawY = INT_MIN;
        return;
    }

    const bool hostChanged = host != caretHost;
    if (hostChanged) {
        caretHost = host;
        caretOutputHost = outputMode;
        caretIdxLast = -1;
        caretHasBase = false;
        caretHasTarget = false;
        // Output may have a different surface color. Rebuild this frame's brush
        // even if the global blink bucket happens to be unchanged.
        g_caretAlpha = -1;
        caretDrawX = caretDrawY = INT_MIN;
    }

    // Viewport recolor is only useful while the code editor is active. Keeping
    // it below the focus gate avoids a RichEdit query/apply pass every 66 ms
    // while an output input or another app owns focus.
    if (!outputMode) {
        static double g_lastRecolor = 0;
        if (now - g_lastRecolor > 0.066) {
            g_lastRecolor = now;
            static int hlFirstLine = -1;
            int fl = (int)SendMessage(hCode, EM_GETFIRSTVISIBLELINE, 0, 0);
            if (fl != hlFirstLine) { hlFirstLine = fl; applyViewportSpans(); }
        }
    }

    DWORD a = 0, b = 0;
    if (outputMode) CallWindowProc(outOrig, hOut, EM_GETSEL, (WPARAM)&a, (LPARAM)&b);
    else SendMessage(hCode, EM_GETSEL, (WPARAM)&a, (LPARAM)&b);
    if (a > b) std::swap(a, b);
    // Output has a visible custom caret only in the live stdin region.
    if (a != b || (outputMode && (int)b < progLen)) {
        if (caretVis) { ShowWindow(hCaret, SW_HIDE); caretVis = false; }
        caretIdxLast = -1;
        caretHasBase = false;
        caretHasTarget = false;
        caretDrawX = caretDrawY = INT_MIN;
        return;
    }

    const int idx = (int)b;
    const bool idxChanged = idx != caretIdxLast;
    POINTL pt{ 0, 0 };

    if (outputMode) {
        // Output input can be pushed by arriving process text. Cache the
        // insertion point and ride its scroll delta, just as the code caret
        // does, so idle frames avoid EM_POSFROMCHAR.
        POINT sp{};
        CallWindowProc(outOrig, hOut, EM_GETSCROLLPOS, 0, (LPARAM)&sp);
        if (idxChanged) {
            caretHasBase = false;
        } else if (caretHasBase && sp.y != caretScrollBase) {
            const int dScroll = caretScrollBase - sp.y;
            caretY += (float)dScroll;
            caretTY += (float)dScroll;
            caretScrollBase = sp.y;
        }
        if (idxChanged || !caretHasTarget) {
            if (!outCaretClientPoint(idx, pt)) {
                if (caretVis) { ShowWindow(hCaret, SW_HIDE); caretVis = false; }
                return;
            }
            // RichEdit gives the line-box position. Keep the shared custom bar
            // in the lower glyph band without an excessive large-zoom offset.
            const int lineLeading = (std::max(1, g_lineH) - std::max(1, g_fontH)) / 2;
            pt.y += std::max(3, std::min(5, lineLeading));
            caretTX = (float)pt.x;
            caretTY = (float)pt.y;
            caretHasTarget = true;
            caretScrollBase = sp.y;
            caretHasBase = true;
        }
    } else {
        // Code-pane scroll coupling. While the index is unchanged the bar rides
        // the text instead of rubber-banding against a stale RichEdit sample.
        POINT sp{};
        SendMessage(hCode, EM_GETSCROLLPOS, 0, (LPARAM)&sp);
        if (idxChanged) {
            caretHasBase = false;
        } else if (caretHasBase && sp.y != caretScrollBase) {
            const int dScroll = caretScrollBase - sp.y;
            caretY += (float)dScroll;
            caretTY += (float)dScroll;
            caretScrollBase = sp.y;
        }

        // EM_POSFROMCHAR is synchronous and relatively expensive. The target
        // only changes when the caret index changes; in between, the scroll
        // coupling above moves the cached target without another text query.
        if (idxChanged || !caretHasTarget) {
            LRESULT res = SendMessage(hCode, EM_POSFROMCHAR, (WPARAM)&pt, (LPARAM)idx);
            if (res == -1 || pt.x < 0) {
                // doc-end: RichEdit can refuse cp == length — derive the insertion
                // point from the preceding character rather than hiding the bar.
                if (idx > 0) {
                    res = SendMessage(hCode, EM_POSFROMCHAR, (WPARAM)&pt, (LPARAM)(idx - 1));
                    if (res != -1 && pt.x >= 0) {
                        wchar_t wc[2] = {};
                        TEXTRANGEW gtr{ { idx - 1, idx }, wc };
                        SendMessage(hCode, EM_GETTEXTRANGE, 0, (LPARAM)&gtr);
                        if (wc[0] == L'\r' || wc[0] == L'\n') {
                            pt.x = 0;
                            pt.y += g_lineH;
                        } else {
                            HDC dc = GetDC(hCode);
                            if (dc) {
                                HFONT of = (HFONT)SelectObject(dc, hFontCode);
                                SIZE extent{};
                                GetTextExtentPoint32W(dc, wc, 1, &extent);
                                SelectObject(dc, of);
                                ReleaseDC(hCode, dc);
                                pt.x += extent.cx;
                            }
                        }
                    } else {
                        pt.x = -1;
                    }
                } else {
                    pt.x = -1;
                }
            }
            if (pt.x >= 0) {
                caretTX = (float)pt.x;
                caretTY = (float)pt.y;
                caretHasTarget = true;
                caretScrollBase = sp.y;
                caretHasBase = true;
            }
        }
    }

    if (!caretHasTarget) return;
    caretIdxLast = idx;
    caretX = caretTX;
    caretY = caretTY;

    RECT rcV{};
    GetClientRect(host, &rcV);
    if (caretY + g_caretTopPad < -g_caretH ||
        caretY + g_caretTopPad > rcV.bottom) {
        if (caretVis) { ShowWindow(hCaret, SW_HIDE); caretVis = false; }
        return;
    }

    // Absolute, standalone blink: this phase is never restarted by typing,
    // selection movement, caret glide, scroll, or a syntax update.
    constexpr DWORD CARET_BLINK_MS = 1000;
    const DWORD tickNow = GetTickCount();
    const float ph = (float)(tickNow % CARET_BLINK_MS) / (float)CARET_BLINK_MS;
    const float alpha = 0.18f + 0.82f * (0.5f + 0.5f * cosf(ph * 6.2831853f));
    const int q = (int)(alpha * 63.99f);
    if (q != g_caretAlpha) {
        g_caretAlpha = q;
        if (hCaretBrush) DeleteObject(hCaretBrush);
        const float t = q / 63.0f;
        const COLORREF bg = caretOutputHost ? th.bgOutput : th.bgCard;
        const COLORREF fg = th.text;
        hCaretBrush = CreateSolidBrush(RGB(
            (int)(GetRValue(bg) + (GetRValue(fg) - GetRValue(bg)) * t),
            (int)(GetGValue(bg) + (GetGValue(fg) - GetGValue(bg)) * t),
            (int)(GetBValue(bg) + (GetBValue(fg) - GetBValue(bg)) * t)));
        if (caretVis) InvalidateRect(hCaret, NULL, FALSE);
    }

    RECT rcH{};
    GetWindowRect(host, &rcH);
    POINT origin{ rcH.left, rcH.top };
    ScreenToClient(hMain, &origin);
    const int drawX = origin.x + (int)lroundf(caretX);
    const int drawY = origin.y + (int)lroundf(caretY) + g_caretTopPad;
    if (!caretVis || drawX != caretDrawX || drawY != caretDrawY ||
        g_caretW != caretDrawW || g_caretH != caretDrawH) {
        SetWindowPos(hCaret, HWND_TOP, drawX, drawY, g_caretW, g_caretH,
                     SWP_NOACTIVATE | (caretVis ? 0 : SWP_SHOWWINDOW));
        caretDrawX = drawX; caretDrawY = drawY;
        caretDrawW = g_caretW; caretDrawH = g_caretH;
    }
    if (!caretVis) { caretVis = true; InvalidateRect(hCaret, NULL, FALSE); }
}

// Output Console Subclass (Interactive Stdin & Read-Only Protection)
// Caret only in the live input zone (>= progLen) while running; pin invisible otherwise.
// RichEdit can reject EM_POSFROMCHAR at cp == document-length. In that case
// derive the actual insertion point from the final character — especially a
// trailing CR, which belongs to the next visual line.
static bool outCaretClientPoint(int caretIdx, POINTL& pt) {
    pt = { 0, 0 };
    LRESULT res = CallWindowProc(outOrig, hOut, EM_POSFROMCHAR, (WPARAM)&pt, (LPARAM)caretIdx);
    if (res != -1 && pt.x >= 0) return true;
    if (caretIdx <= 0) return false;

    res = CallWindowProc(outOrig, hOut, EM_POSFROMCHAR, (WPARAM)&pt, (LPARAM)(caretIdx - 1));
    if (res == -1 || pt.x < 0) return false;

    wchar_t last[2] = {};
    TEXTRANGEW tr{ { caretIdx - 1, caretIdx }, last };
    CallWindowProc(outOrig, hOut, EM_GETTEXTRANGE, 0, (LPARAM)&tr);
    if (last[0] == L'\r' || last[0] == L'\n') {
        pt.x = 0;
        pt.y += std::max(1, g_lineH);
    } else {
        HDC dc = GetDC(hOut);
        if (dc) {
            HFONT old = (HFONT)SelectObject(dc, hFontCode);
            SIZE extent{};
            GetTextExtentPoint32W(dc, last, 1, &extent);
            SelectObject(dc, old);
            ReleaseDC(hOut, dc);
            pt.x += extent.cx;
        }
    }
    return true;
}

// Output now uses the same hCaret overlay as the source editor. Keep
// RichEdit's system caret permanently invisible; tickCaret() decides whether
// the shared overlay appears in the live stdin region and where it belongs.
void syncOutCaret() {
    if (!hOut || !outOrig) return;
    // Pin unconditionally: program output appends while focus sits in the
    // editor, and that path left RichEdit's default caret showing as a 1px
    // dot at the end of output. (CodeProc re-pins the editor on its own
    // messages when focused, so this can't steal its caret.)
    pinNativeCaret(hOut);
}

// g++ diagnostic "path\file.cpp:<line>[:<col>]:" — prefer a line:col pair over
// a bare line token (include-chain "x.hpp:1," must not clobber "b.cpp:10:5:")
static bool parseDiagLoc(const std::wstring& s, long* outLine, long* outCol) {
    // stripped display form: line starts with "5:10  error..."
    size_t c1 = 0;
    while (c1 < s.size() && iswdigit(s[c1])) c1++;
    if (c1 > 0 && c1 + 1 < s.size() && s[c1] == L':') {
        long a = 0;
        for (size_t x = 0; x < c1; x++) { a = a * 10 + (s[x] - L'0'); if (a > 999999) break; }
        size_t c2 = c1 + 1;
        long b = 0;
        while (c2 < s.size() && iswdigit(s[c2])) { b = b * 10 + (s[c2] - L'0'); c2++; if (b > 99999) break; }
        if (a >= 1 && c2 > c1 + 1 && (c2 >= s.size() || !iswdigit(s[c2]))) {
            *outLine = a;
            *outCol = b;
            return true;
        }
    }
    long lnPair = -1, colPair = -1, lnOnly = -1;
    for (size_t i = 0; i + 1 < s.size(); i++) {
        if (s[i] != L':' || !iswdigit(s[i + 1])) continue;
        size_t j = i + 1;
        long a = 0;
        while (j < s.size() && iswdigit(s[j])) { a = a * 10 + (s[j] - L'0'); j++; if (a > 999999) break; }
        if (j >= s.size() || s[j] != L':') {
            if (a >= 1 && lnOnly < 0) lnOnly = a;
            continue;
        }
        long b = -1;
        if (j + 1 < s.size() && iswdigit(s[j + 1])) {
            size_t k = j + 1;
            b = 0;
            while (k < s.size() && iswdigit(s[k])) {
                b = b * 10 + (s[k] - L'0');
                k++;
                if (b > 99999) break;
            }
        }
        if (a >= 1 && b >= 0) { lnPair = a; colPair = b; }
        else if (a >= 1 && lnOnly < 0) lnOnly = a;
    }
    if (lnPair >= 0) { *outLine = lnPair; *outCol = colPair; return true; }
    if (lnOnly >= 0) { *outLine = lnOnly; *outCol = -1; return true; }
    return false;
}

// Double-clicked output line → caret to that line:col in the editor (compile errors)
static bool jumpToOutputLine(HWND ho, int yClick) {
    int total = (int)SendMessage(ho, EM_GETLINECOUNT, 0, 0);
    int lineH = g_lineH > 0 ? g_lineH : 22;
    int hit = -1;
    for (int ln = 0; ln < total; ln++) {
        int ix = (int)SendMessage(ho, EM_LINEINDEX, ln, 0);
        POINTL pt{};
        SendMessage(ho, EM_POSFROMCHAR, (WPARAM)&pt, ix);
        long topY = pt.y, botY = topY + lineH;
        if (ln + 1 < total) {
            POINTL np{};
            SendMessage(ho, EM_POSFROMCHAR, (WPARAM)&np, (int)SendMessage(ho, EM_LINEINDEX, ln + 1, 0));
            if (np.y > topY) botY = np.y;
        }
        if (yClick >= topY && yClick < botY) { hit = ln; break; }
        if (pt.y > yClick) break;
    }
    if (hit < 0) return false;

    long ix = 0, end = 0;
    paraBounds(ho, (long)SendMessage(ho, EM_LINEINDEX, hit, 0), ix, end);
    std::wstring line;
    if (end > ix) {
        line.resize((size_t)(end - ix) + 1, 0);
        TEXTRANGEW tr{ { ix, end }, line.data() };
        SendMessage(ho, EM_GETTEXTRANGE, 0, (LPARAM)&tr);
        line.resize((size_t)(end - ix));
    }
    long tl = -1, tc = -1;
    if (!parseDiagLoc(line, &tl, &tc)) return false;

    // tl/tc are LOGICAL file line:col — EM_LINEINDEX is visual under wrap
    long idx = charOfPara(tl - 1);
    if (tc > 0) idx += tc - 1;
    long len = outDocLen(hCode);
    if (idx > len) idx = len;
    SendMessage(hCode, EM_SETSEL, idx, idx);

    // scroll the target into view with a little context above it
    RECT rcCode;
    GetClientRect(hCode, &rcCode);
    long visLines = g_lineH > 0 ? rcCode.bottom / g_lineH : 20;
    if (visLines < 3) visLines = 3;
    long firstVis = (long)SendMessage(hCode, EM_GETFIRSTVISIBLELINE, 0, 0);
    long want = (long)SendMessage(hCode, EM_LINEFROMCHAR, (WPARAM)idx, 0); // visual row of target
    if (want < firstVis || want > firstVis + visLines - 2)
        SendMessage(hCode, EM_LINESCROLL, 0, want - firstVis - 2);
    SendMessage(hCode, EM_SCROLLCARET, 0, 0);
    SetFocus(hCode);
    return true;
}

LRESULT CALLBACK OutProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_LBUTTONDBLCLK) {
        if (jumpToOutputLine(h, GET_Y_LPARAM(l))) return 0;
        return CallWindowProc(outOrig, h, m, w, l);
    }
    if (m == WM_LBUTTONDOWN) {
        // fallback: some RichEdit builds don't set CS_DBLCLKS (no WM_LBUTTONDBLCLK ever arrives)
        static ULONGLONG lastMs = 0;
        static DWORD lastPt = 0xFFFFFFFF;
        ULONGLONG now = GetTickCount64();
        DWORD p = (DWORD)l;
        int ddx = abs((int)(short)LOWORD(p) - (int)(short)LOWORD(lastPt));
        int ddy = abs((int)(short)HIWORD(p) - (int)(short)HIWORD(lastPt));
        bool dbl = ddx <= 4 && ddy <= 4 && lastMs != 0 && (now - lastMs) <= GetDoubleClickTime();
        lastMs = now;
        lastPt = p;
        if (dbl) {
            lastMs = 0; // one jump per double-click, no triple-click re-fire
            if (jumpToOutputLine(h, GET_Y_LPARAM(l))) return 0;
        }
    }
    if (m == WM_MOUSEWHEEL) {
        if ((GetKeyState(VK_CONTROL) & 0x8000) != 0) {
            short delta = GET_WHEEL_DELTA_WPARAM(w);
            if (delta > 0) setZoom(codeFontSize + 1);
            else if (delta < 0) setZoom(codeFontSize - 1);
            return 0;
        }
        smartWheel(smartOut, w);
        return 0;
    }

    if (m == WM_SETFOCUS || m == WM_KILLFOCUS) {
        LRESULT r = CallWindowProc(outOrig, h, m, w, l);
        syncOutCaret();
        return r;
    }

    if (m == WM_KEYDOWN && (w == VK_UP || w == VK_DOWN || w == VK_PRIOR ||
        w == VK_NEXT || w == VK_HOME || w == VK_END)) {
        // Keyboard scrolling must not compete with an older wheel target.
        smartOut.active = false;
    }

    if (!sAlive) {
        // When not running, output window is completely READ-ONLY!
        // Allow selection, scrolling, and copying shortcuts:
        if (m == WM_KEYDOWN) {
            bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            if (ctrl && (w == 'C' || w == 'A' || w == VK_INSERT)) {
                return CallWindowProc(outOrig, h, m, w, l);
            }
            if (w == VK_UP || w == VK_DOWN || w == VK_LEFT || w == VK_RIGHT ||
                w == VK_HOME || w == VK_END || w == VK_PRIOR || w == VK_NEXT) {
                LRESULT r = CallWindowProc(outOrig, h, m, w, l);
                syncOutCaret();
                return r;
            }
            // Block typing, Backspace, Delete, Enter
            return 0;
        }
        if (m == WM_CHAR || m == WM_PASTE || m == WM_CUT || m == WM_CLEAR || m == WM_UNDO) {
            return 0;
        }
        LRESULT r = CallWindowProc(outOrig, h, m, w, l);
        if (m == WM_LBUTTONDOWN || m == WM_LBUTTONUP) syncOutCaret();
        return r;
    }

    // ---- interactive run (sAlive) ----
    // Swallow the WM_CHAR TranslateMessage posts after we handled Enter/Backspace on KEYDOWN
    static bool outSwallowChar = false;
    if (m == WM_KEYDOWN) {
        if (outSwallowChar && w != VK_BACK && w != VK_RETURN) outSwallowChar = false;
        if (w == VK_RETURN) {
            DWORD a = 0, b = 0;
            CallWindowProc(outOrig, h, EM_GETSEL, (WPARAM)&a, (LPARAM)&b);
            if (a > b) std::swap(a, b);
            int fullLen = outDocLen(h);
            std::wstring lineStr;
            if (fullLen > progLen) {
                std::vector<wchar_t> allText((size_t)(fullLen - progLen) + 1, 0);
                TEXTRANGEW tr{ { progLen, fullLen }, allText.data() };
                CallWindowProc(outOrig, h, EM_GETTEXTRANGE, 0, (LPARAM)&tr);
                lineStr = allText.data();
            }
            while (!lineStr.empty() && (lineStr.back() == L'\r' || lineStr.back() == L'\n')) {
                lineStr.pop_back();
            }
            std::string u = w2u(lineStr) + "\n";
            DWORD wr = 0;
            if (sInW) WriteFile(sInW, u.c_str(), (DWORD)u.size(), &wr, NULL);
            CallWindowProc(outOrig, h, EM_SETSEL, fullLen, fullLen);
            CallWindowProc(outOrig, h, EM_REPLACESEL, FALSE, (LPARAM)L"\r\n");
            progLen = outDocLen(h);
            outSwallowChar = true;
            syncOutCaret();
            return 0;
        }
        // Backspace on KEYDOWN only — RichEdit deletes past progLen if it sees VK_BACK
        if (w == VK_BACK) {
            DWORD a = 0, b = 0;
            CallWindowProc(outOrig, h, EM_GETSEL, (WPARAM)&a, (LPARAM)&b);
            if (a > b) std::swap(a, b);
            if (a == b) {
                if ((int)a <= progLen) { outSwallowChar = true; syncOutCaret(); return 0; }
                CallWindowProc(outOrig, h, EM_SETSEL, a - 1, a);
            } else {
                if ((int)a < progLen) {
                    if ((int)b <= progLen) { outSwallowChar = true; syncOutCaret(); return 0; }
                    CallWindowProc(outOrig, h, EM_SETSEL, progLen, b);
                }
            }
            CallWindowProc(outOrig, h, EM_REPLACESEL, TRUE, (LPARAM)L"");
            outSwallowChar = true;
            syncOutCaret();
            return 0;
        }
        // Forward delete — same progLen clamp as Backspace (RichEdit won't apply one)
        if (w == VK_DELETE) {
            DWORD a = 0, b = 0;
            CallWindowProc(outOrig, h, EM_GETSEL, (WPARAM)&a, (LPARAM)&b);
            if (a > b) std::swap(a, b);
            int fullLen = outDocLen(h);
            if (a == b) {
                if ((int)a < progLen) { outSwallowChar = true; syncOutCaret(); return 0; }
                if ((int)a < fullLen) CallWindowProc(outOrig, h, EM_SETSEL, a, a + 1);
                else { outSwallowChar = true; syncOutCaret(); return 0; }
            } else {
                if ((int)a < progLen) {
                    if ((int)b <= progLen) { outSwallowChar = true; syncOutCaret(); return 0; }
                    CallWindowProc(outOrig, h, EM_SETSEL, progLen, b);
                }
            }
            CallWindowProc(outOrig, h, EM_REPLACESEL, TRUE, (LPARAM)L"");
            outSwallowChar = true;
            syncOutCaret();
            return 0;
        }
        // Other keys: let RichEdit handle navigation/editing it owns
        CallWindowProc(outOrig, h, m, w, l);
        syncOutCaret();
        return 0;
    }

    if (m == WM_CHAR) {
        if (outSwallowChar) {
            outSwallowChar = false;
            return 0;
        }
        // Control chars (Ctrl+C → 0x03, etc.) must not reach RichEdit as inserts
        if (w < 32 && w != L'\t') return 0;
        // Printable — keep caret out of program output
        if (w >= 32) {
            DWORD a = 0, b = 0;
            CallWindowProc(outOrig, h, EM_GETSEL, (WPARAM)&a, (LPARAM)&b);
            if ((int)a < progLen) {
                int e = outDocLen(h);
                CallWindowProc(outOrig, h, EM_SETSEL, e, e);
            }
        }
    } else if (m == WM_PASTE) {
        DWORD a = 0, b = 0;
        CallWindowProc(outOrig, h, EM_GETSEL, (WPARAM)&a, (LPARAM)&b);
        if ((int)a < progLen) {
            int e = outDocLen(h);
            CallWindowProc(outOrig, h, EM_SETSEL, e, e);
        }
        // Text-only paste here too — no images/OLE in the output pane.
        if (pastePlainText(h, true, outOrig)) { syncOutCaret(); return 0; }
    } else if (m == WM_CUT || m == WM_CLEAR || m == WM_UNDO) {
        DWORD a = 0, b = 0;
        CallWindowProc(outOrig, h, EM_GETSEL, (WPARAM)&a, (LPARAM)&b);
        if ((int)a < progLen) return 0;
    }

    LRESULT r = CallWindowProc(outOrig, h, m, w, l);
    if (m == WM_CHAR || m == WM_LBUTTONDOWN || m == WM_LBUTTONUP)
        syncOutCaret();
    return r;
}

// Gutter Line Numbers Proc
// - one shared line box with RichEdit (same font, same rule-4 line height) => pixel-aligned
// - right-aligned + Google Sans Code tabular digits => horizontally stable across 1/10/100
// - caret line drawn brighter + bold
LRESULT CALLBACK GutterProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_SETCURSOR) {
        POINT pt; GetCursorPos(&pt); ScreenToClient(h, &pt);
        int total = (int)SendMessage(hCode, EM_GETLINECOUNT, 0, 0);
        int lineH = g_lineH > 0 ? g_lineH : 22;
        return uiCursor(pt.y < total * lineH ? IDC_HAND : IDC_ARROW);
    }
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT r;
        GetClientRect(h, &r);
        int gw = r.right - r.left, gh = r.bottom - r.top;

        HDC memDC = CreateCompatibleDC(dc);
        HBITMAP memBmp = CreateCompatibleBitmap(dc, gw, gh);
        HGDIOBJ oldBmp = SelectObject(memDC, memBmp);

        FillRect(memDC, &r, hBrBgCard ? hBrBgCard : (HBRUSH)GetStockObject(BLACK_BRUSH));

        SetBkMode(memDC, TRANSPARENT);

        int total = (int)SendMessage(hCode, EM_GETLINECOUNT, 0, 0);
        int first = (int)SendMessage(hCode, EM_GETFIRSTVISIBLELINE, 0, 0);
        DWORD s0 = 0, s1 = 0;
        SendMessage(hCode, EM_GETSEL, (WPARAM)&s0, (LPARAM)&s1);
        // The live selection end is mapped to a logical paragraph below.
        const int lineH = g_lineH > 0 ? g_lineH : 22;
        // Align by the glyph-cell BOTTOM, not arbitrary top/center nudges.
        // hFontCode is the same measured font used by RichEdit, and
        // EM_POSFROMCHAR gives the RichEdit line origin. Giving DrawText a
        // glyph-height box with DT_BOTTOM therefore makes the two ink bottoms
        // coincide across zoom/DPI/font fallback changes.
        // Baseline-planting box: g_glyphH is baseOff + descent, so a DT_BOTTOM
        // single line lands its baseline on pty + g_baseOff — the exact line
        // baseline RichEdit uses.  Shared by the empty-doc "1." row and numbered
        // rows, and correct at every zoom/DPI.
        const int glyphH = g_glyphH > 0 ? g_glyphH : std::max(1, g_fontH) + 2;

        if (GetWindowTextLengthW(hCode) == 0) {
            SelectObject(memDC, hFontBold);
            SetTextColor(memDC, th.text);
            RECT rcLine = { 0, 0, gw - 8, glyphH };
            DrawTextW(memDC, L"1.", -1, &rcLine, DT_RIGHT | DT_BOTTOM | DT_SINGLELINE);
        } else {
            // Hard-line numbers: EM_LINE* are visual rows once wrap is on — number a
            // row only where its paragraph starts. The CR-space paragraph table is
            // rebuilt only after edits, rather than copied/scanned on every paint.
            ensureParaCache();
            long caretChar = (s0 != s1) ? s1 : s0;
            int caretPara = paraIndexAt(caretChar);
            for (int ln = first; ln < total; ln++) {
                long ix = (long)SendMessage(hCode, EM_LINEINDEX, ln, 0);
                POINTL pt{};
                SendMessage(hCode, EM_POSFROMCHAR, (WPARAM)&pt, ix);
                if (pt.y > (long)gh) break;
                if (pt.y < -lineH) continue;
                if (!std::binary_search(g_paraStarts.begin(), g_paraStarts.end(), ix))
                    continue; // wrapped continuation row
                int paraIndex = paraIndexAt(ix);
                int paraNum = paraIndex + 1;
                bool active = (paraIndex == caretPara);
                SelectObject(memDC, active ? hFontBold : hFontCode);
                SetTextColor(memDC, active ? th.text : th.gutterText);

                wchar_t b[32];
                wsprintfW(b, L"%d.", paraNum);
                // Bottom-align GDI's digits to the RichEdit glyph cell on this line.
                RECT rcLine = { 0, pt.y, gw - 8, pt.y + glyphH };
                DrawTextW(memDC, b, -1, &rcLine, DT_RIGHT | DT_BOTTOM | DT_SINGLELINE);
            }
        }

        BitBlt(dc, 0, 0, gw, gh, memDC, 0, 0, SRCCOPY);
        SelectObject(memDC, oldBmp);
        DeleteObject(memBmp);
        DeleteDC(memDC);

        EndPaint(h, &ps);
        return 0;
    }
    if (m == WM_LBUTTONDOWN) {
        int y = GET_Y_LPARAM(l);
        int total = (int)SendMessage(hCode, EM_GETLINECOUNT, 0, 0);
        int lineH = g_lineH > 0 ? g_lineH : 22;
        int hit = -1;
        if (GetWindowTextLengthW(hCode) == 0) hit = 0;
        for (int ln = 0; ln < total && hit < 0; ln++) {
            int ix = (int)SendMessage(hCode, EM_LINEINDEX, ln, 0);
            POINTL pt{};
            SendMessage(hCode, EM_POSFROMCHAR, (WPARAM)&pt, ix);
            long topY = pt.y, botY = topY + lineH;
            if (ln + 1 < total) {
                POINTL np{};
                SendMessage(hCode, EM_POSFROMCHAR, (WPARAM)&np, (int)SendMessage(hCode, EM_LINEINDEX, ln + 1, 0));
                if (np.y > topY) botY = np.y; // wrapped lines span multiple boxes
            }
            if (y >= topY && y < botY) { hit = ln; break; }
            if (pt.y > y) break;
        }
        if (hit >= 0) {
            long s = 0, e = 0;
            paraBounds(hCode, (long)SendMessage(hCode, EM_LINEINDEX, (WPARAM)hit, 0), s, e);
            SendMessage(hCode, EM_SETSEL, s, e);
            SendMessage(hCode, EM_SCROLLCARET, 0, 0);
            SetFocus(hCode);
        }
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }
    return DefWindowProc(h, m, w, l);
}

// ============================================================================
// Files panel window — header + tree rows, chevrons + GDI+ stroke glyphs
// ============================================================================
static int expVisRows(HWND h) {
    RECT rc;
    GetClientRect(h, &rc);
    int n = (rc.bottom - rc.top - EXP_HEAD_H) / EXP_ROW_H;
    return n < 1 ? 1 : n;
}

static int expRowAt(int y) {
    if (y < EXP_HEAD_H) return -1;
    int idx = expScroll + (y - EXP_HEAD_H) / EXP_ROW_H;
    if (idx < 0 || idx >= (int)expRows.size()) return -1;
    return idx;
}

LRESULT CALLBACK TreeProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_ERASEBKGND) return 1;
    if (m == WM_SETCURSOR) {
        POINT pt; GetCursorPos(&pt); ScreenToClient(h, &pt);
        return uiCursor(expRowAt(pt.y) >= 0 ? IDC_HAND : IDC_ARROW);
    }
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT r;
        GetClientRect(h, &r);
        int gw = r.right, gh = r.bottom;

        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, gw, gh);
        HGDIOBJ old = SelectObject(mem, bmp);
        FillRect(mem, &r, hBrBgCard ? hBrBgCard : (HBRUSH)GetStockObject(BLACK_BRUSH));
        SetBkMode(mem, TRANSPARENT);

        // header: EXPLORER label + open folder name
        SelectObject(mem, hFontPill);
        SetTextColor(mem, th.gutterText);
        RECT hr = { 12, 4, gw - 8, 20 };
        DrawTextW(mem, L"EXPLORER", -1, &hr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        std::wstring rootName = baseName(expRoot);
        SetTextColor(mem, th.text);
        RECT rr = { 12, 21, gw - 8, EXP_HEAD_H - 5 };
        DrawTextW(mem, rootName.c_str(), -1, &rr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

        // hairline under the header — aligned with the header text box (12..gw-8)
        {
            HPEN sep = CreatePen(PS_SOLID, 1, th.border);
            HGDIOBJ oldPen = SelectObject(mem, sep);
            MoveToEx(mem, 12, EXP_HEAD_H - 1, NULL);
            LineTo(mem, gw - 8, EXP_HEAD_H - 1);
            SelectObject(mem, oldPen);
            DeleteObject(sep);
        }

        int vis = expVisRows(h);
        int maxScroll = (int)expRows.size() - vis;
        if (maxScroll < 0) maxScroll = 0;
        if (expScroll > maxScroll) expScroll = maxScroll;
        if (expScroll < 0) expScroll = 0;

        Graphics g(mem);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        std::wstring curPath = (cur >= 0 && cur < (int)docs.size()) ? docs[cur].path : L"";

        // Gliding hover band (drawn under the rows) — one band that eases
        // between rows and fades in on enter / out on leave, instead of a
        // per-row instant flip.
        if (animExpHoverA > 0.01f) {
            BYTE a = (BYTE)std::lround(std::min(1.0f, animExpHoverA) * 255.0f);
            SolidBrush hs(Color(a, GetRValue(th.runBtnBg), GetGValue(th.runBtnBg), GetBValue(th.runBtnBg)));
            GraphicsPath sp;
            addRect(sp, 6.0f, animExpHoverY, (float)(gw - 12), (float)EXP_ROW_H);
            g.FillPath(&hs, &sp);
        }

        for (int i = 0; i < vis; i++) {
            int idx = expScroll + i;
            if (idx >= (int)expRows.size()) break;
            TreeNode* n = expRows[idx].node;
            int ry = EXP_HEAD_H + i * EXP_ROW_H;
            bool isCur = !n->dir && !curPath.empty() && _wcsicmp(n->path.c_str(), curPath.c_str()) == 0;
            if (isCur) {
                COLORREF bgc = th.runBtnBg;
                SolidBrush sb(Color(255, GetRValue(bgc), GetGValue(bgc), GetBValue(bgc)));
                GraphicsPath sp;
                addRect(sp, 6.0f, (float)ry, (float)(gw - 12), (float)EXP_ROW_H);
                g.FillPath(&sb, &sp);
            }

            int x = 8 + expRows[idx].depth * 14;
            int cy = ry + EXP_ROW_H / 2;

            // chevron for folders
            if (n->dir) {
                COLORREF cc = th.gutterText;
                Pen cpen(Color(255, GetRValue(cc), GetGValue(cc), GetBValue(cc)), 1.6f);
                float cx = (float)x + 4.0f, cyc = (float)cy;
                PointF tri[3];
                if (n->open) {
                    tri[0] = PointF(cx - 3.5f, cyc - 2.0f);
                    tri[1] = PointF(cx + 3.5f, cyc - 2.0f);
                    tri[2] = PointF(cx, cyc + 3.5f);
                } else {
                    tri[0] = PointF(cx - 2.0f, cyc - 3.5f);
                    tri[1] = PointF(cx - 2.0f, cyc + 3.5f);
                    tri[2] = PointF(cx + 3.5f, cyc);
                }
                g.DrawPolygon(&cpen, tri, 3);
            }

            // folder / file glyph — filled, reads at 100% without squinting
            int gx = x + 14;
            COLORREF gc = n->dir ? th.type : th.text;
            BYTE gr = GetRValue(gc), gg = GetGValue(gc), gb = GetBValue(gc);
            Pen gpen(Color(255, gr, gg, gb), 1.3f);
            float fx = (float)gx, fy = (float)cy - 6.0f;
            if (n->dir) {
                PointF body[7] = {
                    PointF(fx, fy + 3.0f), PointF(fx, fy + 12.5f), PointF(fx + 14, fy + 12.5f),
                    PointF(fx + 14, fy + 3.0f), PointF(fx + 7.5f, fy + 3.0f),
                    PointF(fx + 6.0f, fy + 0.5f), PointF(fx + 1.5f, fy + 0.5f)
                };
                GraphicsPath fp;
                fp.AddLines(body, 7);
                fp.CloseFigure();
                SolidBrush fb(Color(110, gr, gg, gb)); // ponytail: translucent fill, not a second outline pass
                g.FillPath(&fb, &fp);
                g.DrawPath(&gpen, &fp);
            } else {
                PointF page[5] = {
                    PointF(fx + 1.5f, fy + 0.5f), PointF(fx + 8.5f, fy + 0.5f),
                    PointF(fx + 11.5f, fy + 3.5f), PointF(fx + 11.5f, fy + 12.5f),
                    PointF(fx + 1.5f, fy + 12.5f)
                };
                GraphicsPath fp;
                fp.AddLines(page, 5);
                fp.CloseFigure();
                SolidBrush pb(Color(36, 255, 255, 255));
                g.FillPath(&pb, &fp);
                g.DrawPath(&gpen, &fp);
                g.DrawLine(&gpen, fx + 8.5f, fy + 0.5f, fx + 8.5f, fy + 3.5f);
                g.DrawLine(&gpen, fx + 8.5f, fy + 3.5f, fx + 11.5f, fy + 3.5f);
            }

            SelectObject(mem, hFontPill);
            SetTextColor(mem, isCur ? th.function : th.text);
            RECT tr = { gx + 20, ry, gw - 6, ry + EXP_ROW_H };
            DrawTextW(mem, n->name.c_str(), -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        }

        BitBlt(dc, 0, 0, gw, gh, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(h, &ps);
        return 0;
    }
    if (m == WM_MOUSEWHEEL) {
        expScroll += (GET_WHEEL_DELTA_WPARAM(w) > 0 ? -3 : 3);
        if (expScroll < 0) expScroll = 0;
        int maxScroll = (int)expRows.size() - expVisRows(h);
        if (maxScroll < 0) maxScroll = 0;
        if (expScroll > maxScroll) expScroll = maxScroll;
        // wheel lparam is screen coords (forwarded) — re-pick hover row or the
        // highlight stays on the old row until the mouse moves
        POINT pt = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        ScreenToClient(h, &pt);
        int idx = expRowAt(pt.y);
        if (idx != expHover) expHover = idx;
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }
    if (m == WM_MOUSEMOVE) {
        TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, h, 0 };
        TrackMouseEvent(&tme);
        int idx = expRowAt(GET_Y_LPARAM(l));
        if (idx != expHover) {
            expHover = idx;
            InvalidateRect(h, NULL, FALSE);
        }
        return 0;
    }
    if (m == WM_MOUSELEAVE) {
        if (expHover != -1) {
            expHover = -1;
            InvalidateRect(h, NULL, FALSE);
        }
        return 0;
    }
    if (m == WM_LBUTTONDOWN) {
        int idx = expRowAt(GET_Y_LPARAM(l));
        if (idx >= 0) {
            TreeNode* n = expRows[idx].node;
            if (n->dir) {
                if (!n->scanned) expScan(*n);
                n->open = !n->open;
            } else if (!(cur >= 0 && cur < (int)docs.size() && docs[cur].path == n->path)) {
                loadPath(n->path);
            }
            expRebuild(); // after every toggle/scan — expRows holds pointers into the tree
            InvalidateRect(h, NULL, FALSE);
        }
        return 0;
    }
    return DefWindowProc(h, m, w, l);
}

// ---- Scrollbar hit testing / dragging --------------------------------------
// 1 = editor pane, 2 = output pane, 0 = neither.  The hit area is the whole
// reserved strip, not just the thumb, so grabbing the rail is easy.
static int sbHit(int x, int y) {
    RECT L, Rr, E;
    panels(&L, &Rr, &E);
    sbGeom(L, smartCode, sbCode);
    sbGeom(Rr, smartOut, sbOut);
    auto strip = [&](const ScrollbarGeom& g) {
        return !g.shown ? RECT{ 0, 0, 0, 0 }
                        : RECT{ g.track.left - 5, g.track.top - 4, g.track.right + 5, g.track.bottom + 4 };
    };
    auto inside = [](const RECT& r, int px, int py) {
        return px >= r.left && px < r.right && py >= r.top && py < r.bottom;
    };
    if (sbCode.shown && inside(strip(sbCode), x, y)) return 1;
    if (sbOut.shown && inside(strip(sbOut), x, y)) return 2;
    return 0;
}

static void sbScrollTo(SmartScroll& sc, const ScrollbarGeom& g, int y) {
    const int trackH = g.track.bottom - g.track.top;
    const int thumbH = g.thumb.bottom - g.thumb.top;
    const int span = std::max(1, trackH - thumbH);
    double frac = (double)(y - g.dragOff - g.track.top) / (double)span;
    frac = std::max(0.0, std::min(frac, 1.0));
    const int line = (int)lround(frac * sc.maxFirst);
    sc.animLine = sc.targetLine = (float)line;
    sc.active = false; // dragging is direct manipulation, not a wheel gesture
    smartSetFirstLine(sc, line);
    if (&sc == &smartCode && hGutter) InvalidateRect(hGutter, NULL, FALSE);
    InvalidateRect(hMain, NULL, FALSE);
}

// Main Window Procedure
LRESULT CALLBACK WndProc(HWND hwnd, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_CREATE) {
        hMain = hwnd;
        refreshVisualPacing(hwnd);
        DragAcceptFiles(hwnd, TRUE); // drop a source file or project folder anywhere on the app
        LoadLibraryW(L"Msftedit.dll");

        hCode = CreateWindowExW(0, MSFTEDIT_CLASS, NULL,
            WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
            0, 0, 0, 0, hwnd, NULL, NULL, NULL);
        if (!hCode) {
            hCode = CreateWindowExW(0, L"EDIT", NULL,
                WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
                0, 0, 0, 0, hwnd, NULL, NULL, NULL);
        }

        hGutter = CreateWindowW(L"Gutter", NULL, WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, NULL, NULL, NULL);

        hFiles = CreateWindowExW(0, L"CatPP_Tree", NULL, WS_CHILD | WS_CLIPSIBLINGS,
            0, 0, 0, 0, hwnd, NULL, GetModuleHandleW(NULL), NULL);

        // caret: hidden until first tickCaret (needs focus + metrics)
        hCaret = CreateWindowExW(0, L"CatPP_Caret", NULL, WS_CHILD | WS_CLIPSIBLINGS,
            0, 0, 0, 0, hwnd, NULL, GetModuleHandleW(NULL), NULL);

        hOut = CreateWindowExW(0, MSFTEDIT_CLASS, NULL,
            WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
            0, 0, 0, 0, hwnd, NULL, NULL, NULL);
        if (!hOut) {
            hOut = CreateWindowExW(0, L"EDIT", NULL,
                WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
                0, 0, 0, 0, hwnd, NULL, NULL, NULL);
        }

        smartCode.editor = hCode;
        smartOut.editor = hOut;
        // Retain RichEdit's vertical line-scroll plumbing but hide the native
        // chrome before the first paint.
        ShowScrollBar(hCode, SB_VERT, FALSE);
        ShowScrollBar(hOut, SB_VERT, FALSE);

        // Caps typed input, paste and EM_REPLACESEL — not WM_SETTEXT, which is
        // what loads use. 2M therefore refused every edit and paste in files the
        // app had just opened fine (and truncated a long program log), so use
        // RichEdit's own default instead.
        SendMessage(hCode, EM_SETLIMITTEXT, 0x7FFFFFFE, 0);
        SendMessage(hOut, EM_SETLIMITTEXT, 0x7FFFFFFE, 0);
        // RichEdit defaults vary between versions; make the requested 25-step
        // content history deterministic and keep formatting outside the stack.
        SendMessage(hCode, EM_SETUNDOLIMIT, 25, 0);
        // ENM_SELCHANGE: selection moves fire EN_SELCHANGE — tickCaret reads sel every frame
        SendMessage(hCode, EM_SETEVENTMASK, 0, ENM_CHANGE | ENM_SELCHANGE);
        SendMessage(hOut, EM_SETEVENTMASK, 0, ENM_SELCHANGE);

        codeOrig = (WNDPROC)SetWindowLongPtr(hCode, GWLP_WNDPROC, (LONG_PTR)CodeProc);
        outOrig  = (WNDPROC)SetWindowLongPtr(hOut,  GWLP_WNDPROC, (LONG_PTR)OutProc);

        // A 4 ms wake-up cadence can service panels through 240 Hz; actual
        // visual work is separately QPC-paced to the active monitor rate.
        SetTimer(hwnd, 102, 4, NULL);

        updateControlFormatting();
        newDoc();
        // Crash/close recovery: an autosaved buffer from the last session wins
        // over the empty new doc, so a mistyped close never loses work.
        {
            std::wstring sText, sPath; long sCaret = 0;
            // Only unsaved work counts as recovery. A session whose bytes are
            // already on disk kept nothing back from the last run, so it is a
            // leftover (interrupted exit, older build) — start empty instead of
            // resurrecting whatever file happened to be open.
            bool unsaved = loadSession(sText, sPath, sCaret);
            if (unsaved && !sPath.empty()) {
                std::string onDisk;
                if (readFileBytes(sPath, onDisk)) {
                    std::wstring d = u2w(onDisk);
                    foldEol(d);
                    foldEol(sText);
                    unsaved = d != sText;
                }
            }
            if (unsaved) {
                docs[0].text = sText;
                docs[0].path = sPath;
                docs[0].dirty = true; // unsaved work — keeps the "*" honest
                showDoc(0);
                if (sCaret > 0) {
                    long n = outDocLen(hCode);
                    SendMessage(hCode, EM_SETSEL, std::min(sCaret, n), std::min(sCaret, n));
                }
                refreshChrome();
            } else {
                // Nothing worth recovering — drop the leftover so it is not
                // re-read (and re-judged) on every following launch.
                DeleteFileW(sessionPath().c_str());
            }
        }

        SendMessage(hOut, EM_SETREADONLY, TRUE, 0);
        COLORREF noBorder = 0xFFFFFFFE; // DWMWA_COLOR_NONE (disables borders on Windows 11)
        DwmSetWindowAttribute(hwnd, 34 /* DWMWA_BORDER_COLOR */, &noBorder, sizeof(noBorder));
        COLORREF capColor = th.bgWindow;
        DwmSetWindowAttribute(hwnd, 35 /* DWMWA_CAPTION_COLOR */, &capColor, sizeof(capColor));
        int cornerPref = 1; // DWMWCP_DONOTROUND - square window corners on Windows 11
        DwmSetWindowAttribute(hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &cornerPref, sizeof(cornerPref));

        MARGINS margins = { 0, 0, 0, 0 };
        DwmExtendFrameIntoClientArea(hwnd, &margins);
    }
    else if (m == WM_NCPAINT) {
        return 0;
    }
    else if (m == WM_NCACTIVATE) {
        return TRUE;
    }
    else if (m == WM_TIMER && w == 101) {
        KillTimer(hwnd, 101);
        sessionTick(); // crash-safe autosave rides the existing re-lex timer
        // Re-lexing a large document while Windows is sending resize messages
        // makes the resize path hitch. WM_EXITSIZEMOVE schedules the final pass.
        if (!gWindowSizing) highlightCode();
        return 0;
    }
    else if (m == WM_TIMER && w == 103) {
        KillTimer(hwnd, 103); // one-shot: next edit or doc show re-arms it
        checkTick();
        return 0;
    }
    else if (m == WM_TIMER && w == 102) {
        static double lastSmartTick = 0.0;
        static double lastDeferredLayout = 0.0;
        const double now = nowSec();
        if (!takeVisualFrame(now)) return 0;

        // RichEdit reflow is materially heavier than a paint. Resize/splitter
        // bursts therefore commit at most at 90 Hz (or once each 60 Hz frame),
        // while all inexpensive visual interpolation still follows the display.
        if ((gLayoutDirty || expWDirty) &&
            (lastDeferredLayout <= 0.0 || now - lastDeferredLayout >= 1.0 / 90.0)) {
            gLayoutDirty = false;
            expWDirty = false;
            lastDeferredLayout = now;
            doLayout();
        }

        double dt = lastSmartTick > 0.0 ? now - lastSmartTick : g_displayFrameSec;
        // A debugger pause or modal dialog must not make eased animations leap.
        dt = std::max(0.0, std::min(dt, 0.050));
        lastSmartTick = now;
        tickSmartScrolls(dt);
        tickAnimations();
        tickCaret();
        return 0;
    }
    else if (m == WM_GETMINMAXINFO) {
        MINMAXINFO* mmi = (MINMAXINFO*)l;
        // The gutter, two cards, and an optional explorer need real room.  With
        // no track minimum the old layout could produce negative child widths.
        mmi->ptMinTrackSize.x = 800;
        mmi->ptMinTrackSize.y = 460;
        HMONITOR hMon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = { sizeof(mi) };
        if (GetMonitorInfoW(hMon, &mi)) {
            mmi->ptMaxPosition.x = mi.rcWork.left - mi.rcMonitor.left;
            mmi->ptMaxPosition.y = mi.rcWork.top - mi.rcMonitor.top;
            mmi->ptMaxSize.x = mi.rcWork.right - mi.rcWork.left;
            mmi->ptMaxSize.y = mi.rcWork.bottom - mi.rcWork.top;
        }
        return 0;
    }
    else if (m == WM_NCCALCSIZE) {
        if (w) {
            if (IsZoomed(hwnd)) {
                HMONITOR hMon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
                MONITORINFO mi = { sizeof(mi) };
                if (GetMonitorInfoW(hMon, &mi)) {
                    NCCALCSIZE_PARAMS* params = (NCCALCSIZE_PARAMS*)l;
                    params->rgrc[0] = mi.rcWork;
                }
            }
            return 0;
        }
        return DefWindowProc(hwnd, m, w, l);
    }
    else if (m == WM_NCHITTEST) {
        POINT pt = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        RECT rc;
        GetWindowRect(hwnd, &rc);

        // Edge resizing borders (8px) when not maximized
        if (!IsZoomed(hwnd)) {
            int b = 8;
            bool top = (pt.y < rc.top + b);
            bool bottom = (pt.y >= rc.bottom - b);
            bool left = (pt.x < rc.left + b);
            bool right = (pt.x >= rc.right - b);

            if (top && left) return HTTOPLEFT;
            if (top && right) return HTTOPRIGHT;
            if (bottom && left) return HTBOTTOMLEFT;
            if (bottom && right) return HTBOTTOMRIGHT;
            if (top) return HTTOP;
            if (bottom) return HTBOTTOM;
            if (left) return HTLEFT;
            if (right) return HTRIGHT;
        }

        // Custom Topbar hit testing
        if (pt.y < rc.top + TOPBAR_H) {
            POINT cpt = pt;
            ScreenToClient(hwnd, &cpt);
            if (getMacBtnHit(cpt.x, cpt.y) != -1) {
                return HTCLIENT;
            }
            if (isOverRunBtn(cpt.x, cpt.y, rc.right - rc.left)) {
                return HTCLIENT;
            }
            return HTCAPTION; // Everywhere else in topbar is draggable caption!
        }

        return HTCLIENT;
    }
    else if (m == WM_MOUSEMOVE) {
        int x = GET_X_LPARAM(l);
        int y = GET_Y_LPARAM(l);
        // A grabbed scrollbar owns the mouse: direct manipulation, so the pane
        // follows the pointer immediately (no wheel animation in between).
        if (sbCode.drag || sbOut.drag) {
            if (sbCode.drag) sbScrollTo(smartCode, sbCode, y);
            else sbScrollTo(smartOut, sbOut, y);
            return 0;
        }
        if (expDrag) {
            int dragW = x - M;
            if (dragW < EXP_W) dragW = EXP_W;
            if (dragW > EXP_W_MAX) dragW = EXP_W_MAX;
            // ponytail: layout at timer rate (60fps), not mouse rate — a richedit
            // reflow + full GDI+ repaint per WM_MOUSEMOVE is what looked janky
            if (dragW != expWCur) { expWCur = dragW; animExpWCur = (float)dragW; expSliding = false; expWDirty = true; }
            return 0;
        }
        RECT rcClient;
        GetClientRect(hwnd, &rcClient);
        int winW = rcClient.right - rcClient.left;

        bool overRun = isOverRunBtn(x, y, winW);
        if (overRun != hoveredRunBtn) {
            hoveredRunBtn = overRun;
            RECT rcRun; runBtnRect(winW, &rcRun);
            InvalidateRect(hwnd, &rcRun, FALSE);
        }

        int hit = getMacBtnHit(x, y);
        if (hit != hoveredBtn) {
            hoveredBtn = hit;
            RECT rcTopBar = { 0, 0, 80, TOPBAR_H };
            InvalidateRect(hwnd, &rcTopBar, FALSE);
        }

        // Rail hover: only the strip under the pointer lights its thumb up, so
        // the two-card look stays calm everywhere else.
        int sbPane = sbHit(x, y);
        if ((sbPane == 1) != sbCode.hover || (sbPane == 2) != sbOut.hover) {
            sbCode.hover = (sbPane == 1);
            sbOut.hover = (sbPane == 2);
            sbInvalidate(hMain);
        }

        if (!trackingMouse && (hit != -1 || overRun || sbPane)) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tme);
            trackingMouse = true;
        }
    }
    else if (m == WM_MOUSELEAVE) {
        trackingMouse = false;
        if (sbCode.hover || sbOut.hover) {
            sbCode.hover = sbOut.hover = false;
            sbInvalidate(hMain);
        }
        if (hoveredBtn != -1) {
            hoveredBtn = -1;
            RECT rcTopBar = { 0, 0, 84, TOPBAR_H };
            InvalidateRect(hwnd, &rcTopBar, FALSE);
        }
        if (hoveredRunBtn) {
            hoveredRunBtn = false;
            RECT rcClient;
            GetClientRect(hwnd, &rcClient);
            RECT rcRun; runBtnRect(rcClient.right - rcClient.left, &rcRun);
            InvalidateRect(hwnd, &rcRun, FALSE);
        }
    }
    else if (m == WM_LBUTTONDOWN) {
        int x = GET_X_LPARAM(l);
        int y = GET_Y_LPARAM(l);
        if (expEdgeHit(x, y)) {
            expDrag = true;
            SetCapture(hwnd);
            return 0;
        }
        RECT rcClient;
        GetClientRect(hwnd, &rcClient);
        int winW = rcClient.right - rcClient.left;

        if (isOverRunBtn(x, y, winW)) {
            pressedRunBtn = true;
            RECT rcRun; runBtnRect(winW, &rcRun);
            InvalidateRect(hwnd, &rcRun, FALSE);
            SetCapture(hwnd);
            return 0;
        }

        int hit = getMacBtnHit(x, y);
        if (hit != -1) {
            pressedMacBtn = hit;
            SetCapture(hwnd);
            return 0;
        }

        int sbPane = sbHit(x, y);
        if (sbPane) {
            ScrollbarGeom& dg = (sbPane == 1) ? sbCode : sbOut;
            const bool onThumb = y >= dg.thumb.top && y < dg.thumb.bottom;
            dg.drag = true;
            dg.hover = true;
            // A track click recentres the thumb on the pointer, then keeps
            // dragging from there — one gesture covers jump and scrub.
            dg.dragOff = onThumb ? y - dg.thumb.top : (dg.thumb.bottom - dg.thumb.top) / 2;
            SetCapture(hwnd);
            sbScrollTo(sbPane == 1 ? smartCode : smartOut, dg, y);
            return 0;
        }
    }
    else if (m == WM_LBUTTONUP) {
        if (sbCode.drag || sbOut.drag) {
            sbCode.drag = sbOut.drag = false;
            if (GetCapture() == hwnd) ReleaseCapture();
            sbInvalidate(hMain);
            return 0;
        }
        if (expDrag) {
            expDrag = false;
            // Commit the final width immediately; intermediate drag updates are
            // display-paced by timer 102 to avoid repeated RichEdit reflow.
            expWDirty = false;
            doLayout();
            if (GetCapture() == hwnd) ReleaseCapture();
            return 0;
        }
        if (GetCapture() == hwnd) ReleaseCapture();
        int x = GET_X_LPARAM(l);
        int y = GET_Y_LPARAM(l);
        RECT rcClient;
        GetClientRect(hwnd, &rcClient);
        int winW = rcClient.right - rcClient.left;

        const bool runClick = pressedRunBtn && isOverRunBtn(x, y, winW);
        if (pressedRunBtn) {
            pressedRunBtn = false;
            RECT rcRun; runBtnRect(winW, &rcRun);
            InvalidateRect(hwnd, &rcRun, FALSE);
        }

        int hit = getMacBtnHit(x, y);
        const int macClick = (pressedMacBtn == hit) ? hit : -1;
        pressedMacBtn = -1;
        // Activate a topbar action only when the press began on that control.
        // Previously, ending a title-bar drag over Close/Run could close or run.
        if (macClick == 0) {
            PostMessage(hwnd, WM_CLOSE, 0, 0);
        } else if (macClick == 1) {
            ShowWindow(hwnd, SW_MINIMIZE);
        } else if (macClick == 2) {
            ShowWindow(hwnd, IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE);
        } else if (runClick) {
            if (sAlive) stopSession(true);
            else startRun();
        }
        return 0;
    }
    else if (m == WM_SETCURSOR) {
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(hwnd, &pt);
        RECT rcClient;
        GetClientRect(hwnd, &rcClient);
        int winW = rcClient.right - rcClient.left;

        if (getMacBtnHit(pt.x, pt.y) != -1 || isOverRunBtn(pt.x, pt.y, winW)) {
            SetCursor(LoadCursor(NULL, IDC_HAND));
            return TRUE;
        }
        if (expEdgeHit(pt.x, pt.y) || expDrag) {
            SetCursor(LoadCursor(NULL, IDC_SIZEWE));
            return TRUE;
        }
    }
    else if (m == WM_DPICHANGED) {
        // Per-monitor-V2 sends a suggested physical rectangle.  Recreate fonts
        // and reapply the wrap target after accepting it, otherwise a window
        // dragged to another-DPI monitor has mismatched text, gutter and caret.
        const RECT* suggested = reinterpret_cast<const RECT*>(l);
        if (suggested) {
            SetWindowPos(hwnd, NULL, suggested->left, suggested->top,
                suggested->right - suggested->left, suggested->bottom - suggested->top,
                SWP_NOZORDER | SWP_NOACTIVATE);
        }
        gWrapApplied = -1;
        refreshVisualPacing(hwnd);
        updateFonts();
        updateControlFormatting();
        gLayoutDirty = false;
        doLayout();
        return 0;
    }
    else if (m == WM_ENTERSIZEMOVE) {
        gWindowSizing = true;
        gLayoutDirty = false;
        hideSuggestions();
        return 0;
    }
    else if (m == WM_EXITSIZEMOVE) {
        gWindowSizing = false;
        gLayoutDirty = false;
        // Commit exact wrapping/visible syntax only once after the resize drag.
        gWrapApplied = -1;
        doLayout();
        return 0;
    }
    else if (m == WM_SIZE) {
        hideSuggestions();
        // Windows sends WM_SIZE with a 0-sized client area on minimize. During
        // an interactive resize, coalesce the high-frequency size messages to
        // the paced animation loop instead of reflowing RichEdit per pixel.
        if (w != SIZE_MINIMIZED) {
            if (gWindowSizing) gLayoutDirty = true;
            else doLayout();
        }
        return 0;
    }
    else if (m == WM_MOVE) {
        // Only re-queries the display if the window crossed monitors; same-
        // monitor drag messages are a cached no-op.
        refreshVisualPacing(hwnd);
        hideSuggestions();
    }
    else if (m == WM_ACTIVATE) {
        if (LOWORD(w) == WA_INACTIVE) {
            hideSuggestions();
        }
    }
    else if (m == WM_ERASEBKGND) {
        return 1; // handled in WM_PAINT
    }
    else if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        paintAll(dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    else if (m == WM_APPEND) {
        std::string* s = (std::string*)l;
        if (w != (WPARAM)sGen || !sAlive) {
            delete s;
            return 0;
        }
        gOutHasProg = true; // streamed program output — auto-check won't wipe it
        gOutIsCheck = false;
        uiAppend(*s);
        delete s;
        return 0;
    }
    else if (m == WM_DONE) {
        if (w != (WPARAM)sGen || !sAlive) return 0;
        sAlive = false;
        DWORD ec = 0;
        if (sProc) {
            GetExitCodeProcess(sProc, &ec);
            CloseHandle(sProc);
            sProc = NULL;
        }
        if (sInW) { CloseHandle(sInW); sInW = NULL; }
        if (ec != 0) { // ponytail: success banner dropped, only errors shown
            wchar_t hdr[64];
            wsprintfW(hdr, L"\r\n[Process exited with code %lu]\r\n", ec);
            insertProg(hdr);
        }
        if (hOut) SendMessage(hOut, EM_SETREADONLY, TRUE, 0);
        syncOutCaret();
        RECT rcClient;
        GetClientRect(hMain, &rcClient);
        RECT rcRun; runBtnRect(rcClient.right - rcClient.left, &rcRun);
        InvalidateRect(hMain, &rcRun, FALSE);
        RECT L, Rr; panels(&L, &Rr);
        RECT rcPill = { Rr.left, 0, Rr.right, Rr.top + 24 };
        InvalidateRect(hMain, &rcPill, FALSE);
        return 0;
    }
    else if (m == WM_DROPFILES) {
        HDROP drop = (HDROP)w;
        UINT chars = DragQueryFileW(drop, 0, NULL, 0);
        if (chars > 0) {
            std::wstring dropped((size_t)chars + 1, L'\0');
            DragQueryFileW(drop, 0, dropped.data(), chars + 1);
            dropped.resize(chars);
            DWORD attr = GetFileAttributesW(dropped.c_str());
            if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY))
                expSetRoot(dropped);
            else
                loadPath(dropped);
        }
        DragFinish(drop);
        return 0;
    }
    else if (m == WM_CONTEXTMENU) {
        contextMenu(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        return 0;
    }
    else if (m == WM_COMMAND) {
        int id = LOWORD(w);
        if (id == 11) startRun();
        else if (id == 13) newDoc();
        else if (id == 14) openDoc();
        else if (id == 15) saveDoc(false);
        else if (id == 16) saveDoc(true);
        else if (id == 17) toggleTheme();
        else if (id == 19) showShortcuts();
        else if (id == 23) openFolder();
        else if (id == 25) findUI();
        else if (id == 26) findReplace();
        else if (id == 28) formatCode();
        else if (id == 29) startCheck(true);
        else if ((HWND)l == hCode && HIWORD(w) == EN_CHANGE && cur >= 0) {
            // modify flag separates real content edits from caret/format noise
            // (pinNativeCaret, EM_SETCHARFORMAT also raise EN_CHANGE); applies
            // (inHighlight/inApply) are programmatic — skip dirty + re-highlight.
            BOOL chg = (BOOL)SendMessage(hCode, EM_GETMODIFY, 0, 0);
            SendMessage(hCode, EM_SETMODIFY, FALSE, 0);
            if (!inHighlight && !inApply && chg) {
                if (cur < (int)docs.size()) {
                    if (!docs[cur].dirty) {
                        docs[cur].dirty = true;
                        SetWindowTextW(hMain, mainTitle().c_str());
                        RECT rcTop; // saved → unsaved badge lives in the topbar
                        GetClientRect(hwnd, &rcTop);
                        rcTop.bottom = TOPBAR_H;
                        InvalidateRect(hwnd, &rcTop, FALSE);
                    }
                }
                g_hlValid = false; // indices shifted — cached spans are stale until timer101 re-lexes
                g_paraCacheValid = false;
                SetTimer(hwnd, 101, 180, NULL);
                SetTimer(hwnd, 103, 1000, NULL); // auto syntax check once typing pauses
                // gutter width = measured "N." — re-layout only when digit count
                // of the line count crosses a power of 10 (9→10, 99→100, ...)
                static int gutDigits = -1;
                int tl2 = (int)SendMessage(hCode, EM_GETLINECOUNT, 0, 0), dg2 = 1;
                for (int t = 10; t <= tl2 && dg2 < 6; t *= 10) dg2++;
                if (dg2 != gutDigits) { gutDigits = dg2; doLayout(); }
            }
            InvalidateRect(hGutter, NULL, FALSE);
        }
        else if ((HWND)l == hCode && HIWORD(w) == EN_SELCHANGE) {
            InvalidateRect(hGutter, NULL, FALSE);
        }
        else if ((HWND)l == hOut && HIWORD(w) == EN_SELCHANGE) {
            syncOutCaret();
        }
        return 0;
    }
    else if (m == WM_CLOSE) {
        if (!confirmDiscard()) return 0;
        AnimateWindow(hwnd, 130, AW_BLEND | AW_HIDE);
        DestroyWindow(hwnd);
        return 0;
    }
    else if (m == WM_DESTROY) {
        KillTimer(hwnd, 101);
        KillTimer(hwnd, 102);
        // Clean exit: drop the recovery file, otherwise the next launch would
        // "restore" a session the user just closed on purpose. A crash leaves
        // the file behind, which is exactly when we want it.
        DeleteFileW(sessionPath().c_str());
        stopSession(true);
        // Best-effort cleanup; a child that ignored termination may still lock
        // an executable, hence each run has a unique path as an additional guard.
        if (!runSourcePath.empty()) DeleteFileW(runSourcePath.c_str());
        if (!runExePath.empty()) DeleteFileW(runExePath.c_str());
        hMain = NULL;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, m, w, l);
}

// Entry Point
int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, LPWSTR, int) {
    timeBeginPeriod(1); // 1ms timer resolution — default ~15.6ms made the fade stutter

    // Set Per-Monitor V2 DPI awareness for crystal-clear high-res rendering
    HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
    if (hUser32) {
        typedef BOOL (WINAPI *SetProcessDpiAwarenessContextProc)(DPI_AWARENESS_CONTEXT);
        SetProcessDpiAwarenessContextProc setDpiContext = (SetProcessDpiAwarenessContextProc)GetProcAddress(hUser32, "SetProcessDpiAwarenessContext");
        if (setDpiContext) {
            setDpiContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        }
    }

    // Compiler discovery is intentionally deferred until Run.  Deep package
    // scans can take seconds on a machine with a large WinGet/Chocolatey cache.

    GdiplusStartupInput gsi;
    ULONG_PTR tok;
    GdiplusStartup(&tok, &gsi, NULL);

    wchar_t tmp[MAX_PATH] = {};
    if (GetTempPathW(MAX_PATH, tmp) > 0) tmpDir = tmp;
    if (!tmpDir.empty() && (tmpDir.back() == L'\\' || tmpDir.back() == L'/')) tmpDir.pop_back();
    if (tmpDir.empty()) tmpDir = exeDirW();
    std::wstring instanceTmp = tmpDir + L"\\CatPP_" + std::to_wstring(GetCurrentProcessId());
    if (CreateDirectoryW(instanceTmp.c_str(), NULL) || GetLastError() == ERROR_ALREADY_EXISTS) {
        tmpDir = instanceTmp;
        ownsTmpDir = true;
    }

    hPaperBrush = CreateSolidBrush(th.bgWindow);

    std::wstring dir = exeDirW();

    // Fonts: prefer files next to the exe; else extract bundled RCDATA to %TEMP%
    auto installFontRes = [&](int resId, const wchar_t* fileName) {
        std::wstring p = dir + L"\\" + fileName;
        if (GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES) {
            HRSRC hr = FindResourceW(hi, MAKEINTRESOURCEW(resId), RT_RCDATA);
            if (hr) {
                DWORD sz = SizeofResource(hi, hr);
                HGLOBAL hg = LoadResource(hi, hr);
                void* data = hg ? LockResource(hg) : NULL;
                if (data && sz) {
                    p = tmpDir + L"\\" + fileName;
                    HANDLE hf = CreateFileW(p.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
                    if (hf != INVALID_HANDLE_VALUE) {
                        DWORD wrote = 0;
                        WriteFile(hf, data, sz, &wrote, NULL);
                        CloseHandle(hf);
                        if (wrote != sz) p.clear();
                    } else p.clear();
                }
            }
        }
        if (!p.empty() && GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) {
            AddFontResourceExW(p.c_str(), FR_PRIVATE, NULL);
            // Remember where the faces live: their own table metrics decide the
            // line-number baseline and the chrome cap height (see fontTableMetrics).
            if (resId == 101) g_codeFontFile = p;
            else if (resId == 103) g_uiFontFile = p;
        }
    };
    installFontRes(101, L"GoogleSansCode-Var.ttf");
    installFontRes(102, L"GoogleSansCode-Var-Italic.ttf");
    // Space Grotesk ships as RCDATA 103: read from next to the exe when it is
    // there, otherwise extracted to %TEMP% like the two Google Sans Code files.
    // GDI exposes the variable font as "Space Grotesk" (400 + Bold 700) plus the
    // separate families "Space Grotesk Light" and "Space Grotesk Medium".
    installFontRes(103, L"SpaceGrotesk-VariableFont_wght.ttf");
    spaceGroteskFamilyOk = FontFamilyAvailable(spaceGroteskFamily.c_str());

    // App icon: embedded via app.rc (id 1), disk fallback
    HICON hIcoBig = NULL, hIcoSm = NULL;
    hIcoBig = (HICON)LoadImageW(hi, MAKEINTRESOURCEW(1), IMAGE_ICON, 0, 0, LR_DEFAULTSIZE);
    hIcoSm = (HICON)LoadImageW(hi, MAKEINTRESOURCEW(1), IMAGE_ICON, 32, 32, 0);
    if (!hIcoBig) {
        std::wstring icoPath = dir + L"\\logo.ico";
        hIcoBig = (HICON)LoadImageW(NULL, icoPath.c_str(), IMAGE_ICON, 0, 0, LR_LOADFROMFILE);
        hIcoSm = (HICON)LoadImageW(NULL, icoPath.c_str(), IMAGE_ICON, 32, 32, LR_LOADFROMFILE | LR_SHARED);
    }

    // Create fonts
    updateFonts();

    WNDCLASSW g{0};
    g.lpfnWndProc = GutterProc;
    g.hInstance = hi;
    g.lpszClassName = L"Gutter";
    RegisterClassW(&g);

    WNDCLASSW tr{0};
    tr.lpfnWndProc = TreeProc;
    tr.hInstance = hi;
    tr.lpszClassName = L"CatPP_Tree";
    tr.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassW(&tr);

    WNDCLASSW cc{0};
    cc.lpfnWndProc = CaretProc;
    cc.hInstance = hi;
    cc.lpszClassName = L"CatPP_Caret";
    cc.hCursor = LoadCursor(NULL, IDC_IBEAM); // fallback — HTTRANSPARENT lets the editor show its own I-beam
    RegisterClassW(&cc);

    WNDCLASSEXW c{0};
    c.cbSize = sizeof(c);
    c.lpfnWndProc = WndProc;
    c.hInstance = hi;
    c.lpszClassName = L"CatPP";
    c.hbrBackground = hPaperBrush;
    c.hCursor = LoadCursor(NULL, IDC_ARROW);
    c.hIcon = hIcoBig;
    c.hIconSm = hIcoSm;
    RegisterClassExW(&c);

    WNDCLASSW sg{0};
    sg.lpfnWndProc = SuggestProc;
    sg.hInstance = hi;
    sg.lpszClassName = L"CatPP_Suggest";
    sg.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassW(&sg);

    // Themed dialogs + context menu (one class per proc)
    {
        WNDCLASSW d{0};
        d.lpfnWndProc = UiMsgProc;
        d.hInstance = hi;
        d.lpszClassName = L"CatPP_Dlg";
        d.hCursor = LoadCursor(NULL, IDC_ARROW);
        d.hIcon = hIcoBig;
        RegisterClassW(&d);

        WNDCLASSW ip{0};
        ip.lpfnWndProc = UiInputProc;
        ip.hInstance = hi;
        ip.lpszClassName = L"CatPP_Input";
        ip.hCursor = LoadCursor(NULL, IDC_ARROW);
        ip.hIcon = hIcoBig;
        RegisterClassW(&ip);

        WNDCLASSW f{0};
        f.lpfnWndProc = UiFileProc;
        f.hInstance = hi;
        f.lpszClassName = L"CatPP_File";
        f.style = CS_DBLCLKS;
        f.hCursor = LoadCursor(NULL, IDC_ARROW);
        f.hIcon = hIcoBig;
        RegisterClassW(&f);

        WNDCLASSW mn{0};
        mn.lpfnWndProc = UiMenuProc;
        mn.hInstance = hi;
        mn.lpszClassName = L"CatPP_Menu";
        mn.hCursor = LoadCursor(NULL, IDC_ARROW);
        RegisterClassW(&mn);
    }

    // Initial window size: compact, centered on screen
    RECT workArea;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
    int screenW = workArea.right - workArea.left;
    int screenH = workArea.bottom - workArea.top;

    int winW = (int)std::min(1040.0, screenW * 0.86);
    int winH = (int)std::min(640.0, screenH * 0.86);
    if (winW < 720) winW = 720;
    if (winH < 460) winH = 460;
    int winX = workArea.left + (screenW - winW) / 2;
    int winY = workArea.top + (screenH - winH) / 2;

    HWND hwnd = CreateWindowExW(WS_EX_APPWINDOW, L"CatPP", L"CatPP",
        WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_CLIPCHILDREN,
        winX, winY, winW, winH, NULL, NULL, hi, NULL);

    if (hIcoBig) {
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)hIcoBig);
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)(hIcoSm ? hIcoSm : hIcoBig));
    }

    hSuggest = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED,
        L"CatPP_Suggest", NULL,
        WS_POPUP | WS_CLIPCHILDREN,
        0, 0, 0, 0, hwnd, NULL, hi, NULL);
    if (hSuggest) {
        typedef HRESULT (WINAPI *SetAttrFn)(HWND, DWORD, LPCVOID, DWORD);
        static SetAttrFn setAttrSug = (SetAttrFn)GetProcAddress(
            GetModuleHandleW(L"dwmapi.dll"), "DwmSetWindowAttribute");
        if (setAttrSug) {
            COLORREF noBorder = 0xFFFFFFFE;
            setAttrSug(hSuggest, 34, &noBorder, sizeof(noBorder));
            int cornerPref = 1; // DWMWCP_DONOTROUND
            setAttrSug(hSuggest, 33, &cornerPref, sizeof(cornerPref));
        }
    }

    // Open file or folder from command line (e.g. CatPP.exe file.cpp | CatPP.exe some\dir)
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc > 1) {
        std::wstring f = argv[1];
        DWORD fa = GetFileAttributesW(f.c_str());
        if (fa != INVALID_FILE_ATTRIBUTES && (fa & FILE_ATTRIBUTE_DIRECTORY)) {
            expSetRoot(f);
        } else {
            loadPath(f); // full path: sets path, invalidates explorer
        }
    }
    if (argv) LocalFree(argv);

    // Open animation (AW_BLEND) — ShowWindow would skip the fade
    AnimateWindow(hwnd, 180, AW_BLEND);
    UpdateWindow(hwnd);
    // AW_BLEND eats the pre-show InvalidateRect — force main + children to draw
    RedrawWindow(hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW | RDW_ALLCHILDREN);

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        // Wheel goes to the window under the cursor (tree / editor / output).
        // Ctrl+wheel stays with the focused window (editor zoom).
        if (msg.message == WM_MOUSEWHEEL && (GetKeyState(VK_CONTROL) & 0x8000) == 0) {
            POINT pt = { (short)LOWORD(msg.lParam), (short)HIWORD(msg.lParam) };
            HWND hit = WindowFromPoint(pt);
            HWND tgt = NULL;
            if (hFiles && IsWindowVisible(hFiles) && (hit == hFiles || IsChild(hFiles, hit))) tgt = hFiles;
            else if (hOut && (hit == hOut || IsChild(hOut, hit))) tgt = hOut;
            else if (hCode && (hit == hCode || IsChild(hCode, hit))) tgt = hCode;
            // The rail is the top-level window's own pixels, so aim the wheel at
            // the pane that bar belongs to instead of leaving it to focus.
            else if (hit == hMain) {
                POINT c = pt;
                ScreenToClient(hMain, &c);
                int pane = sbHit(c.x, c.y);
                if (pane == 1) tgt = hCode;
                else if (pane == 2) tgt = hOut;
            }
            if (tgt) {
                SendMessage(tgt, WM_MOUSEWHEEL, msg.wParam, msg.lParam);
                continue;
            }
        }
        if (msg.message == WM_KEYDOWN) {
            bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            if (ctrl) {
                if (shift && (msg.wParam == 'K')) { deleteLine(); continue; }
                if (shift && msg.wParam == 'O') { openFolder(); continue; }
                if (shift && msg.wParam == 'F') { formatCode(); continue; }
                if (msg.wParam == 'R') { startRun(); continue; }
                if (msg.wParam == 'B') { startCheck(true); continue; } // Ctrl+B check errors
                if (msg.wParam == 'S') { saveDoc(false); continue; }
                if (msg.wParam == 'T') { toggleTheme(); continue; }
                if (msg.wParam == 'O') { openDoc(); continue; }
                if (msg.wParam == 'E') { toggleExplorer(); continue; }
                if (msg.wParam == 'N') { newDoc(); continue; }
                if (msg.wParam == 'K') { showShortcuts(); continue; }
                if (msg.wParam == 'D') { duplicateLine(); continue; }
                if (msg.wParam == 'F') { findUI(); continue; }
                if (msg.wParam == 'H') { findReplace(); continue; }
                if (msg.wParam == VK_OEM_PLUS || msg.wParam == VK_ADD || msg.wParam == 187) { setZoom(codeFontSize + 1); continue; }
                if (msg.wParam == VK_OEM_MINUS || msg.wParam == VK_SUBTRACT || msg.wParam == 189) { setZoom(codeFontSize - 1); continue; }
                if (msg.wParam == '0' || msg.wParam == VK_NUMPAD0) { setZoom(11); continue; }
                if (msg.wParam == VK_OEM_2 || msg.wParam == VK_DIVIDE) { toggleComment(); continue; } // Ctrl+/
                if (msg.wParam == VK_HOME) { SendMessage(hCode, EM_SETSEL, 0, 0); continue; }
                if (msg.wParam == VK_END) {
                    long len = outDocLen(hCode);
                    SendMessage(hCode, EM_SETSEL, len, len);
                    continue;
                }
            }
            if (msg.wParam == VK_F3) { findNext(); continue; }
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    DeleteObject(hFontCode);
    DeleteObject(hFontPill);
    DeleteObject(hFontBold);
    DeleteObject(hFontHeavy);
    DeleteObject(hFontUI);
    DeleteObject(hFontUIBold);
    if (hbmCaretInvis) { DeleteObject(hbmCaretInvis); hbmCaretInvis = NULL; }
    if (hCaretBrush) { DeleteObject(hCaretBrush); hCaretBrush = NULL; }
    DeleteObject(hPaperBrush);
    if (hBrBgWin) DeleteObject(hBrBgWin);
    if (hBrBgCard) DeleteObject(hBrBgCard);
    if (hBrBgOut) DeleteObject(hBrBgOut);
    // Font resources extracted for this private instance must be removed before
    // deleting their files.  Cleanup is best-effort because a stubborn child
    // process may still hold its executable open.
    if (ownsTmpDir) {
        std::wstring regularFont = tmpDir + L"\\GoogleSansCode-Var.ttf";
        std::wstring italicFont = tmpDir + L"\\GoogleSansCode-Var-Italic.ttf";
        RemoveFontResourceExW(regularFont.c_str(), FR_PRIVATE, NULL);
        RemoveFontResourceExW(italicFont.c_str(), FR_PRIVATE, NULL);
        DeleteFileW(regularFont.c_str());
        DeleteFileW(italicFont.c_str());
        std::wstring groteskFont = tmpDir + L"\\SpaceGrotesk-VariableFont_wght.ttf";
        RemoveFontResourceExW(groteskFont.c_str(), FR_PRIVATE, NULL);
        DeleteFileW(groteskFont.c_str());
        DeleteFileW(runSourcePath.c_str());
        DeleteFileW(runExePath.c_str());
        RemoveDirectoryW(tmpDir.c_str());
    }
    GdiplusShutdown(tok);
    timeEndPeriod(1);
    return 0;
}
