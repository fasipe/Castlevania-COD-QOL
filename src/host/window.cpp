#include "host/window.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#include <immintrin.h>
#include <mmsystem.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include "host/controls_dialog.h"
#include "host/icon.h"
#include "host/screenshot.h"
#include "host/settings.h"
#include "iop/pad.h"

namespace {
// The keyboard layout is in the settings (F1 changes it).
bool gHeld[256] = {};
WINDOWPLACEMENT gWindowed = {sizeof(WINDOWPLACEMENT)};
std::wstring gTitle;

// The title says which proportion is on and which keys change the picture and the settings.
void updateTitle(HWND hwnd) {
  const std::wstring mode = settings().wide ? L"16:9" : L"4:3";
  SetWindowTextW(hwnd, (gTitle + L"  |  " + mode +
                        (english() ? L"  |  Alt+Enter: full screen  |  F10: 4:3 / 16:9  |  F9: FPS  |  F1: options and controls"
                                   : L"  |  Alt+Enter: tela cheia  |  F10: 4:3 / 16:9  |  F9: FPS  |  F1: opções e controles"))
                           .c_str());
}

// Alt+Enter / F11: borderless window covering the monitor, and back.
void toggleFullscreen(HWND hwnd) {
  const LONG style = GetWindowLongW(hwnd, GWL_STYLE);
  if (style & WS_OVERLAPPEDWINDOW) {
    MONITORINFO mi = {sizeof(mi)};
    if (!GetWindowPlacement(hwnd, &gWindowed) || !GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi)) return;
    SetWindowLongW(hwnd, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
    SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                 mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
  } else {
    SetWindowLongW(hwnd, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
    SetWindowPlacement(hwnd, &gWindowed);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
  }
  InvalidateRect(hwnd, nullptr, TRUE);
  settings().fullscreen = (style & WS_OVERLAPPEDWINDOW) != 0;  // it was windowed: now it is not
  settings().save();
}
PadInput gKeyboard;

void setKey(int vk, bool down);

// The mouse turns the camera while the window holds it: the cursor hidden and kept inside, its
// motion read raw (no pointer acceleration) and added up until the frame takes it. A click in the
// window holds it; Esc, F1 or another window lets it go.
bool gMouseHeld = false;
std::atomic<int> gMouseX{0}, gMouseY{0};

void holdMouse(HWND hwnd, bool on) {
  on = on && settings().mouseCamera && !IsIconic(hwnd);
  if (on) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    MapWindowPoints(hwnd, nullptr, reinterpret_cast<POINT*>(&rc), 2);
    ClipCursor(&rc);
  } else if (gMouseHeld) {
    ClipCursor(nullptr);
  }
  if (on != gMouseHeld) {
    gMouseHeld = on;
    if (!on)  // its buttons are let go with it
      for (int vk : {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON}) setKey(vk, false);
    gMouseX = 0, gMouseY = 0;
    // The cursor: hidden over the window while held (and in WM_SETCURSOR), shown again when let go.
    SetCursor(on ? nullptr : LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW)));
  }
}

int mouseButton(UINT msg) {
  return msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP ? VK_LBUTTON : msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP ? VK_RBUTTON : VK_MBUTTON;
}

bool cursorInside(HWND hwnd) {
  POINT pt;
  RECT rc;
  if (!GetCursorPos(&pt) || !ScreenToClient(hwnd, &pt) || !GetClientRect(hwnd, &rc)) return false;
  return PtInRect(&rc, pt) != 0;
}

