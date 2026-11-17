#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <ctime>
#include <thread>
#include <atomic>
#include <unordered_map>
#include <dbt.h>
#define WM_TIMETABLE_SHOW (WM_APP + 101)
#define WM_TIMETABLE_HIDE (WM_APP + 102)
#define WM_TIMETABLE_RELOAD (WM_APP + 103)
namespace timetable_shared {

struct StatusRecord {
    int activeStartSeconds = 0;
    int activeEndSeconds = 0;
    std::wstring mainText;
    std::wstring subText;
    bool showMain = true;
    bool showSub = true;
    int countdownEndSeconds = 0;
};

bool ParseClockTime(const std::wstring& text, int& seconds) {
    int hour = -1;
    int minute = -1;
    int second = 0;
    const int parsed = swscanf_s(text.c_str(), L"%d:%d:%d", &hour, &minute, &second);
    if ((parsed != 2 && parsed != 3) || hour < 0 || hour > 23 ||
        minute < 0 || minute > 59 || second < 0 || second > 59) return false;
    seconds = hour * 3600 + minute * 60 + second;
    return true;
}

std::wstring FormatClockTime(int seconds) {
    seconds %= 24 * 60 * 60;
    if (seconds < 0) seconds += 24 * 60 * 60;
    wchar_t text[16] = {};
    swprintf_s(text, L"%02d:%02d:%02d", seconds / 3600, (seconds / 60) % 60, seconds % 60);
    return text;
}

std::wstring EscapeCsvField(const std::wstring& value) {
    if (value.find_first_of(L",\"\r\n") == std::wstring::npos) return value;
    std::wstring escaped = L"\"";
    for (wchar_t character : value) {
        if (character == L'"') escaped += L'"';
        escaped += character;
    }
    escaped += L'"';
    return escaped;
}

bool ParseStatusRow(const std::vector<std::wstring>& fields, StatusRecord& status) {
    if (fields.size() < 8 || fields[0] != L"status") return false;
    int showMain = -1;
    int showSub = -1;
    try {
        size_t consumed = 0;
        showMain = std::stoi(fields[5], &consumed);
        if (consumed != fields[5].size()) return false;
        showSub = std::stoi(fields[6], &consumed);
        if (consumed != fields[6].size()) return false;
    }
    catch (const std::exception&) {
        return false;
    }
    if ((showMain != 0 && showMain != 1) || (showSub != 0 && showSub != 1) ||
        !ParseClockTime(fields[1], status.activeStartSeconds) ||
        !ParseClockTime(fields[2], status.activeEndSeconds) ||
        !ParseClockTime(fields[7], status.countdownEndSeconds) ||
        status.activeStartSeconds >= status.activeEndSeconds) return false;
    status.mainText = fields[3];
    status.subText = fields[4];
    status.showMain = showMain == 1;
    status.showSub = showSub == 1;
    return true;
}

bool SameStatus(const StatusRecord& left, const StatusRecord& right) {
    return left.activeStartSeconds == right.activeStartSeconds &&
        left.activeEndSeconds == right.activeEndSeconds &&
        left.mainText == right.mainText && left.subText == right.subText &&
        left.showMain == right.showMain && left.showSub == right.showSub &&
        left.countdownEndSeconds == right.countdownEndSeconds;
}

}

namespace usb_watcher {


#define TIMER_HIDE_TEXT    1
#define TEXT_DISPLAY_MS    3000
#define REINSERT_BLOCK_MS  10000
#define DUP_BLOCK_MS       3000

HINSTANCE g_hInst = nullptr;
HWND g_hMainWnd = nullptr;
HWND g_hTextWnd = nullptr;
std::atomic_bool g_autoOpenEnabled{ true };

std::unordered_map<wchar_t, ULONGLONG> g_lastOpenTime;
std::unordered_map<wchar_t, ULONGLONG> g_lastRemoveTime;

std::wstring g_displayText;
int g_textWidth = 0;
int g_textHeight = 0;
int g_fontHeight = 36;

LRESULT CALLBACK MainWndProc(HWND, UINT, WPARAM, LPARAM);

void SetAutoOpenEnabled(bool enabled)
{
    g_autoOpenEnabled.store(enabled, std::memory_order_relaxed);
}

bool IsAutoOpenEnabled()
{
    return g_autoOpenEnabled.load(std::memory_order_relaxed);
}

void EnableDpiAwareness()
{
    HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
    if (hUser32)
    {
        typedef BOOL(WINAPI* PSetCtx)(HANDLE);
        PSetCtx pfn = (PSetCtx)GetProcAddress(
            hUser32,
            "SetProcessDpiAwarenessContext");
        if (pfn && pfn((HANDLE)-4))
            return;
    }
    HMODULE hShcore = LoadLibraryW(L"shcore.dll");
    if (hShcore)
    {
        typedef HRESULT(WINAPI* PSetAw)(int);
        PSetAw pfn = (PSetAw)GetProcAddress(
            hShcore,
            "SetProcessDpiAwareness");
        if (pfn && SUCCEEDED(pfn(2)))
        {
            FreeLibrary(hShcore);
            return;
        }
        FreeLibrary(hShcore);
    }
    SetProcessDPIAware();
}

std::wstring GetDriveLetterFromMask(DWORD mask)
{
    for (int i = 0; i < 26; ++i)
    {
        if (mask & (1u << i))
            return std::wstring(1, (wchar_t)(L'A' + i));
    }
    return L"";
}

bool WaitForRemovableDrive(
    const std::wstring& root,
    int timeoutMs)
{
    ULONGLONG start = GetTickCount64();
    while (GetTickCount64() - start <= (ULONGLONG)timeoutMs)
    {
        UINT t = GetDriveTypeW(root.c_str());
        if (t == DRIVE_REMOVABLE)
            return true;
        if (t != DRIVE_NO_ROOT_DIR && t != DRIVE_UNKNOWN)
            return false;
        Sleep(50);
    }
    return false;
}

void GetVolumeInfoSafe(
    const std::wstring& root,
    std::wstring& volName,
    std::wstring& fsName)
{
    volName.clear();
    fsName.clear();
    wchar_t vn[MAX_PATH] = {};
    wchar_t fs[MAX_PATH] = {};
    DWORD serial = 0;
    DWORD maxComp = 0;
    DWORD flags = 0;
    BOOL ok = GetVolumeInformationW(
        root.c_str(), vn, MAX_PATH,
        &serial, &maxComp, &flags,
        fs, MAX_PATH);
    if (ok)
    {
        volName = vn;
        fsName = fs;
    }
}

void HideTextWindow()
{
    KillTimer(g_hMainWnd, TIMER_HIDE_TEXT);
    if (g_hTextWnd)
    {
        DestroyWindow(g_hTextWnd);
        g_hTextWnd = nullptr;
    }
}

void DrawAndUpdate()
{
    if (!g_hTextWnd)
        return;

    HDC hdcScreen = GetDC(nullptr);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = g_textWidth;
    bmi.bmiHeader.biHeight = -g_textHeight;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* pBits = nullptr;
    HBITMAP hBmp = CreateDIBSection(
        hdcScreen, &bmi, DIB_RGB_COLORS,
        &pBits, nullptr, 0);
    HGDIOBJ oldBmp = SelectObject(hdcMem, hBmp);

    size_t szBytes = (size_t)g_textWidth
        * (size_t)g_textHeight * 4;
    ZeroMemory(pBits, szBytes);

    HFONT hFont = CreateFontW(
        g_fontHeight, 0, 0, 0, FW_BOLD,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        ANTIALIASED_QUALITY,
        DEFAULT_PITCH | FF_SWISS,
        L"Microsoft YaHei");
    HGDIOBJ oldFont = SelectObject(hdcMem, hFont);

    SetBkMode(hdcMem, TRANSPARENT);
    SetTextColor(hdcMem, RGB(0, 102, 204));

    RECT rc = { 20, 12,
        g_textWidth - 20,
        g_textHeight - 12 };
    UINT dtFlags = DT_SINGLELINE
        | DT_CENTER | DT_VCENTER | DT_NOPREFIX;
    DrawTextW(
        hdcMem,
        g_displayText.c_str(),
        -1, &rc, dtFlags);

    const int tr = 0;
    const int tg = 102;
    const int tb = 204;
    const int maxC = 204;
    BYTE* p = (BYTE*)pBits;
    int total = g_textWidth * g_textHeight;
    for (int i = 0; i < total; ++i)
    {
        BYTE b = p[0];
        BYTE g = p[1];
        BYTE r = p[2];
        int m = r > g ? r : g;
        if (b > m)
            m = b;
        if (m == 0)
        {
            p[3] = 0;
        }
        else
        {
            int a = m * 255 / maxC;
            if (a > 255)
                a = 255;
            p[0] = (BYTE)(tb * a / 255);
            p[1] = (BYTE)(tg * a / 255);
            p[2] = (BYTE)(tr * a / 255);
            p[3] = (BYTE)a;
        }
        p += 4;
    }

    RECT rcWnd;
    GetWindowRect(g_hTextWnd, &rcWnd);

    POINT ptSrc = { 0, 0 };
    POINT ptDst = { rcWnd.left, rcWnd.top };
    SIZE size = { g_textWidth, g_textHeight };
    BLENDFUNCTION blend =
    {
        AC_SRC_OVER, 0, 255, AC_SRC_ALPHA
    };
    UpdateLayeredWindow(
        g_hTextWnd, hdcScreen,
        &ptDst, &size,
        hdcMem, &ptSrc, 0,
        &blend, ULW_ALPHA);

    SelectObject(hdcMem, oldFont);
    SelectObject(hdcMem, oldBmp);
    DeleteObject(hFont);
    DeleteObject(hBmp);
    DeleteDC(hdcMem);
    ReleaseDC(nullptr, hdcScreen);
}

void ShowTextOnScreen(const std::wstring& text)
{
    g_displayText = text;
    HideTextWindow();

    HDC hdc = GetDC(nullptr);
    int dpi = GetDeviceCaps(hdc, LOGPIXELSY);
    ReleaseDC(nullptr, hdc);
    g_fontHeight = MulDiv(24, dpi, 96);

    HDC hdcMem = CreateCompatibleDC(nullptr);
    HFONT hFont = CreateFontW(
        g_fontHeight, 0, 0, 0, FW_BOLD,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        ANTIALIASED_QUALITY,
        DEFAULT_PITCH | FF_SWISS,
        L"Microsoft YaHei");
    HGDIOBJ oldFont = SelectObject(hdcMem, hFont);

    RECT rc = { 0, 0, 0, 0 };
    UINT dtFlags = DT_CALCRECT
        | DT_SINGLELINE | DT_NOPREFIX;
    DrawTextW(
        hdcMem,
        g_displayText.c_str(),
        -1, &rc, dtFlags);

    g_textWidth = rc.right - rc.left + 40;
    g_textHeight = rc.bottom - rc.top + 24;
    SelectObject(hdcMem, oldFont);
    DeleteObject(hFont);
    DeleteDC(hdcMem);

    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int x = (screenW - g_textWidth) / 2;
    int y = 10;

    DWORD exStyle = WS_EX_LAYERED
        | WS_EX_TRANSPARENT
        | WS_EX_TOPMOST
        | WS_EX_TOOLWINDOW
        | WS_EX_NOACTIVATE;
    g_hTextWnd = CreateWindowExW(
        exStyle,
        L"STATIC",
        L"",
        WS_POPUP,
        x, y,
        g_textWidth, g_textHeight,
        nullptr,
        nullptr,
        g_hInst,
        nullptr);
    if (!g_hTextWnd)
        return;

    DrawAndUpdate();
    ShowWindow(g_hTextWnd, SW_SHOWNOACTIVATE);
    SetTimer(
        g_hMainWnd,
        TIMER_HIDE_TEXT,
        TEXT_DISPLAY_MS,
        nullptr);
}

void OnDeviceArrival(DEV_BROADCAST_VOLUME* pVol)
{
    if (!IsAutoOpenEnabled())
        return;

    std::wstring drive =
        GetDriveLetterFromMask(pVol->dbcv_unitmask);
    if (drive.empty())
        return;

    wchar_t letter = drive[0];
    ULONGLONG now = GetTickCount64();

    auto itOpen = g_lastOpenTime.find(letter);
    if (itOpen != g_lastOpenTime.end())
    {
        ULONGLONG dt = now - itOpen->second;
        if (dt < DUP_BLOCK_MS)
            return;
    }

    auto itRem = g_lastRemoveTime.find(letter);
    if (itRem != g_lastRemoveTime.end())
    {
        ULONGLONG dt = now - itRem->second;
        if (dt < REINSERT_BLOCK_MS)
            return;
        g_lastRemoveTime.erase(itRem);
    }

    std::wstring root = drive + L":\\";

    if (!WaitForRemovableDrive(root, 2000))
        return;
    if (!IsAutoOpenEnabled())
        return;

    std::wstring volName;
    std::wstring fsName;
    GetVolumeInfoSafe(root, volName, fsName);

    g_lastOpenTime[letter] = GetTickCount64();

    std::wstring text =
        L"\u81EA\u52A8\u6253\u5F00 ";
    text += drive + L":";
    if (!volName.empty())
        text += L"  " + volName;
    if (!fsName.empty())
        text += L"  (" + fsName + L")";

    ShowTextOnScreen(text);

    ShellExecuteW(
        nullptr,
        L"open",
        root.c_str(),
        nullptr, nullptr,
        SW_SHOWNORMAL);
}

void OnDeviceRemoval(DEV_BROADCAST_VOLUME* pVol)
{
    std::wstring drive =
        GetDriveLetterFromMask(pVol->dbcv_unitmask);
    if (drive.empty())
        return;
    g_lastRemoveTime[drive[0]] = GetTickCount64();
}

LRESULT CALLBACK MainWndProc(
    HWND hwnd, UINT msg,
    WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_DEVICECHANGE:
    {
        PDEV_BROADCAST_HDR pHdr =
            (PDEV_BROADCAST_HDR)lParam;
        if (!pHdr)
            return TRUE;
        if (pHdr->dbch_devicetype
            != DBT_DEVTYP_VOLUME)
            return TRUE;
        DEV_BROADCAST_VOLUME* pVol =
            (DEV_BROADCAST_VOLUME*)pHdr;
        if (wParam == DBT_DEVICEARRIVAL)
            OnDeviceArrival(pVol);
        else if (wParam == DBT_DEVICEREMOVECOMPLETE)
            OnDeviceRemoval(pVol);
        return TRUE;
    }
    case WM_TIMER:
        if (wParam == TIMER_HIDE_TEXT)
            HideTextWindow();
        return 0;

    case WM_DESTROY:
        HideTextWindow();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}


void Initialize(HINSTANCE hInstance){g_hInst=hInstance;EnableDpiAwareness();WNDCLASSEXW wc={};wc.cbSize=sizeof(wc);wc.lpfnWndProc=MainWndProc;wc.hInstance=hInstance;wc.lpszClassName=L"UsbWatcherMain";if(!RegisterClassExW(&wc)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)return;g_hMainWnd=CreateWindowExW(WS_EX_TOOLWINDOW,L"UsbWatcherMain",L"",WS_POPUP,0,0,0,0,nullptr,nullptr,hInstance,nullptr);}
void Shutdown(){HideTextWindow();if(g_hMainWnd){DestroyWindow(g_hMainWnd);g_hMainWnd=nullptr;}}
}
namespace timetable_app {
#define WinMain TimetableWinMain
// ttaV8.8.0.cpp（由 lchV8.3.0 + ttaV8.5.0 合并版整合 exam 显示而来）
// 电子课程表 - 子窗口常显，主窗口上课时隐藏，下课/预备/放学时显示；
// CSV 里 exam 行的时间段显示考试窗口（考前20分钟开始，结束后5分钟隐藏），显示时主/子窗口隐藏。
// 编译：cl /utf-8 /EHsc /std:c++17 ttaV8.8.0.cpp /link user32.lib gdi32.lib shell32.lib
// 详见同目录的 CSV格式说明.md 与 README.md


#if defined(_MSC_VER)
#pragma execution_character_set("utf-8")
#endif

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "dwmapi.lib")

// ==================== 常量 ====================
#define FONT_NAME L"Microsoft YaHei"

const wchar_t* MAIN_CLASS_NAME = L"TimetableMainApp";
const wchar_t* SUB_CLASS_NAME = L"TimetableSubApp";

const int MAIN_WINDOW_HEIGHT = 90;
int SUB_WINDOW_WIDTH = 400;
int SUB_WINDOW_HEIGHT = 40;

// 颜色
const COLORREF BG_COLOR = RGB(26, 26, 28);
const COLORREF CURRENT_COLOR = RGB(86, 156, 86);
const COLORREF NEXT_COLOR = RGB(0, 255, 0);
const COLORREF TEXT_COLOR = RGB(248, 248, 248);
const COLORREF DISABLED_COLOR = RGB(100, 100, 110);
const COLORREF COURSE_COLOR = RGB(71, 231, 231);
const COLORREF TIMER_COLOR = RGB(0, 255, 0);
const COLORREF WARNING_COLOR = RGB(252, 243, 70);

// 显示模式
enum DisplayMode { MODE_AUTO = 0, MODE_ALWAYS = 1, MODE_NEVER = 2 };

// ==================== 数据结构 ====================
struct CourseInfo {
    std::wstring name;
    std::wstring abbreviation;
    std::wstring startTime;
    std::wstring endTime;
    int displayFlag;
    int weekPattern;
    int subDisplayFlag;
};

struct CountdownEvent {
    int year;
    int month;
    int day;
    std::wstring name;
};

struct FooterItem {
    bool isMessage;
    CountdownEvent countdown;
    std::wstring message;
};

// ==================== 全局变量 ====================
std::vector<CourseInfo> courses;
std::vector<timetable_shared::StatusRecord> statusRecords;

// ==================== 考试（exam 行） ====================
// exam 行格式：exam,科目,开始HH:MM,结束HH:MM（与 courses/status 行混排在同一份 CSV 里）
// exam 行不进入 courses，不影响上课/预备/下课判定；只决定考试窗口是否显示。
struct ExamInfo {
    std::wstring subject;
    std::wstring startTime;
    std::wstring endTime;
    int startSec = 0;
    int endSec = 0;
};
std::vector<ExamInfo> examRows;
std::vector<CountdownEvent> g_countdownEvents;
std::vector<FooterItem> g_footerItems;
HFONT hFontLarge = nullptr;
HFONT hFontSmall = nullptr;
HFONT hFontSub = nullptr;
HFONT hFontBadge = nullptr;

const BYTE WINDOW_TEXT_ALPHA = 160;
const BYTE WINDOW_BACKGROUND_ALPHA = 60;

HWND g_hMainWnd = nullptr;
HWND g_hSubWnd = nullptr;
HWND g_hExamWnd = nullptr;
bool g_launcherVisibilityEnabled = true;
UINT g_currentDpi = 96;

int ScaleForDpi(int value, UINT dpi = g_currentDpi) {
    return MulDiv(value, static_cast<int>(dpi), 96);
}

void ApplyRoundedCorners(HWND hWnd) {
    if (!hWnd) return;
    const DWM_WINDOW_CORNER_PREFERENCE preference = DWMWCP_ROUND;
    DwmSetWindowAttribute(hWnd, DWMWA_WINDOW_CORNER_PREFERENCE, &preference, sizeof(preference));
}

void RebuildFonts(UINT dpi) {
    HFONT newLarge = CreateFontW(ScaleForDpi(46, dpi), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, FONT_NAME);
    HFONT newSmall = CreateFontW(ScaleForDpi(22, dpi), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, FONT_NAME);
    HFONT newSub = CreateFontW(ScaleForDpi(32, dpi), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, 1, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, FONT_NAME);
    HFONT newBadge = CreateFontW(ScaleForDpi(28, dpi), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, FONT_NAME);
    if (!newLarge || !newSmall || !newSub || !newBadge) {
        if (newLarge) DeleteObject(newLarge);
        if (newSmall) DeleteObject(newSmall);
        if (newSub) DeleteObject(newSub);
        if (newBadge) DeleteObject(newBadge);
        return;
    }
    if (hFontLarge) DeleteObject(hFontLarge);
    if (hFontSmall) DeleteObject(hFontSmall);
    if (hFontSub) DeleteObject(hFontSub);
    if (hFontBadge) DeleteObject(hFontBadge);
    hFontLarge = newLarge;
    hFontSmall = newSmall;
    hFontSub = newSub;
    hFontBadge = newBadge;
    g_currentDpi = dpi;
}

