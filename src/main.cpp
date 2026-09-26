#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <algorithm>
#include <array>
#include <cwchar>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <vector>
#include "MapSession.h"
#include "OverlayVisibility.h"
#include "resource.h"

namespace {
constexpr UINT InputMessage = WM_APP + 1, TrayMessage = WM_APP + 2, FocusLostMessage = WM_APP + 3, ShowHomeMessage = WM_APP + 4;
constexpr COLORREF Transparent = RGB(1, 2, 3);
constexpr UINT MenuSettings = 1, MenuExit = 2, MenuMap = 3, MenuHome = 4;
enum class Action { Key, Pick, Pan, Zoom };
struct Input { Action action; DWORD key; POINT point; HWND source; int wheelDelta; };
struct MapName { const wchar_t* id; const wchar_t* label; };
constexpr MapName maps[] = {
    {L"Erangel", L"海岛 / Erangel"}, {L"Miramar", L"沙漠 / Miramar"},
    {L"Taego", L"泰戈 / Taego"}, {L"Deston", L"帝斯顿 / Deston"},
    {L"Vikendi", L"维寒迪 / Vikendi"}, {L"Rondo", L"荣都 / Rondo"},
    {L"Sanhok", L"萨诺 / Sanhok"}, {L"Karakin", L"卡拉金 / Karakin"},
    {L"Paramo", L"帕拉莫 / Paramo"}, {L"Haven", L"褐湾 / Haven"}
};
struct Config {
    double reference = 100;
    std::wstring process = L"TslGame.exe";
    int panelX = 20, panelY = 20;
    RECT roi{};
    int width{}, height{}, dpi{};
    int mapIndex{};
    int fontSize = 13, distanceSize = 28, lineGap = 1, panelWidth = 340;
};
HINSTANCE instance;
HWND overlay{}, target{}, demoWindow{}, homeWindow{};
HHOOK keyboardHook{}, mouseHook{};
HWINEVENTHOOK foregroundHook{};
NOTIFYICONDATAW tray{};
Config config;
mortar::MapSession session;
mortar::OverlayVisibility overlayVisibility;
auto& measurement = session.measurement;
bool& active = session.active;
RECT clientScreen{}, roi{};
bool wasForeground{}, demo{}, settingsOpen{}, trayAdded{};
int roiStep{}, currentDpi = 96;
POINT roiFirst{};
std::wstring notice = L"打开地图后按 F8 启用，或用 M 切换。";
std::wstring configPath;
UINT taskbarCreated{};
void showMapDialog(HWND owner = nullptr);
void showHome();
void updateHome();

std::wstring number(double value, int precision = 1) {
    std::wostringstream out;
    out << std::fixed << std::setprecision(precision) << value;
    return out.str();
}
void redraw() {
    if (overlay) InvalidateRect(overlay, nullptr, FALSE);
    if (demoWindow) InvalidateRect(demoWindow, nullptr, FALSE);
}
RECT clientBounds() { return {0, 0, clientScreen.right - clientScreen.left, clientScreen.bottom - clientScreen.top}; }
bool inside(RECT rect, POINT p) { return PtInRect(&rect, p) != FALSE; }
POINT local(POINT screen) { return {screen.x - clientScreen.left, screen.y - clientScreen.top}; }
bool validRoi(RECT r, int w, int h) {
    return r.left >= 0 && r.top >= 0 && r.right <= w && r.bottom <= h &&
        r.right - r.left >= 100 && r.bottom - r.top >= 100;
}
bool parseNumber(const std::wstring& s, double& out) {
    wchar_t* end{};
    out = std::wcstod(s.c_str(), &end);
    return end != s.c_str() && *end == 0 && std::isfinite(out);
}
std::wstring readConfig(const wchar_t* key, const wchar_t* fallback) {
    wchar_t text[512]{};
    GetPrivateProfileStringW(L"AutoMortar", key, fallback, text, 512, configPath.c_str());
    return text;
}
int readInt(const wchar_t* key, int fallback) {
    double result{};
    return parseNumber(readConfig(key, L""), result) && result >= 0 && result <= 100000
        ? static_cast<int>(result) : fallback;
}
int readBounded(const wchar_t* key, int fallback, int minimum, int maximum) {
    const int value = readInt(key, fallback);
    return value >= minimum && value <= maximum ? value : fallback;
}
void loadConfig() {
    double reference{};
    if (parseNumber(readConfig(L"ReferenceMeters", L"100"), reference) && reference > 0 && reference <= 10000)
        config.reference = reference;
    config.process = readConfig(L"TargetProcess", L"TslGame.exe");
    if (config.process.empty() || config.process.find_first_of(L"\\/\r\n") != std::wstring::npos)
        config.process = L"TslGame.exe";
    config.panelX = readInt(L"PanelX", 20); config.panelY = readInt(L"PanelY", 20);
    config.roi = {readInt(L"RoiLeft", 0), readInt(L"RoiTop", 0), readInt(L"RoiRight", 0), readInt(L"RoiBottom", 0)};
    config.width = readInt(L"ClientWidth", 0); config.height = readInt(L"ClientHeight", 0);
    config.dpi = readInt(L"Dpi", 0);
    config.fontSize = readBounded(L"FontSize", 13, 9, 28);
    config.distanceSize = readBounded(L"DistanceSize", 28, 16, 64);
    config.lineGap = readBounded(L"LineGap", 1, 0, 16);
    config.panelWidth = readBounded(L"PanelWidth", 340, 220, 800);
    const auto selected = readConfig(L"LastMap", L"Erangel");
    for (int i = 0; i < static_cast<int>(std::size(maps)); ++i)
        if (selected == maps[i].id) config.mapIndex = i;
}
bool saveConfig() {
    bool ok = true;
    auto write = [&](const wchar_t* key, const std::wstring& value) {
        if (!WritePrivateProfileStringW(L"AutoMortar", key, value.c_str(), configPath.c_str())) ok = false;
    };
    write(L"ReferenceMeters", number(config.reference, 3)); write(L"TargetProcess", config.process);
    write(L"PanelX", std::to_wstring(config.panelX)); write(L"PanelY", std::to_wstring(config.panelY));
    write(L"RoiLeft", std::to_wstring(config.roi.left)); write(L"RoiTop", std::to_wstring(config.roi.top));
    write(L"RoiRight", std::to_wstring(config.roi.right)); write(L"RoiBottom", std::to_wstring(config.roi.bottom));
    write(L"ClientWidth", std::to_wstring(config.width)); write(L"ClientHeight", std::to_wstring(config.height));
    write(L"Dpi", std::to_wstring(config.dpi));
    write(L"LastMap", maps[config.mapIndex].id);
    write(L"FontSize", std::to_wstring(config.fontSize));
    write(L"DistanceSize", std::to_wstring(config.distanceSize));
    write(L"LineGap", std::to_wstring(config.lineGap));
    write(L"PanelWidth", std::to_wstring(config.panelWidth));
    return ok;
}
std::wstring profileKey() {
    const RECT bounds = clientBounds();
    return L"Scale." + std::wstring(maps[config.mapIndex].id) + L"." +
        std::to_wstring(bounds.right) + L"x" + std::to_wstring(bounds.bottom) + L"." + std::to_wstring(currentDpi);
}
void loadCurrentProfile() {
    const auto key = profileKey();
    if (session.profiles.find(key) == session.profiles.end()) {
        mortar::Scales scales{};
        for (int i = 0; i < 5; ++i) {
            wchar_t value[128]{};
            const auto field = L"Zoom" + std::to_wstring(i);
            GetPrivateProfileStringW(key.c_str(), field.c_str(), L"", value, 128, configPath.c_str());
            double scale{};
            if (parseNumber(value, scale) && scale > 0 && scale <= 10000) scales[i] = scale;
        }
        session.profiles[key] = scales;
    }
    session.useProfile(key);
}
bool saveCurrentScale() {
    const auto scale = session.savedScale();
    if (!scale || session.profile.empty()) return false;
    const auto field = L"Zoom" + std::to_wstring(session.zoom);
    return WritePrivateProfileStringW(session.profile.c_str(), field.c_str(),
        number(*scale, 12).c_str(), configPath.c_str()) != FALSE;
}
std::wstring scaleNotice() {
    return L"缩放 " + std::to_wstring(session.zoom) + L" / 4：" +
        (session.savedScale() ? L"已载入标定，可直接取点。" : L"此档未标定，请按 F6。");
}
void clearView(const std::wstring& reason) {
    session.restoreScale(); roiStep = 0; notice = reason; redraw();
}
void setActive(bool value) {
    overlayVisibility.mapStateChanged(active, value, GetTickCount64());
    session.setActive(value); roiStep = 0;
    notice = value ? scaleNotice() : L"3 秒后隐藏，距离已保留。M / F8 再次显示。";
    redraw();
}

bool matchesTarget(HWND hwnd) {
    if (!hwnd || hwnd == overlay || !IsWindowVisible(hwnd) || IsIconic(hwnd)) return false;
    if (demo) return hwnd == demoWindow;
    DWORD pid{}; GetWindowThreadProcessId(hwnd, &pid);
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;
    wchar_t path[32768]; DWORD size = 32768;
    const BOOL ok = QueryFullProcessImageNameW(process, 0, path, &size);
    CloseHandle(process);
    if (!ok) return false;
    const wchar_t* name = std::wcsrchr(path, L'\\');
    return _wcsicmp(name ? name + 1 : path, config.process.c_str()) == 0;
}
void updateTarget() {
    HWND foreground = GetForegroundWindow();
    if (settingsOpen || !matchesTarget(foreground)) {
        if (wasForeground) {
            clearView(L"已保留地图和缩放状态，返回后可继续测量。");
        }
        wasForeground = false; ShowWindow(overlay, SW_HIDE); return;
    }
    RECT rect{}; GetClientRect(foreground, &rect);
    POINT origin{}; ClientToScreen(foreground, &origin); OffsetRect(&rect, origin.x, origin.y);
    const int dpi = static_cast<int>(GetDpiForWindow(foreground));
    if (target != foreground || !EqualRect(&rect, &clientScreen) || dpi != currentDpi) {
        target = foreground; clientScreen = rect; currentDpi = dpi;
        loadCurrentProfile();
        clearView(L"窗口已定位。F5 选择本局地图；F10 复位到最小档。");
        const RECT bounds = clientBounds();
        roi = bounds;
        if (config.width == bounds.right && config.height == bounds.bottom && config.dpi == dpi &&
            validRoi(config.roi, bounds.right, bounds.bottom)) roi = config.roi;
        SetWindowPos(overlay, HWND_TOPMOST, rect.left, rect.top, bounds.right, bounds.bottom,
            SWP_NOACTIVATE);
    }
    wasForeground = true;
    if (overlayVisibility.visible(active, GetTickCount64())) {
        SetWindowPos(overlay, HWND_TOPMOST, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    } else ShowWindow(overlay, SW_HIDE);
}

void postInput(Action action, DWORD key = 0, POINT point = {}, int wheelDelta = 0) {
    HWND foreground = GetForegroundWindow();
    if (settingsOpen || !wasForeground || foreground != target) return;
    auto event = std::make_unique<Input>(Input{action, key, point, foreground, wheelDelta});
    if (PostMessageW(overlay, InputMessage, 0, reinterpret_cast<LPARAM>(event.get()))) event.release();
}
void CALLBACK foregroundProc(HWINEVENTHOOK, DWORD, HWND hwnd, LONG, LONG, DWORD, DWORD) {
    if (target && hwnd != target) PostMessageW(overlay, FocusLostMessage, 0, 0);
}
LRESULT CALLBACK keyboardProc(int code, WPARAM message, LPARAM param) {
    static std::array<bool, 256> down{};
    if (code == HC_ACTION) {
        const auto& key = *reinterpret_cast<KBDLLHOOKSTRUCT*>(param);
        if (!(key.flags & LLKHF_INJECTED) && key.vkCode < down.size()) {
            const bool pressed = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
            const bool released = message == WM_KEYUP || message == WM_SYSKEYUP;
            if (pressed && !down[key.vkCode]) {
                down[key.vkCode] = true;
                if (key.vkCode == VK_F5 || key.vkCode == VK_F10 || key.vkCode == VK_F6 || key.vkCode == VK_F7 || key.vkCode == VK_F8 ||
                    key.vkCode == VK_F9 || key.vkCode == VK_ESCAPE || key.vkCode == 'M' || key.vkCode == VK_SPACE)
                    postInput(Action::Key, key.vkCode);
            }
            if (released) down[key.vkCode] = false;
        }
    }
    return CallNextHookEx(nullptr, code, message, param);
}
LRESULT CALLBACK mouseProc(int code, WPARAM message, LPARAM param) {
    if (code == HC_ACTION) {
        const auto& mouse = *reinterpret_cast<MSLLHOOKSTRUCT*>(param);
        if (!(mouse.flags & LLMHF_INJECTED)) {
            if (message == WM_MOUSEWHEEL) postInput(Action::Zoom, 0, mouse.pt, GET_WHEEL_DELTA_WPARAM(mouse.mouseData));
            if (message == WM_LBUTTONDOWN || message == WM_MBUTTONDOWN) postInput(Action::Pan, 0, mouse.pt);
            if (message == WM_RBUTTONDOWN) {
                if ((GetAsyncKeyState(VK_MENU) & 0x8000) && !(GetAsyncKeyState(VK_LBUTTON) & 0x8000))
                    postInput(Action::Pick, 0, mouse.pt);
                else postInput(Action::Pan, 0, mouse.pt);
            }
        }
    }
    // Always preserve the original game input, including Alt+right-click.
    return CallNextHookEx(nullptr, code, message, param);
}
void handleInput(const Input& event) {
    // Reconcile geometry and focus before consuming queued screen coordinates.
    updateTarget();
    if (!wasForeground || event.source != target || GetForegroundWindow() != target) return;
    if (event.action == Action::Key) {
        switch (event.key) {
        case VK_F8: case 'M': setActive(!active); break;
        case VK_ESCAPE: setActive(false); break;
        case VK_F5: showMapDialog(); break;
        case VK_F10:
            session.resetZoom(); roiStep = 0;
            notice = L"已复位到 0 档。请确保游戏地图也处于最小缩放。";
            break;
        case VK_F6:
            if (active) {
                roiStep = 0; measurement.calibrate(config.reference);
                notice = L"Alt+右键选择已知线段起点 / 终点（" + number(config.reference, 0) + L" m）。";
            }
            break;
        case VK_F7:
            if (active) { clearView(L"Alt+右键选择地图有效区域左上角，再选右下角。"); roiStep = 1; }
            break;
        case VK_F9:
            session.clearResult(); roiStep = 0; notice = L"已清除上次距离和取点，已标定档位保留。";
            break;
        case VK_SPACE:
            if (active) clearView(L"重新居中：已清点，保留上次距离。" );
            break;
        }
    } else if (active) {
        POINT p = local(event.point);
        if (!inside(clientBounds(), p)) return;
        if (event.action == Action::Zoom) {
            if (!inside(roi, p)) return;
            if (session.wheel(event.wheelDelta)) {
                roiStep = 0; notice = scaleNotice();
            }
        } else if (event.action == Action::Pan) {
            clearView(L"检测到点击 / 拖动：已清点，保留上次距离。" );
        } else if (roiStep) {
            if (roiStep == 1) { roiFirst = p; roiStep = 2; notice = L"请选择地图区域右下角。"; }
            else {
                RECT selected{roiFirst.x, roiFirst.y, p.x, p.y};
                const RECT bounds = clientBounds();
                if (!validRoi(selected, bounds.right, bounds.bottom)) {
                    notice = L"区域无效：按左上、右下顺序，宽高至少 100 像素。";
                } else {
                    roi = selected; roiStep = 0; config.roi = roi;
                    config.width = bounds.right; config.height = bounds.bottom; config.dpi = currentDpi;
                    notice = saveConfig() ? L"地图区域已保存。" + scaleNotice() : L"区域已应用，但配置保存失败。";
                }
            }
        } else if (!inside(roi, p)) {
            notice = L"点击在地图有效区域之外，已忽略。";
        } else {
            const auto before = measurement.phase;
            if (!session.click({static_cast<double>(p.x), static_cast<double>(p.y)})) {
                notice = before == mortar::Phase::CalibrateSecond
                    ? L"标定线段至少需要 20 像素，请重新选终点。" : L"比例尺无效，请先按 F6 标定。";
            } else if (before == mortar::Phase::CalibrateSecond)
                notice = saveCurrentScale() ? L"本地图此档比例尺已保存，可开始取点。" : L"标定已生效，但保存失败；请检查配置目录权限。";
            else if (measurement.distance()) notice = L"测量完成。再次取点开始下一组。";
            else notice = L"已记录起点，请保持视图不变，再选终点。";
        }
    }
    redraw();
}

void text(HDC dc, const std::wstring& value, RECT rect, HFONT font, COLORREF color, UINT flags = DT_LEFT | DT_SINGLELINE | DT_VCENTER) {
    auto old = SelectObject(dc, font); SetBkMode(dc, TRANSPARENT);
    // Outline only the glyphs: the entire panel background remains transparent.
    SetTextColor(dc, RGB(0, 0, 0));
    for (POINT offset : {POINT{-1, 0}, POINT{1, 0}, POINT{0, -1}, POINT{0, 1}}) {
        RECT shadow = rect; OffsetRect(&shadow, offset.x, offset.y);
        DrawTextW(dc, value.c_str(), -1, &shadow, flags | DT_NOPREFIX);
    }
    SetTextColor(dc, color);
    DrawTextW(dc, value.c_str(), -1, &rect, flags | DT_NOPREFIX);
    SelectObject(dc, old);
}
void fill(HDC dc, RECT rect, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color); FillRect(dc, &rect, brush); DeleteObject(brush);
}
void segment(HDC dc, mortar::Point a, mortar::Point b, COLORREF color, int thickness = 2) {
    HPEN pen = CreatePen(PS_SOLID, thickness, color); auto old = SelectObject(dc, pen);
    MoveToEx(dc, static_cast<int>(a.x), static_cast<int>(a.y), nullptr);
    LineTo(dc, static_cast<int>(b.x), static_cast<int>(b.y));
    SelectObject(dc, old); DeleteObject(pen);
}
void marker(HDC dc, mortar::Point p, COLORREF color) {
    HPEN pen = CreatePen(PS_SOLID, 2, color); auto oldPen = SelectObject(dc, pen);
    auto oldBrush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
    const int x = static_cast<int>(p.x), y = static_cast<int>(p.y);
    Ellipse(dc, x - 7, y - 7, x + 8, y + 8);
    MoveToEx(dc, x - 12, y, nullptr); LineTo(dc, x + 13, y);
    MoveToEx(dc, x, y - 12, nullptr); LineTo(dc, x, y + 13);
    SelectObject(dc, oldBrush); SelectObject(dc, oldPen); DeleteObject(pen);
}
void draw(HDC dc, RECT bounds, bool opaquePreview = false, RECT* layoutBounds = nullptr) {
    fill(dc, bounds, opaquePreview ? RGB(36, 49, 47) : Transparent);
    if (opaquePreview) {
        for (int x = 0; x < bounds.right; x += 80) segment(dc, {double(x), 0}, {double(x), double(bounds.bottom)}, RGB(65, 79, 76), 1);
        for (int y = 0; y < bounds.bottom; y += 80) segment(dc, {0, double(y)}, {double(bounds.right), double(y)}, RGB(65, 79, 76), 1);
    }
    if (active) {
        if (measurement.first) marker(dc, *measurement.first, RGB(70, 231, 170));
        if (measurement.second) {
            segment(dc, *measurement.first, *measurement.second, RGB(247, 204, 104));
            marker(dc, *measurement.second, RGB(247, 204, 104));
        }
        if (roiStep == 2) marker(dc, {double(roiFirst.x), double(roiFirst.y)}, RGB(130, 190, 255));
    }
    const double scale = static_cast<double>(currentDpi) / 96;
    auto px = [&](int n) { return static_cast<int>(std::lround(n * scale)); };
    const int width = std::min(px(config.panelWidth), int(bounds.right));
    const int padding = px(4), gap = px(config.lineGap);
    HFONT font = CreateFontW(-px(config.fontSize), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
    HFONT big = CreateFontW(-px(config.distanceSize), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
    struct Row { std::wstring value; HFONT font; COLORREF color; int height{}; };
    std::vector<Row> rows;
    const COLORREF body = RGB(235, 242, 250);
    auto add = [&](const std::wstring& value) { rows.push_back({value, font, body}); };
    add(active ? L"AutoMortar · 测距中" : L"AutoMortar · 地图已关闭");
    const auto current = measurement.distance();
    const auto distance = current ? current : session.lastDistance;
    rows.push_back({distance ? number(*distance, 0) + L" m" : L"— m", big, RGB(247, 204, 104)});
    add(!current && distance ? L"上次结果 · 水平距离 · 未修正高差" : L"水平距离 · 未修正高差");
    if (active) {
    add(std::wstring(maps[config.mapIndex].label) + L" · 缩放 " + std::to_wstring(session.zoom) + L" / 4");
    std::wstring calibrated = L"已标定档位：";
    const auto found = session.profiles.find(session.profile);
    bool any = false;
    for (int i = 0; i < 5; ++i) if (found != session.profiles.end() && found->second[i]) {
        calibrated += std::to_wstring(i) + L"  "; any = true;
    }
    if (!any) calibrated += L"暂无";
    add(calibrated);
    add(measurement.metersPerPixel ? L"比例尺 " + number(*measurement.metersPerPixel, 3) + L" m / px" : L"当前档待标定 · F6");
    }
    add(notice);
    if (active) {
        add(L"F5 地图  F10 复位  F8 启停");
        add(L"F6 标定  F7 区域  F9 清除  Alt+右键取点");
    }
    int height = padding * 2;
    const int contentWidth = std::max(1, width - 2 * padding);
    for (auto& row : rows) {
        auto old = SelectObject(dc, row.font);
        RECT measured{0, 0, contentWidth, 0};
        DrawTextW(dc, row.value.c_str(), -1, &measured, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
        SelectObject(dc, old);
        row.height = measured.bottom + 2; // Space for the one-pixel glyph outline.
        height += row.height + gap;
    }
    height -= gap;
    const int left = std::clamp(config.panelX, 0, std::max(0, int(bounds.right) - width));
    const int top = std::clamp(config.panelY, 0, std::max(0, int(bounds.bottom) - height));
    if (layoutBounds) *layoutBounds = {left, top, left + width, top + height};
    int y = top + padding;
    for (const auto& row : rows) {
        text(dc, row.value, {left + padding, y, left + width - padding, y + row.height}, row.font, row.color, DT_LEFT | DT_WORDBREAK);
        y += row.height + gap;
    }
    DeleteObject(font); DeleteObject(big);
}
void paint(HWND hwnd) {
    PAINTSTRUCT ps{}; HDC dc = BeginPaint(hwnd, &ps);
    RECT bounds{}; GetClientRect(hwnd, &bounds);
    if (bounds.right > 0 && bounds.bottom > 0) {
        HDC memory = CreateCompatibleDC(dc);
        HBITMAP bitmap = CreateCompatibleBitmap(dc, bounds.right, bounds.bottom);
        auto old = SelectObject(memory, bitmap);
        draw(memory, bounds);
        BitBlt(dc, 0, 0, bounds.right, bounds.bottom, memory, 0, 0, SRCCOPY);
        SelectObject(memory, old); DeleteObject(bitmap); DeleteDC(memory);
    }
    EndPaint(hwnd, &ps);
}

std::wstring controlText(HWND dialog, int id) {
    wchar_t value[512]{}; GetDlgItemTextW(dialog, id, value, 512); return value;
}
INT_PTR CALLBACK mapProc(HWND dialog, UINT message, WPARAM wp, LPARAM) {
    if (message == WM_INITDIALOG) {
        for (const auto& map : maps) SendDlgItemMessageW(dialog, IDC_MAP, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(map.label));
        SendDlgItemMessageW(dialog, IDC_MAP, CB_SETCURSEL, config.mapIndex, 0);
        return TRUE;
    }
    if (message == WM_COMMAND && LOWORD(wp) == IDOK) {
        const auto selected = SendDlgItemMessageW(dialog, IDC_MAP, CB_GETCURSEL, 0, 0);
        if (selected < 0 || selected >= static_cast<LRESULT>(std::size(maps))) return TRUE;
        config.mapIndex = static_cast<int>(selected);
        loadCurrentProfile(); session.newMatch(session.profile); roiStep = 0;
        overlayVisibility.reset();
        notice = L"本局地图已选择，缩放 0 档。打开地图后可继续。";
        if (!saveConfig()) MessageBoxW(dialog, L"已切换地图，但配置保存失败。", L"AutoMortar", MB_OK | MB_ICONERROR);
        EndDialog(dialog, IDOK); return TRUE;
    }
    if (message == WM_CLOSE || (message == WM_COMMAND && LOWORD(wp) == IDCANCEL)) { EndDialog(dialog, IDCANCEL); return TRUE; }
    return FALSE;
}
void showMapDialog(HWND owner) {
    settingsOpen = true; clearView(L"选择本局地图。"); ShowWindow(overlay, SW_HIDE);
    HWND returnWindow = owner ? owner : target;
    DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_MAP), owner, mapProc, 0);
    settingsOpen = false;
    if (IsWindow(returnWindow)) SetForegroundWindow(returnWindow);
    redraw();
}
INT_PTR CALLBACK settingsProc(HWND dialog, UINT message, WPARAM wp, LPARAM) {
    if (message == WM_INITDIALOG) {
        SetDlgItemTextW(dialog, IDC_REFERENCE, number(config.reference, 3).c_str());
        SetDlgItemTextW(dialog, IDC_TARGET, config.process.c_str());
        SetDlgItemInt(dialog, IDC_PANEL_X, config.panelX, FALSE);
        SetDlgItemInt(dialog, IDC_PANEL_Y, config.panelY, FALSE);
        SetDlgItemInt(dialog, IDC_FONT_SIZE, config.fontSize, FALSE);
        SetDlgItemInt(dialog, IDC_DISTANCE_SIZE, config.distanceSize, FALSE);
        SetDlgItemInt(dialog, IDC_LINE_GAP, config.lineGap, FALSE);
        SetDlgItemInt(dialog, IDC_PANEL_WIDTH, config.panelWidth, FALSE);
        return TRUE;
    }
    if (message == WM_COMMAND && LOWORD(wp) == IDOK) {
        double reference{}, x{}, y{};
        const auto process = controlText(dialog, IDC_TARGET);
        if (!parseNumber(controlText(dialog, IDC_REFERENCE), reference) || reference <= 0 || reference > 10000 ||
            !parseNumber(controlText(dialog, IDC_PANEL_X), x) || x < 0 || x > 100000 ||
            !parseNumber(controlText(dialog, IDC_PANEL_Y), y) || y < 0 || y > 100000 ||
            process.empty() || process.find_first_of(L"\\/\r\n") != std::wstring::npos) {
            MessageBoxW(dialog, L"请输入有效长度（0–10000 米）、非负面板坐标和进程文件名。", L"设置无效", MB_OK); return TRUE;
        }
        auto integer = [&](int id, int low, int high, int& value) {
            double number{};
            if (!parseNumber(controlText(dialog, id), number) || number < low || number > high || number != std::floor(number)) return false;
            value = static_cast<int>(number); return true;
        };
        int fontSize{}, distanceSize{}, lineGap{}, panelWidth{};
        if (!integer(IDC_FONT_SIZE, 9, 28, fontSize) || !integer(IDC_DISTANCE_SIZE, 16, 64, distanceSize) ||
            !integer(IDC_LINE_GAP, 0, 16, lineGap) || !integer(IDC_PANEL_WIDTH, 220, 800, panelWidth)) {
            MessageBoxW(dialog, L"请在标注范围内输入整数：正文字号 9–28，距离字号 16–64，行距 0–16，宽度 220–800。", L"菜单外观无效", MB_OK); return TRUE;
        }
        config.fontSize = fontSize; config.distanceSize = distanceSize;
        config.lineGap = lineGap; config.panelWidth = panelWidth;
        if (config.process != process) { config.roi = {}; config.width = config.height = config.dpi = 0; target = nullptr; }
        config.reference = reference; config.process = process;
        config.panelX = static_cast<int>(x); config.panelY = static_cast<int>(y);
        if (!saveConfig()) MessageBoxW(dialog, L"设置已应用，但无法保存 AutoMortar.ini。", L"保存失败", MB_OK | MB_ICONERROR);
        redraw();
        EndDialog(dialog, IDOK); return TRUE;
    }
    if (message == WM_CLOSE || (message == WM_COMMAND && LOWORD(wp) == IDCANCEL)) { EndDialog(dialog, IDCANCEL); return TRUE; }
    return FALSE;
}
void showSettings(HWND owner = nullptr) {
    settingsOpen = true; clearView(L"调整设置。"); ShowWindow(overlay, SW_HIDE);
    DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_SETTINGS), owner, settingsProc, 0);
    settingsOpen = false; redraw();
}
void refreshHome(HWND dialog) {
    const bool connected = target && IsWindow(target);
    std::wstring status = connected ? L"已找到游戏窗口" : L"等待游戏窗口（启动游戏后切回游戏即可）";
    status += L"\r\n当前地图：" + std::wstring(maps[config.mapIndex].label) + L" · 缩放 " + std::to_wstring(session.zoom) + L" / 4";
    if (controlText(dialog, IDC_HOME_STATUS) != status) SetDlgItemTextW(dialog, IDC_HOME_STATUS, status.c_str());
}
void updateHome() { if (homeWindow && IsWindowVisible(homeWindow)) refreshHome(homeWindow); }
INT_PTR CALLBACK homeProc(HWND dialog, UINT message, WPARAM wp, LPARAM) {
    if (message == WM_INITDIALOG) {
        HFONT heading = CreateFontW(-MulDiv(24, GetDpiForWindow(dialog), 96), 0, 0, 0, FW_BOLD,
            FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
        SetPropW(dialog, L"HeadingFont", heading);
        SendDlgItemMessageW(dialog, IDC_HOME_TITLE, WM_SETFONT, reinterpret_cast<WPARAM>(heading), FALSE);
        SendMessageW(dialog, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(LoadIconW(nullptr, IDI_APPLICATION)));
        refreshHome(dialog); return TRUE;
    }
    if (message == WM_CLOSE || (message == WM_COMMAND && (LOWORD(wp) == IDCANCEL || LOWORD(wp) == IDC_HOME_HIDE))) {
        ShowWindow(dialog, SW_HIDE); return TRUE;
    }
    if (message == WM_COMMAND) {
        if (LOWORD(wp) == IDC_HOME_MAP) { showMapDialog(dialog); refreshHome(dialog); return TRUE; }
        if (LOWORD(wp) == IDC_HOME_SETTINGS) { showSettings(dialog); refreshHome(dialog); return TRUE; }
        if (LOWORD(wp) == IDC_HOME_EXIT) { PostMessageW(overlay, WM_CLOSE, 0, 0); return TRUE; }
    }
    if (message == WM_NCDESTROY) {
        DeleteObject(RemovePropW(dialog, L"HeadingFont"));
        if (homeWindow == dialog) homeWindow = nullptr;
    }
    return FALSE;
}
void showHome() {
    if (!IsWindow(homeWindow)) homeWindow = CreateDialogParamW(instance, MAKEINTRESOURCEW(IDD_HOME), nullptr, homeProc, 0);
    if (!homeWindow) {
        MessageBoxW(nullptr, L"无法打开主窗口，请从托盘打开设置。", L"AutoMortar", MB_OK | MB_ICONERROR); return;
    }
    refreshHome(homeWindow);
    ShowWindow(homeWindow, SW_RESTORE); SetForegroundWindow(homeWindow);
}
bool addTray() {
    tray = {}; tray.cbSize = sizeof(tray); tray.hWnd = overlay; tray.uID = 1;
    tray.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    tray.uCallbackMessage = TrayMessage; tray.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(tray.szTip, L"AutoMortar — 双击打开主窗口，右键更多操作");
    trayAdded = Shell_NotifyIconW(NIM_ADD, &tray) != FALSE;
    return trayAdded;
}
LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    if (taskbarCreated && message == taskbarCreated) { addTray(); return 0; }
    switch (message) {
    case WM_PAINT: paint(hwnd); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_NCHITTEST: return HTTRANSPARENT;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_TIMER: updateTarget(); updateHome(); return 0;
    case ShowHomeMessage: showHome(); return 0;
    case FocusLostMessage:
        clearView(L"已保留地图状态与上次距离。"); wasForeground = false; ShowWindow(hwnd, SW_HIDE); return 0;
    case WM_DISPLAYCHANGE: target = nullptr; clearView(L"显示设置变化，重新匹配比例尺配置。"); return 0;
    case WM_DPICHANGED: target = nullptr; return 0;
    case InputMessage: {
        std::unique_ptr<Input> event(reinterpret_cast<Input*>(lp)); handleInput(*event); return 0;
    }
    case TrayMessage:
        if (lp == WM_LBUTTONDBLCLK) { showHome(); return 0; }
        if (lp == WM_RBUTTONUP || lp == WM_CONTEXTMENU) {
            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, MenuHome, L"打开主窗口");
            AppendMenuW(menu, MF_STRING, MenuMap, L"选择本局地图… (F5)");
            AppendMenuW(menu, MF_STRING, MenuSettings, L"设置…"); AppendMenuW(menu, MF_STRING, MenuExit, L"退出");
            POINT point{}; GetCursorPos(&point); SetForegroundWindow(hwnd);
            const UINT selected = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, 0, hwnd, nullptr);
            DestroyMenu(menu); PostMessageW(hwnd, WM_NULL, 0, 0);
            if (selected == MenuExit) DestroyWindow(hwnd);
            if (selected == MenuHome) showHome();
            if (selected == MenuMap) showMapDialog();
            if (selected == MenuSettings) showSettings();
        }
        return 0;
    case WM_CLOSE: DestroyWindow(hwnd); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, message, wp, lp);
}