void setKey(int vk, bool down) {
  if (vk < 0 || vk >= 256) return;
  gHeld[vk] = down;
  const Settings& s = settings();
  uint16_t buttons = 0;
  uint8_t stick = 0;  // left stick up/down/left/right, then the right one (setPad)
  for (int c = 0; c < Settings::kControls; ++c) {
    if (!s.keys[c] || !gHeld[s.keys[c]]) continue;
    if (c < Settings::kButtons) buttons |= uint16_t(1u << c);
    else stick |= uint8_t(1u << (c - Settings::kLeftUp));
  }
  setPad(gKeyboard, buttons, stick);
}

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_KEYDOWN: case WM_SYSKEYDOWN:
      if (wp == VK_F10) {
        if (!(lp & (1 << 30))) {
          settings().wide = !settings().wide;
          settings().save();
          updateTitle(hwnd);
          InvalidateRect(hwnd, nullptr, TRUE);
        }
        return 0;
      }
      if (wp == VK_F8) {
        if (!(lp & (1 << 30))) {
          settings().minimap = !settings().minimap;
          settings().save();
        }
        return 0;
      }
      if (wp == VK_F12) {  // a screenshot, in screenshots\ next to the memory card folder
        if (!(lp & (1 << 30))) {
          SYSTEMTIME t;
          GetLocalTime(&t);
          std::filesystem::path dir = iconFile().parent_path();
          if (dir.filename() == "build") dir = dir.parent_path();
          wchar_t name[64];
          swprintf(name, 64, L"castlevania-%04u%02u%02u-%02u%02u%02u.png", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
          requestScreenshot(dir / L"screenshots" / name);
        }
        return 0;
      }
      if (wp == VK_F9) {
        if (!(lp & (1 << 30))) {
          settings().showFps = !settings().showFps;
          settings().save();
          InvalidateRect(hwnd, nullptr, TRUE);
        }
        return 0;
      }
      if (wp == VK_F1) {
        if (!(lp & (1 << 30))) {
          holdMouse(hwnd, false);
          std::fill(std::begin(gHeld), std::end(gHeld), false);
          gKeyboard = {};
          showControlsDialog(hwnd);
          updateTitle(hwnd);  // in the language chosen there
        }
        return 0;
      }
      if (wp == VK_F11 || (wp == VK_RETURN && msg == WM_SYSKEYDOWN)) {  // F11, Alt+Enter
        if (!(lp & (1 << 30))) toggleFullscreen(hwnd);
        return 0;
      }
      if (wp == VK_ESCAPE) holdMouse(hwnd, false);
      setKey(int(wp), true);
      return 0;
    case WM_KEYUP: case WM_SYSKEYUP: setKey(int(wp), false); return 0;
    case WM_KILLFOCUS:
      holdMouse(hwnd, false);
      std::fill(std::begin(gHeld), std::end(gHeld), false);
      gKeyboard = {};
      return 0;
    case WM_ACTIVATE:  // back by keyboard (Alt+Tab) with the cursor over the game: held again
      if (LOWORD(wp) == WA_ACTIVE && cursorInside(hwnd)) holdMouse(hwnd, true);
      break;
    // Mouse buttons are controls like keys, while the window holds the mouse (or the mouse does
    // not turn the camera); a click that makes the window hold it only does that.
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN:
      if (gMouseHeld || !settings().mouseCamera) setKey(mouseButton(msg), true);
      else holdMouse(hwnd, true);
      return 0;
    case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP: setKey(mouseButton(msg), false); return 0;
    case WM_SIZE: case WM_MOVE:
      if (gMouseHeld) holdMouse(hwnd, true);  // the cursor's bounds follow the window
      break;
    case WM_SETCURSOR:
      if (gMouseHeld && LOWORD(lp) == HTCLIENT) {
        SetCursor(nullptr);
        return TRUE;
      }
      break;
    case WM_INPUT: {
      RAWINPUT in;
      UINT size = sizeof(in);
      if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT, &in, &size, sizeof(RAWINPUTHEADER)) != UINT(-1) &&
          in.header.dwType == RIM_TYPEMOUSE && !(in.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) && gMouseHeld) {
        gMouseX += in.data.mouse.lLastX;
        gMouseY += in.data.mouse.lLastY;
      }
      break;  // DefWindowProc cleans up
    }
    case WM_EXITSIZEMOVE:  // resized by hand: that size next time
      if ((GetWindowLongW(hwnd, GWL_STYLE) & WS_OVERLAPPEDWINDOW) && !IsZoomed(hwnd)) {
        RECT rc;
        GetClientRect(hwnd, &rc);
        if (rc.right != settings().windowWidth || rc.bottom != settings().windowHeight) {
          settings().windowWidth = rc.right, settings().windowHeight = rc.bottom;
          settings().save();
        }
      }
      return 0;
    case WM_CLOSE: DestroyWindow(hwnd); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}
}  // namespace

