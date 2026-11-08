#include "host/controls_dialog.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <xinput.h>

#include <algorithm>
#include <string>
#include <vector>

#include "host/gamepad.h"
#include "host/settings.h"
#include "host/window.h"

// The current look for the list and buttons (common controls 6).
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {
enum : int { kList = 100, kClear, kDefaults, kClose, kHint, kTabs };

// The options page: a check box per setting, the resolution in a list.
enum : int { kFullscreen = 200, kWide, kFilter, kFps, kMinimap, kFastLoading, kSprint, kAttackTurn, kMouseCamera, kInvertY,
             kScale, kScaleLabel, kOptionsNote, kWindowSize, kSensitivity };
bool* optionOf(int id) {
  Settings& s = settings();
  switch (id) {
    case kFilter: return &s.textureFilter;
    case kFps: return &s.showFps;
    case kMinimap: return &s.minimap;
    case kFastLoading: return &s.fastLoading;
    case kSprint: return &s.sprint;
    case kAttackTurn: return &s.attackTurn;
    case kMouseCamera: return &s.mouseCamera;
    case kInvertY: return &s.invertMouseY;
  }
  return nullptr;
}

// The list, what is used most first (each is a control of Settings).
constexpr int kRows[Settings::kControls] = {
    15, 1, 14, 13, 12, 11, 9, 8, 10,                               // square, L3, cross, circle, triangle, R1, R2, L2, L1
    Settings::kLeftUp, Settings::kLeftDown, Settings::kLeftLeft, Settings::kLeftRight,
    Settings::kRightUp, Settings::kRightDown, Settings::kRightLeft, Settings::kRightRight,
    0, 3, 4, 5, 6, 7, 2,                                           // select, start, D-pad, R3
};

struct Dialog {
  HWND wnd = nullptr, list = nullptr, hint = nullptr, owner = nullptr, tabs = nullptr;
  std::vector<HWND> controlsPage, optionsPage;
  std::vector<std::pair<int, int>> sizes;  // the window size list's entries
  HFONT font = nullptr;
  bool capturing = false;
  int control = -1;
  bool keysAtStart[256] = {};
  bool ignoreClick = false;  // the click that chose a mouse button is not a click on the list
  uint16_t padAtStart = 0;
  bool done = false;
};
Dialog* gDialog = nullptr;

std::wstring widen(const std::string& s) {
  if (s.empty()) return {};
  std::wstring w(size_t(MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0)), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), int(w.size()));
  return w;
}

void setHint(const wchar_t* text) { SetWindowTextW(gDialog->hint, text); }

void setCell(int row, int column, const std::wstring& text) {
  LVITEMW item{};
  item.iSubItem = column;
  item.pszText = const_cast<wchar_t*>(text.c_str());
  SendMessageW(gDialog->list, LVM_SETITEMTEXTW, WPARAM(row), LPARAM(&item));
}

void refreshRow(int row) {
  const Settings& s = settings();
  const int c = kRows[row];
  setCell(row, 1, s.keys[c] ? widen(keyName(s.keys[c])) : L"—");
  setCell(row, 2, c < Settings::kButtons ? (s.pad[c] ? widen(padName(s.pad[c])) : L"—") : L"(analógico)");
  setCell(row, 3, widen(Settings::ps2Name(c)));
}

void refreshControl(int c) {
  for (int row = 0; row < Settings::kControls; ++row)
    if (kRows[row] == c) refreshRow(row);
}

uint16_t padHeld() {
  XINPUT_STATE state{};
  return XInputGetState(0, &state) == ERROR_SUCCESS ? padInputs(state.Gamepad) : 0;
}

int selectedRow() { return int(SendMessageW(gDialog->list, LVM_GETNEXTITEM, WPARAM(-1), LVNI_SELECTED)); }

void startCapture() {
  const int row = selectedRow();
  if (row < 0) return;
  Dialog& d = *gDialog;
  const int c = kRows[row];
  d.capturing = true, d.control = c;
  for (int vk = 0; vk < 256; ++vk) d.keysAtStart[vk] = GetAsyncKeyState(vk) & 0x8000;
  d.padAtStart = padHeld();
  const std::wstring name = widen(Settings::label(c));
  setHint((c < Settings::kButtons ? L"Aperte a tecla, o botão do controle ou clique aqui na lista com o botão do mouse para \"" + name + L"\" (Esc cancela)."
                                  : L"Aperte a tecla para \"" + name + L"\"; no controle é sempre o analógico (Esc cancela).").c_str());
  SetTimer(d.wnd, 1, 15, nullptr);
}