LRESULT CALLBACK demoProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_PAINT) {
        PAINTSTRUCT ps{}; HDC dc = BeginPaint(hwnd, &ps); RECT bounds{}; GetClientRect(hwnd, &bounds);
        fill(dc, bounds, RGB(36, 49, 47));
        const double factor = 1.0 + 0.25 * session.zoom;
        const int grid = static_cast<int>(80 * factor);
        constexpr int centerX = 800, centerY = 400;
        for (int x = centerX % grid; x < bounds.right; x += grid) segment(dc, {double(x), 0}, {double(x), double(bounds.bottom)}, RGB(105, 121, 115), 1);
        for (int y = centerY % grid; y < bounds.bottom; y += grid) segment(dc, {0, double(y)}, {double(bounds.right), double(y)}, RGB(105, 121, 115), 1);
        marker(dc, {centerX - 120 * factor, centerY - 160 * factor}, RGB(70, 231, 170));
        marker(dc, {centerX + 120 * factor, centerY + 160 * factor}, RGB(247, 204, 104));
        HFONT font = CreateFontW(-20, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
        text(dc, L"演示地图：每格 100m / " + std::to_wstring(grid) + L"px；两十字相距 500m。", {480, 30, 1150, 80}, font, RGB(235, 240, 235));
        text(dc, L"F8 → F6 → Alt+右键选择一格两端 → 选择两十字", {480, 80, 1150, 120}, font, RGB(235, 240, 235));
        DeleteObject(font); EndPaint(hwnd, &ps); return 0;
    }
    if (message == WM_CLOSE) { DestroyWindow(overlay); DestroyWindow(hwnd); return 0; }
    return DefWindowProcW(hwnd, message, wp, lp);
}