// 显示模式（从配置文件读取）
DisplayMode g_mainMode = MODE_AUTO;
DisplayMode g_subMode = MODE_AUTO;
int g_timeOffsetSec = 0;           // 时间偏移（秒）

// 拖拽
bool isDragging = false;
POINT dragStartPoint, windowStartPoint;
bool isSubDragging = false;
POINT subDragStartPoint, subWindowStartPoint;

// 黑屏窗口
bool g_enableBlackScreen = false;
HWND g_hBlackWnd = nullptr;
bool g_blackScreenDismissed = false;
const wchar_t* BLACK_CLASS_NAME = L"TimetableBlackApp";
bool isBlackDragging = false;
POINT blackDragStartPoint, blackWindowStartPoint;

// 状态记录
bool g_wasInClass = false;
int  g_classEndTime = 0;

// 子窗口位置标记
bool g_subAtRight = false;        // 当前是否在右侧
bool g_subManualMoved = false;    // 用户上课期间是否手动移动过子窗口
bool g_wasInClassOrPrep = false;  // 上一帧是否处于上课/预备状态（用于重置手动标记）


// 重绘优化
std::wstring g_lastMainText;
std::wstring g_lastSubText;
int g_lastRemaining = -1;
int g_lastState = -1;
COLORREF g_lastMainColor = CLR_INVALID;
COLORREF g_lastSubColor = CLR_INVALID;
int g_lastSecond = -1;
int g_lastMinute = -1;
int g_lastActiveStatusIndex = -2;
bool g_mainNeedsCentering = true;

// 配置文件路径
const wchar_t* CONFIG_PATH = L"D:\\Timetable\\config.inf";
FILETIME g_lastConfigTime = { 0 }; // 上次读取的配置文件时间
std::wstring g_lastTimetablePath;
FILETIME g_lastTimetableTime = {};
int g_lastTimetableWeek = -1;

// 函数声明
int  CalculateMainWindowWidth();
int  CalculateMainWindowHeight();
int  CalculateCourseCellWidth();
void UpdateMainWindowSize();
int  TimeToSeconds(const std::wstring& t);
void UpdateSubWindowPosition();
void MoveSubWindowToRight();
void ResetSubWindowPosition();
void LoadConfig();
void HideExamWindow();
bool UpdateExamWindow();

// 追加子窗口显示数据函数声明
void GetSubDisplayData(int state, int remaining, const std::wstring& courseName,
    std::wstring& mainText, std::wstring& subText, COLORREF& mainColor, COLORREF& subColor);

// 日志
void Log(const std::wstring& msg) {
    static std::wstring lastMessage;
    if (msg == lastMessage) return;
    lastMessage = msg;

    SYSTEMTIME st;
    GetLocalTime(&st);
    CreateDirectoryW(L"D:\\Timetable", NULL);
    CreateDirectoryW(L"D:\\Timetable\\Logs", NULL);
    wchar_t logFile[MAX_PATH];
    swprintf_s(logFile, L"D:\\Timetable\\Logs\\log_%04d%02d%02d.txt", st.wYear, st.wMonth, st.wDay);
    FILE* f = nullptr;
    _wfopen_s(&f, logFile, L"a,ccs=UTF-8");
    if (f) {
        fwprintf(f, L"[%02d:%02d:%02d] %s\n", st.wHour, st.wMinute, st.wSecond, msg.c_str());
        fclose(f);
    }
}

// ==================== 辅助函数 ====================
std::vector<std::wstring> ParseCsvRow(const std::wstring& row) {
    std::vector<std::wstring> fields;
    std::wstring field;
    bool quoted = false;
    for (size_t i = 0; i < row.size(); ++i) {
        const wchar_t ch = row[i];
        if (quoted && ch == L'"' && i + 1 < row.size() && row[i + 1] == L'"') {
            field += L'"';
            ++i;
        }
        else if (ch == L'"') {
            quoted = !quoted;
        }
        else if (ch == L',' && !quoted) {
            fields.push_back(field);
            field.clear();
        }
        else {
            field += ch;
        }
    }
    fields.push_back(field);
    return fields;
}

bool ReadTextLinesAuto(const std::wstring& path, std::vector<std::wstring>& lines) {
    lines.clear();
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size) || size.QuadPart <= 0 || size.QuadPart > 64 * 1024 * 1024) {
        CloseHandle(h);
        return false;
    }
    std::vector<BYTE> bytes(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    const bool ok = ReadFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr);
    CloseHandle(h);
    if (!ok) return false;
    bytes.resize(read);

    UINT codePage = CP_ACP;
    size_t offset = 0;
    std::wstring text;
    if (bytes.size() >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE) {
        const wchar_t* wide = reinterpret_cast<const wchar_t*>(bytes.data() + 2);
        size_t chars = (bytes.size() - 2) / sizeof(wchar_t);
        text.assign(wide, wide + chars);
    } else {
        if (bytes.size() >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) {
            codePage = CP_UTF8;
            offset = 3;
        }
        int chars = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
            reinterpret_cast<const char*>(bytes.data() + offset),
            static_cast<int>(bytes.size() - offset), nullptr, 0);
        if (chars <= 0) {
            codePage = CP_ACP;
            chars = MultiByteToWideChar(codePage, 0,
                reinterpret_cast<const char*>(bytes.data() + offset),
                static_cast<int>(bytes.size() - offset), nullptr, 0);
        }
        if (chars <= 0) return false;
        text.assign(static_cast<size_t>(chars), L'\0');
        MultiByteToWideChar(codePage, 0,
            reinterpret_cast<const char*>(bytes.data() + offset),
            static_cast<int>(bytes.size() - offset), text.data(), chars);
    }

    std::vector<std::wstring> split;
    std::wstring current;
    for (wchar_t ch : text) {
        if (ch == L'\n') {
            if (!current.empty() && current.back() == L'\r') current.pop_back();
            split.push_back(std::move(current));
            current.clear();
        } else {
            current += ch;
        }
    }
    if (!current.empty()) split.push_back(std::move(current));
    lines.swap(split);
    return true;
}

int GetCurrentWeekNumber() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    SYSTEMTIME semesterStart = { 0 };
    semesterStart.wYear = 2025;
    semesterStart.wMonth = 9;
    semesterStart.wDay = 1;
    FILETIME ftStart, ftCurrent;
    SystemTimeToFileTime(&semesterStart, &ftStart);
    SystemTimeToFileTime(&st, &ftCurrent);
    ULARGE_INTEGER ulStart, ulCurrent;
    ulStart.LowPart = ftStart.dwLowDateTime;
    ulStart.HighPart = ftStart.dwHighDateTime;
    ulCurrent.LowPart = ftCurrent.dwLowDateTime;
    ulCurrent.HighPart = ftCurrent.dwHighDateTime;
    ULONGLONG diff = (ulCurrent.QuadPart - ulStart.QuadPart) / 10000000ULL / 86400ULL;
    return (int)(diff / 7) + 1;
}

std::wstring GetWeekdayName(int weekday) {
    const wchar_t* names[] = { L"Sunday", L"Monday", L"Tuesday", L"Wednesday",
                               L"Thursday", L"Friday", L"Saturday" };
    return (weekday >= 0 && weekday < 7) ? names[weekday] : L"Unknown";
}

std::wstring GetChineseWeekdayName(int weekday) {
    const wchar_t* names[] = { L"周日", L"周一", L"周二", L"周三", L"周四", L"周五", L"周六" };
    return (weekday >= 0 && weekday < 7) ? names[weekday] : L"未知";
}

bool ShouldDisplayCourse(const CourseInfo& c, int week) {
    if (c.weekPattern == 3) return true;
    if (c.weekPattern == 1 && week % 2 == 1) return true;
    if (c.weekPattern == 2 && week % 2 == 0) return true;
    return false;
}

int TimeToSeconds(const std::wstring& t) {
    int h, m;
    if (swscanf_s(t.c_str(), L"%d:%d", &h, &m) == 2)
        return h * 3600 + m * 60;
    return 0;
}

// 获取当前时间（含偏移）
int GetCurrentSeconds() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    int secs = st.wHour * 3600 + st.wMinute * 60 + st.wSecond;
    secs += g_timeOffsetSec;
    if (secs < 0) secs = 0;
    return secs;
}

int GetActiveStatusIndex() {
    int now = GetCurrentSeconds() % (24 * 60 * 60);
    if (now < 0) now += 24 * 60 * 60;
    int activeIndex = -1;
    int latestStart = -1;
    for (size_t i = 0; i < statusRecords.size(); ++i) {
        const auto& status = statusRecords[i];
        if (now >= status.activeStartSeconds && now < status.activeEndSeconds &&
            status.activeStartSeconds >= latestStart) {
            activeIndex = static_cast<int>(i);
            latestStart = status.activeStartSeconds;
        }
    }
    return activeIndex;
}

const timetable_shared::StatusRecord* GetActiveStatus() {
    const int index = GetActiveStatusIndex();
    return index >= 0 && static_cast<size_t>(index) < statusRecords.size()
        ? &statusRecords[static_cast<size_t>(index)] : nullptr;
}

int CalculateCountdownDays(const CountdownEvent& eventItem) {
    SYSTEMTIME today = {};
    GetLocalTime(&today);
    today.wHour = today.wMinute = today.wSecond = today.wMilliseconds = 0;
    SYSTEMTIME target = {};
    target.wYear = static_cast<WORD>(eventItem.year);
    target.wMonth = static_cast<WORD>(eventItem.month);
    target.wDay = static_cast<WORD>(eventItem.day);
    FILETIME todayFile = {}, targetFile = {};
    if (!SystemTimeToFileTime(&today, &todayFile) || !SystemTimeToFileTime(&target, &targetFile)) return -1;
    ULARGE_INTEGER todayValue = {}, targetValue = {};
    todayValue.LowPart = todayFile.dwLowDateTime;
    todayValue.HighPart = todayFile.dwHighDateTime;
    targetValue.LowPart = targetFile.dwLowDateTime;
    targetValue.HighPart = targetFile.dwHighDateTime;
    const LONGLONG difference = static_cast<LONGLONG>(targetValue.QuadPart - todayValue.QuadPart);
    return static_cast<int>(difference / 864000000000LL);
}

bool IsTiantianlianCourse(const CourseInfo& course) {
    return course.name.size() >= 3 && course.name.compare(0, 3, L"天天练") == 0;
}

struct CourseTextLayout {
    bool hasBadge = false;
    std::wstring mainGlyph;
    std::wstring badgeGlyph;
    SIZE mainSize = {};
    SIZE badgeSize = {};
    SIZE abbreviationSize = {};
    int contentWidth = 0;
};

CourseTextLayout MeasureCourseText(HDC hdc, const CourseInfo& course) {
    CourseTextLayout layout;
    HFONT oldFont = static_cast<HFONT>(SelectObject(hdc, hFontLarge));
    layout.hasBadge = IsTiantianlianCourse(course) && course.abbreviation.size() >= 2;
    if (layout.hasBadge) {
        layout.mainGlyph = L"练";
        layout.badgeGlyph.assign(1, course.abbreviation[0]);
        GetTextExtentPoint32W(hdc, layout.mainGlyph.c_str(), static_cast<int>(layout.mainGlyph.length()), &layout.mainSize);
        HFONT largeFont = static_cast<HFONT>(SelectObject(hdc, hFontBadge));
        GetTextExtentPoint32W(hdc, layout.badgeGlyph.c_str(), static_cast<int>(layout.badgeGlyph.length()), &layout.badgeSize);
        SelectObject(hdc, largeFont);
        layout.contentWidth = static_cast<int>(layout.mainSize.cx + layout.badgeSize.cx);
    }
    else {
        GetTextExtentPoint32W(hdc, course.abbreviation.c_str(), static_cast<int>(course.abbreviation.length()), &layout.abbreviationSize);
        layout.contentWidth = static_cast<int>(layout.abbreviationSize.cx);
    }
    SelectObject(hdc, oldFont);
    return layout;
}

int GetMorningBoundarySeconds() {
    for (const auto& course : courses) {
        if (course.abbreviation == L"午休") return TimeToSeconds(course.endTime);
    }
    return 12 * 3600 + 30 * 60;
}