void finishCapture(const std::wstring& message) {
  KillTimer(gDialog->wnd, 1);
  gDialog->capturing = false;
  setHint(message.c_str());
}

// Saved; says so, and whether another action answers to the same key or button, as both then happen.
void finishAssigned(bool isKey) {
  const Settings& s = settings();
  const int c = gDialog->control;
  s.save();
  refreshControl(c);
  for (int other = 0; other < Settings::kControls; ++other) {
    if (other == c) continue;
    const bool same = isKey ? s.keys[other] == s.keys[c] : other < Settings::kButtons && s.pad[other] == s.pad[c];
    if (same) return finishCapture(L"Pronto. Atenção: \"" + widen(Settings::label(other)) + L"\" usa o mesmo; apertar faz as duas coisas.");
  }
  finishCapture(L"Pronto.");
}

// Polled while waiting for a key or button: the first one pressed after the wait began.
void pollCapture() {
  Dialog& d = *gDialog;
  Settings& s = settings();
  // Mouse buttons count when clicked over the list (elsewhere they are clicks on the window).
  for (int vk : {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON}) {
    const bool held = GetAsyncKeyState(vk) & 0x8000;
    if (!held) d.keysAtStart[vk] = false;
    if (!held || d.keysAtStart[vk]) continue;
    POINT pt;
    GetCursorPos(&pt);
    if (WindowFromPoint(pt) != d.list) continue;
    s.keys[d.control] = uint8_t(vk);
    d.ignoreClick = vk == VK_LBUTTON;
    return finishAssigned(true);
  }
  for (int vk = 8; vk < 256; ++vk) {
    const bool held = GetAsyncKeyState(vk) & 0x8000;
    if (!held) d.keysAtStart[vk] = false;
    if (!held || d.keysAtStart[vk]) continue;
    if (vk == VK_ESCAPE) return finishCapture(L"Cancelado.");
    // The generic Shift/Ctrl/Alt codes are what the game window receives.
    if (vk == VK_LSHIFT || vk == VK_RSHIFT || vk == VK_LCONTROL || vk == VK_RCONTROL || vk == VK_LMENU || vk == VK_RMENU) continue;
    if (vk == VK_F1 || vk == VK_F10 || vk == VK_F11) return finishCapture(L"F1, F10 e F11 são do port; escolha outra tecla.");
    s.keys[d.control] = uint8_t(vk);
    return finishAssigned(true);
  }
  if (d.control >= Settings::kButtons) return;
  const uint16_t held = padHeld(), fresh = held & ~d.padAtStart;
  d.padAtStart &= held;
  for (uint16_t bit = 1; bit; bit <<= 1) {
    if (!(fresh & bit)) continue;
    s.pad[d.control] = bit;
    return finishAssigned(false);
  }
}

HWND button(HWND parent, int id, const wchar_t* text, int x, int y, int w) {
  HWND b = CreateWindowW(L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, x, y, w, 30, parent,
                         HMENU(INT_PTR(id)), GetModuleHandleW(nullptr), nullptr);
  SendMessageW(b, WM_SETFONT, WPARAM(gDialog->font), TRUE);
  return b;
}

HWND child(const wchar_t* cls, const wchar_t* text, DWORD style, int id, int x, int y, int w, int h) {
  HWND c = CreateWindowW(cls, text, WS_CHILD | style, x, y, w, h, gDialog->wnd, HMENU(INT_PTR(id)), GetModuleHandleW(nullptr), nullptr);
  SendMessageW(c, WM_SETFONT, WPARAM(gDialog->font), TRUE);
  return c;
}