bool renderPreview() {
    constexpr int width = 1200, height = 760;
    HDC dc = CreateCompatibleDC(nullptr);
    BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width; info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
    void* bits{}; HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap) { DeleteDC(dc); return false; }
    auto old = SelectObject(dc, bitmap);
    currentDpi = 96; config.panelX = config.panelY = 20;
    session.newMatch(L"preview"); session.setActive(true);
    measurement.calibrate(100); session.click({0, 0}); session.click({80, 0});
    session.click({560, 320}); session.click({800, 640});
    notice = L"测量完成。再次取点开始下一组。";
    draw(dc, {0, 0, width, height}, true); GdiFlush();
    BITMAPFILEHEADER header{}; header.bfType = 0x4d42;
    header.bfOffBits = sizeof(header) + sizeof(BITMAPINFOHEADER);
    header.bfSize = header.bfOffBits + width * height * 4;
    std::ofstream file("overlay-preview.bmp", std::ios::binary);
    file.write(reinterpret_cast<char*>(&header), sizeof(header));
    file.write(reinterpret_cast<char*>(&info.bmiHeader), sizeof(BITMAPINFOHEADER));
    file.write(static_cast<char*>(bits), width * height * 4);
    const bool ok = file.good();
    SelectObject(dc, old); DeleteObject(bitmap); DeleteDC(dc); return ok;
}
bool renderDialogPreview(int resource, DLGPROC procedure, const char* filename) {
    HWND dialog = CreateDialogParamW(instance, MAKEINTRESOURCEW(resource), nullptr, procedure, 0);
    if (!dialog) return false;
    // Native edit controls only print reliably while visible. Keep this render-only
    // window outside the virtual desktop and never activate it.
    const int offscreen = GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN) + 100;
    SetWindowPos(dialog, HWND_BOTTOM, offscreen, 100, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    UpdateWindow(dialog);
    RECT bounds{}; GetClientRect(dialog, &bounds);
    const int width = bounds.right, height = bounds.bottom;
    HDC dc = CreateCompatibleDC(nullptr);
    BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width; info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
    void* bits{};
    HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap) { DeleteDC(dc); DestroyWindow(dialog); return false; }
    auto old = SelectObject(dc, bitmap);
    FillRect(dc, &bounds, GetSysColorBrush(COLOR_3DFACE));
    const BOOL printed = PrintWindow(dialog, dc, PW_CLIENTONLY);
    GdiFlush();
    BITMAPFILEHEADER header{}; header.bfType = 0x4d42;
    header.bfOffBits = sizeof(header) + sizeof(BITMAPINFOHEADER);
    header.bfSize = header.bfOffBits + width * height * 4;
    std::ofstream file(filename, std::ios::binary);
    file.write(reinterpret_cast<char*>(&header), sizeof(header));
    file.write(reinterpret_cast<char*>(&info.bmiHeader), sizeof(BITMAPINFOHEADER));
    file.write(static_cast<char*>(bits), width * height * 4);
    const bool ok = printed && file.good();
    SelectObject(dc, old); DeleteObject(bitmap); DeleteDC(dc); DestroyWindow(dialog);
    return ok;
}
bool profilePersistenceCheck() {
    wchar_t temporary[MAX_PATH]{};
    if (!GetTempFileNameW(L".", L"amt", 0, temporary)) return false;
    const auto originalPath = configPath;
    const auto originalConfig = config;
    const auto originalSession = session;
    const auto originalRect = clientScreen;
    const int originalDpi = currentDpi;
    configPath = std::filesystem::absolute(temporary).wstring();
    config.mapIndex = 0; currentDpi = 96; clientScreen = {0, 0, 1920, 1080};
    session = {};
    loadCurrentProfile();
    session.setActive(true); measurement.calibrate(100);
    session.click({0, 0}); session.click({73, 0});
    bool ok = saveCurrentScale();
    session.wheel(120); measurement.calibrate(100);
    session.click({0, 0}); session.click({160, 0}); ok = saveCurrentScale() && ok;
    const auto firstKey = session.profile;
    // Malformed slots are ignored without discarding the valid slots.
    WritePrivateProfileStringW(firstKey.c_str(), L"Zoom2", L"nan", configPath.c_str());
    WritePrivateProfileStringW(firstKey.c_str(), L"Zoom3", L"-2", configPath.c_str());
    config.mapIndex = 1; loadCurrentProfile();
    session.measurement.calibrate(100); session.click({0, 0}); session.click({200, 0});
    config.fontSize = 11; config.distanceSize = 24; config.lineGap = 0; config.panelWidth = 280;
    ok = saveCurrentScale() && saveConfig() && ok;
    config.mapIndex = 0; config.fontSize = 28; config.distanceSize = 64; config.lineGap = 16; config.panelWidth = 800;
    loadConfig();
    ok = config.mapIndex == 1 && config.fontSize == 11 && config.distanceSize == 24 && config.lineGap == 0 && config.panelWidth == 280 && ok;
    WritePrivateProfileStringW(L"AutoMortar", L"FontSize", L"9999", configPath.c_str());
    WritePrivateProfileStringW(L"AutoMortar", L"PanelWidth", L"-1", configPath.c_str());
    loadConfig(); ok = config.fontSize == 13 && config.panelWidth == 340 && ok;
    session = {}; config.mapIndex = 0; loadCurrentProfile();
    ok = session.savedScale() && std::abs(*session.savedScale() - 100.0 / 73) < 1e-10 && ok;
    session.setActive(true); session.wheel(120); ok = session.savedScale() == 0.625 && ok;
    session.wheel(120); ok = !session.savedScale() && ok;
    session.wheel(120); ok = !session.savedScale() && ok;
    session.resetZoom(); config.mapIndex = 1; loadCurrentProfile();
    session.wheel(120); ok = session.savedScale() == 0.5 && ok;
    clientScreen = {0, 0, 2560, 1440}; loadCurrentProfile(); ok = !session.savedScale() && ok;
    configPath = originalPath; config = originalConfig; session = originalSession;
    clientScreen = originalRect; currentDpi = originalDpi;
    DeleteFileW(temporary);
    return ok;
}
}