void CenterMainWindowOnce() {
    if (!g_hMainWnd || !g_mainNeedsCentering) return;
    RECT windowRect = {};
    GetWindowRect(g_hMainWnd, &windowRect);
    const int width = windowRect.right - windowRect.left;
    const int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    const int x = (screenWidth - width) / 2;
    const int y = ScaleForDpi(50);
    SetWindowPos(g_hMainWnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    g_mainNeedsCentering = false;
}

int GetUpcomingCountdownCount() {
    int count = 0;
    for (const auto& eventItem : g_countdownEvents)
        if (CalculateCountdownDays(eventItem) >= 0) ++count;
    return count;
}

std::wstring BuildMainFooterText() {
    SYSTEMTIME currentTime = {};
    GetLocalTime(&currentTime);
    static const wchar_t* weekdayNames[] = {
        L"星期日", L"星期一", L"星期二", L"星期三",
        L"星期四", L"星期五", L"星期六"
    };
    std::wstring text = std::to_wstring(currentTime.wMonth) + L"月" +
        std::to_wstring(currentTime.wDay) + L"日 " +
        weekdayNames[currentTime.wDayOfWeek] + L"  ";
    bool hasItem = false;
    for (const auto& item : g_footerItems) {
        if (item.isMessage) {
            if (hasItem) text += L"  ";
            text += item.message;
            hasItem = true;
            continue;
        }

        const int days = CalculateCountdownDays(item.countdown);
        if (days >= 0) {
            if (hasItem) text += L"  ";
            text += L"距离" + item.countdown.name + L"：" + std::to_wstring(days) + L"天";
            hasItem = true;
        }
    }
    return text;
}

std::wstring FormatTime(int sec) {
    if (sec <= 0) return L"00:00";
    int m = sec / 60, s = sec % 60;
    if (m >= 60) {
        int h = m / 60; m %= 60;
        wchar_t buf[32];
        swprintf_s(buf, L"%d:%02d:%02d", h, m, s);
        return buf;
    }
    else {
        wchar_t buf[32];
        swprintf_s(buf, L"%02d:%02d", m, s);
        return buf;
    }
}

// 加载课程数据
std::wstring GetTimetableFilePath() {
    SYSTEMTIME st = {};
    GetLocalTime(&st);
    wchar_t dateFile[MAX_PATH];
    swprintf_s(dateFile, L"D:\\Timetable\\Sources\\timetable_%04d-%02d-%02d.csv", st.wYear, st.wMonth, st.wDay);
    WIN32_FILE_ATTRIBUTE_DATA attributes = {};
    if (GetFileAttributesExW(dateFile, GetFileExInfoStandard, &attributes)) return dateFile;

    wchar_t weekdayFile[MAX_PATH];
    swprintf_s(weekdayFile, L"D:\\Timetable\\Sources\\timetable_%s.csv", GetWeekdayName(st.wDayOfWeek).c_str());
    if (GetFileAttributesExW(weekdayFile, GetFileExInfoStandard, &attributes)) return weekdayFile;
    return L"";
}

bool SameCourses(const std::vector<CourseInfo>& left, const std::vector<CourseInfo>& right) {
    if (left.size() != right.size()) return false;
    for (size_t i = 0; i < left.size(); ++i) {
        const CourseInfo& a = left[i];
        const CourseInfo& b = right[i];
        if (a.name != b.name || a.abbreviation != b.abbreviation || a.startTime != b.startTime ||
            a.endTime != b.endTime || a.displayFlag != b.displayFlag ||
            a.weekPattern != b.weekPattern || a.subDisplayFlag != b.subDisplayFlag) return false;
    }
    return true;
}

bool SameStatuses(const std::vector<timetable_shared::StatusRecord>& left,
    const std::vector<timetable_shared::StatusRecord>& right) {
    if (left.size() != right.size()) return false;
    for (size_t i = 0; i < left.size(); ++i)
        if (!timetable_shared::SameStatus(left[i], right[i])) return false;
    return true;
}

bool SameExams(const std::vector<ExamInfo>& left, const std::vector<ExamInfo>& right) {
    if (left.size() != right.size()) return false;
    for (size_t i = 0; i < left.size(); ++i) {
        if (left[i].subject != right[i].subject || left[i].startTime != right[i].startTime ||
            left[i].endTime != right[i].endTime) return false;
    }
    return true;
}

bool LoadTimetable() {
    const std::vector<CourseInfo> previousCourses = courses;
    const std::vector<timetable_shared::StatusRecord> previousStatuses = statusRecords;
    const std::vector<ExamInfo> previousExams = examRows;
    courses.clear();
    statusRecords.clear();
    examRows.clear();
    int currentWeek = GetCurrentWeekNumber();
    const std::wstring timetablePath = GetTimetableFilePath();

    std::vector<std::wstring> lines;
    if (!timetablePath.empty() && ReadTextLinesAuto(timetablePath, lines)) {
        for (std::wstring s : lines) {
            std::vector<std::wstring> parts = ParseCsvRow(s);
            if (parts.empty()) continue;
            if (!parts[0].empty() && parts[0][0] == 0xFEFF)
                parts[0].erase(parts[0].begin());
            if (parts[0] == L"status") {
                timetable_shared::StatusRecord status;
                if (timetable_shared::ParseStatusRow(parts, status)) statusRecords.push_back(status);
                continue;
            }
            if (parts[0] == L"exam") {
                if (parts.size() >= 4) {
                    ExamInfo e;
                    e.subject = parts[1];
                    e.startTime = parts[2];
                    e.endTime = parts[3];
                    e.startSec = TimeToSeconds(e.startTime);
                    e.endSec = TimeToSeconds(e.endTime);
                    if (!e.subject.empty() && e.endSec > e.startSec) examRows.push_back(e);
                }
                continue;
            }
            if (parts[0] == L"课程全称") continue;
            std::vector<std::wstring> courseParts = parts;
            size_t dataOffset = 0;
            if (courseParts[0] == L"course" || courseParts[0] == L"courses") dataOffset = 1;
            else if (courseParts[0] == L"courses=") dataOffset = 1;
            else if (courseParts[0].compare(0, 8, L"courses=") == 0)
                courseParts[0] = courseParts[0].substr(8);
            else if (courseParts.size() >= 6) dataOffset = 0;
            else continue;
            if (courseParts.size() < dataOffset + 6) continue;
            try {
                CourseInfo c;
                c.name = courseParts[dataOffset];
                c.abbreviation = courseParts[dataOffset + 1];
                c.startTime = courseParts[dataOffset + 2];
                c.endTime = courseParts[dataOffset + 3];
                c.displayFlag = std::stoi(courseParts[dataOffset + 4]);
                c.weekPattern = std::stoi(courseParts[dataOffset + 5]);
                c.subDisplayFlag = courseParts.size() > dataOffset + 6 &&
                    !courseParts[dataOffset + 6].empty()
                    ? std::stoi(courseParts[dataOffset + 6]) : 1;
                if (ShouldDisplayCourse(c, currentWeek)) courses.push_back(c);
            }
            catch (...) {
            }
        }
    }

    if (courses.empty()) {
        CourseInfo defaultCourse = { L"空课程", L"空", L"00:00", L"00:00", 1, 3, 1 };
        courses.push_back(defaultCourse);
    }

    std::stable_sort(courses.begin(), courses.end(), [](const CourseInfo& a, const CourseInfo& b) {
        return TimeToSeconds(a.startTime) < TimeToSeconds(b.startTime);
        });

    std::stable_sort(examRows.begin(), examRows.end(), [](const ExamInfo& a, const ExamInfo& b) {
        return a.startSec < b.startSec;
        });

    g_lastTimetablePath = timetablePath;
    g_lastTimetableWeek = currentWeek;
    g_lastTimetableTime = {};
    WIN32_FILE_ATTRIBUTE_DATA attributes = {};
    if (!timetablePath.empty() && GetFileAttributesExW(timetablePath.c_str(), GetFileExInfoStandard, &attributes))
        g_lastTimetableTime = attributes.ftLastWriteTime;
    Log(L"LoadTimetable: loaded " + std::to_wstring(courses.size()) + L" courses");
    return !SameCourses(previousCourses, courses) || !SameStatuses(previousStatuses, statusRecords) || !SameExams(previousExams, examRows);
}

bool CheckTimetableUpdate() {
    const std::wstring timetablePath = GetTimetableFilePath();
    WIN32_FILE_ATTRIBUTE_DATA attributes = {};
    FILETIME writeTime = {};
    if (!timetablePath.empty() && GetFileAttributesExW(timetablePath.c_str(), GetFileExInfoStandard, &attributes))
        writeTime = attributes.ftLastWriteTime;
    if (timetablePath == g_lastTimetablePath && g_lastTimetableWeek == GetCurrentWeekNumber() &&
        CompareFileTime(&writeTime, &g_lastTimetableTime) == 0) return false;
    return LoadTimetable();
}

// 状态判定（含偏移）
int GetCurrentState(int& courseIndex, int& remainingSeconds, std::wstring& courseName) {
    int now = GetCurrentSeconds();
    courseIndex = -1;
    remainingSeconds = 0;
    courseName = L"";

    size_t nextCourseIndex = courses.size();
    int nextCourseStart = 24 * 60 * 60;
    for (size_t i = 0; i < courses.size(); ++i) {
        const int start = TimeToSeconds(courses[i].startTime);
        if (start > now && start < nextCourseStart) {
            nextCourseIndex = i;
            nextCourseStart = start;
        }
    }
    bool suppressPreparation = false;
    if (nextCourseIndex < courses.size()) {
        const CourseInfo& nextCourse = courses[nextCourseIndex];
        if (nextCourse.name == L"午餐") {
            suppressPreparation = true;
        }
        else if (nextCourse.abbreviation == L"自习" && nextCourseIndex > 0) {
            const CourseInfo& previousCourse = courses[nextCourseIndex - 1];
            suppressPreparation = previousCourse.abbreviation == L"自习";
        }
    }
    if (nextCourseIndex < courses.size() && nextCourseStart - now <= 120 &&
        !suppressPreparation) {
        courseIndex = static_cast<int>(nextCourseIndex);
        remainingSeconds = nextCourseStart - now;
        courseName = courses[nextCourseIndex].name;
        return 2;   // 预备优先于当前课程和下课
    }

    for (size_t i = 0; i < courses.size(); ++i) {
        int start = TimeToSeconds(courses[i].startTime);
        int end = TimeToSeconds(courses[i].endTime);
        if (now >= start && now < end) {
            courseIndex = (int)i;
            remainingSeconds = end - now;
            courseName = courses[i].name;
            return 0;   // 上课中
        }
        if (now < start) {
            courseIndex = (int)i;
            remainingSeconds = start - now;
            courseName = courses[i].name;
            if (remainingSeconds <= 120) return 2;   // 预备
            else return 1;                           // 下课/课间
        }
    }
    return 3;   // 放学后
}

// 读取配置文件
void LoadConfig() {
    DisplayMode newMain = MODE_AUTO;
    DisplayMode newSub = MODE_AUTO;
    int newOffset = 0;
    bool newBlackScreen = g_enableBlackScreen;
    bool newUsbAutoOpen = usb_watcher::IsAutoOpenEnabled();
    std::vector<CountdownEvent> newCountdownEvents;
    std::vector<FooterItem> newFooterItems;

    FILE* f = nullptr;
    if (_wfopen_s(&f, CONFIG_PATH, L"r,ccs=UTF-8") == 0 && f) {
        wchar_t line[256];
        while (fgetws(line, 256, f)) {
            std::wstring s(line);
            if (s.find(L"main_mode=") == 0) {
                std::wstring val = s.substr(10);
                val.erase(std::remove(val.begin(), val.end(), L'\r'), val.end());
                val.erase(std::remove(val.begin(), val.end(), L'\n'), val.end());
                if (val == L"always") newMain = MODE_ALWAYS;
                else if (val == L"never") newMain = MODE_NEVER;
                else newMain = MODE_AUTO;
            }
            else if (s.find(L"sub_mode=") == 0) {
                std::wstring val = s.substr(9);
                val.erase(std::remove(val.begin(), val.end(), L'\r'), val.end());
                val.erase(std::remove(val.begin(), val.end(), L'\n'), val.end());
                if (val == L"always") newSub = MODE_ALWAYS;
                else if (val == L"never") newSub = MODE_NEVER;
                else newSub = MODE_AUTO;
            }
            else if (s.find(L"time_offset=") == 0) {
                std::wstring val = s.substr(12);
                val.erase(std::remove(val.begin(), val.end(), L'\r'), val.end());
                val.erase(std::remove(val.begin(), val.end(), L'\n'), val.end());
                newOffset = std::stoi(val);
            }
            else if (s.find(L"black_screen=") == 0) {
                std::wstring val = s.substr(13);
                val.erase(std::remove(val.begin(), val.end(), L'\r'), val.end());
                val.erase(std::remove(val.begin(), val.end(), L'\n'), val.end());
                if (val == L"enable") newBlackScreen = true;
                else if (val == L"disable") newBlackScreen = false;
            }
            else if (s.find(L"autodiak=") == 0) {
                std::wstring val = s.substr(9);
                val.erase(std::remove(val.begin(), val.end(), L'\r'), val.end());
                val.erase(std::remove(val.begin(), val.end(), L'\n'), val.end());
                if (val == L"enable") newUsbAutoOpen = true;
                else if (val == L"disable") newUsbAutoOpen = false;
            }
            else if (s.find(L"countdown=") == 0) {
                std::wstring val = s.substr(10);
                val.erase(std::remove(val.begin(), val.end(), L'\r'), val.end());
                val.erase(std::remove(val.begin(), val.end(), L'\n'), val.end());
                const size_t separator = val.find(L'|');
                if (separator != std::wstring::npos) {
                    CountdownEvent eventItem = {};
                    if (swscanf_s(val.substr(0, separator).c_str(), L"%d-%d-%d", &eventItem.year, &eventItem.month, &eventItem.day) == 3) {
                        eventItem.name = val.substr(separator + 1);
                        if (!eventItem.name.empty()) {
                            newCountdownEvents.push_back(eventItem);
                            newFooterItems.push_back({ false, eventItem, L"" });
                        }
                    }
                }
            }
            else if (s.find(L"message=") == 0) {
                std::wstring message = s.substr(8);
                message.erase(std::remove(message.begin(), message.end(), L'\r'), message.end());
                message.erase(std::remove(message.begin(), message.end(), L'\n'), message.end());
                if (!message.empty()) {
                    newFooterItems.push_back({ true, {}, message });
                }
            }
        }
        fclose(f);
    }

    bool footerChanged = newFooterItems.size() != g_footerItems.size();
    if (!footerChanged) {
        for (size_t i = 0; i < newFooterItems.size(); ++i) {
            const FooterItem& oldItem = g_footerItems[i];
            const FooterItem& newItem = newFooterItems[i];
            if (oldItem.isMessage != newItem.isMessage || oldItem.message != newItem.message ||
                oldItem.countdown.year != newItem.countdown.year ||
                oldItem.countdown.month != newItem.countdown.month ||
                oldItem.countdown.day != newItem.countdown.day ||
                oldItem.countdown.name != newItem.countdown.name) {
                footerChanged = true;
                break;
            }
        }
    }

    if (newMain != g_mainMode || newSub != g_subMode || newOffset != g_timeOffsetSec ||
        newBlackScreen != g_enableBlackScreen || footerChanged ||
        newUsbAutoOpen != usb_watcher::IsAutoOpenEnabled()) {
        g_mainMode = newMain;
        g_subMode = newSub;
        g_timeOffsetSec = newOffset;
        g_enableBlackScreen = newBlackScreen;
        usb_watcher::SetAutoOpenEnabled(newUsbAutoOpen);
        g_countdownEvents = newCountdownEvents;
        g_footerItems = newFooterItems;
        Log(L"配置已更新：main=" + std::to_wstring(g_mainMode) + L" sub=" + std::to_wstring(g_subMode) +
            L" offset=" + std::to_wstring(g_timeOffsetSec) + L" black=" + std::to_wstring(g_enableBlackScreen));
        if (g_hMainWnd) {
            UpdateMainWindowSize();
            InvalidateRect(g_hMainWnd, nullptr, FALSE);
        }
    }
}

// 检查配置文件是否更新
void CheckConfigUpdate() {
    WIN32_FILE_ATTRIBUTE_DATA attr;
    if (GetFileAttributesExW(CONFIG_PATH, GetFileExInfoStandard, &attr)) {
        if (CompareFileTime(&attr.ftLastWriteTime, &g_lastConfigTime) != 0) {
            g_lastConfigTime = attr.ftLastWriteTime;
            LoadConfig();
        }
    }
}

// ==================== 窗口位置 ====================
int CalculateMainWindowWidth() {
    if (const auto* status = GetActiveStatus()) {
        HDC hdc = GetDC(g_hMainWnd);
        if (hdc && hFontLarge) {
            HFONT oldFont = static_cast<HFONT>(SelectObject(hdc, hFontLarge));
            SIZE textSize = {};
            GetTextExtentPoint32W(hdc, status->mainText.c_str(),
                static_cast<int>(status->mainText.length()), &textSize);
            SelectObject(hdc, oldFont);
            ReleaseDC(g_hMainWnd, hdc);
            RECT workArea = {};
            SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
            const int workWidth = MulDiv(workArea.right - workArea.left, 96, static_cast<int>(g_currentDpi));
            const int availableWidth = (std::max)(300, workWidth - 40);
            const int requestedWidth = MulDiv(textSize.cx, 96, static_cast<int>(g_currentDpi)) + 60;
            return (std::max)(300, (std::min)(requestedWidth, availableWidth));
        }
        if (hdc) ReleaseDC(g_hMainWnd, hdc);
    }

    int visibleCount = 0;
    for (const auto& course : courses)
        if (course.displayFlag == 1) ++visibleCount;

    RECT workArea = {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
    const int workWidth = MulDiv(workArea.right - workArea.left, 96, static_cast<int>(g_currentDpi));
    const int availableWidth = (std::max)(300, workWidth - 40);
    const int cellWidth = CalculateCourseCellWidth();
    const int maxColumns = (std::max)(1, (availableWidth - 60) / cellWidth);
    const int columns = (std::max)(1, (std::min)(visibleCount, maxColumns));
    return (std::max)(300, (std::min)(columns * cellWidth + 60, availableWidth));
}

int CalculateCourseCellWidth() {
    HDC hdc = GetDC(g_hMainWnd);
    if (!hdc || !hFontLarge || !hFontBadge) {
        if (hdc) ReleaseDC(g_hMainWnd, hdc);
        return 70;
    }

    HFONT oldFont = static_cast<HFONT>(SelectObject(hdc, hFontLarge));
    int maxTextWidth = 0;
    for (const auto& course : courses) {
        if (course.displayFlag != 1) continue;
        const CourseTextLayout layout = MeasureCourseText(hdc, course);
        maxTextWidth = (std::max)(maxTextWidth, layout.contentWidth);
    }
    SIZE standardGlyph = {};
    GetTextExtentPoint32W(hdc, L"字", 1, &standardGlyph);
    SelectObject(hdc, oldFont);
    ReleaseDC(g_hMainWnd, hdc);
    const int logicalTextWidth = MulDiv(maxTextWidth, 96, static_cast<int>(g_currentDpi));
    const int standardGlyphWidth = MulDiv(standardGlyph.cx, 96, static_cast<int>(g_currentDpi));
    const int textGap = (std::max)(1, (standardGlyphWidth + 1) / 2);
    return (std::max)(1, logicalTextWidth + textGap);
}

int CalculateMainWindowHeight() {
    if (GetActiveStatus()) return 58;

    int visibleCount = 0;
    for (const auto& course : courses)
        if (course.displayFlag == 1) ++visibleCount;

    const int cellWidth = CalculateCourseCellWidth();
    const int columns = (std::max)(1, (CalculateMainWindowWidth() - 60) / cellWidth);
    const int rows = (std::max)(1, (visibleCount + columns - 1) / columns);
    const int footerFontSize = 17;
    const int bottomPadding = (footerFontSize * 2) / 3;
    return 10 + rows * 48 + footerFontSize + bottomPadding;
}

void UpdateMainWindowSize() {
    if (!g_hMainWnd) return;
    int newWidth = ScaleForDpi(CalculateMainWindowWidth());
    int newHeight = ScaleForDpi(CalculateMainWindowHeight());
    RECT rc;
    GetWindowRect(g_hMainWnd, &rc);
    SetWindowPos(g_hMainWnd, NULL, rc.left, rc.top, newWidth, newHeight, SWP_NOZORDER | SWP_NOACTIVATE);
}

void UpdateSubWindowWidth() {
    if (!g_hSubWnd) return;

    int state, idx, remaining;
    std::wstring courseName;
    state = GetCurrentState(idx, remaining, courseName);
    std::wstring mainText, subText;
    COLORREF mainColor, subColor;
    GetSubDisplayData(state, remaining, courseName, mainText, subText, mainColor, subColor);

    HDC hdc = GetDC(g_hSubWnd);
    if (!hdc) return;
    HFONT oldFont = (HFONT)SelectObject(hdc, hFontSub);
    SIZE mainSize = { 0, 0 };
    SIZE subSize = { 0, 0 };
    TEXTMETRICW metrics = {};
    GetTextExtentPoint32W(hdc, mainText.c_str(), (int)mainText.length(), &mainSize);
    GetTextExtentPoint32W(hdc, subText.c_str(), (int)subText.length(), &subSize);
    GetTextMetricsW(hdc, &metrics);
    SelectObject(hdc, oldFont);
    ReleaseDC(g_hSubWnd, hdc);

    const int paddingX = ScaleForDpi(24);
    const int gap = subText.empty() ? 0 : ScaleForDpi(18);
    const int textHeight = (std::max)({ static_cast<int>(mainSize.cy), static_cast<int>(subSize.cy), static_cast<int>(metrics.tmHeight) });
    const int paddingY = (textHeight + 2) / 3;
    const int requestedWidth = static_cast<int>(mainSize.cx) + static_cast<int>(subSize.cx) + gap + paddingX * 2;
    SUB_WINDOW_WIDTH = (std::min)((std::max)(ScaleForDpi(120), requestedWidth), ScaleForDpi(1200));
    const int requestedHeight = textHeight + paddingY * 2;
    SUB_WINDOW_HEIGHT = (std::min)((std::max)(ScaleForDpi(44), requestedHeight), ScaleForDpi(240));
    SetWindowPos(g_hSubWnd, NULL, 0, 0, SUB_WINDOW_WIDTH, SUB_WINDOW_HEIGHT, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}


void UpdateSubWindowPosition() {
    if (g_hMainWnd && g_hSubWnd) {
        RECT rc;
        GetWindowRect(g_hMainWnd, &rc);
        SetWindowPos(g_hSubWnd, NULL, rc.left, rc.bottom, SUB_WINDOW_WIDTH, SUB_WINDOW_HEIGHT, SWP_NOZORDER);
    }
}

void MoveSubWindowToRight() {
    if (!g_hSubWnd) return;
    int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    int x = screenWidth * 3 / 4;

    // 检查当前是不是上课或预备状态，并判断缩写是否为“自习”
    bool isZixi = false;
    int state, idx, remaining;
    std::wstring courseName;
    state = GetCurrentState(idx, remaining, courseName);
    if ((state == 0 || state == 2) && idx >= 0 && static_cast<size_t>(idx) < courses.size()) {
        if (courses[idx].abbreviation == L"自习") {
            isZixi = true;
        }
    }

    if (isZixi) {
        x = screenWidth / 8; // 左侧八分之一对齐
    }

    int y = ScaleForDpi(60);
    SetWindowPos(g_hSubWnd, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    g_subAtRight = true; // 复用该标记表示处于“上课特定的非下方位置”
    Log(L"子窗口已移至上课/自习位置");
}

void ResetSubWindowPosition() {
    if (!g_hSubWnd) return;
    UpdateSubWindowPosition();  // 跟随主窗口下方
    g_subAtRight = false;
    Log(L"子窗口已还原至主窗口下方");
}

// ==================== 绘制 ====================
void DrawMainTimetable(HDC hdc, RECT clientRect) {
    HBRUSH bg = CreateSolidBrush(BG_COLOR);
    FillRect(hdc, &clientRect, bg);
    DeleteObject(bg);

    if (const auto* status = GetActiveStatus()) {
        HFONT oldStatusFont = static_cast<HFONT>(SelectObject(hdc, hFontLarge));
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, TEXT_COLOR);
        DrawTextW(hdc, status->mainText.c_str(), -1, &clientRect,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        SelectObject(hdc, oldStatusFont);
        return;
    }

    HFONT oldFont = (HFONT)SelectObject(hdc, hFontLarge);
    SetBkMode(hdc, TRANSPARENT);
    const int currentSeconds = GetCurrentSeconds();
    const int currentMin = currentSeconds / 60;
    const int marginX = ScaleForDpi(30);
    const int top = ScaleForDpi(10);
    const int cellWidth = ScaleForDpi(CalculateCourseCellWidth());
    const int rowHeight = ScaleForDpi(48);
    const int textHeight = ScaleForDpi(40);
    const int logicalCellWidth = CalculateCourseCellWidth();
    const int columns = (std::max)(1, (CalculateMainWindowWidth() - 60) / logicalCellWidth);

    int nextIdx = -1;
    for (size_t i = 0; i < courses.size(); ++i) {
        if (courses[i].displayFlag == 1 && currentSeconds < TimeToSeconds(courses[i].startTime)) {
            nextIdx = (int)i;
            break;
        }
    }

    const int morningBoundary = GetMorningBoundarySeconds();
    int separatorPreviousIndex = -1;
    int separatorNextIndex = -1;
    int visibleCourseCount = 0;
    int previousVisibleIndex = -1;
    for (size_t i = 0; i < courses.size(); ++i) {
        if (courses[i].displayFlag == 0) continue;
        if (TimeToSeconds(courses[i].startTime) > morningBoundary) {
            if (previousVisibleIndex >= 0 && visibleCourseCount % columns > 0) {
                separatorPreviousIndex = previousVisibleIndex;
                separatorNextIndex = static_cast<int>(i);
            }
            break;
        }
        previousVisibleIndex = static_cast<int>(i);
        ++visibleCourseCount;
    }

    const int separatorGap = ScaleForDpi(46) / 3;
    const COLORREF separatorColor = currentSeconds < morningBoundary ? TEXT_COLOR : DISABLED_COLOR;
    int visibleIndex = 0;
    for (size_t i = 0; i < courses.size(); ++i) {
        if (courses[i].displayFlag == 0) continue;
        int startMin = TimeToSeconds(courses[i].startTime) / 60;
        int endMin = TimeToSeconds(courses[i].endTime) / 60;
        COLORREF color;
        if (currentMin > endMin) color = DISABLED_COLOR;
        else if (currentMin >= startMin && currentMin <= endMin) color = CURRENT_COLOR;
        else if ((int)i == nextIdx) color = NEXT_COLOR;
        else color = TEXT_COLOR;

        const int column = visibleIndex % columns;
        const int row = visibleIndex / columns;
        const int x = marginX + column * cellWidth;
        const int y = top + row * rowHeight;
        const bool beforeSeparator = static_cast<int>(i) == separatorPreviousIndex;
        const bool afterSeparator = static_cast<int>(i) == separatorNextIndex;
        if (afterSeparator) {
            const int separatorX = x - 1;
            HPEN separatorPen = CreatePen(PS_SOLID, ScaleForDpi(2), separatorColor);
            HPEN oldPen = static_cast<HPEN>(SelectObject(hdc, separatorPen));
            MoveToEx(hdc, separatorX, y + ScaleForDpi(7), nullptr);
            LineTo(hdc, separatorX, y + textHeight - ScaleForDpi(7));
            SelectObject(hdc, oldPen);
            DeleteObject(separatorPen);
        }
        SetTextColor(hdc, color);
        const CourseTextLayout layout = MeasureCourseText(hdc, courses[i]);
        const int contentWidth = layout.contentWidth;

        int contentLeft = x + (cellWidth - contentWidth) / 2;
        if (beforeSeparator) contentLeft = x + cellWidth - separatorGap - 1 - contentWidth;
        else if (afterSeparator) contentLeft = x + separatorGap + 1;
        if (layout.hasBadge) {
            RECT mainGlyphRect = { contentLeft, y, contentLeft + layout.mainSize.cx, y + textHeight };
            DrawTextW(hdc, layout.mainGlyph.c_str(), static_cast<int>(layout.mainGlyph.length()), &mainGlyphRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            SelectObject(hdc, hFontBadge);
            RECT badgeRect = { contentLeft + layout.mainSize.cx, y, contentLeft + contentWidth, y + textHeight };
            DrawTextW(hdc, layout.badgeGlyph.c_str(), static_cast<int>(layout.badgeGlyph.length()), &badgeRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            SelectObject(hdc, hFontLarge);
        }
        else {
            RECT abbreviationRect = { contentLeft, y, contentLeft + contentWidth, y + textHeight };
            DrawTextW(hdc, courses[i].abbreviation.c_str(), -1, &abbreviationRect,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
        ++visibleIndex;
    }

    int footerLeft = marginX;
    for (const auto& course : courses) {
        if (course.displayFlag == 1) {
            SIZE firstCourseSize = { 0, 0 };
            SelectObject(hdc, hFontLarge);
            const CourseTextLayout layout = MeasureCourseText(hdc, course);
            firstCourseSize.cx = layout.contentWidth;
            footerLeft += (std::max)(0, (cellWidth - static_cast<int>(firstCourseSize.cx)) / 2);
            break;
        }
    }

    SelectObject(hdc, hFontSmall);
    SetTextColor(hdc, RGB(255, 255, 0));
    const std::wstring info = BuildMainFooterText();
    const int usedRows = (std::max)(1, (visibleIndex + columns - 1) / columns);
    const int footerTop = top + usedRows * rowHeight;
    RECT textRect = { footerLeft, footerTop, clientRect.right - marginX, clientRect.bottom - ScaleForDpi(4) };
    DrawTextW(hdc, info.c_str(), -1, &textRect, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(hdc, oldFont);
}

void GetSubDisplayData(int state, int remaining, const std::wstring& courseName,
    std::wstring& mainText, std::wstring& subText, COLORREF& mainColor, COLORREF& subColor) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    bool reading = (st.wHour == 7 && st.wMinute >= 8 && st.wMinute < 10);
    bool lis = (st.wHour == 17 && st.wMinute >= 00 && st.wMinute < 59);

    mainColor = COURSE_COLOR;
    subColor = TIMER_COLOR;

    if (const auto* status = GetActiveStatus()) {
        int now = GetCurrentSeconds() % (24 * 60 * 60);
        if (now < 0) now += 24 * 60 * 60;
        int countdownDeadline = status->countdownEndSeconds;
        if (countdownDeadline < status->activeStartSeconds) countdownDeadline += 24 * 60 * 60;
        mainText = status->subText;
        subText = FormatTime((std::max)(0, countdownDeadline - now));
    }
    else if (reading) {
        mainText = L"早读";
        subText = L"进行中";
        mainColor = WARNING_COLOR;
    }
    //if (lis) {
    //    mainText = L"家长会";
    //    subText = L"进行中";
    //    mainColor = WARNING_COLOR;
    //}
    else if (state == 0) {
        mainText = courseName;
        subText = FormatTime(remaining);
    }
    else if (state == 1) {
        const int currentSeconds = GetCurrentSeconds();
        const bool isWeekday = st.wDayOfWeek >= 1 && st.wDayOfWeek <= 5;
        const bool inLongBreakWindow = isWeekday && remaining >= 15 * 60 && (
            (currentSeconds >= 8 * 3600 + 10 * 60 && currentSeconds < 9 * 3600 + 50 * 60) ||
            (currentSeconds >= 14 * 3600 + 40 * 60 && currentSeconds < 15 * 3600 + 10 * 60) ||
            (currentSeconds >= 16 * 3600 + 20 * 60 && currentSeconds < 16 * 3600 + 50 * 60));
        mainText = inLongBreakWindow ? L"大课间" : L"下课";
        subText = FormatTime(remaining);
        mainColor = WARNING_COLOR;
    }
    else if (state == 2) {
        mainText = L"预备 " + courseName;
        subText = FormatTime(remaining);
        mainColor = COURSE_COLOR; // 课程名称保持浅蓝色
    }
    else {
        wchar_t buf[20];
        swprintf_s(buf, L"%02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond);
        mainText = L"放学时间";
        subText = buf;
    }
}

void DrawSubTimetable(HDC hdc, RECT clientRect) {
    HBRUSH background = CreateSolidBrush(BG_COLOR);
    FillRect(hdc, &clientRect, background);
    DeleteObject(background);

    int state, idx, remaining;
    std::wstring courseName;
    state = GetCurrentState(idx, remaining, courseName);
    std::wstring mainText, subText;
    COLORREF mainColor, subColor;
    GetSubDisplayData(state, remaining, courseName, mainText, subText, mainColor, subColor);

    HFONT oldFont = (HFONT)SelectObject(hdc, hFontSub);
    SetBkMode(hdc, TRANSPARENT);
    SIZE mainSize = { 0, 0 };
    SIZE subSize = { 0, 0 };
    GetTextExtentPoint32W(hdc, mainText.c_str(), (int)mainText.length(), &mainSize);
    GetTextExtentPoint32W(hdc, subText.c_str(), (int)subText.length(), &subSize);
    const int gap = subText.empty() ? 0 : ScaleForDpi(18);
    const int totalWidth = mainSize.cx + gap + subSize.cx;
    int x = clientRect.left + ((clientRect.right - clientRect.left) - totalWidth) / 2;

    RECT mainRect = { x, clientRect.top, x + mainSize.cx, clientRect.bottom };
    if (state == 2 && !GetActiveStatus()) {
        const std::wstring preparationText = L"预备 ";
        SIZE preparationSize = { 0, 0 };
        GetTextExtentPoint32W(hdc, preparationText.c_str(), static_cast<int>(preparationText.length()), &preparationSize);

        RECT preparationRect = { x, clientRect.top, x + preparationSize.cx, clientRect.bottom };
        SetTextColor(hdc, WARNING_COLOR);
        DrawTextW(hdc, preparationText.c_str(), -1, &preparationRect,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

        RECT courseRect = { x + preparationSize.cx, clientRect.top, x + mainSize.cx, clientRect.bottom };
        SetTextColor(hdc, mainColor);
        DrawTextW(hdc, courseName.c_str(), -1, &courseRect,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    else {
        SetTextColor(hdc, mainColor);
        DrawTextW(hdc, mainText.c_str(), -1, &mainRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    x += mainSize.cx + gap;
    if (!subText.empty()) {
        RECT subRect = { x, clientRect.top, x + subSize.cx, clientRect.bottom };
        SetTextColor(hdc, subColor);
        DrawTextW(hdc, subText.c_str(), -1, &subRect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    SelectObject(hdc, oldFont);
}

void RenderLayeredWindow(HWND hWnd, bool drawMainWindow) {
    RECT clientRect = {};
    GetClientRect(hWnd, &clientRect);
    const int width = clientRect.right - clientRect.left;
    const int height = clientRect.bottom - clientRect.top;
    if (width <= 0 || height <= 0) return;

    HDC screenDc = GetDC(nullptr);
    HDC memoryDc = CreateCompatibleDC(screenDc);
    BITMAPINFO bitmapInfo = {};
    bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmapInfo.bmiHeader.biWidth = width;
    bitmapInfo.bmiHeader.biHeight = -height;
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;
    void* bitmapBits = nullptr;
    HBITMAP bitmap = CreateDIBSection(screenDc, &bitmapInfo, DIB_RGB_COLORS, &bitmapBits, nullptr, 0);
    if (!memoryDc || !bitmap || !bitmapBits) {
        if (bitmap) DeleteObject(bitmap);
        if (memoryDc) DeleteDC(memoryDc);
        ReleaseDC(nullptr, screenDc);
        return;
    }

    HGDIOBJ oldBitmap = SelectObject(memoryDc, bitmap);
    if (drawMainWindow) DrawMainTimetable(memoryDc, clientRect);
    else DrawSubTimetable(memoryDc, clientRect);

    const BYTE backgroundRed = GetRValue(BG_COLOR);
    const BYTE backgroundGreen = GetGValue(BG_COLOR);
    const BYTE backgroundBlue = GetBValue(BG_COLOR);
    BYTE* pixels = static_cast<BYTE*>(bitmapBits);
    for (int pixelIndex = 0; pixelIndex < width * height; ++pixelIndex) {
        BYTE* pixel = pixels + pixelIndex * 4;
        const bool isBackground = pixel[0] == backgroundBlue && pixel[1] == backgroundGreen && pixel[2] == backgroundRed;
        const BYTE alpha = isBackground ? WINDOW_BACKGROUND_ALPHA : WINDOW_TEXT_ALPHA;
        pixel[0] = static_cast<BYTE>((static_cast<unsigned int>(pixel[0]) * alpha + 127) / 255);
        pixel[1] = static_cast<BYTE>((static_cast<unsigned int>(pixel[1]) * alpha + 127) / 255);
        pixel[2] = static_cast<BYTE>((static_cast<unsigned int>(pixel[2]) * alpha + 127) / 255);
        pixel[3] = alpha;
    }

    RECT windowRect = {};
    GetWindowRect(hWnd, &windowRect);
    POINT destination = { windowRect.left, windowRect.top };
    POINT source = { 0, 0 };
    SIZE size = { width, height };
    BLENDFUNCTION blend = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(hWnd, screenDc, &destination, &size, memoryDc, &source, 0, &blend, ULW_ALPHA);

    SelectObject(memoryDc, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(memoryDc);
    ReleaseDC(nullptr, screenDc);
}


LRESULT CALLBACK MainWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        g_currentDpi = GetDpiForWindow(hWnd);
        RebuildFonts(g_currentDpi);
        ApplyRoundedCorners(hWnd);
        LoadConfig();
        LoadTimetable();
        UpdateMainWindowSize();
        SetTimer(hWnd, 1, 250, NULL);
        SetTimer(hWnd, 2, 5000, NULL);   // 每5秒检查配置文件
        Log(L"主窗口创建成功");
        break;

    case WM_DPICHANGED: {
        const UINT newDpi = HIWORD(wParam);
        const RECT* suggestedRect = reinterpret_cast<const RECT*>(lParam);
        RebuildFonts(newDpi);
        SetWindowPos(hWnd, NULL, suggestedRect->left, suggestedRect->top,
            suggestedRect->right - suggestedRect->left,
            suggestedRect->bottom - suggestedRect->top,
            SWP_NOZORDER | SWP_NOACTIVATE);
        UpdateMainWindowSize();
        UpdateSubWindowWidth();
        if (!g_subManualMoved) UpdateSubWindowPosition();
        InvalidateRect(hWnd, NULL, TRUE);
        if (g_hSubWnd) InvalidateRect(g_hSubWnd, NULL, TRUE);
        return 0;
    }

    case WM_TIMER:
        if (!g_launcherVisibilityEnabled && wParam == 1) return 0;
        if (wParam == 2) {
            CheckConfigUpdate();
            return 0;
        }
        // wParam == 1
        {
            SYSTEMTIME currentClock = {};
            GetLocalTime(&currentClock);
            const bool timetableChanged = CheckTimetableUpdate();
            // 考试窗口：当前时间段是 exam 行才显示，显示时主/子窗口隐藏
            if (UpdateExamWindow()) break;
            if (timetableChanged) {
                UpdateMainWindowSize();
                UpdateSubWindowWidth();
                if (g_hMainWnd) InvalidateRect(g_hMainWnd, NULL, FALSE);
                if (g_hSubWnd) InvalidateRect(g_hSubWnd, NULL, FALSE);
            }
            if (g_lastSecond == currentClock.wSecond && !timetableChanged) break;
            const bool minuteChanged = g_lastMinute != currentClock.wMinute;
            g_lastSecond = currentClock.wSecond;
            g_lastMinute = currentClock.wMinute;
            bool oldInClass = g_wasInClass;

            int state, idx, rem;
            std::wstring name;
            state = GetCurrentState(idx, rem, name);
            const int activeStatusIndex = GetActiveStatusIndex();
            const auto* activeStatus = GetActiveStatus();
            const bool statusChanged = activeStatusIndex != g_lastActiveStatusIndex;
            if (statusChanged) {
                g_lastActiveStatusIndex = activeStatusIndex;
                UpdateMainWindowSize();
                UpdateSubWindowWidth();
                if (g_hMainWnd) InvalidateRect(g_hMainWnd, NULL, FALSE);
                if (g_hSubWnd) InvalidateRect(g_hSubWnd, NULL, FALSE);
            }
            bool nowInClass = (state == 0);
            int nowSec = GetCurrentSeconds();

            // 位置管理：上课/预备 → 子窗口到右侧；下课/放学 → 主窗口下方
            bool isInClassOrPrep = !activeStatus && (state == 0 || state == 2);

            // 状态切换至上課/預備時重置手动标记，以便新课自动移右侧
            if (isInClassOrPrep && !g_wasInClassOrPrep) {
                g_subManualMoved = false;
            }

            if (isInClassOrPrep) {
                // 上课/预备：只有未手动移动且不在右侧时才自动移动到右侧
                if (!g_subManualMoved && !g_subAtRight) {
                    MoveSubWindowToRight();
                }
            }
            else {
                // 下课/放学：强制将子窗口置于主窗口下方（无论之前在哪里）
                ResetSubWindowPosition();
            }

            g_wasInClassOrPrep = isInClassOrPrep;

            // 主窗口显隐（根据显示模式）
            bool showMain = false;
            if (activeStatus) showMain = activeStatus->showMain;
            else if (g_mainMode == MODE_ALWAYS) showMain = true;
            else if (g_mainMode == MODE_NEVER) showMain = false;
            else {  // 自动
                if (state == 0 || state == 2) showMain = false;
                else if (state == 1) {
                    // 下课延时60秒
                    if (oldInClass && !nowInClass) g_classEndTime = nowSec;
                    showMain = (nowSec - g_classEndTime >= 60);
                }
                else showMain = true;  // 放学
            }

            bool mainVisible = (GetWindowLongPtr(g_hMainWnd, GWL_STYLE) & WS_VISIBLE) != 0;
            if (showMain && !mainVisible) {
                g_mainNeedsCentering = true;
                CenterMainWindowOnce();
                ShowWindow(g_hMainWnd, SW_SHOW);
                InvalidateRect(g_hMainWnd, NULL, FALSE);
            }
            else if (!showMain && mainVisible) ShowWindow(g_hMainWnd, SW_HIDE);

            // 子窗口显隐
            bool showSub = false;
            if (activeStatus) showSub = activeStatus->showSub;
            else if (g_subMode == MODE_ALWAYS) showSub = true;
            else if (g_subMode == MODE_NEVER) showSub = false;
            else showSub = !(idx >= 0 && static_cast<size_t>(idx) < courses.size() && courses[idx].subDisplayFlag == 0);

            bool subVisible = IsWindowVisible(g_hSubWnd);
            if (showSub && !subVisible) ShowWindow(g_hSubWnd, SW_SHOW);
            else if (!showSub && subVisible) ShowWindow(g_hSubWnd, SW_HIDE);

            // 黑屏午休逻辑
            bool nowWuxiu = !activeStatus && state == 0 && idx >= 0 &&
                static_cast<size_t>(idx) < courses.size() && courses[idx].abbreviation == L"午休";
            if (!nowWuxiu) {
                g_blackScreenDismissed = false; // 非午休时重置手动关闭标记
                if (g_hBlackWnd && IsWindowVisible(g_hBlackWnd)) {
                    ShowWindow(g_hBlackWnd, SW_HIDE);
                }
            } else {
                // 午休开始三分钟后才允许黑屏。
                const int lunchStartSec = TimeToSeconds(courses[idx].startTime);
                const bool lunchDelayElapsed = nowSec >= lunchStartSec + 3 * 60;
                if (g_enableBlackScreen && !g_blackScreenDismissed && lunchDelayElapsed) {
                    if (g_hBlackWnd && !IsWindowVisible(g_hBlackWnd)) {
                        int sw = GetSystemMetrics(SM_CXSCREEN);
                        int sh = GetSystemMetrics(SM_CYSCREEN);
                        SetWindowPos(g_hBlackWnd, HWND_TOPMOST, 0, 0, sw, sh, SWP_SHOWWINDOW);
                    }
                } else if (g_hBlackWnd && IsWindowVisible(g_hBlackWnd)) {
                    ShowWindow(g_hBlackWnd, SW_HIDE);
                }
            }

            // 重绘优化
            std::wstring curMainText, curSubText;
            COLORREF tmp1, tmp2;
            GetSubDisplayData(state, rem, name, curMainText, curSubText, tmp1, tmp2);
            const bool subChanged = curMainText != g_lastMainText || curSubText != g_lastSubText ||
                rem != g_lastRemaining || state != g_lastState ||
                tmp1 != g_lastMainColor || tmp2 != g_lastSubColor || statusChanged;
            const bool mainChanged = minuteChanged || timetableChanged || state != g_lastState || statusChanged;
            if (subChanged) {
                g_lastMainText = curMainText;
                g_lastSubText = curSubText;
                g_lastRemaining = rem;
                g_lastState = state;
                g_lastMainColor = tmp1;
                g_lastSubColor = tmp2;
                UpdateSubWindowWidth();
                if (g_hSubWnd) InvalidateRect(g_hSubWnd, NULL, FALSE);
            }
            if (mainChanged) InvalidateRect(hWnd, NULL, FALSE);
            g_wasInClass = nowInClass;
        }
        break;

    case WM_LBUTTONDOWN:
        isDragging = true;
        dragStartPoint.x = LOWORD(lParam);
        dragStartPoint.y = HIWORD(lParam);
        GetCursorPos(&windowStartPoint);
        SetCapture(hWnd);
        break;

    case WM_MOUSEMOVE:
        if (isDragging) {
            POINT cur;
            GetCursorPos(&cur);
            RECT rc;
            GetWindowRect(hWnd, &rc);
            SetWindowPos(hWnd, NULL, rc.left + cur.x - windowStartPoint.x,
                rc.top + cur.y - windowStartPoint.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
            windowStartPoint = cur;
        }
        break;

    case WM_LBUTTONUP:
        if (isDragging) { isDragging = false; ReleaseCapture(); }
        break;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hWnd, &ps);
        EndPaint(hWnd, &ps);
        RenderLayeredWindow(hWnd, true);
        break;
    }

    case WM_TIMETABLE_SHOW:
        g_launcherVisibilityEnabled = true;
        ShowWindow(g_hMainWnd, SW_SHOW);
        ShowWindow(g_hSubWnd, SW_SHOW);
        InvalidateRect(g_hMainWnd, nullptr, FALSE);
        InvalidateRect(g_hSubWnd, nullptr, FALSE);
        return 0;
    case WM_TIMETABLE_HIDE:
        g_launcherVisibilityEnabled = false;
        ShowWindow(g_hMainWnd, SW_HIDE);
        ShowWindow(g_hSubWnd, SW_HIDE);
        if (g_hBlackWnd) ShowWindow(g_hBlackWnd, SW_HIDE);
        HideExamWindow();  // 关闭课表时一并关闭考试窗口（只隐藏，进程不退出）
        return 0;
    case WM_TIMETABLE_RELOAD:
        LoadTimetable();
        UpdateMainWindowSize();
        UpdateSubWindowWidth();
        g_launcherVisibilityEnabled = true;
        ShowWindow(g_hMainWnd, SW_SHOW);
        ShowWindow(g_hSubWnd, SW_SHOW);
        InvalidateRect(g_hMainWnd, nullptr, FALSE);
        InvalidateRect(g_hSubWnd, nullptr, FALSE);
        return 0;
    case WM_DESTROY:
        if (g_hExamWnd) DestroyWindow(g_hExamWnd);
        KillTimer(hWnd, 1);
        KillTimer(hWnd, 2);
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProc(hWnd, msg, wParam, lParam);
    }
    return 0;
}

LRESULT CALLBACK SubWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        ApplyRoundedCorners(hWnd);
        break;

    case WM_DPICHANGED: {
        const UINT newDpi = HIWORD(wParam);
        const RECT* suggestedRect = reinterpret_cast<const RECT*>(lParam);
        RebuildFonts(newDpi);
        SetWindowPos(hWnd, NULL, suggestedRect->left, suggestedRect->top,
            suggestedRect->right - suggestedRect->left,
            suggestedRect->bottom - suggestedRect->top,
            SWP_NOZORDER | SWP_NOACTIVATE);
        UpdateSubWindowWidth();
        InvalidateRect(hWnd, NULL, TRUE);
        return 0;
    }

    case WM_LBUTTONDOWN:
        isSubDragging = true;
        subDragStartPoint.x = LOWORD(lParam);
        subDragStartPoint.y = HIWORD(lParam);
        GetCursorPos(&subWindowStartPoint);
        SetCapture(hWnd);
        break;

    case WM_MOUSEMOVE:
        if (isSubDragging) {
            POINT cur;
            GetCursorPos(&cur);
            RECT rc;
            GetWindowRect(hWnd, &rc);
            SetWindowPos(hWnd, NULL, rc.left + cur.x - subWindowStartPoint.x,
                rc.top + cur.y - subWindowStartPoint.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
            subWindowStartPoint = cur;
            g_subAtRight = false;      // 移开右侧，清除右侧标记
            g_subManualMoved = true;   // 标记用户已手动移动
        }
        break;

    case WM_LBUTTONUP:
        if (isSubDragging) { isSubDragging = false; ReleaseCapture(); }
        break;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hWnd, &ps);
        EndPaint(hWnd, &ps);
        RenderLayeredWindow(hWnd, false);
        break;
    }

    case WM_DESTROY:
        break;

    default:
        return DefWindowProc(hWnd, msg, wParam, lParam);
    }
    return 0;
}

LRESULT CALLBACK BlackWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_LBUTTONDOWN:
        isBlackDragging = true;
        blackDragStartPoint.x = LOWORD(lParam);
        blackDragStartPoint.y = HIWORD(lParam);
        GetCursorPos(&blackWindowStartPoint);
        SetCapture(hWnd);
        break;

    case WM_MOUSEMOVE:
        if (isBlackDragging) {
            POINT cur;
            GetCursorPos(&cur);
            RECT rc;
            GetWindowRect(hWnd, &rc);
            SetWindowPos(hWnd, NULL, rc.left + cur.x - blackWindowStartPoint.x,
                rc.top + cur.y - blackWindowStartPoint.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
            blackWindowStartPoint = cur;
        }
        break;

    case WM_LBUTTONUP:
        if (isBlackDragging) { isBlackDragging = false; ReleaseCapture(); }
        break;

    case WM_RBUTTONUP:
        g_blackScreenDismissed = true; // 右击关闭后本节课不再自动弹出
        ShowWindow(hWnd, SW_HIDE);
        break;

    default:
        return DefWindowProc(hWnd, msg, wParam, lParam);
    }
    return 0;
}

// ==================== 考试窗口 ====================
// 来源：源程序 ExamDisplay.cpp，按当前 CSV 读写逻辑重写。
// 数据来源：LoadTimetable() 解析出的 examRows（exam,科目,开始HH:MM,结束HH:MM），
// 不再另读 timetable_*.txt，也不再用 mode:exam / exam: 这种旧格式。
// 显示窗口：[开始-20分钟, 结束+5分钟)，其它时间一律不显示。
// 修复了源程序的两个毛病：①“还有15分钟结束”原来只显示 1 分钟（end-15 到 end-14），
// 现在是 [end-15分钟, end) 整段显示；②结束后不再 PostQuitMessage，只隐藏窗口。
const wchar_t* EXAM_CLASS_NAME = L"ExamDisplayApp";
const COLORREF EXAM_BG_COLOR = RGB(0, 0, 0);
const COLORREF EXAM_TEXT_WHITE = RGB(255, 255, 255);
const COLORREF EXAM_TEXT_BLUE = RGB(0, 150, 255);
const COLORREF EXAM_TEXT_YELLOW = RGB(255, 255, 0);
const COLORREF EXAM_TEXT_RED = RGB(255, 0, 0);
const UINT_PTR EXAM_TIMER_ID = 7001;

bool g_examWindowVisible = false;
int g_examDismissedEndSec = -1;  // 手动关闭的本场考试的结束秒数，本场内不再自动重开
int g_examCurrentEndSec = -1;    // 当前正在显示的那场考试的结束秒数

enum ExamPhase {
    EXAM_PHASE_NONE = -1,
    EXAM_PHASE_ENTER = 0,       // 考前20~15分钟：考试进入考场
    EXAM_PHASE_INVIGILATOR = 1, // 考前15~5分钟：监考员进入考场
    EXAM_PHASE_PAPER = 2,       // 考前5~0分钟：依次分发答题卡、试卷
    EXAM_PHASE_RUNNING = 3,     // 考试中
    EXAM_PHASE_LAST15 = 4,      // 结束前15分钟：本场考试还有15分钟结束
    EXAM_PHASE_FINISHED = 5     // 结束后5分钟内：考试结束
};

// 找到当前时间段对应的考试行；phase 输出所处阶段
const ExamInfo* GetActiveExam(int nowSec, int& phase) {
    phase = EXAM_PHASE_NONE;
    for (const auto& e : examRows) {
        if (nowSec >= e.startSec - 20 * 60 && nowSec < e.endSec + 5 * 60) {
            if (nowSec < e.startSec - 15 * 60) phase = EXAM_PHASE_ENTER;
            else if (nowSec < e.startSec - 5 * 60) phase = EXAM_PHASE_INVIGILATOR;
            else if (nowSec < e.startSec) phase = EXAM_PHASE_PAPER;
            else if (nowSec < e.endSec - 15 * 60) phase = EXAM_PHASE_RUNNING;
            else if (nowSec < e.endSec) phase = EXAM_PHASE_LAST15;
            else phase = EXAM_PHASE_FINISHED;
            return &e;
        }
    }
    return nullptr;
}

void HideExamWindow() {
    if (g_hExamWnd) {
        KillTimer(g_hExamWnd, EXAM_TIMER_ID);
        ShowWindow(g_hExamWnd, SW_HIDE);
    }
    g_examWindowVisible = false;
}

void ShowExamWindow() {
    if (!g_hExamWnd) return;
    ShowWindow(g_hExamWnd, SW_MAXIMIZE);
    SetTimer(g_hExamWnd, EXAM_TIMER_ID, 1000, nullptr);
    g_examWindowVisible = true;
    InvalidateRect(g_hExamWnd, nullptr, TRUE);
}

// 每秒由主窗口定时器调用：决定考试窗口的显隐。
// 返回 true 表示考试窗口正在显示（主窗口定时器应跳过主/子窗口的显隐逻辑）。
bool UpdateExamWindow() {
    const int nowSec = GetCurrentSeconds();
    int phase = EXAM_PHASE_NONE;
    const ExamInfo* active = GetActiveExam(nowSec, phase);
    if (active && g_examDismissedEndSec == active->endSec) active = nullptr;  // 本场已手动关闭
    if (active) {
        g_examCurrentEndSec = active->endSec;
        if (!g_examWindowVisible) {
            ShowExamWindow();
            if (g_hMainWnd) ShowWindow(g_hMainWnd, SW_HIDE);
            if (g_hSubWnd) ShowWindow(g_hSubWnd, SW_HIDE);
            if (g_hBlackWnd) ShowWindow(g_hBlackWnd, SW_HIDE);
        }
        return true;
    }
    if (g_examWindowVisible) {
        HideExamWindow();
        g_examCurrentEndSec = -1;
        g_lastSecond = -1;  // 强制下一拍重新计算主/子窗口显隐
    }
    return false;
}

void DrawExamTextCentered(HDC hdc, const wchar_t* text, int y, int fontSize, COLORREF color) {
    HFONT hFont = CreateFontW(fontSize, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
    HFONT oldFont = (HFONT)SelectObject(hdc, hFont);
    SetTextColor(hdc, color);
    SetBkMode(hdc, TRANSPARENT);
    RECT rc;
    GetClientRect(g_hExamWnd, &rc);
    RECT textRect = { 0, y, rc.right, y + fontSize + 10 };
    DrawTextW(hdc, text, -1, &textRect, DT_CENTER | DT_TOP | DT_SINGLELINE);
    SelectObject(hdc, oldFont);
    DeleteObject(hFont);
}

void DrawExamScreen(HDC hdc) {
    RECT rc;
    GetClientRect(g_hExamWnd, &rc);
    HBRUSH bg = CreateSolidBrush(EXAM_BG_COLOR);
    FillRect(hdc, &rc, bg);
    DeleteObject(bg);

    const int nowSec = GetCurrentSeconds();
    int phase = EXAM_PHASE_NONE;
    const ExamInfo* active = GetActiveExam(nowSec, phase);
    if (!active) return;

    // 排版按客户区高度比例缩放（源程序是写死的 y 坐标，换屏幕会跑偏）
    const int h = rc.bottom - rc.top;
    auto sy = [h](int y) { return h * y / 720; };
    auto sf = [h](int size) { return (std::max)(12, h * size / 720); };

    std::wstring subjectText = std::wstring(L"考试科目：") + active->subject;
    DrawExamTextCentered(hdc, subjectText.c_str(), sy(100), sf(48), EXAM_TEXT_WHITE);
    DrawExamTextCentered(hdc, active->subject.c_str(), sy(160), sf(72), EXAM_TEXT_BLUE);

    std::wstring timeText = std::wstring(L"考试时间：") + active->startTime + L" - " + active->endTime;
    DrawExamTextCentered(hdc, timeText.c_str(), sy(260), sf(36), EXAM_TEXT_WHITE);

    DrawExamTextCentered(hdc, L"剩余时间：", sy(340), sf(28), EXAM_TEXT_WHITE);

    int remainingSec = 0;
    const wchar_t* noteText = L"";
    COLORREF noteColor = EXAM_TEXT_WHITE;
    int noteSize = 20;
    if (nowSec < active->startSec) {
        remainingSec = active->startSec - nowSec;
        noteText = L"(距离考试开始)";
    } else if (nowSec < active->endSec) {
        remainingSec = active->endSec - nowSec;
        noteText = L"(请以实际铃声为准)";
    } else {
        remainingSec = 0;
        noteText = L"考试已结束";
        noteColor = EXAM_TEXT_RED;
        noteSize = 28;
    }
    if (remainingSec < 0) remainingSec = 0;
    wchar_t buf[64];
    swprintf_s(buf, L"%02d:%02d:%02d", remainingSec / 3600, (remainingSec % 3600) / 60, remainingSec % 60);
    DrawExamTextCentered(hdc, buf, sy(400), sf(72), EXAM_TEXT_WHITE);
    DrawExamTextCentered(hdc, noteText, sy(480), sf(noteSize), noteColor);

    const wchar_t* phaseText = L"";
    COLORREF phaseColor = EXAM_TEXT_YELLOW;
    int phaseSize = 32;
    switch (phase) {
    case EXAM_PHASE_ENTER: phaseText = L"考试进入考场"; break;
    case EXAM_PHASE_INVIGILATOR: phaseText = L"监考员进入考场"; break;
    case EXAM_PHASE_PAPER: phaseText = L"依次分发答题卡、试卷"; break;
    case EXAM_PHASE_LAST15: phaseText = L"本场考试还有15分钟结束"; phaseColor = EXAM_TEXT_RED; phaseSize = 36; break;
    case EXAM_PHASE_FINISHED: phaseText = L"考试结束"; phaseColor = EXAM_TEXT_RED; phaseSize = 36; break;
    default: break;
    }
    if (phaseText[0] != L'\0')
        DrawExamTextCentered(hdc, phaseText, sy(550), sf(phaseSize), phaseColor);
}

LRESULT CALLBACK ExamWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_TIMER:
        if (wParam == EXAM_TIMER_ID) InvalidateRect(hWnd, nullptr, TRUE);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        DrawExamScreen(hdc);
        EndPaint(hWnd, &ps);
        return 0;
    }
    case WM_CLOSE:
        // 手动关闭只对本场考试有效：记下本场结束秒数，本场内定时器不再自动重开；
        // 下一场 exam 或考试结束 5 分钟后自动失效。绝不 PostQuitMessage。
        if (g_examCurrentEndSec >= 0) g_examDismissedEndSec = g_examCurrentEndSec;
        HideExamWindow();
        g_lastSecond = -1;
        return 0;
    case WM_DESTROY:
        KillTimer(hWnd, EXAM_TIMER_ID);
        g_hExamWnd = nullptr;
        g_examWindowVisible = false;
        break;
    default:
        return DefWindowProc(hWnd, msg, wParam, lParam);
    }
    return 0;
}

// ==================== 主入口 ====================
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    Log(L"程序启动");
    usb_watcher::Initialize(hInstance);

    // 字体
    g_currentDpi = GetDpiForSystem();
    RebuildFonts(g_currentDpi);

    // 注册窗口类
    WNDCLASSEXW wcMain = { sizeof(WNDCLASSEXW) };
    wcMain.style = CS_HREDRAW | CS_VREDRAW;
    wcMain.lpfnWndProc = MainWndProc;
    wcMain.hInstance = hInstance;
    wcMain.hCursor = LoadCursor(NULL, IDC_ARROW);
    wcMain.hbrBackground = CreateSolidBrush(BG_COLOR);
    wcMain.lpszClassName = MAIN_CLASS_NAME;
    RegisterClassExW(&wcMain);

    WNDCLASSEXW wcSub = { sizeof(WNDCLASSEXW) };
    wcSub.style = CS_HREDRAW | CS_VREDRAW;
    wcSub.lpfnWndProc = SubWndProc;
    wcSub.hInstance = hInstance;
    wcSub.hCursor = LoadCursor(NULL, IDC_ARROW);
    wcSub.hbrBackground = CreateSolidBrush(BG_COLOR);
    wcSub.lpszClassName = SUB_CLASS_NAME;
    RegisterClassExW(&wcSub);

    WNDCLASSEXW wcBlack = { sizeof(WNDCLASSEXW) };
    wcBlack.style = CS_HREDRAW | CS_VREDRAW;
    wcBlack.lpfnWndProc = BlackWndProc;
    wcBlack.hInstance = hInstance;
    wcBlack.hCursor = LoadCursor(NULL, IDC_ARROW);
    wcBlack.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wcBlack.lpszClassName = BLACK_CLASS_NAME;
    RegisterClassExW(&wcBlack);

    // 创建主窗口
    int screenWidth = GetSystemMetrics(SM_CXSCREEN);
    int mainWidth = ScaleForDpi(CalculateMainWindowWidth());
    int mainHeight = ScaleForDpi(CalculateMainWindowHeight());
    const int initialMainX = (screenWidth - mainWidth) / 2;
    g_hMainWnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        MAIN_CLASS_NAME, L"课程表主程序", WS_POPUP | WS_VISIBLE,
        initialMainX, ScaleForDpi(50), mainWidth, mainHeight,
        NULL, NULL, hInstance, NULL);
    ApplyRoundedCorners(g_hMainWnd);

    // 创建子窗口
    RECT rc;
    GetWindowRect(g_hMainWnd, &rc);
    g_hSubWnd = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        SUB_CLASS_NAME, L"课程表子程序", WS_POPUP | WS_VISIBLE,
        rc.left, rc.bottom, SUB_WINDOW_WIDTH, SUB_WINDOW_HEIGHT,
        NULL, NULL, hInstance, NULL);
    ApplyRoundedCorners(g_hSubWnd);

    // 创建黑屏窗口 (初始隐藏)
    int screenHeight = GetSystemMetrics(SM_CYSCREEN);
    g_hBlackWnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        BLACK_CLASS_NAME, L"午休黑屏", WS_POPUP,
        0, 0, screenWidth, screenHeight,
        NULL, NULL, hInstance, NULL);

    // 注册考试窗口类并创建考试窗口（初始隐藏，只在 exam 时间段显示）
    WNDCLASSEXW wcExam = { sizeof(WNDCLASSEXW) };
    wcExam.style = CS_HREDRAW | CS_VREDRAW;
    wcExam.lpfnWndProc = ExamWndProc;
    wcExam.hInstance = hInstance;
    wcExam.hCursor = LoadCursor(NULL, IDC_ARROW);
    wcExam.hbrBackground = CreateSolidBrush(EXAM_BG_COLOR);
    wcExam.lpszClassName = EXAM_CLASS_NAME;
    RegisterClassExW(&wcExam);

    g_hExamWnd = CreateWindowExW(WS_EX_TOPMOST, EXAM_CLASS_NAME, L"考试显示",
        WS_OVERLAPPEDWINDOW, 0, 0, screenWidth, screenHeight,
        NULL, NULL, hInstance, NULL);

    // 初始化配置文件时间戳
    WIN32_FILE_ATTRIBUTE_DATA attr;
    if (GetFileAttributesExW(CONFIG_PATH, GetFileExInfoStandard, &attr))
        g_lastConfigTime = attr.ftLastWriteTime;

    UpdateMainWindowSize();
    CenterMainWindowOnce();
    ShowWindow(g_hMainWnd, SW_SHOW);
    ShowWindow(g_hSubWnd, SW_SHOW);
    UpdateWindow(g_hMainWnd);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    DeleteObject(hFontLarge);
    DeleteObject(hFontSmall);
    DeleteObject(hFontSub);
    DeleteObject(hFontBadge);
    usb_watcher::Shutdown();
    Log(L"程序退出");
    return (int)msg.wParam;
}




#undef WinMain
}
namespace launcher_app {
#define WinMain LauncherWinMain
// 编译：cl /utf-8 /EHsc /std:c++17 TimetableLauncher.cpp /link user32.lib shell32.lib comctl32.lib


#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "dwmapi.lib")

#define WM_TRAYICON (WM_USER + 1)
#define ID_TRAY_ICON 1001
#define ID_TRAY_EXIT  1002
#define ID_TRAY_SHOW_LAUNCHER 1003
#define ID_TRAY_EDIT_COURSES 1004
#define EDITOR_CHECK_TIMER 9103

#define IDC_MAIN_MODE   2001
#define IDC_SUB_MODE    2002
#define IDC_FAST_BTN    2003
#define IDC_SLOW_BTN    2004
#define IDC_OFFSET_LABEL 2005
#define IDC_START_BTN   2006
#define IDC_KILL_BTN    2007
#define IDC_CLOSE_BTN   2008
#define IDC_OFFSET_INPUT 2009
#define IDC_BLACK_SCREEN 2010
#define IDC_MAIN_LABEL 2011
#define IDC_SUB_LABEL 2012
#define IDC_BLACK_LABEL 2013
#define IDC_OFFSET_INPUT_LABEL 2014
#define IDC_SECONDS_LABEL 2015
#define IDC_STATUS_BTN 2016
#define IDC_TIME_BTN     2019
#define IDC_AUTODIAK_LABEL 2017
#define IDC_AUTODIAK 2018

#define IDC_STATUS_LIST 3001
#define IDC_STATUS_START 3002
#define IDC_STATUS_END 3003
#define IDC_STATUS_MAIN_TEXT 3004
#define IDC_STATUS_SUB_TEXT 3005
#define IDC_STATUS_MAIN_VISIBLE 3006
#define IDC_STATUS_SUB_VISIBLE 3007
#define IDC_STATUS_COUNTDOWN_END 3008
#define IDC_STATUS_ADD 3009
#define IDC_STATUS_DELETE 3010
#define IDC_STATUS_CLOSE 3011
#define IDC_STATUS_ACTIVE_LABEL 3012

#define LAUNCHER_CLASS L"TimetableLauncher"
#define CONTROL_CENTER_CLASS L"TimetableControlCenter"
#define STATUS_MANAGER_CLASS L"TimetableStatusManager"

const wchar_t* CONFIG_PATH = L"D:\\Timetable\\config.inf";
std::wstring g_courseEditorExe = L"D:\\VisualStudioProjects\\EditV8.5.0\\x64\\Debug\\EditV8.5.0.exe";
HANDLE g_courseEditorProcess = nullptr;
std::vector<std::wstring> g_countdownConfigLines;
std::vector<std::vector<std::wstring>> g_statusCourseRows;
std::vector<std::vector<std::wstring>> g_examRows;  // exam 行（保存时原样写回，不丢失）
std::vector<timetable_shared::StatusRecord> g_statusItems;
std::wstring g_statusSourcePath;
std::wstring g_statusOutputPath;
HWND g_hStatusManager = nullptr;
bool g_statusCanSave = false;

enum DisplayMode { MODE_AUTO = 0, MODE_ALWAYS = 1, MODE_NEVER = 2 };
DisplayMode g_mainMode = MODE_AUTO;
DisplayMode g_subMode = MODE_AUTO;
bool g_blackScreenEnable = false;
bool g_usbAutoOpenEnable = true;
int g_timeOffsetSec = 0;

HWND g_hControlCenter = nullptr;
UINT g_controlCenterDpi = 96;
HFONT g_hControlCenterFont = nullptr;

const UINT_PTR SHUTDOWN_CHECK_TIMER = 9101;
const UINT_PTR SHUTDOWN_DIALOG_TIMEOUT_TIMER = 9104;
const wchar_t* SHUTDOWN_DIALOG_CLASS = L"TimetableShutdownPrompt";
int g_shutdownHandledDate = 0;
int g_shutdownPromptedDate = 0;
bool g_shutdownCancelledToday = false;
HWND g_shutdownDialog = nullptr;
bool g_shutdownDialogClassRegistered = false;

int ScaleShutdownPromptForDpi(int value) {
    return MulDiv(value, static_cast<int>(g_controlCenterDpi), 96);
}

int CurrentDateKey() {
    SYSTEMTIME now = {};
    GetLocalTime(&now);
    return now.wYear * 10000 + now.wMonth * 100 + now.wDay;
}

bool RunShutdownCommand(HWND hWnd, const wchar_t* arguments, const wchar_t* action) {
    STARTUPINFOW startup = { sizeof(startup) };
    PROCESS_INFORMATION process = {};
    std::wstring command = L"shutdown.exe ";
    command += arguments;
    if (CreateProcessW(NULL, command.data(), NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &startup, &process)) {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return true;
    }

    wchar_t errorMessage[160];
    swprintf_s(errorMessage, L"无法%s系统关机命令（错误代码：%lu）。", action, GetLastError());
    MessageBoxW(hWnd, errorMessage, L"定时关机失败", MB_OK | MB_ICONERROR | MB_TOPMOST);
    return false;
}

bool BeginDelayedShutdown(HWND hWnd) {
    return RunShutdownCommand(hWnd, L"/s /t 300", L"启动");
}

void CancelDelayedShutdown(HWND hWnd) {
    RunShutdownCommand(hWnd, L"/a", L"取消");
}

void FinishShutdownPrompt(HWND dialog, bool cancelShutdown) {
    KillTimer(dialog, SHUTDOWN_DIALOG_TIMEOUT_TIMER);
    HWND owner = GetWindow(dialog, GW_OWNER);
    g_shutdownDialog = nullptr;
    DestroyWindow(dialog);

    if (cancelShutdown) {
        g_shutdownCancelledToday = true;
        CancelDelayedShutdown(owner);
    }
}

LRESULT CALLBACK ShutdownPromptWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        const int width = ScaleShutdownPromptForDpi(440);
        const int buttonWidth = ScaleShutdownPromptForDpi(100);
        const int buttonHeight = ScaleShutdownPromptForDpi(34);
        HWND message = CreateWindowExW(0, L"STATIC", L"设备将在五分钟后关闭。",
            WS_CHILD | WS_VISIBLE | SS_CENTER, ScaleShutdownPromptForDpi(20), ScaleShutdownPromptForDpi(28),
            width - ScaleShutdownPromptForDpi(40), ScaleShutdownPromptForDpi(36), hWnd, NULL, GetModuleHandleW(NULL), NULL);
        HWND confirm = CreateWindowExW(0, L"BUTTON", L"确定",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
            (width - buttonWidth * 2 - ScaleShutdownPromptForDpi(20)) / 2, ScaleShutdownPromptForDpi(92),
            buttonWidth, buttonHeight, hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDOK)), GetModuleHandleW(NULL), NULL);
        HWND abort = CreateWindowExW(0, L"BUTTON", L"中止",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP,
            (width + ScaleShutdownPromptForDpi(20)) / 2, ScaleShutdownPromptForDpi(92),
            buttonWidth, buttonHeight, hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDCANCEL)), GetModuleHandleW(NULL), NULL);
        if (!message || !confirm || !abort) return -1;
        HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        SendMessageW(message, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        SendMessageW(confirm, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        SendMessageW(abort, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK) {
            FinishShutdownPrompt(hWnd, false);
            return 0;
        }
        if (LOWORD(wParam) == IDCANCEL) {
            FinishShutdownPrompt(hWnd, true);
            return 0;
        }
        break;
    case WM_TIMER:
        if (wParam == SHUTDOWN_DIALOG_TIMEOUT_TIMER) {
            FinishShutdownPrompt(hWnd, false);
            return 0;
        }
        break;
    case WM_CLOSE:
        FinishShutdownPrompt(hWnd, false);
        return 0;
    case WM_DESTROY:
        if (g_shutdownDialog == hWnd) g_shutdownDialog = nullptr;
        break;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

HWND ShowShutdownPrompt(HWND owner) {
    if (!g_shutdownDialogClassRegistered) return nullptr;

    RECT workArea = {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
    const int width = ScaleShutdownPromptForDpi(440);
    const int height = ScaleShutdownPromptForDpi(180);
    RECT windowRect = { 0, 0, width, height };
    AdjustWindowRectEx(&windowRect, WS_POPUP | WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_TOPMOST | WS_EX_DLGMODALFRAME);
    const int actualWidth = windowRect.right - windowRect.left;
    const int actualHeight = windowRect.bottom - windowRect.top;
    const int x = workArea.left + ((workArea.right - workArea.left) - actualWidth) / 2;
    const int y = workArea.top + ((workArea.bottom - workArea.top) - actualHeight) / 2;
    HWND dialog = CreateWindowExW(WS_EX_TOPMOST | WS_EX_DLGMODALFRAME,
        SHUTDOWN_DIALOG_CLASS, L"定时关机", WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        x, y, actualWidth, actualHeight, owner, NULL, GetModuleHandleW(NULL), NULL);
    if (!dialog) return nullptr;
    if (!SetTimer(dialog, SHUTDOWN_DIALOG_TIMEOUT_TIMER, 3 * 60 * 1000, NULL)) {
        DestroyWindow(dialog);
        return nullptr;
    }
    SetForegroundWindow(dialog);
    BringWindowToTop(dialog);
    return dialog;
}

void CheckScheduledShutdown(HWND hWnd) {
    SYSTEMTIME now = {};
    GetLocalTime(&now);
    const int dateKey = CurrentDateKey();
    if (g_shutdownHandledDate != dateKey) {
        g_shutdownHandledDate = dateKey;
        g_shutdownCancelledToday = false;
    }
    if (g_shutdownCancelledToday || g_shutdownPromptedDate == dateKey ||
        now.wHour != 21 || now.wMinute != 40) return;
    g_shutdownPromptedDate = dateKey;
    g_shutdownDialog = ShowShutdownPrompt(hWnd);
    if (!g_shutdownDialog) {
        OutputDebugStringW(L"lch: failed to create the shutdown prompt; automatic shutdown skipped\n");
        return;
    }
    if (!BeginDelayedShutdown(hWnd)) FinishShutdownPrompt(g_shutdownDialog, false);
}

int ScaleForDpi(int value, UINT dpi = g_controlCenterDpi) {
    return MulDiv(value, static_cast<int>(dpi), 96);
}

void ApplyRoundedCorners(HWND hWnd) {
    if (!hWnd) return;
    const DWM_WINDOW_CORNER_PREFERENCE preference = DWMWCP_ROUND;
    DwmSetWindowAttribute(hWnd, DWMWA_WINDOW_CORNER_PREFERENCE, &preference, sizeof(preference));
}

void RebuildControlCenterFont(UINT dpi) {
    HFONT newFont = CreateFontW(-ScaleForDpi(13, dpi), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH, L"Microsoft YaHei");
    if (!newFont) return;
    if (g_hControlCenterFont) DeleteObject(g_hControlCenterFont);
    g_hControlCenterFont = newFont;
    g_controlCenterDpi = dpi;
}

void ApplyControlCenterFont(HWND hWnd) {
    if (!hWnd || !g_hControlCenterFont) return;
    SendMessageW(hWnd, WM_SETFONT, reinterpret_cast<WPARAM>(g_hControlCenterFont), TRUE);
    for (HWND child = GetWindow(hWnd, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(g_hControlCenterFont), TRUE);
    }
}
NOTIFYICONDATAW nid = {};

void SaveConfig() {
    CreateDirectoryW(L"D:\\Timetable", NULL);
    FILE* f = nullptr;
    _wfopen_s(&f, CONFIG_PATH, L"w,ccs=UTF-8");
    if (f) {
        const wchar_t* mainStr = L"auto";
        if (g_mainMode == MODE_ALWAYS) mainStr = L"always";
        else if (g_mainMode == MODE_NEVER) mainStr = L"never";
        const wchar_t* subStr = L"auto";
        if (g_subMode == MODE_ALWAYS) subStr = L"always";
        else if (g_subMode == MODE_NEVER) subStr = L"never";
        const wchar_t* bsStr = g_blackScreenEnable ? L"enable" : L"disable";
        const wchar_t* usbAutoOpenStr = g_usbAutoOpenEnable ? L"enable" : L"disable";
        fwprintf(f, L"main_mode=%s\nsub_mode=%s\nblack_screen=%s\nautodiak=%s\ntime_offset=%d\n",
                 mainStr, subStr, bsStr, usbAutoOpenStr, g_timeOffsetSec);
        fwprintf(f, L"course_editor=%s\n", g_courseEditorExe.c_str());
        for (const auto& countdownLine : g_countdownConfigLines) {
            fwprintf(f, L"%s\n", countdownLine.c_str());
        }
        fclose(f);
    }
}

void LoadConfig() {
    g_countdownConfigLines.clear();
    FILE* f = nullptr;
    if (_wfopen_s(&f, CONFIG_PATH, L"r,ccs=UTF-8") == 0 && f) {
        wchar_t line[256];
        while (fgetws(line, 256, f)) {
            std::wstring s(line);
            if (s.find(L"main_mode=") == 0) {
                std::wstring val = s.substr(10);
                val.erase(std::remove(val.begin(), val.end(), L'\r'), val.end());
                val.erase(std::remove(val.begin(), val.end(), L'\n'), val.end());
                if (val == L"always") g_mainMode = MODE_ALWAYS;
                else if (val == L"never") g_mainMode = MODE_NEVER;
                else g_mainMode = MODE_AUTO;
            }
            else if (s.find(L"sub_mode=") == 0) {
                std::wstring val = s.substr(9);
                val.erase(std::remove(val.begin(), val.end(), L'\r'), val.end());
                val.erase(std::remove(val.begin(), val.end(), L'\n'), val.end());
                if (val == L"always") g_subMode = MODE_ALWAYS;
                else if (val == L"never") g_subMode = MODE_NEVER;
                else g_subMode = MODE_AUTO;
            }
            else if (s.find(L"black_screen=") == 0) {
                std::wstring val = s.substr(13);
                val.erase(std::remove(val.begin(), val.end(), L'\r'), val.end());
                val.erase(std::remove(val.begin(), val.end(), L'\n'), val.end());
                if (val == L"enable") g_blackScreenEnable = true;
                else if (val == L"disable") g_blackScreenEnable = false;
            }
            else if (s.find(L"autodiak=") == 0) {
                std::wstring val = s.substr(9);
                val.erase(std::remove(val.begin(), val.end(), L'\r'), val.end());
                val.erase(std::remove(val.begin(), val.end(), L'\n'), val.end());
                if (val == L"enable") g_usbAutoOpenEnable = true;
                else if (val == L"disable") g_usbAutoOpenEnable = false;
            }
            else if (s.find(L"time_offset=") == 0) {
                std::wstring val = s.substr(12);
                val.erase(std::remove(val.begin(), val.end(), L'\r'), val.end());
                val.erase(std::remove(val.begin(), val.end(), L'\n'), val.end());
                g_timeOffsetSec = std::stoi(val);
            }
            else if (s.find(L"course_editor=") == 0) {
                s.erase(std::remove(s.begin(), s.end(), L'\r'), s.end());
                s.erase(std::remove(s.begin(), s.end(), L'\n'), s.end());
                g_courseEditorExe = s.substr(14);
                if (g_courseEditorExe.size() >= 2 &&
                    g_courseEditorExe.front() == L'"' &&
                    g_courseEditorExe.back() == L'"') {
                    g_courseEditorExe = g_courseEditorExe.substr(1, g_courseEditorExe.size() - 2);
                }
            }
            else if (s.find(L"countdown=") == 0) {
                s.erase(std::remove(s.begin(), s.end(), L'\r'), s.end());
                s.erase(std::remove(s.begin(), s.end(), L'\n'), s.end());
                if (!s.empty()) g_countdownConfigLines.push_back(s);
            }
            else {
                // 合并后的 tta 自身包含启动器和课表窗口，配置文件不再接受独立 tta 路径。
            }
        }
        fclose(f);
    }
    usb_watcher::SetAutoOpenEnabled(g_usbAutoOpenEnable);
}

bool IsTimetableRunning() {
    return FindWindowW(L"TimetableMainApp", nullptr) != nullptr;
}

void StartTimetable() {
    if (HWND timetable = FindWindowW(L"TimetableMainApp", nullptr)) {
        PostMessageW(timetable, WM_TIMETABLE_SHOW, 0, 0);
        return;
    }
}

void KillTimetable() {
    HWND hWnd = FindWindowW(L"TimetableMainApp", nullptr);
    if (hWnd) {
        PostMessageW(hWnd, WM_TIMETABLE_HIDE, 0, 0);
    }
}

std::wstring GetTodayStatusFilePath() {
    SYSTEMTIME now = {};
    GetLocalTime(&now);
    wchar_t path[MAX_PATH] = {};
    swprintf_s(path, L"D:\\Timetable\\Sources\\timetable_%04d-%02d-%02d.csv",
        now.wYear, now.wMonth, now.wDay);
    return path;
}

bool ReadStatusManagerData() {
    g_statusCourseRows.clear();
    g_examRows.clear();
    g_statusItems.clear();
    g_statusSourcePath = timetable_app::GetTimetableFilePath();
    g_statusOutputPath = GetTodayStatusFilePath();
    g_statusCanSave = true;
    if (g_statusSourcePath.empty()) return true;

    FILE* file = nullptr;
    if (_wfopen_s(&file, g_statusSourcePath.c_str(), L"r,ccs=UTF-8") != 0 || !file) {
        g_statusCanSave = false;
        return false;
    }

    bool valid = true;
    wchar_t line[4096] = {};
    while (fgetws(line, static_cast<int>(std::size(line)), file)) {
        std::wstring row(line);
        while (!row.empty() && (row.back() == L'\n' || row.back() == L'\r')) row.pop_back();
        if (row.empty()) continue;
        std::vector<std::wstring> fields = timetable_app::ParseCsvRow(row);
        if (fields.empty()) continue;
        if (!fields[0].empty() && fields[0][0] == 0xFEFF)
            fields[0].erase(fields[0].begin());
        if (fields[0] == L"课程全称") continue;
        if (fields[0] == L"status") {
            timetable_shared::StatusRecord status;
            if (!timetable_shared::ParseStatusRow(fields, status)) {
                valid = false;
                break;
            }
            g_statusItems.push_back(std::move(status));
            continue;
        }
        if (fields[0] == L"exam") {
            if (fields.size() < 4) {
                valid = false;
                break;
            }
            g_examRows.push_back(fields);
            continue;
        }
        if (fields[0] == L"course" || fields[0] == L"courses") fields.erase(fields.begin());
        else if (fields[0] == L"courses=") fields.erase(fields.begin());
        else if (fields[0].compare(0, 8, L"courses=") == 0) fields[0] = fields[0].substr(8);
        else if (fields.size() >= 6) {
            g_statusCourseRows.push_back(std::move(fields));
            continue;
        } else {
            valid = false;
            break;
        }
        if (fields.size() < 6) {
            valid = false;
            break;
        }
        g_statusCourseRows.push_back(std::move(fields));
    }
    if (ferror(file)) valid = false;
    fclose(file);
    if (!valid) {
        g_statusCourseRows.clear();
        g_examRows.clear();
        g_statusItems.clear();
        g_statusCanSave = false;
    }
    return valid;
}

std::wstring JoinCsvRow(const std::vector<std::wstring>& fields) {
    std::wstring row;
    for (size_t i = 0; i < fields.size(); ++i) {
        if (i) row += L',';
        row += timetable_shared::EscapeCsvField(fields[i]);
    }
    return row;
}

bool WriteStatusManagerData() {
    CreateDirectoryW(L"D:\\Timetable", NULL);
    CreateDirectoryW(L"D:\\Timetable\\Sources", NULL);
    const std::wstring temporaryPath = g_statusOutputPath + L".tmp";
    FILE* file = nullptr;
    if (_wfopen_s(&file, temporaryPath.c_str(), L"w,ccs=UTF-8") != 0 || !file) return false;

    bool success = true;
    auto writeRow = [&file, &success](const std::vector<std::wstring>& fields) {
        if (!success) return;
        const std::wstring row = JoinCsvRow(fields);
        if (fputws(row.c_str(), file) == WEOF || fputwc(L'\n', file) == WEOF) success = false;
    };

    for (const auto& course : g_statusCourseRows) {
        std::vector<std::wstring> row;
        row.reserve(course.size() + 1);
        row.push_back(L"courses");
        row.insert(row.end(), course.begin(), course.end());
        writeRow(row);
    }
    for (const auto& examRow : g_examRows) {
        writeRow(examRow);
    }
    for (const auto& status : g_statusItems) {
        writeRow({
            L"status",
            timetable_shared::FormatClockTime(status.activeStartSeconds),
            timetable_shared::FormatClockTime(status.activeEndSeconds),
            status.mainText,
            status.subText,
            status.showMain ? L"1" : L"0",
            status.showSub ? L"1" : L"0",
            timetable_shared::FormatClockTime(status.countdownEndSeconds)
        });
    }
    if (fflush(file) != 0 || ferror(file)) success = false;
    if (fclose(file) != 0) success = false;
    if (!success || !MoveFileExW(temporaryPath.c_str(), g_statusOutputPath.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporaryPath.c_str());
        return false;
    }

    g_statusSourcePath = g_statusOutputPath;
    if (HWND timetable = FindWindowW(timetable_app::MAIN_CLASS_NAME, nullptr))
        PostMessageW(timetable, WM_TIMETABLE_RELOAD, 0, 0);
    return true;
}

std::wstring GetStatusListText(const timetable_shared::StatusRecord& status, bool active) {
    std::wstring text = active ? L"当前  " : L"        ";
    text += timetable_shared::FormatClockTime(status.activeStartSeconds) + L" - " +
        timetable_shared::FormatClockTime(status.activeEndSeconds) + L"  " +
        status.mainText + L" / " + status.subText + L"  倒计时至 " +
        timetable_shared::FormatClockTime(status.countdownEndSeconds);
    return text;
}

int GetStatusManagerCurrentSeconds() {
    SYSTEMTIME now = {};
    GetLocalTime(&now);
    int seconds = now.wHour * 3600 + now.wMinute * 60 + now.wSecond + g_timeOffsetSec;
    seconds %= 24 * 60 * 60;
    return seconds < 0 ? seconds + 24 * 60 * 60 : seconds;
}

void RefreshStatusManagerList(HWND hWnd) {
    HWND list = GetDlgItem(hWnd, IDC_STATUS_LIST);
    if (!list) return;
    SendMessageW(list, LB_RESETCONTENT, 0, 0);
    const int now = GetStatusManagerCurrentSeconds();
    int activeIndex = -1;
    int latestStart = -1;
    for (size_t i = 0; i < g_statusItems.size(); ++i) {
        const auto& status = g_statusItems[i];
        if (now >= status.activeStartSeconds && now < status.activeEndSeconds &&
            status.activeStartSeconds >= latestStart) {
            activeIndex = static_cast<int>(i);
            latestStart = status.activeStartSeconds;
        }
    }
    for (size_t i = 0; i < g_statusItems.size(); ++i) {
        const std::wstring text = GetStatusListText(g_statusItems[i], static_cast<int>(i) == activeIndex);
        SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
    }
    wchar_t label[64] = {};
    swprintf_s(label, L"今天的单次 status（共 %zu 条）", g_statusItems.size());
    SetDlgItemTextW(hWnd, IDC_STATUS_ACTIVE_LABEL, label);
}

std::wstring GetStatusManagerEditText(HWND hWnd, int controlId) {
    HWND control = GetDlgItem(hWnd, controlId);
    const int length = control ? GetWindowTextLengthW(control) : 0;
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    if (control) GetWindowTextW(control, text.data(), length + 1);
    text.resize(static_cast<size_t>(length));
    return text;
}

void SelectStatusInManager(HWND hWnd, int index) {
    if (index < 0 || static_cast<size_t>(index) >= g_statusItems.size()) return;
    const auto& status = g_statusItems[static_cast<size_t>(index)];
    SetDlgItemTextW(hWnd, IDC_STATUS_START, timetable_shared::FormatClockTime(status.activeStartSeconds).c_str());
    SetDlgItemTextW(hWnd, IDC_STATUS_END, timetable_shared::FormatClockTime(status.activeEndSeconds).c_str());
    SetDlgItemTextW(hWnd, IDC_STATUS_MAIN_TEXT, status.mainText.c_str());
    SetDlgItemTextW(hWnd, IDC_STATUS_SUB_TEXT, status.subText.c_str());
    SetDlgItemTextW(hWnd, IDC_STATUS_COUNTDOWN_END, timetable_shared::FormatClockTime(status.countdownEndSeconds).c_str());
    SendDlgItemMessageW(hWnd, IDC_STATUS_MAIN_VISIBLE, BM_SETCHECK, status.showMain ? BST_CHECKED : BST_UNCHECKED, 0);
    SendDlgItemMessageW(hWnd, IDC_STATUS_SUB_VISIBLE, BM_SETCHECK, status.showSub ? BST_CHECKED : BST_UNCHECKED, 0);
}

void AddStatusFromManager(HWND hWnd) {
    timetable_shared::StatusRecord status;
    if (!timetable_shared::ParseClockTime(GetStatusManagerEditText(hWnd, IDC_STATUS_START), status.activeStartSeconds) ||
        !timetable_shared::ParseClockTime(GetStatusManagerEditText(hWnd, IDC_STATUS_END), status.activeEndSeconds) ||
        !timetable_shared::ParseClockTime(GetStatusManagerEditText(hWnd, IDC_STATUS_COUNTDOWN_END), status.countdownEndSeconds) ||
        status.activeStartSeconds >= status.activeEndSeconds) {
        MessageBoxW(hWnd, L"请填写有效时间，且 status 结束时间必须晚于开始时间。", L"输入无效", MB_OK | MB_ICONWARNING);
        return;
    }
    status.mainText = GetStatusManagerEditText(hWnd, IDC_STATUS_MAIN_TEXT);
    status.subText = GetStatusManagerEditText(hWnd, IDC_STATUS_SUB_TEXT);
    status.showMain = SendDlgItemMessageW(hWnd, IDC_STATUS_MAIN_VISIBLE, BM_GETCHECK, 0, 0) == BST_CHECKED;
    status.showSub = SendDlgItemMessageW(hWnd, IDC_STATUS_SUB_VISIBLE, BM_GETCHECK, 0, 0) == BST_CHECKED;

    g_statusItems.push_back(std::move(status));
    if (!WriteStatusManagerData()) {
        g_statusItems.pop_back();
        MessageBoxW(hWnd, L"保存单次 status 失败，请检查 D:\\Timetable\\Sources 文件夹权限或文件占用。", L"保存失败", MB_OK | MB_ICONERROR);
        return;
    }
    RefreshStatusManagerList(hWnd);
}

void DeleteSelectedStatus(HWND hWnd) {
    const int index = static_cast<int>(SendDlgItemMessageW(hWnd, IDC_STATUS_LIST, LB_GETCURSEL, 0, 0));
    if (index < 0 || static_cast<size_t>(index) >= g_statusItems.size()) {
        MessageBoxW(hWnd, L"请先选择要删除的 status。", L"未选择 status", MB_OK | MB_ICONINFORMATION);
        return;
    }
    const auto previousStatuses = g_statusItems;
    g_statusItems.erase(g_statusItems.begin() + index);
    if (!WriteStatusManagerData()) {
        g_statusItems = previousStatuses;
        MessageBoxW(hWnd, L"删除 status 时保存失败，原有内容已保留。", L"保存失败", MB_OK | MB_ICONERROR);
        return;
    }
    RefreshStatusManagerList(hWnd);
}

LRESULT CALLBACK StatusManagerWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        auto createControl = [hWnd](const wchar_t* className, const wchar_t* text, DWORD style,
            int x, int y, int width, int height, int id) {
            return CreateWindowExW(0, className, text, WS_CHILD | WS_VISIBLE | style,
                ScaleForDpi(x), ScaleForDpi(y), ScaleForDpi(width), ScaleForDpi(height),
                hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(NULL), NULL);
        };
        HWND label = createControl(L"STATIC", L"今天的单次 status（当前日期文件优先，否则读取星期课表）",
            0, 15, 10, 590, 22, IDC_STATUS_ACTIVE_LABEL);
        HWND list = createControl(L"LISTBOX", L"", LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_BORDER,
            15, 36, 590, 125, IDC_STATUS_LIST);
        createControl(L"STATIC", L"显示开始", 0, 18, 174, 66, 24, 0);
        HWND start = createControl(L"EDIT", L"08:00:00", WS_BORDER | ES_AUTOHSCROLL,
            84, 170, 105, 28, IDC_STATUS_START);
        createControl(L"STATIC", L"显示结束", 0, 205, 174, 66, 24, 0);
        HWND end = createControl(L"EDIT", L"09:00:00", WS_BORDER | ES_AUTOHSCROLL,
            270, 170, 105, 28, IDC_STATUS_END);
        createControl(L"STATIC", L"倒计时截止", 0, 390, 174, 78, 24, 0);
        HWND countdownEnd = createControl(L"EDIT", L"09:00:00", WS_BORDER | ES_AUTOHSCROLL,
            478, 170, 120, 28, IDC_STATUS_COUNTDOWN_END);
        createControl(L"STATIC", L"主窗口文字", 0, 18, 213, 78, 24, 0);
        HWND mainText = createControl(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL,
            100, 208, 498, 28, IDC_STATUS_MAIN_TEXT);
        createControl(L"STATIC", L"子窗口文字", 0, 18, 251, 78, 24, 0);
        HWND subText = createControl(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL,
            100, 246, 498, 28, IDC_STATUS_SUB_TEXT);
        HWND mainVisible = createControl(L"BUTTON", L"显示主窗口", BS_AUTOCHECKBOX | WS_TABSTOP,
            100, 284, 160, 28, IDC_STATUS_MAIN_VISIBLE);
        HWND subVisible = createControl(L"BUTTON", L"显示子窗口", BS_AUTOCHECKBOX | WS_TABSTOP,
            300, 284, 160, 28, IDC_STATUS_SUB_VISIBLE);
        HWND add = createControl(L"BUTTON", L"新增 status", WS_TABSTOP | BS_DEFPUSHBUTTON,
            160, 326, 105, 32, IDC_STATUS_ADD);
        HWND remove = createControl(L"BUTTON", L"删除选中", WS_TABSTOP,
            276, 326, 105, 32, IDC_STATUS_DELETE);
        HWND close = createControl(L"BUTTON", L"关闭", WS_TABSTOP,
            392, 326, 105, 32, IDC_STATUS_CLOSE);
        createControl(L"STATIC", g_statusOutputPath.c_str(), 0, 15, 376, 590, 32, 0);
        if (!label || !list || !start || !end || !countdownEnd || !mainText ||
            !subText || !mainVisible || !subVisible || !add || !remove || !close) return -1;

        HFONT font = g_hControlCenterFont
            ? g_hControlCenterFont : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        for (HWND child = GetWindow(hWnd, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
            SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        SendDlgItemMessageW(hWnd, IDC_STATUS_MAIN_VISIBLE, BM_SETCHECK, BST_CHECKED, 0);
        SendDlgItemMessageW(hWnd, IDC_STATUS_SUB_VISIBLE, BM_SETCHECK, BST_CHECKED, 0);
        if (!g_statusCanSave) {
            EnableWindow(add, FALSE);
            EnableWindow(remove, FALSE);
        }
        RefreshStatusManagerList(hWnd);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_STATUS_LIST && HIWORD(wParam) == LBN_SELCHANGE) {
            SelectStatusInManager(hWnd, static_cast<int>(SendDlgItemMessageW(hWnd, IDC_STATUS_LIST, LB_GETCURSEL, 0, 0)));
            return 0;
        }
        if (LOWORD(wParam) == IDC_STATUS_ADD) {
            AddStatusFromManager(hWnd);
            return 0;
        }
        if (LOWORD(wParam) == IDC_STATUS_DELETE) {
            DeleteSelectedStatus(hWnd);
            return 0;
        }
        if (LOWORD(wParam) == IDC_STATUS_CLOSE) {
            DestroyWindow(hWnd);
            return 0;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(hWnd);
        return 0;
    case WM_DESTROY:
        if (g_hStatusManager == hWnd) g_hStatusManager = nullptr;
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

void ShowStatusManager() {
    if (g_hStatusManager && IsWindow(g_hStatusManager)) {
        ShowWindow(g_hStatusManager, SW_SHOW);
        SetForegroundWindow(g_hStatusManager);
        return;
    }
    if (!ReadStatusManagerData()) {
        MessageBoxW(g_hControlCenter,
            L"无法读取当天课表，或者文件中的 status/课程行格式无效；为避免覆盖文件，status 管理窗口未打开。",
            L"读取失败", MB_OK | MB_ICONERROR);
        return;
    }

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = { sizeof(WNDCLASSEXW) };
        wc.lpfnWndProc = StatusManagerWndProc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = STATUS_MANAGER_CLASS;
        registered = RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }
    if (!registered) {
        MessageBoxW(g_hControlCenter, L"无法创建 status 管理窗口。", L"错误", MB_OK | MB_ICONERROR);
        return;
    }

    const int clientWidth = ScaleForDpi(620);
    const int clientHeight = ScaleForDpi(420);
    RECT windowRect = { 0, 0, clientWidth, clientHeight };
    AdjustWindowRectEx(&windowRect, WS_POPUP | WS_CAPTION | WS_SYSMENU, FALSE,
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW);
    const int width = windowRect.right - windowRect.left;
    const int height = windowRect.bottom - windowRect.top;
    RECT workArea = {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
    const int x = workArea.left + ((workArea.right - workArea.left) - width) / 2;
    const int y = workArea.top + ((workArea.bottom - workArea.top) - height) / 2;
    g_hStatusManager = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        STATUS_MANAGER_CLASS, L"单次 status 管理",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        x, y, width, height, g_hControlCenter, NULL, GetModuleHandleW(NULL), NULL);
    if (!g_hStatusManager)
        MessageBoxW(g_hControlCenter, L"无法创建 status 管理窗口。", L"错误", MB_OK | MB_ICONERROR);
}

void LayoutControlCenter(HWND hWnd) {
    const int margin = ScaleForDpi(9);
    const int labelWidth = ScaleForDpi(67);
    const int fieldX = ScaleForDpi(81);
    const int fieldWidth = ScaleForDpi(167);
    const int controlHeight = ScaleForDpi(24);
    const int buttonHeight = ScaleForDpi(28);
    const int gap = ScaleForDpi(5);
    int y = margin;

    auto place = [hWnd](int id, int x, int yPos, int width, int height) {
        HWND control = GetDlgItem(hWnd, id);
        if (control) SetWindowPos(control, NULL, x, yPos, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
    };

    place(IDC_MAIN_LABEL, margin, y + ScaleForDpi(5), labelWidth, controlHeight);
    place(IDC_MAIN_MODE, fieldX, y, fieldWidth, ScaleForDpi(133));
    y += controlHeight + gap;
    place(IDC_SUB_LABEL, margin, y + ScaleForDpi(5), labelWidth, controlHeight);
    place(IDC_SUB_MODE, fieldX, y, fieldWidth, ScaleForDpi(133));
    y += controlHeight + gap;
    place(IDC_BLACK_LABEL, margin, y + ScaleForDpi(5), labelWidth, controlHeight);
    place(IDC_BLACK_SCREEN, fieldX, y, fieldWidth, ScaleForDpi(133));
    y += controlHeight + gap;
    place(IDC_AUTODIAK_LABEL, margin, y + ScaleForDpi(5), labelWidth, controlHeight);
    place(IDC_AUTODIAK, fieldX, y, fieldWidth, ScaleForDpi(133));
    y += controlHeight + gap;
    place(IDC_OFFSET_INPUT_LABEL, margin, y + ScaleForDpi(5), labelWidth, controlHeight);
    place(IDC_OFFSET_INPUT, fieldX, y, ScaleForDpi(100), controlHeight);
    place(IDC_SECONDS_LABEL, ScaleForDpi(188), y + ScaleForDpi(3), ScaleForDpi(34), controlHeight);
    y += controlHeight + gap;
    const int adjustButtonGap = ScaleForDpi(8);
    const int adjustButtonWidth = ScaleForDpi(116);
    place(IDC_FAST_BTN, margin, y, adjustButtonWidth, buttonHeight);
    place(IDC_SLOW_BTN, margin + adjustButtonWidth + adjustButtonGap, y, adjustButtonWidth, buttonHeight);
    y += buttonHeight + gap;
    place(IDC_OFFSET_LABEL, margin, y, ScaleForDpi(239), controlHeight);
    y += controlHeight + gap * 2;
    const int actionButtonGap = ScaleForDpi(8);
    const int actionButtonWidth = ScaleForDpi(75);
    place(IDC_START_BTN, margin, y, actionButtonWidth, buttonHeight);
    place(IDC_KILL_BTN, margin + actionButtonWidth + actionButtonGap, y, actionButtonWidth, buttonHeight);
    place(IDC_CLOSE_BTN, margin + (actionButtonWidth + actionButtonGap) * 2, y, actionButtonWidth, buttonHeight);
    y += buttonHeight + gap;
    place(IDC_STATUS_BTN, margin + actionButtonWidth + actionButtonGap, y, actionButtonWidth, buttonHeight);
    place(IDC_TIME_BTN, margin, y, actionButtonWidth, buttonHeight);
}

void FitControlCenterToContent(HWND hWnd) {
    if (!hWnd) return;

    const int margin = ScaleForDpi(9);
    const int controlHeight = ScaleForDpi(24);
    const int buttonHeight = ScaleForDpi(28);
    const int gap = ScaleForDpi(5);
    const int contentWidth = ScaleForDpi(257);
    const int contentHeight = margin + (controlHeight + gap) * 5 +
        buttonHeight + gap + controlHeight + gap * 2 + buttonHeight + gap + buttonHeight + margin;

    RECT requiredRect = { 0, 0, contentWidth, contentHeight };
    AdjustWindowRectExForDpi(&requiredRect, WS_POPUP | WS_CAPTION, FALSE,
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW, g_controlCenterDpi);

    HMONITOR monitor = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitorInfo = { sizeof(MONITORINFO) };
    GetMonitorInfoW(monitor, &monitorInfo);
    const int workWidth = static_cast<int>(monitorInfo.rcWork.right - monitorInfo.rcWork.left);
    const int workHeight = static_cast<int>(monitorInfo.rcWork.bottom - monitorInfo.rcWork.top);
    const int requiredWidth = static_cast<int>(requiredRect.right - requiredRect.left);
    const int requiredHeight = static_cast<int>(requiredRect.bottom - requiredRect.top);
    const int width = (std::min)(requiredWidth, workWidth);
    const int height = (std::min)(requiredHeight, workHeight);

    RECT current = {};
    GetWindowRect(hWnd, &current);
    int x = current.left;
    int y = current.top;
    if (x + width > monitorInfo.rcWork.right) x = monitorInfo.rcWork.right - width;
    if (y + height > monitorInfo.rcWork.bottom) y = monitorInfo.rcWork.bottom - height;
    if (x < monitorInfo.rcWork.left) x = monitorInfo.rcWork.left;
    if (y < monitorInfo.rcWork.top) y = monitorInfo.rcWork.top;

    SetWindowPos(hWnd, NULL, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
}

// ==================== 时间窗口 ====================
// 独立窗口：与课程表主/子窗口、考试窗口互不影响。
// “关闭课表”只隐藏/关闭课表窗口，不动它；控制中心关了也不动它；
// 只有用户点它自己的关闭按钮、在控制中心再点一次“关闭时间”，
// 或整个程序从托盘退出时，它才关闭。
// 显示的是真实本地时间（不加控制中心里的矫正秒数）。
const wchar_t* TIME_WINDOW_CLASS = L"TimetableClockWindow";
const UINT_PTR CLOCK_TIMER_ID = 7002;
HWND g_hTimeWnd = nullptr;
int g_clockLastSecond = -1;

void UpdateTimeButtonText() {
    if (!g_hControlCenter || !IsWindow(g_hControlCenter)) return;
    SetDlgItemTextW(g_hControlCenter, IDC_TIME_BTN,
        (g_hTimeWnd && IsWindow(g_hTimeWnd)) ? L"关闭时间" : L"显示时间");
}

LRESULT CALLBACK TimeWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        SetTimer(hWnd, CLOCK_TIMER_ID, 250, nullptr);
        return 0;
    case WM_TIMER:
        if (wParam == CLOCK_TIMER_ID) {
            SYSTEMTIME st;
            GetLocalTime(&st);
            if (st.wSecond != g_clockLastSecond) {
                g_clockLastSecond = st.wSecond;
                InvalidateRect(hWnd, nullptr, TRUE);
            }
        }
        return 0;
    case WM_SIZE:
        InvalidateRect(hWnd, nullptr, TRUE);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        RECT rc;
        GetClientRect(hWnd, &rc);
        HBRUSH bg = CreateSolidBrush(RGB(255, 255, 255));
        FillRect(hdc, &rc, bg);
        DeleteObject(bg);

        SYSTEMTIME st;
        GetLocalTime(&st);
        const wchar_t* weekNames[] = { L"星期日", L"星期一", L"星期二", L"星期三",
                                       L"星期四", L"星期五", L"星期六" };
        wchar_t dateText[128];
        swprintf_s(dateText, L"%04d年%02d月%02d日 %s",
            st.wYear, st.wMonth, st.wDay, weekNames[st.wDayOfWeek]);
        wchar_t timeText[32];
        swprintf_s(timeText, L"%02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond);

        const int w = rc.right - rc.left;
        const int h = rc.bottom - rc.top;
        const int timeFontH = (std::max)(24, (std::min)(w * 17 / 100, h * 35 / 100));
        const int dateFontH = (std::max)(14, timeFontH / 4);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(0, 0, 0));

        HFONT hDateFont = CreateFontW(dateFontH, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
        HFONT oldFont = (HFONT)SelectObject(hdc, hDateFont);
        RECT dateRect = { 0, h / 4 - dateFontH, w, h / 4 + dateFontH };
        DrawTextW(hdc, dateText, -1, &dateRect, DT_CENTER | DT_TOP | DT_SINGLELINE);
        SelectObject(hdc, oldFont);
        DeleteObject(hDateFont);

        HFONT hTimeFont = CreateFontW(timeFontH, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Microsoft YaHei UI");
        oldFont = (HFONT)SelectObject(hdc, hTimeFont);
        RECT timeRect = { 0, h / 2 - timeFontH / 2, w, h / 2 + timeFontH };
        DrawTextW(hdc, timeText, -1, &timeRect, DT_CENTER | DT_TOP | DT_SINGLELINE);
        SelectObject(hdc, oldFont);
        DeleteObject(hTimeFont);

        EndPaint(hWnd, &ps);
        return 0;
    }
    case WM_DESTROY:
        KillTimer(hWnd, CLOCK_TIMER_ID);
        g_hTimeWnd = nullptr;
        g_clockLastSecond = -1;
        UpdateTimeButtonText();  // 若控制中心还开着，把按钮改回“显示时间”
        break;
    default:
        return DefWindowProc(hWnd, msg, wParam, lParam);
    }
    return 0;
}

void ToggleTimeWindow() {
    if (g_hTimeWnd && IsWindow(g_hTimeWnd)) {
        DestroyWindow(g_hTimeWnd);  // WM_DESTROY 里会清句柄并改按钮文字
        return;
    }
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = { sizeof(WNDCLASSEXW) };
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = TimeWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
        wc.lpszClassName = TIME_WINDOW_CLASS;
        RegisterClassExW(&wc);
        registered = true;
    }
    const int screenW = GetSystemMetrics(SM_CXSCREEN);
    const int screenH = GetSystemMetrics(SM_CYSCREEN);
    g_hTimeWnd = CreateWindowExW(0, TIME_WINDOW_CLASS, L"时间",
        WS_OVERLAPPEDWINDOW, 0, 0, screenW, screenH,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!g_hTimeWnd) return;
    ShowWindow(g_hTimeWnd, SW_MAXIMIZE);
    UpdateTimeButtonText();
}

// 控制中心窗口过程
LRESULT CALLBACK ControlCenterWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        g_controlCenterDpi = GetDpiForWindow(hWnd);
        RebuildControlCenterFont(g_controlCenterDpi);
        ApplyRoundedCorners(hWnd);
        LoadConfig();
        HFONT hFont = g_hControlCenterFont;
        int y = 20, gap = 10, hCtrl = 48, hBtn = 56;

        // 主窗口模式
        CreateWindowW(L"STATIC", L"主窗口：", WS_CHILD | WS_VISIBLE, 20, y + 8, 140, 40, hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_MAIN_LABEL)), NULL, NULL);
        HWND cbMain = CreateWindowW(WC_COMBOBOXW, NULL, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
            180, y, 200, 200, hWnd, (HMENU)IDC_MAIN_MODE, NULL, NULL);
        SendMessageW(cbMain, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(cbMain, CB_ADDSTRING, 0, (LPARAM)L"自动");
        SendMessageW(cbMain, CB_ADDSTRING, 0, (LPARAM)L"常显");
        SendMessageW(cbMain, CB_ADDSTRING, 0, (LPARAM)L"常隐");
        SendMessageW(cbMain, CB_SETCURSEL, (int)g_mainMode, 0);
        y += hCtrl + gap;

        // 子窗口模式
        CreateWindowW(L"STATIC", L"子窗口：", WS_CHILD | WS_VISIBLE, 20, y + 8, 140, 40, hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SUB_LABEL)), NULL, NULL);
        HWND cbSub = CreateWindowW(WC_COMBOBOXW, NULL, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
            180, y, 200, 200, hWnd, (HMENU)IDC_SUB_MODE, NULL, NULL);
        SendMessageW(cbSub, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(cbSub, CB_ADDSTRING, 0, (LPARAM)L"自动");
        SendMessageW(cbSub, CB_ADDSTRING, 0, (LPARAM)L"常显");
        SendMessageW(cbSub, CB_ADDSTRING, 0, (LPARAM)L"常隐");
        SendMessageW(cbSub, CB_SETCURSEL, (int)g_subMode, 0);
        y += hCtrl + gap + 8;

        // 午休黑屏
        CreateWindowW(L"STATIC", L"午休黑屏：", WS_CHILD | WS_VISIBLE, 20, y + 8, 140, 40, hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BLACK_LABEL)), NULL, NULL);
        HWND cbBs = CreateWindowW(WC_COMBOBOXW, NULL, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
            180, y, 200, 200, hWnd, (HMENU)IDC_BLACK_SCREEN, NULL, NULL);
        SendMessageW(cbBs, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(cbBs, CB_ADDSTRING, 0, (LPARAM)L"启用");
        SendMessageW(cbBs, CB_ADDSTRING, 0, (LPARAM)L"禁用");
        SendMessageW(cbBs, CB_SETCURSEL, g_blackScreenEnable ? 0 : 1, 0);
        y += hCtrl + gap + 8;

        // U盘自启
        CreateWindowW(L"STATIC", L"U盘自启：", WS_CHILD | WS_VISIBLE, 20, y + 8, 140, 40, hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_AUTODIAK_LABEL)), NULL, NULL);
        HWND cbUsbAutoOpen = CreateWindowW(WC_COMBOBOXW, NULL, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
            180, y, 200, 200, hWnd, (HMENU)IDC_AUTODIAK, NULL, NULL);
        SendMessageW(cbUsbAutoOpen, WM_SETFONT, (WPARAM)hFont, TRUE);
        SendMessageW(cbUsbAutoOpen, CB_ADDSTRING, 0, (LPARAM)L"启用");
        SendMessageW(cbUsbAutoOpen, CB_ADDSTRING, 0, (LPARAM)L"禁用");
        SendMessageW(cbUsbAutoOpen, CB_SETCURSEL, g_usbAutoOpenEnable ? 0 : 1, 0);
        y += hCtrl + gap + 8;

        // 矫正计时
        CreateWindowW(L"STATIC", L"调整秒数：", WS_CHILD | WS_VISIBLE, 20, y + 8, 140, 40, hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_OFFSET_INPUT_LABEL)), NULL, NULL);
        CreateWindowW(L"EDIT", L"1", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER, 
            180, y, 160, hCtrl, hWnd, (HMENU)IDC_OFFSET_INPUT, NULL, NULL);
        CreateWindowW(L"STATIC", L"秒", WS_CHILD | WS_VISIBLE, 360, y + 8, 60, 40, hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SECONDS_LABEL)), NULL, NULL);
        y += hCtrl + gap;

        CreateWindowW(L"BUTTON", L"课表快了 (-)", WS_CHILD | WS_VISIBLE,
            20, y, 250, hBtn, hWnd, (HMENU)IDC_FAST_BTN, NULL, NULL);
        CreateWindowW(L"BUTTON", L"课表慢了 (+)", WS_CHILD | WS_VISIBLE,
            290, y, 250, hBtn, hWnd, (HMENU)IDC_SLOW_BTN, NULL, NULL);
        y += hBtn + gap;

        wchar_t buf[64];
        swprintf_s(buf, L"当前偏移：%+d 秒", g_timeOffsetSec);
        CreateWindowW(L"STATIC", buf, WS_CHILD | WS_VISIBLE, 20, y + 8, 500, 40, hWnd, (HMENU)IDC_OFFSET_LABEL, NULL, NULL);
        y += 40 + gap * 2;

        // 启动/关闭
        CreateWindowW(L"BUTTON", L"启动课表", WS_CHILD | WS_VISIBLE,
            20, y, 160, hBtn, hWnd, (HMENU)IDC_START_BTN, NULL, NULL);
        CreateWindowW(L"BUTTON", L"关闭课表", WS_CHILD | WS_VISIBLE,
            200, y, 160, hBtn, hWnd, (HMENU)IDC_KILL_BTN, NULL, NULL);
        CreateWindowW(L"BUTTON", L"关闭中心", WS_CHILD | WS_VISIBLE,
            380, y, 160, hBtn, hWnd, (HMENU)IDC_CLOSE_BTN, NULL, NULL);
        y += hBtn + 20;

        CreateWindowW(L"BUTTON", L"status", WS_CHILD | WS_VISIBLE,
            200, y, 160, hBtn, hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_STATUS_BTN)), NULL, NULL);
        // 最下面第二行第一格：显示时间（与 status 按钮同行同高）
        CreateWindowW(L"BUTTON", (g_hTimeWnd && IsWindow(g_hTimeWnd)) ? L"关闭时间" : L"显示时间",
            WS_CHILD | WS_VISIBLE,
            20, y, 160, hBtn, hWnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_TIME_BTN)), NULL, NULL);
        y += hBtn + gap;

        // 统一设置字体
        for (HWND child = GetWindow(hWnd, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
            SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(hFont), TRUE);

        RECT rc = { 0, 0, ScaleForDpi(386), y };
        AdjustWindowRect(&rc, WS_POPUP | WS_CAPTION, FALSE);
        SetWindowPos(hWnd, NULL, 0, 0, rc.right - rc.left, rc.bottom - rc.top, SWP_NOMOVE | SWP_NOZORDER);
        LayoutControlCenter(hWnd);
        ApplyControlCenterFont(hWnd);
        FitControlCenterToContent(hWnd);
        break;
    }
    case WM_SIZE:
        LayoutControlCenter(hWnd);
        return 0;
    case WM_DPICHANGED: {
        const UINT newDpi = HIWORD(wParam);
        const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
        RebuildControlCenterFont(newDpi);
        SetWindowPos(hWnd, NULL, suggested->left, suggested->top,
            suggested->right - suggested->left, suggested->bottom - suggested->top,
            SWP_NOZORDER | SWP_NOACTIVATE);
        ApplyControlCenterFont(hWnd);
        LayoutControlCenter(hWnd);
        FitControlCenterToContent(hWnd);
        InvalidateRect(hWnd, NULL, TRUE);
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_MAIN_MODE:
            if (HIWORD(wParam) == CBN_SELCHANGE) {
                g_mainMode = (DisplayMode)SendMessageW(GetDlgItem(hWnd, IDC_MAIN_MODE), CB_GETCURSEL, 0, 0);
                SaveConfig();
            }
            break;
        case IDC_SUB_MODE:
            if (HIWORD(wParam) == CBN_SELCHANGE) {
                g_subMode = (DisplayMode)SendMessageW(GetDlgItem(hWnd, IDC_SUB_MODE), CB_GETCURSEL, 0, 0);
                SaveConfig();
            }
            break;
        case IDC_BLACK_SCREEN:
            if (HIWORD(wParam) == CBN_SELCHANGE) {
                int sel = (int)SendMessageW(GetDlgItem(hWnd, IDC_BLACK_SCREEN), CB_GETCURSEL, 0, 0);
                g_blackScreenEnable = (sel == 0); // 0是启用，1是禁用
                SaveConfig();
            }
            break;
        case IDC_AUTODIAK:
            if (HIWORD(wParam) == CBN_SELCHANGE) {
                int sel = static_cast<int>(SendMessageW(GetDlgItem(hWnd, IDC_AUTODIAK), CB_GETCURSEL, 0, 0));
                g_usbAutoOpenEnable = (sel == 0);
                usb_watcher::SetAutoOpenEnabled(g_usbAutoOpenEnable);
                SaveConfig();
            }
            break;
        case IDC_FAST_BTN: {
            wchar_t valBuf[32];
            GetDlgItemTextW(hWnd, IDC_OFFSET_INPUT, valBuf, 32);
            int sec = _wtoi(valBuf);
            g_timeOffsetSec -= sec;
            SaveConfig();
            wchar_t buf[64];
            swprintf_s(buf, L"当前偏移：%+d 秒", g_timeOffsetSec);
            SetDlgItemTextW(hWnd, IDC_OFFSET_LABEL, buf);
            break;
        }
        case IDC_SLOW_BTN: {
            wchar_t valBuf[32];
            GetDlgItemTextW(hWnd, IDC_OFFSET_INPUT, valBuf, 32);
            int sec = _wtoi(valBuf);
            g_timeOffsetSec += sec;
            SaveConfig();
            wchar_t buf[64];
            swprintf_s(buf, L"当前偏移：%+d 秒", g_timeOffsetSec);
            SetDlgItemTextW(hWnd, IDC_OFFSET_LABEL, buf);
            break;
        }
        case IDC_START_BTN:
            StartTimetable();
            break;
        case IDC_KILL_BTN:
            KillTimetable();
            break;
        case IDC_CLOSE_BTN:
            DestroyWindow(hWnd);
            g_hControlCenter = nullptr;
            break;
        case IDC_STATUS_BTN:
            ShowStatusManager();
            break;
        case IDC_TIME_BTN:
            ToggleTimeWindow();
            break;
        }
        break;
    case WM_DESTROY:
        KillTimer(hWnd, SHUTDOWN_CHECK_TIMER);
        g_hControlCenter = nullptr;
        break;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