const PadInput& keyboardPad() { return gKeyboard; }

void takeMouseMotion(int& dx, int& dy) {
  dx = gMouseX.exchange(0), dy = gMouseY.exchange(0);
}

namespace {
bool gConsole = false;  // output can be read: redirected, or a console attached
}  // namespace

void useParentConsole() {
  const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
  if (out && out != INVALID_HANDLE_VALUE) {  // a file or a pipe already
    gConsole = true;
    return;
  }
  if (!AttachConsole(ATTACH_PARENT_PROCESS)) return;
  FILE* f = nullptr;
  freopen_s(&f, "CONOUT$", "w", stdout);
  freopen_s(&f, "CONOUT$", "w", stderr);
  gConsole = true;
}

std::filesystem::path askForIso() {
  wchar_t file[MAX_PATH * 4] = {};
  OPENFILENAMEW ofn{sizeof(ofn)};
  ofn.lpstrFilter = L"Imagem de disco / Disc image (*.iso)\0*.iso\0";
  ofn.lpstrFile = file;
  ofn.nMaxFile = DWORD(std::size(file));
  ofn.lpstrTitle = L"Castlevania: Curse of Darkness (SLES-53755) - ISO";
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
  return GetOpenFileNameW(&ofn) ? std::filesystem::path(file) : std::filesystem::path();
}

void reportError(const std::string& message) {
  std::printf("error: %s\n", message.c_str());
  std::fflush(stdout);
  if (gConsole) return;
  std::wstring text(size_t(MultiByteToWideChar(CP_UTF8, 0, message.data(), int(message.size()), nullptr, 0)), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, message.data(), int(message.size()), text.data(), int(text.size()));
  MessageBoxW(nullptr, text.c_str(), L"Castlevania: Curse of Darkness", MB_OK | MB_ICONERROR);
}
bool wideScreen() { return settings().wide; }

void setFullscreen(void* window, bool on) {
  HWND hwnd = static_cast<HWND>(window);
  if (hwnd && on == !(GetWindowLongW(hwnd, GWL_STYLE) & WS_OVERLAPPEDWINDOW)) return;
  if (hwnd) toggleFullscreen(hwnd);
}

namespace {
// The outer size of a window whose inside is width x height, and the work area of its screen.
RECT outerFor(int width, int height) {
  RECT r{0, 0, width, height};
  AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
  return r;
}
RECT workArea(HWND hwnd) {
  MONITORINFO mi = {sizeof(mi)};
  GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
  return mi.rcWork;
}
// The window's rectangle for an inside of width x height, centred on its screen's work area and
// shrunk (keeping the proportion) where it would not fit.
RECT placed(HWND hwnd, int width, int height) {
  const RECT work = workArea(hwnd), frame = outerFor(0, 0);
  const int fw = frame.right - frame.left, fh = frame.bottom - frame.top;
  const int maxW = int(work.right - work.left) - fw, maxH = int(work.bottom - work.top) - fh;
  if (width > maxW || height > maxH) {
    const double k = std::min(double(maxW) / width, double(maxH) / height);
    width = int(width * k), height = int(height * k);
  }
  const int ow = width + fw, oh = height + fh;
  const int x = int(work.left) + (int(work.right - work.left) - ow) / 2, y = int(work.top) + (int(work.bottom - work.top) - oh) / 2;
  return RECT{x, y, x + ow, y + oh};
}
}  // namespace

