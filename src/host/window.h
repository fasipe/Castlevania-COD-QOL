#pragma once
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

// The game window: shows each frame scaled to the window and maps the keyboard to the PS2
// controller (gPadInput).
class Window {
public:
  bool create(const wchar_t* title, int width, int height);
  void present(const std::vector<uint32_t>& rgba, uint32_t width, uint32_t height);
  bool pump();  // handles window messages; false once the window was closed
  void* handle() const { return hwnd_; }  // the HWND
  ~Window();

private:
  void* hwnd_ = nullptr;
};

// Gives the calling thread (the emulated CPUs, the graphics thread) a fast core of its own and
// priority over the GS workers, which call lowerWorkerThread() to stay off those cores.
void claimFastCore();
void lowerWorkerThread();
int workerCpuCount();  // logical processors the GS workers get (0 if not set up)
double threadCpuSeconds();              // CPU time used by the calling thread
double threadCpuSeconds(void* thread);  // ... by the thread with this handle
double processCpuSeconds();  // CPU time used by all threads

// Waits until `due`, to well under a millisecond: the frame pacing. (std::this_thread::sleep_until
// wakes on the system timer's ticks, 15.6 ms apart unless something asks for finer ones.)
void sleepUntil(std::chrono::steady_clock::time_point due);

// codport has no console of its own: its output goes where it is redirected, or else to the
// console it was started from (none when opened from Explorer).
void useParentConsole();
// A message for the player that stops the program: printed, and shown in a box when there is no
// console to read it in.
void reportError(const std::string& message);
// Asks the player for the game's disc image (an Open dialog); empty if they cancel.
std::filesystem::path askForIso();

bool wideScreen();  // F10: the picture is shown 16:9 instead of 4:3
// The F1 window's options page changes these on the game window (an HWND) as its keys do.
void setFullscreen(void* window, bool on);
void setWide(void* window, bool on);
// The usual window sizes (inside, in pixels) that fit on the screen holding `window`, smallest first;
// setWindowSize() resizes it (or, in full screen, what it goes back to) and saves the size.
std::vector<std::pair<int, int>> windowSizes(void* window);
void setWindowSize(void* window, int width, int height);
// Settings the program applies itself while running (the texture filter): it says how here, and
// the options page calls settingsChanged() after each change.
void onSettingsChanged(std::function<void()> apply);
void settingsChanged();
// The FPS counter's text, called once per picture of the game: "shown/possible" frames per second,
// the second without the game's 50/60 Hz pace; empty when the counter is off (F9). `shownEach`:
// pictures shown for each of the game's (more with interpolation).
std::string fpsText(double shownEach = 1);
// Where "possible" comes from: the working seconds so far of each part that takes its turn every
// frame (threads' CPU time, the graphics card's time), the busiest of which sets the pace.
void setWorkClocks(std::function<std::vector<double>()> clocks);
void* currentThreadHandle();  // a handle to the calling thread that other threads can use
// The counter's 3x5 glyphs ('0'-'9', '/'): bit 14 - (row * 3 + column) is lit.
inline uint16_t fpsGlyph(char c) {
  constexpr uint16_t digits[10] = {0x7b6f, 0x2c97, 0x73e7, 0x73cf, 0x5bc9, 0x79cf, 0x79ef, 0x7249, 0x7bef, 0x7bcf};
  return c == '/' ? 0x12a4 : c >= '0' && c <= '9' ? digits[c - '0'] : 0;
}

struct PadInput;
const PadInput& keyboardPad();  // the controller state the window's keyboard layout gives
// The mouse's motion (raw counts, y down) since the last call, while the window holds the mouse.
void takeMouseMotion(int& dx, int& dy);

// Writes interleaved 16-bit stereo samples as a WAV file.
bool writeWav(const char* path, const std::vector<int16_t>& samples, int rate);

// Writes a frame as a 32-bit BMP (used by --dump to inspect output without a window).
bool writeBmp(const char* path, const std::vector<uint32_t>& rgba, uint32_t width, uint32_t height);