void createOptions() {
  Dialog& d = *gDialog;
  const Settings& s = settings();
  struct Box {
    int id;
    const wchar_t* text;
    bool on;
  };
  const Box image[] = {{kFullscreen, L"Tela cheia (também Alt+Enter ou F11)", s.fullscreen},
                       {kWide, L"Imagem 16:9; desligada, 4:3 como no PS2 (também F10)", s.wide},
                       {kFilter, L"Texturas suavizadas, como o PS2 desenha", s.textureFilter},
                       {kFps, L"Quadros por segundo no canto (também F9)", s.showFps}};
  const Box game[] = {{kMinimap, L"Minimapa no canto da tela (também F8)", s.minimap},
                      {kFastLoading, L"Carregamento rápido nas portas (a música para durante ele)", s.fastLoading},
                      {kSprint, L"Correr mais rápido com L3 (vale ao abrir o jogo)", s.sprint},
                      {kAttackTurn, L"Ataques viram para o inimigo travado (vale ao abrir o jogo)", s.attackTurn}};
  int y = 52;
  d.optionsPage.push_back(child(L"STATIC", L"Imagem", 0, -1, 28, y, 600, 20));
  y += 26;
  for (const Box& b : image) {
    HWND c = child(L"BUTTON", b.text, WS_TABSTOP | BS_AUTOCHECKBOX, b.id, 40, y, 600, 24);
    SendMessageW(c, BM_SETCHECK, b.on ? BST_CHECKED : BST_UNCHECKED, 0);
    d.optionsPage.push_back(c);
    y += 26;
  }
  // The window's size: the usual ones that fit on this screen, and the one it has if it is none of them.
  d.optionsPage.push_back(child(L"STATIC", L"Tamanho da janela (em tela cheia, o do monitor):", 0, -1, 40, y + 4, 300, 22));
  HWND sizeList = child(WC_COMBOBOXW, L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, kWindowSize, 344, y, 220, 300);
  d.sizes = windowSizes(d.owner);
  const std::pair<int, int> current{s.windowWidth, s.windowHeight};
  if (std::find(d.sizes.begin(), d.sizes.end(), current) == d.sizes.end()) d.sizes.insert(d.sizes.begin(), current);
  for (const auto& [w, h] : d.sizes) {
    const std::wstring text = std::to_wstring(w) + L" x " + std::to_wstring(h);
    SendMessageW(sizeList, CB_ADDSTRING, 0, LPARAM(text.c_str()));
  }
  SendMessageW(sizeList, CB_SETCURSEL, WPARAM(std::find(d.sizes.begin(), d.sizes.end(), current) - d.sizes.begin()), 0);
  d.optionsPage.push_back(sizeList);
  y += 34;
  d.optionsPage.push_back(child(L"STATIC", L"Resolução do desenho (vale ao abrir o jogo):", 0, kScaleLabel, 40, y + 4, 300, 22));
  HWND scale = child(WC_COMBOBOXW, L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, kScale, 344, y, 220, 200);
  for (const wchar_t* option : {L"1x (a do PS2)", L"2x", L"3x", L"4x"}) SendMessageW(scale, CB_ADDSTRING, 0, LPARAM(option));
  SendMessageW(scale, CB_SETCURSEL, WPARAM(std::clamp(s.scale, 1, 4) - 1), 0);
  d.optionsPage.push_back(scale);
  y += 40;
  d.optionsPage.push_back(child(L"STATIC", L"Jogo", 0, -1, 28, y, 600, 20));
  y += 26;
  for (const Box& b : game) {
    HWND c = child(L"BUTTON", b.text, WS_TABSTOP | BS_AUTOCHECKBOX, b.id, 40, y, 600, 24);
    SendMessageW(c, BM_SETCHECK, b.on ? BST_CHECKED : BST_UNCHECKED, 0);
    d.optionsPage.push_back(c);
    y += 26;
  }
  y += 14;
  d.optionsPage.push_back(child(L"STATIC", L"Mouse", 0, -1, 28, y, 600, 20));
  y += 26;
  HWND mouse = child(L"BUTTON", L"Câmera com o mouse (Esc solta o cursor, um clique na janela prende de novo)",
                     WS_TABSTOP | BS_AUTOCHECKBOX, kMouseCamera, 40, y, 600, 24);
  SendMessageW(mouse, BM_SETCHECK, s.mouseCamera ? BST_CHECKED : BST_UNCHECKED, 0);
  d.optionsPage.push_back(mouse);
  y += 30;
  d.optionsPage.push_back(child(L"STATIC", L"Sensibilidade:", 0, -1, 40, y + 4, 300, 22));
  HWND sensitivity = child(WC_COMBOBOXW, L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, kSensitivity, 344, y, 220, 300);
  for (int i = 1; i <= 10; ++i) {
    const std::wstring text = std::to_wstring(i) + (i == 5 ? L" (padrão)" : L"");
    SendMessageW(sensitivity, CB_ADDSTRING, 0, LPARAM(text.c_str()));
  }
  SendMessageW(sensitivity, CB_SETCURSEL, WPARAM(std::clamp(s.mouseSensitivity, 1, 10) - 1), 0);
  d.optionsPage.push_back(sensitivity);
  y += 34;
  HWND invert = child(L"BUTTON", L"Inverter o vertical (mouse para a frente olha para baixo)", WS_TABSTOP | BS_AUTOCHECKBOX, kInvertY,
                      40, y, 600, 24);
  SendMessageW(invert, BM_SETCHECK, s.invertMouseY ? BST_CHECKED : BST_UNCHECKED, 0);
  d.optionsPage.push_back(invert);
  d.optionsPage.push_back(child(L"STATIC", L"As mudanças são salvas na hora, no codport.ini.", 0, kOptionsNote, 28, 528, 600, 22));
}