std::vector<std::pair<int, int>> windowSizes(void* window) {
  static constexpr std::pair<int, int> kSizes[] = {{960, 720},   {1280, 720},  {1366, 768},  {1440, 1080}, {1600, 900},
                                                   {1920, 1080}, {2048, 1080}, {2560, 1080}, {2560, 1440}, {3440, 1440},
                                                   {3840, 2160}, {5120, 1440}, {5120, 2880}};
  const RECT work = workArea(static_cast<HWND>(window)), frame = outerFor(0, 0);
  std::vector<std::pair<int, int>> out;
  for (const auto& [w, h] : kSizes)
    if (w + frame.right - frame.left <= work.right - work.left && h + frame.bottom - frame.top <= work.bottom - work.top) out.push_back({w, h});
  return out;
}

void setWindowSize(void* window, int width, int height) {
  HWND hwnd = static_cast<HWND>(window);
  settings().windowWidth = width, settings().windowHeight = height;
  settings().save();
  if (!hwnd) return;
  const RECT r = placed(hwnd, width, height);
  if (!(GetWindowLongW(hwnd, GWL_STYLE) & WS_OVERLAPPEDWINDOW)) {  // full screen: for when it leaves it (workspace coordinates)
    const RECT work = workArea(hwnd);
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &mi);
    gWindowed.showCmd = SW_SHOWNORMAL;
    gWindowed.rcNormalPosition = RECT{r.left - (work.left - mi.rcMonitor.left), r.top - (work.top - mi.rcMonitor.top),
                                      r.right - (work.left - mi.rcMonitor.left), r.bottom - (work.top - mi.rcMonitor.top)};
    return;
  }
  ShowWindow(hwnd, SW_RESTORE);
  SetWindowPos(hwnd, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOOWNERZORDER);
}

void setWide(void* window, bool on) {
  settings().wide = on;
  settings().save();
  if (HWND hwnd = static_cast<HWND>(window)) updateTitle(hwnd), InvalidateRect(hwnd, nullptr, TRUE);
}

namespace {
std::function<void()> gApplySettings;
}  // namespace
void onSettingsChanged(std::function<void()> apply) { gApplySettings = std::move(apply); }
void settingsChanged() {
  if (gApplySettings) gApplySettings();
}

void sleepUntil(std::chrono::steady_clock::time_point due) {
  using namespace std::chrono;
  static const bool fineTimer = timeBeginPeriod(1) == TIMERR_NOERROR;
  (void)fineTimer;
  // Sleep can overshoot by about a tick, so it stops early and the rest is spun.
  for (auto left = due - steady_clock::now(); left > milliseconds(2); left = due - steady_clock::now())
    Sleep(DWORD(duration_cast<milliseconds>(left).count() - 1));
  while (steady_clock::now() < due) _mm_pause();
}

namespace {
std::mutex gClocksMutex;
std::function<std::vector<double>()> gClocks;
}  // namespace

void setWorkClocks(std::function<std::vector<double>()> clocks) {
  std::lock_guard lock(gClocksMutex);
  gClocks = std::move(clocks);
}

void* currentThreadHandle() { return OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, GetCurrentThreadId()); }

std::string fpsText(double shownEach) {
  static LARGE_INTEGER frequency{}, since{};
  static int frames = 0, shown = 0, possible = 0;
  static std::vector<double> lastClocks;
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  if (!frequency.QuadPart) QueryPerformanceFrequency(&frequency), since = now;
  ++frames;
  const double elapsed = double(now.QuadPart - since.QuadPart) / double(frequency.QuadPart);
  if (elapsed >= 0.5) {  // twice a second
    std::vector<double> clocks;
    {
      std::lock_guard lock(gClocksMutex);
      if (gClocks) clocks = gClocks();
    }
    double busiest = 0;
    for (size_t i = 0; i < clocks.size() && i < lastClocks.size(); ++i) busiest = std::max(busiest, clocks[i] - lastClocks[i]);
    shown = int(frames * shownEach / elapsed + 0.5);
    possible = busiest > 0 ? int(std::min(frames * shownEach / busiest, 9999.0) + 0.5) : shown;
    lastClocks = std::move(clocks), frames = 0, since = now;
  }
  if (!settings().showFps) return {};
  return std::to_string(std::min(shown, 9999)) + '/' + std::to_string(std::max(possible, std::min(shown, 9999)));
}