void ShowControlCenter() {
    if (g_hControlCenter && IsWindow(g_hControlCenter)) {
        ShowWindow(g_hControlCenter, SW_SHOW);
        SetForegroundWindow(g_hControlCenter);
        return;
    }

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = { sizeof(WNDCLASSEXW) };
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = ControlCenterWndProc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = CreateSolidBrush(RGB(50, 50, 55));
        wc.lpszClassName = CONTROL_CENTER_CLASS;
        RegisterClassExW(&wc);
        registered = true;
    }

    const int windowWidth = ScaleForDpi(276);
    const int windowHeight = ScaleForDpi(280);
    POINT cursor = {};
    GetCursorPos(&cursor);
    HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitorInfo = { sizeof(MONITORINFO) };
    GetMonitorInfoW(monitor, &monitorInfo);
    const int workWidth = monitorInfo.rcWork.right - monitorInfo.rcWork.left;
    const int workHeight = monitorInfo.rcWork.bottom - monitorInfo.rcWork.top;
    const int actualWidth = (std::min)(windowWidth, workWidth);
    const int actualHeight = (std::min)(windowHeight, workHeight);
    const int x = monitorInfo.rcWork.left + (workWidth - actualWidth) / 2;
    const int y = monitorInfo.rcWork.top + (workHeight - actualHeight) / 2;
    g_hControlCenter = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        CONTROL_CENTER_CLASS, L"课程表控制中心",
        WS_POPUP | WS_CAPTION | WS_VISIBLE,
        x, y, actualWidth, actualHeight,
        NULL, NULL, GetModuleHandleW(NULL), NULL);
    SetForegroundWindow(g_hControlCenter);
}