void showPage(int page) {
  Dialog& d = *gDialog;
  for (HWND c : d.controlsPage) ShowWindow(c, page == 0 ? SW_SHOW : SW_HIDE);
  for (HWND c : d.optionsPage) ShowWindow(c, page == 1 ? SW_SHOW : SW_HIDE);
}

// A box of the options page clicked: the setting follows it at once.
void optionClicked(int id, bool on) {
  Settings& s = settings();
  if (id == kFullscreen) return setFullscreen(gDialog->owner, on);
  if (id == kWide) return setWide(gDialog->owner, on);
  bool* option = optionOf(id);
  if (!option) return;
  *option = on;
  s.save();
  if (id == kFps) InvalidateRect(gDialog->owner, nullptr, TRUE);
  settingsChanged();
}

void create(HWND wnd) {
  Dialog& d = *gDialog;
  d.wnd = wnd;
  NONCLIENTMETRICSW metrics{sizeof(metrics)};
  SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0);
  d.font = CreateFontIndirectW(&metrics.lfMessageFont);
  // The tabs, behind what each page shows.
  d.tabs = CreateWindowW(WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_TABSTOP, 6, 6, 668, 548, wnd, HMENU(INT_PTR(kTabs)),
                         GetModuleHandleW(nullptr), nullptr);
  SendMessageW(d.tabs, WM_SETFONT, WPARAM(d.font), TRUE);
  for (const wchar_t* name : {L"Controles", L"Opções"}) {
    TCITEMW tab{};
    tab.mask = TCIF_TEXT;
    tab.pszText = const_cast<wchar_t*>(name);
    SendMessageW(d.tabs, TCM_INSERTITEMW, WPARAM(name[0] == L'C' ? 0 : 1), LPARAM(&tab));
  }
  d.list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                           16, 40, 648, 456, wnd, HMENU(INT_PTR(kList)), GetModuleHandleW(nullptr), nullptr);
  SendMessageW(d.list, WM_SETFONT, WPARAM(d.font), TRUE);
  SendMessageW(d.list, LVM_SETEXTENDEDLISTVIEWSTYLE, 0, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
  const wchar_t* titles[] = {L"Ação", L"Teclado", L"Controle (Xbox)", L"No PS2"};
  const int widths[] = {296, 100, 110, 116};
  for (int i = 0; i < 4; ++i) {
    LVCOLUMNW col{};
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    col.pszText = const_cast<wchar_t*>(titles[i]);
    col.cx = widths[i];
    SendMessageW(d.list, LVM_INSERTCOLUMNW, WPARAM(i), LPARAM(&col));
  }
  for (int row = 0; row < Settings::kControls; ++row) {
    const std::wstring label = widen(Settings::label(kRows[row]));
    LVITEMW item{};
    item.mask = LVIF_TEXT;
    item.iItem = row;
    item.pszText = const_cast<wchar_t*>(label.c_str());
    SendMessageW(d.list, LVM_INSERTITEMW, 0, LPARAM(&item));
    refreshRow(row);
  }
  d.hint = CreateWindowW(L"STATIC", L"Clique na ação que quer mudar e aperte a tecla ou o botão do controle.",
                         WS_CHILD | WS_VISIBLE, 16, 506, 648, 40, wnd, HMENU(INT_PTR(kHint)), GetModuleHandleW(nullptr), nullptr);
  SendMessageW(d.hint, WM_SETFONT, WPARAM(d.font), TRUE);
  d.controlsPage = {d.list, d.hint, button(wnd, kClear, L"Limpar a ação", 12, 562, 120),
                    button(wnd, kDefaults, L"Restaurar padrão", 138, 562, 140)};
  button(wnd, kClose, L"Fechar", 578, 562, 90);
  createOptions();
  showPage(0);
}

