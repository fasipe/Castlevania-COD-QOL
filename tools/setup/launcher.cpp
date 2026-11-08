// Setup.exe: opens the setup window (tools/setup/setup.ps1, next to this program) with no console,
// as Setup.bat does, but with the program's icon. Built with the port (CMakeLists.txt).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <string>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  wchar_t path[MAX_PATH * 4];
  const DWORD n = GetModuleFileNameW(nullptr, path, DWORD(std::size(path)));
  std::wstring dir(path, n);
  dir = dir.substr(0, dir.find_last_of(L"\\/"));
  // In a release Setup.exe sits in the top folder; built from the source, in build\.
  std::wstring script = dir + L"\\tools\\setup\\setup.ps1";
  if (GetFileAttributesW(script.c_str()) == INVALID_FILE_ATTRIBUTES) script = dir + L"\\..\\tools\\setup\\setup.ps1";
  if (GetFileAttributesW(script.c_str()) == INVALID_FILE_ATTRIBUTES) {
    MessageBoxW(nullptr, L"tools\\setup\\setup.ps1 was not found next to Setup.exe. Extract the whole zip and run it from there.\n\n"
                         L"tools\\setup\\setup.ps1 não foi encontrado ao lado do Setup.exe. Extraia o zip inteiro e rode-o de lá.",
                L"Castlevania: Curse of Darkness - Setup", MB_ICONERROR);
    return 1;
  }
  std::wstring command = L"powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File \"" + script + L"\"";
  STARTUPINFOW si{sizeof(si)};
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
    MessageBoxW(nullptr, L"Windows PowerShell could not be started.\n\nNão foi possível abrir o Windows PowerShell.",
                L"Castlevania: Curse of Darkness - Setup", MB_ICONERROR);
    return 1;
  }
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return 0;
}