// 托盘回调
LRESULT CALLBACK LauncherWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        // 创建托盘
        memset(&nid, 0, sizeof(nid));
        nid.cbSize = sizeof(nid);
        nid.hWnd = hWnd;
        nid.uID = ID_TRAY_ICON;
        nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
        nid.uCallbackMessage = WM_TRAYICON;
        nid.hIcon = LoadIcon(NULL, IDI_APPLICATION);
        wcscpy_s(nid.szTip, L"课程表启动器");
        Shell_NotifyIconW(NIM_ADD, &nid);
        // 加载配置并自动启动课表？
        LoadConfig();
        // 可选：自动启动课表
        if (!IsTimetableRunning()) StartTimetable();
        SetTimer(hWnd, SHUTDOWN_CHECK_TIMER, 1000, NULL);
        break;

    case WM_TIMER:
        if (wParam == SHUTDOWN_CHECK_TIMER) CheckScheduledShutdown(hWnd);
        else if (wParam == EDITOR_CHECK_TIMER && g_courseEditorProcess && WaitForSingleObject(g_courseEditorProcess, 0) == WAIT_OBJECT_0) {
            KillTimer(hWnd, EDITOR_CHECK_TIMER);
            CloseHandle(g_courseEditorProcess);
            g_courseEditorProcess = nullptr;
            if (HWND timetable = FindWindowW(L"TimetableMainApp", nullptr)) { PostMessageW(timetable, WM_TIMETABLE_RELOAD, 0, 0); PostMessageW(timetable, WM_TIMETABLE_SHOW, 0, 0); }
        }
        break;

    case WM_TRAYICON:
        if (lParam == WM_LBUTTONUP) {
            ShowControlCenter();
        }
        else if (lParam == WM_RBUTTONUP) {
            POINT pt;
            GetCursorPos(&pt);
            HMENU hMenu = CreatePopupMenu();
            AppendMenuW(hMenu, MF_STRING, ID_TRAY_SHOW_LAUNCHER, L"控制中心");
            AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
            AppendMenuW(hMenu, MF_STRING, ID_TRAY_EDIT_COURSES, L"更改课程");
            AppendMenuW(hMenu, MF_STRING, ID_TRAY_EXIT, L"退出");
            SetForegroundWindow(hWnd);
            TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hWnd, NULL);
            PostMessage(hWnd, WM_NULL, 0, 0);
            DestroyMenu(hMenu);
        }
        break;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case ID_TRAY_SHOW_LAUNCHER:
            ShowControlCenter();
            break;
        case ID_TRAY_EDIT_COURSES: {
            if (g_courseEditorProcess) break;
            STARTUPINFOW si = { sizeof(si) };
            PROCESS_INFORMATION pi = {};
            std::wstring command = L"\"" + g_courseEditorExe + L"\"";
            if (CreateProcessW(g_courseEditorExe.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
                CloseHandle(pi.hThread);
                g_courseEditorProcess = pi.hProcess;
                KillTimetable();
                SetTimer(hWnd, EDITOR_CHECK_TIMER, 250, nullptr);
            } else {
                MessageBoxW(g_hControlCenter, L"无法启动课程修改程序，请检查 config.inf 中的 course_editor 路径。", L"错误", MB_OK | MB_ICONERROR);
            }
            break;
        }
        case ID_TRAY_EXIT:
            DestroyWindow(hWnd);
            break;
        }
        break;

    case WM_DESTROY:
        KillTimer(hWnd, SHUTDOWN_CHECK_TIMER);
        KillTimer(hWnd, EDITOR_CHECK_TIMER);
        if (g_shutdownDialog) {
            KillTimer(g_shutdownDialog, SHUTDOWN_DIALOG_TIMEOUT_TIMER);
            DestroyWindow(g_shutdownDialog);
            g_shutdownDialog = nullptr;
        }
        if (g_courseEditorProcess) {
            CloseHandle(g_courseEditorProcess);
            g_courseEditorProcess = nullptr;
        }
        if (g_hTimeWnd) DestroyWindow(g_hTimeWnd);
        Shell_NotifyIconW(NIM_DELETE, &nid);
        if (HWND timetable = FindWindowW(L"TimetableMainApp", nullptr)) PostMessageW(timetable, WM_CLOSE, 0, 0);
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProc(hWnd, msg, wParam, lParam);
    }
    return 0;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    g_controlCenterDpi = GetDpiForSystem();
    RebuildControlCenterFont(g_controlCenterDpi);
    InitCommonControls();
    CreateDirectoryW(L"D:\\Timetable", NULL);

    WNDCLASSEXW shutdownClass = { sizeof(WNDCLASSEXW) };
    shutdownClass.lpfnWndProc = ShutdownPromptWndProc;
    shutdownClass.hInstance = hInstance;
    shutdownClass.hCursor = LoadCursorW(NULL, IDC_ARROW);
    shutdownClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    shutdownClass.lpszClassName = SHUTDOWN_DIALOG_CLASS;
    g_shutdownDialogClassRegistered = RegisterClassExW(&shutdownClass) != 0;
    if (!g_shutdownDialogClassRegistered && GetLastError() == ERROR_CLASS_ALREADY_EXISTS)
        g_shutdownDialogClassRegistered = true;

    WNDCLASSEXW wc = { sizeof(WNDCLASSEXW) };
    wc.lpfnWndProc = LauncherWndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = LAUNCHER_CLASS;
    RegisterClassExW(&wc);

    HWND hWnd = CreateWindowExW(0, LAUNCHER_CLASS, L"", 0, 0, 0, 0, 0, NULL, NULL, hInstance, NULL);
    ShowWindow(hWnd, SW_HIDE);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    return (int)msg.wParam;
}