LRESULT CALLBACK proc(HWND wnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_CREATE: create(wnd); return 0;
    case WM_TIMER: pollCapture(); return 0;
    case WM_CTLCOLORSTATIC:  // labels and check boxes sit on the tab's page, not on the window's grey
      SetBkColor(HDC(wp), GetSysColor(COLOR_WINDOW));
      return LRESULT(GetSysColorBrush(COLOR_WINDOW));
    case WM_COMMAND:
      if (gDialog->capturing && (LOWORD(wp) == kClear || LOWORD(wp) == kDefaults)) finishCapture(L"");
      if (LOWORD(wp) >= kFullscreen && LOWORD(wp) <= kInvertY && HIWORD(wp) == BN_CLICKED)
        return optionClicked(LOWORD(wp), SendMessageW(HWND(lp), BM_GETCHECK, 0, 0) == BST_CHECKED), 0;
      if (LOWORD(wp) == kWindowSize && HIWORD(wp) == CBN_SELCHANGE) {
        const size_t i = size_t(SendMessageW(HWND(lp), CB_GETCURSEL, 0, 0));
        if (i < gDialog->sizes.size()) setWindowSize(gDialog->owner, gDialog->sizes[i].first, gDialog->sizes[i].second);
        return 0;
      }
      if (LOWORD(wp) == kSensitivity && HIWORD(wp) == CBN_SELCHANGE) {
        settings().mouseSensitivity = int(SendMessageW(HWND(lp), CB_GETCURSEL, 0, 0)) + 1;
        settings().save();
        return 0;
      }
      if (LOWORD(wp) == kScale && HIWORD(wp) == CBN_SELCHANGE) {
        settings().scale = int(SendMessageW(HWND(lp), CB_GETCURSEL, 0, 0)) + 1;
        settings().save();
        return 0;
      }
      switch (LOWORD(wp)) {
        case kClear:
          if (const int row = selectedRow(); row >= 0) {
            const int c = kRows[row];
            settings().keys[c] = 0;
            if (c < Settings::kButtons) settings().pad[c] = 0;
            settings().save();
            refreshRow(row);
            setHint(L"Sem tecla nem botão.");
          }
          break;
        case kDefaults: {
          const Settings def = Settings::defaults();
          settings().keys = def.keys, settings().pad = def.pad;
          settings().save();
          for (int row = 0; row < Settings::kControls; ++row) refreshRow(row);
          setHint(L"Teclas e botões voltaram ao padrão.");
          break;
        }
        case kClose: DestroyWindow(wnd); break;
      }
      return 0;
    case WM_NOTIFY: {
      const NMHDR* n = reinterpret_cast<const NMHDR*>(lp);
      if (n->idFrom == kList && (n->code == NM_CLICK || n->code == NM_RETURN)) {
        if (gDialog->ignoreClick && n->code == NM_CLICK) gDialog->ignoreClick = false;
        else startCapture();
      }
      if (n->idFrom == kTabs && n->code == TCN_SELCHANGE) {
        if (gDialog->capturing) finishCapture(L"");
        showPage(int(SendMessageW(gDialog->tabs, TCM_GETCURSEL, 0, 0)));
      }
      return 0;
    }
    case WM_CLOSE: DestroyWindow(wnd); return 0;
    case WM_DESTROY:
      KillTimer(wnd, 1);
      DeleteObject(gDialog->font);
      gDialog->done = true;
      return 0;
  }
  return DefWindowProcW(wnd, msg, wp, lp);
}
}  // namespace

void showControlsDialog(void* ownerHandle) {
  HWND owner = static_cast<HWND>(ownerHandle);
  static bool registered = [] {
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    WNDCLASSW wc{};
    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"codport-controls";
    return RegisterClassW(&wc) != 0;
  }();
  if (!registered || gDialog) return;
  Dialog dialog;
  gDialog = &dialog;
  dialog.owner = owner;
  RECT r{0, 0, 680, 604};
  const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE;
  AdjustWindowRect(&r, style, FALSE);
  RECT o{};
  GetWindowRect(owner, &o);
  const int w = r.right - r.left, h = r.bottom - r.top;
  HWND wnd = CreateWindowW(L"codport-controls", L"Opções e controles", style, (o.left + o.right - w) / 2,
                           (o.top + o.bottom - h) / 2, w, h, owner, nullptr, GetModuleHandleW(nullptr), nullptr);
  if (!wnd) {
    gDialog = nullptr;
    return;
  }
  // Modal: the game window (and so the game) waits until this one closes.
  EnableWindow(owner, FALSE);
  MSG msg;
  while (!dialog.done) {
    if (GetMessageW(&msg, nullptr, 0, 0) <= 0) {  // the program is closing: let the game's loop see it
      PostQuitMessage(0);
      DestroyWindow(wnd);
      break;
    }
    if (!IsDialogMessageW(wnd, &msg)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
  }
  EnableWindow(owner, TRUE);
  SetForegroundWindow(owner);
  gDialog = nullptr;
}