bool Window::create(const wchar_t* title, int width, int height) {
  WNDCLASSW wc{};
  wc.lpfnWndProc = wndProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
  wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
  wc.lpszClassName = L"codport";
  // The icon drawn from the disc (host/icon.h), if it was.
  const std::wstring icon = iconFile().wstring();
  auto loadIcon = [&](int metric) {
    const int size = GetSystemMetrics(metric);
    if (auto h = static_cast<HICON>(LoadImageW(nullptr, icon.c_str(), IMAGE_ICON, size, size, LR_LOADFROMFILE))) return h;
    // Else the program's own (tools/setup/setup.ico).
    return static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON, size, size, 0));
  };
  wc.hIcon = loadIcon(SM_CXICON);
  RegisterClassW(&wc);
  HWND hwnd = CreateWindowW(L"codport", title, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 640, 480, nullptr, nullptr,
                            wc.hInstance, nullptr);
  if (hwnd) {  // the size asked for, on the screen it opened on (smaller if it does not fit)
    const RECT r = placed(hwnd, width, height);
    SetWindowPos(hwnd, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOOWNERZORDER);
    ShowWindow(hwnd, SW_SHOW);
  }
  if (HICON small = hwnd ? loadIcon(SM_CXSMICON) : nullptr) SendMessageW(hwnd, WM_SETICON, ICON_SMALL, LPARAM(small));
  hwnd_ = hwnd;
  gTitle = title;
  if (!hwnd) return false;
  const RAWINPUTDEVICE mouse{0x01, 0x02, 0, hwnd};  // generic desktop, mouse
  RegisterRawInputDevices(&mouse, 1, sizeof(mouse));
  if (GetForegroundWindow() == hwnd && cursorInside(hwnd)) holdMouse(hwnd, true);
  updateTitle(hwnd);
  if (settings().fullscreen) toggleFullscreen(hwnd);
  return true;
}

void Window::present(const std::vector<uint32_t>& rgba, uint32_t width, uint32_t height) {
  HWND hwnd = static_cast<HWND>(hwnd_);
  if (!hwnd || rgba.empty()) return;
  // GDI wants BGRA; the GS gives RGBA.
  std::vector<uint32_t> bgra(rgba.size());
  for (size_t i = 0; i < rgba.size(); ++i) {
    const uint32_t c = rgba[i];
    bgra[i] = (c & 0xFF00FF00u) | ((c & 0xFF) << 16) | ((c >> 16) & 0xFF);
  }
  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
  bi.bmiHeader.biWidth = LONG(width);
  bi.bmiHeader.biHeight = -LONG(height);
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  // The PS2 picture is 4:3 (or 16:9, F10) whatever its size in pixels; it is centred with black bars.
  RECT rc;
  GetClientRect(hwnd, &rc);
  const int aw = wideScreen() ? 16 : 4, ah = wideScreen() ? 9 : 3;
  int w = rc.right, h = rc.bottom;
  if (w * ah > h * aw) w = h * aw / ah; else h = w * ah / aw;
  const int x = (rc.right - w) / 2, y = (rc.bottom - h) / 2;
  HDC dc = GetDC(hwnd);
  const HBRUSH black = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
  const RECT bars[4] = {{0, 0, rc.right, y}, {0, y + h, rc.right, rc.bottom}, {0, y, x, y + h}, {x + w, y, rc.right, y + h}};
  for (const RECT& bar : bars)
    if (bar.right > bar.left && bar.bottom > bar.top) FillRect(dc, &bar, black);
  SetStretchBltMode(dc, COLORONCOLOR);
  StretchDIBits(dc, x, y, w, h, 0, 0, int(width), int(height), bgra.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
  if (const std::string text = fpsText(); !text.empty()) {  // the counter: lit cells of the glyphs on a dark box
    const int cell = std::max(2, int(rc.bottom) / 180), cols = int(text.size()) * 4 + 1;
    const int left = rc.right - (cols + 1) * cell, top = cell;
    const RECT box{left, top, left + cols * cell, top + 7 * cell};
    FillRect(dc, &box, black);
    const HBRUSH lit = CreateSolidBrush(RGB(255, 230, 64));
    for (size_t i = 0; i < text.size(); ++i)
      for (int r = 0; r < 5; ++r)
        for (int c = 0; c < 3; ++c)
          if (fpsGlyph(text[i]) >> (14 - (r * 3 + c)) & 1) {
            const RECT px{left + int(1 + i * 4 + c) * cell, top + (1 + r) * cell, left + int(2 + i * 4 + c) * cell, top + (2 + r) * cell};
            FillRect(dc, &px, lit);
          }
    DeleteObject(lit);
  }
  ReleaseDC(hwnd, dc);
}

bool Window::pump() {
  MSG msg;
  while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
    if (msg.message == WM_QUIT) {
      hwnd_ = nullptr;
      return false;
    }
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  return hwnd_ != nullptr;
}

Window::~Window() {
  if (hwnd_) DestroyWindow(static_cast<HWND>(hwnd_));
}

namespace {
double seconds(const FILETIME& user, const FILETIME& kernel) {
  auto v = [](const FILETIME& f) { return double((uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime) * 1e-7; };
  return v(user) + v(kernel);
}
}  // namespace

double threadCpuSeconds() { return threadCpuSeconds(GetCurrentThread()); }

// From the thread's cycle count (exact, where its CPU times move in 15.6 ms steps), at the cycle
// counter's rate, measured once against the performance counter.
double threadCpuSeconds(void* thread) {
  static const double cyclesPerSecond = [] {
    LARGE_INTEGER f, q0, q1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&q0);
    const uint64_t c0 = __rdtsc();
    Sleep(20);
    QueryPerformanceCounter(&q1);
    return double(__rdtsc() - c0) * double(f.QuadPart) / double(q1.QuadPart - q0.QuadPart);
  }();
  ULONG64 cycles = 0;
  return QueryThreadCycleTime(static_cast<HANDLE>(thread), &cycles) ? double(cycles) / cyclesPerSecond : 0;
}

double processCpuSeconds() {
  FILETIME c, e, k, u;
  return GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u) ? seconds(u, k) : 0;
}