int WINAPI wWinMain(HINSTANCE appInstance, HINSTANCE, PWSTR commandLine, int) {
    instance = appInstance;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const std::wstring command = commandLine;
    demo = command == L"--demo";
    wchar_t path[32768]{}; GetModuleFileNameW(nullptr, path, 32768);
    configPath = (std::filesystem::path(path).parent_path() / (demo ? L"AutoMortar.demo.ini" : L"AutoMortar.ini")).wstring();
    loadConfig();
    if (command == L"--render-preview") return renderPreview() ? 0 : 1;
    if (command == L"--render-ui-preview") {
        return renderDialogPreview(IDD_HOME, homeProc, "home-preview.bmp") &&
            renderDialogPreview(IDD_SETTINGS, settingsProc, "settings-preview.bmp") ? 0 : 1;
    }
    const bool smoke = command == L"--smoke-test";
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\AutoMortar.Desktop.v1");
    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (mutex) CloseHandle(mutex);
        if (!smoke) {
            HWND running = FindWindowW(L"AutoMortar.Overlay", L"AutoMortar");
            if (running) {
                DWORD pid{}; GetWindowThreadProcessId(running, &pid); AllowSetForegroundWindow(pid);
                PostMessageW(running, ShowHomeMessage, 0, 0); return 0;
            }
            MessageBoxW(nullptr, L"AutoMortar 正在启动，请稍后从托盘打开主窗口。", L"AutoMortar", MB_OK);
        }
        return 1;
    }
    WNDCLASSW cls{}; cls.hInstance = instance; cls.lpfnWndProc = windowProc;
    cls.lpszClassName = L"AutoMortar.Overlay"; cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (!RegisterClassW(&cls)) { CloseHandle(mutex); return 2; }
    overlay = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        cls.lpszClassName, L"AutoMortar", WS_POPUP, 0, 0, 1200, 760, nullptr, nullptr, instance, nullptr);
    if (!overlay) { CloseHandle(mutex); return 3; }
    // Only text and markers are opaque; the whole background uses the color key.
    if (!SetLayeredWindowAttributes(overlay, Transparent, 255, LWA_COLORKEY)) {
        DestroyWindow(overlay); CloseHandle(mutex); return 4;
    }
    keyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, keyboardProc, instance, 0);
    mouseHook = SetWindowsHookExW(WH_MOUSE_LL, mouseProc, instance, 0);
    foregroundHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND,
        nullptr, foregroundProc, 0, 0, WINEVENT_OUTOFCONTEXT);
    int result = 0;
    if (!keyboardHook || !mouseHook || !foregroundHook) {
        if (!smoke) MessageBoxW(nullptr, L"键鼠监听安装失败，程序将退出。", L"AutoMortar", MB_OK | MB_ICONERROR);
        result = 5;
    } else if (smoke) {
        const auto styles = GetWindowLongPtrW(overlay, GWL_EXSTYLE);
        const auto required = WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOPMOST;
        if ((styles & required) != required || SendMessageW(overlay, WM_NCHITTEST, 0, 0) != HTTRANSPARENT) result = 6;
        HDC dc = GetDC(overlay); HDC memory = CreateCompatibleDC(dc);
        HBITMAP bitmap = CreateCompatibleBitmap(dc, 1200, 760); auto old = SelectObject(memory, bitmap);
        config.panelX = config.panelY = 20;
        active = true;
        measurement.calibrate(100); measurement.click({0, 0}); measurement.click({80, 0});
        measurement.click({560, 320}); measurement.click({800, 640});
        // Warm the font/window caches before checking steady-state resource use.
        for (int n = 0; n < 100; ++n) draw(memory, {0, 0, 1200, 760});
        GdiFlush();
        if (GetPixel(memory, 21, 21) != Transparent) result = 11;
        const DWORD before = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
        for (int n = 0; n < 100; ++n) draw(memory, {0, 0, 1200, 760});
        GdiFlush();
        const DWORD after = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
        std::fprintf(stdout, "GDI objects before/after 100 draws: %lu / %lu\n", before, after);
        if (after > before) result = 7;
        const Config savedConfig = config;
        config.fontSize = 13; config.distanceSize = 28; config.lineGap = 1; config.panelWidth = 340;
        RECT compact{}; draw(memory, {0, 0, 1200, 760}, false, &compact);
        config.fontSize = 18; config.distanceSize = 42; config.lineGap = 6; config.panelWidth = 460;
        RECT large{}; draw(memory, {0, 0, 1200, 760}, false, &large);
        if (compact.right - compact.left != 340 || large.right - large.left != 460 ||
            compact.bottom - compact.top >= large.bottom - large.top) result = 16;
        std::fprintf(stdout, "Compact / large overlay height: %ld / %ld\n", compact.bottom - compact.top, large.bottom - large.top);
        config = savedConfig;
        SelectObject(memory, old); DeleteObject(bitmap); DeleteDC(memory); ReleaseDC(overlay, dc);
        HWND dialog = CreateDialogParamW(instance, MAKEINTRESOURCEW(IDD_SETTINGS), nullptr, settingsProc, 0);
        if (!dialog || controlText(dialog, IDC_TARGET) != config.process) result = 10;
        if (dialog) DestroyWindow(dialog);
        dialog = CreateDialogParamW(instance, MAKEINTRESOURCEW(IDD_MAP), nullptr, mapProc, 0);
        if (!dialog || SendDlgItemMessageW(dialog, IDC_MAP, CB_GETCOUNT, 0, 0) != std::size(maps)) result = 12;
        if (dialog) DestroyWindow(dialog);
        dialog = CreateDialogParamW(instance, MAKEINTRESOURCEW(IDD_HOME), nullptr, homeProc, 0);
        if (!dialog || !GetDlgItem(dialog, IDC_HOME_SETTINGS) || !GetDlgItem(dialog, IDC_HOME_MAP)) result = 14;
        if (dialog) {
            SendMessageW(dialog, WM_CLOSE, 0, 0);
            if (!IsWindow(dialog) || IsWindowVisible(dialog)) result = 15;
            DestroyWindow(dialog);
        }
        if (!profilePersistenceCheck()) result = 13;
    } else {
        taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
        if (!addTray()) {
            MessageBoxW(nullptr, L"无法创建托盘图标，程序将退出。", L"AutoMortar", MB_OK | MB_ICONERROR); result = 8;
        } else {
            if (demo) {
                cls.lpfnWndProc = demoProc; cls.lpszClassName = L"AutoMortar.Demo"; RegisterClassW(&cls);
                RECT rect{0, 0, 1200, 760}; AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
                demoWindow = CreateWindowW(cls.lpszClassName, L"AutoMortar 演示地图", WS_OVERLAPPEDWINDOW,
                    CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, instance, nullptr);
                ShowWindow(demoWindow, SW_SHOW); SetForegroundWindow(demoWindow);
            }
            SetTimer(overlay, 1, 100, nullptr);
            if (!demo) showHome();
            MSG msg{}; BOOL status;
            while ((status = GetMessageW(&msg, nullptr, 0, 0)) > 0) {
                if (homeWindow && IsWindowVisible(homeWindow) && IsDialogMessageW(homeWindow, &msg)) continue;
                TranslateMessage(&msg); DispatchMessageW(&msg);
            }
            if (status == -1) result = 9;
        }
    }
    if (keyboardHook) UnhookWindowsHookEx(keyboardHook);
    if (mouseHook) UnhookWindowsHookEx(mouseHook);
    if (foregroundHook) UnhookWinEvent(foregroundHook);
    if (trayAdded) Shell_NotifyIconW(NIM_DELETE, &tray);
    KillTimer(overlay, 1);
    if (IsWindow(overlay)) DestroyWindow(overlay);
    if (IsWindow(demoWindow)) DestroyWindow(demoWindow);
    if (IsWindow(homeWindow)) DestroyWindow(homeWindow);
    MSG remaining{};
    while (PeekMessageW(&remaining, nullptr, InputMessage, InputMessage, PM_REMOVE)) delete reinterpret_cast<Input*>(remaining.lParam);
    CloseHandle(mutex);
    if (smoke) std::fprintf(stdout, "Windows smoke result: %d\n", result);
    return result;
}