#undef WinMain
}
namespace combined_app {

constexpr DWORD TimetableStartupTimeoutMs = 5000;

bool WaitForWindowClass(const wchar_t* className, DWORD timeoutMs) {
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    do {
        if (FindWindowW(className, nullptr)) return true;
        Sleep(25);
    } while (GetTickCount64() < deadline);
    return FindWindowW(className, nullptr) != nullptr;
}

void RequestLauncherExit() {
    HWND launcherWindow = FindWindowW(L"TimetableLauncher", nullptr);
    if (launcherWindow) PostMessageW(launcherWindow, WM_CLOSE, 0, 0);
}

void RunLauncherWhenReady(HINSTANCE instance, HINSTANCE previousInstance, LPSTR commandLine) {
    if (!WaitForWindowClass(timetable_app::MAIN_CLASS_NAME, TimetableStartupTimeoutMs)) {
        return;
    }
    launcher_app::LauncherWinMain(instance, previousInstance, commandLine, SW_HIDE);
}

}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previousInstance, LPSTR commandLine, int showCommand) {
    std::thread launcherThread(
        combined_app::RunLauncherWhenReady, instance, previousInstance, commandLine);

    const int timetableExitCode = timetable_app::TimetableWinMain(
        instance, previousInstance, commandLine, showCommand);

    combined_app::RequestLauncherExit();
    if (launcherThread.joinable()) launcherThread.join();
    return timetableExitCode;
}