namespace {
std::mutex gCoreMutex;
std::vector<BYTE> gClaimedCores;    // fast cores given to one thread each
std::vector<ULONG> gClaimedSets;    // ... the logical processor each runs on
std::vector<ULONG> gWorkerCpuSets;  // the fast cores' other logical processors
}

// On hybrid processors a thread can land on a slow (efficiency) core. The emulated CPUs and the
// graphics thread do most of the work, so each gets one logical processor of a fast core of its
// own (the last ones: the first take most of the system's interrupts), and the GS workers the
// fast cores' other logical processors, including the second one of those cores: the claimed
// threads leave them plenty of room, and drawing needs every core it can get. Slow cores are left
// out: workers wait for each other's rows, so one slow worker holds all of them back.
void claimFastCore() {
  // With one processor to run on, a thread that wakes behind anything of equal or higher priority
  // waits for that thing's whole time slice: 5 to 25 ms, seen as lone late frames. The system's
  // boosts reach 15, so time-critical stands above all of them; these threads take 3 of the
  // processors, and sleep between frames.
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
  std::lock_guard lock(gCoreMutex);
  ULONG length = 0;
  GetSystemCpuSetInformation(nullptr, 0, &length, GetCurrentProcess(), 0);
  std::vector<uint8_t> buffer(length);
  auto* first = reinterpret_cast<SYSTEM_CPU_SET_INFORMATION*>(buffer.data());
  if (length == 0 || !GetSystemCpuSetInformation(first, length, &length, GetCurrentProcess(), 0)) return;
  struct Set {
    ULONG id;
    BYTE core, efficiency;
  };
  std::vector<Set> sets;
  for (ULONG offset = 0; offset < length;) {
    const auto* e = reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION*>(buffer.data() + offset);
    if (e->Type == CpuSetInformation) sets.push_back({e->CpuSet.Id, e->CpuSet.CoreIndex, e->CpuSet.EfficiencyClass});
    offset += e->Size;
  }
  if (sets.size() < 4) return;
  BYTE best = 0;
  for (const Set& s : sets) best = std::max(best, s.efficiency);
  auto claimed = [](BYTE core) { return std::find(gClaimedCores.begin(), gClaimedCores.end(), core) != gClaimedCores.end(); };
  const Set* mine = nullptr;
  for (const Set& s : sets)
    if (s.efficiency == best && !claimed(s.core) && (!mine || s.core != mine->core)) mine = &s;
  std::vector<BYTE> freeCores;
  for (const Set& s : sets)
    if (s.efficiency == best && !claimed(s.core) && std::find(freeCores.begin(), freeCores.end(), s.core) == freeCores.end())
      freeCores.push_back(s.core);
  if (!mine || freeCores.size() < 3) return;  // leave the workers at least two
  for (const Set& s : sets)
    if (s.core == mine->core) {
      mine = &s;
      break;
    }
  if (!SetThreadSelectedCpuSets(GetCurrentThread(), &mine->id, 1)) return;
  gClaimedCores.push_back(mine->core);
  gClaimedSets.push_back(mine->id);
  gWorkerCpuSets.clear();
  for (const Set& s : sets)
    if (s.efficiency == best && std::find(gClaimedSets.begin(), gClaimedSets.end(), s.id) == gClaimedSets.end())
      gWorkerCpuSets.push_back(s.id);
}

int workerCpuCount() {
  std::lock_guard lock(gCoreMutex);
  return int(gWorkerCpuSets.size());
}

void lowerWorkerThread() {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
  std::lock_guard lock(gCoreMutex);
  if (!gWorkerCpuSets.empty()) SetThreadSelectedCpuSets(GetCurrentThread(), gWorkerCpuSets.data(), ULONG(gWorkerCpuSets.size()));
}

bool writeBmp(const char* path, const std::vector<uint32_t>& rgba, uint32_t width, uint32_t height) {
  FILE* f = std::fopen(path, "wb");
  if (!f) return false;
  const uint32_t imageSize = width * height * 4;
  BITMAPFILEHEADER fh{};
  fh.bfType = 0x4D42;
  fh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
  fh.bfSize = fh.bfOffBits + imageSize;
  BITMAPINFOHEADER ih{};
  ih.biSize = sizeof(ih);
  ih.biWidth = LONG(width);
  ih.biHeight = -LONG(height);
  ih.biPlanes = 1;
  ih.biBitCount = 32;
  std::fwrite(&fh, sizeof(fh), 1, f);
  std::fwrite(&ih, sizeof(ih), 1, f);
  for (uint32_t c : rgba) {
    const uint32_t bgra = (c & 0xFF00FF00u) | ((c & 0xFF) << 16) | ((c >> 16) & 0xFF);
    std::fwrite(&bgra, 4, 1, f);
  }
  std::fclose(f);
  return true;
}

bool writeWav(const char* path, const std::vector<int16_t>& samples, int rate) {
  FILE* f = std::fopen(path, "wb");
  if (!f) return false;
  const uint32_t dataBytes = uint32_t(samples.size() * 2), riffBytes = 36 + dataBytes, fmtBytes = 16;
  const uint16_t format = 1, channels = 2, bits = 16, align = 4;
  const uint32_t sampleRate = uint32_t(rate), byteRate = sampleRate * 4;
  std::fwrite("RIFF", 1, 4, f);
  std::fwrite(&riffBytes, 4, 1, f);
  std::fwrite("WAVEfmt ", 1, 8, f);
  std::fwrite(&fmtBytes, 4, 1, f);
  std::fwrite(&format, 2, 1, f);
  std::fwrite(&channels, 2, 1, f);
  std::fwrite(&sampleRate, 4, 1, f);
  std::fwrite(&byteRate, 4, 1, f);
  std::fwrite(&align, 2, 1, f);
  std::fwrite(&bits, 2, 1, f);
  std::fwrite("data", 1, 4, f);
  std::fwrite(&dataBytes, 4, 1, f);
  std::fwrite(samples.data(), 2, samples.size(), f);
  std::fclose(f);
  return true;
}
