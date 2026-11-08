// codport: native PC port of Castlevania: Curse of Darkness (PS2, SLES-53755).
//
// Reads the original disc image (read-only), loads the game executable and runs it on the
// reference R5900 interpreter with the EE kernel, IOP and graphics replaced by C++, showing the
// picture in a window. When it stops, it reports exactly where and why.

#include <algorithm>
#include <ctime>
#include <sstream>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "ee/cpu.h"
#include "ee/kernel.h"
#include "ee/memory.h"
#include "ee/vu.h"
#include "elf.h"
#include "gs/graphics.h"
#include "host/audio.h"
#include "host/gamepad.h"
#include "host/icon.h"
#include "host/settings.h"
#include "game/attack_turn.h"
#include "game/loading.h"
#include "game/minimap.h"
#include "game/mouse_camera.h"
#include "game/sprint.h"
#include "game/widescreen.h"
#include "host/sampler.h"
#include "host/window.h"
#include "iop/iopkernel.h"
#include "iop/pad.h"
#include "iop/sif.h"
#include "iop/spu2.h"
#include "iso9660.h"
#include "recomp/generator.h"
#include "recomp/runtime.h"

void registerRecompiled(std::vector<rc::Entry>& out);  // generated (or recomp/none.cpp)

namespace fs = std::filesystem;

namespace {
void usage() {
  std::puts(
      "usage: codport [image.iso] [options]\n"
      "  image.iso        original disc image (default: first .iso in this folder or its parents)\n"
      "  --max N          stop after N instructions (default: run until the window is closed)\n"
      "  --headless       no window\n"
      "  --dump N         save every Nth frame as build/frames/frameNNNNN.bmp\n"
      "  --dump-from N    with --dump: only from frame N on\n"
      "  --stats N        print frame rate and where the time went every N frames\n"
      "  --replay F       play back a session recorded in window mode (build/recording.txt)\n"
      "  --input S        scripted presses, e.g. 200-205:start,300-303:cross+up (frames:buttons)\n"
      "  --calls F        write every function the game called, in first-call order, to F\n"
      "  --calls-from N   count calls (and --profile samples) only from frame N on\n"
      "  --frames N       stop at frame N\n"
      "  --profile        report where the game spends its time\n"
      "  --peek ADDR      print a word of EE memory at the end (hex)\n"
      "  --interp         run everything on the interpreter (ignore recompiled code)\n"
      "  --software       draw with the CPU instead of the graphics card\n"
      "  --wide           16:9 (wider camera, movies kept 4:3; F10 does it in the window)\n"
      "  --attack-turn    attacks turn to the locked enemy in a replay of a recording made without it\n"
      "  --sprint         L3 sprints in a replay of a recording made without it\n"
      "  --recompile L D  translate the functions listed in L (Ghidra export) to C++ in folder D\n"
      "  --make-icon F    draw the program's icon (the game's memory card icon, from the disc) into F\n"
      "  --only F         with --recompile: only the functions whose addresses are in F\n"
      "  --vu-recompile B D  compile the VU micro-program blocks in B (files separated by ';') to C++ in D: collected\n"
      "                   (CODPORT_VU_COLLECT), or an index of where they are in the game's executable (tools/recomp/vu-blocks.idx)");
}

fs::path findIso() {
  for (fs::path dir = fs::current_path(); !dir.empty(); dir = dir.parent_path()) {
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec))
      if (e.is_regular_file() && e.path().extension() == ".iso") return e.path();
    if (dir == dir.parent_path()) break;
  }
  return {};
}

// Scripted controller input: buttons (and left-stick directions) held during a range of frames.
struct Press {
  uint64_t from, to;
  uint16_t buttons;
  uint8_t stick;  // left stick: 1 up, 2 down, 4 left, 8 right; right stick: 16, 32, 64, 128
};

// "200-205:start+cross,300-303:up,400-460:lup+square"
std::vector<Press> parseScript(const std::string& text) {
  static const std::pair<const char*, uint8_t> kStick[] = {{"lup", 1}, {"ldown", 2}, {"lleft", 4}, {"lright", 8},
                                                           {"rup", 16}, {"rdown", 32}, {"rleft", 64}, {"rright", 128}};
  static const std::pair<const char*, uint16_t> kNames[] = {
      {"select", 0x0001}, {"l3", 0x0002}, {"r3", 0x0004}, {"start", 0x0008}, {"up", 0x0010},
      {"right", 0x0020}, {"down", 0x0040}, {"left", 0x0080}, {"l2", 0x0100}, {"r2", 0x0200},
      {"l1", 0x0400}, {"r1", 0x0800}, {"triangle", 0x1000}, {"circle", 0x2000}, {"cross", 0x4000},
      {"square", 0x8000}};
  std::vector<Press> out;
  size_t pos = 0;
  while (pos < text.size()) {
    const size_t end = std::min(text.find(',', pos), text.size());
    const std::string item = text.substr(pos, end - pos);
    const size_t dash = item.find('-'), colon = item.find(':');
    if (dash != std::string::npos && colon != std::string::npos) {
      Press press{std::stoull(item.substr(0, dash)), std::stoull(item.substr(dash + 1, colon - dash - 1)), 0, 0};
      const std::string names = item.substr(colon + 1) + "+";
      for (size_t p = 0, q; (q = names.find('+', p)) != std::string::npos; p = q + 1) {
        for (const auto& [name, bit] : kNames)
          if (names.compare(p, q - p, name) == 0) press.buttons |= bit;
        for (const auto& [name, bit] : kStick)
          if (names.compare(p, q - p, name) == 0) press.stick |= bit;
      }
      out.push_back(press);
    }
    pos = end + 1;
  }
  return out;
}

std::string bootPathFrom(const std::vector<uint8_t>& cnf) {
  const std::string text(cnf.begin(), cnf.end());
  const size_t key = text.find("BOOT2");
  if (key == std::string::npos) return {};
  const size_t start = text.find_first_not_of(" =\t", key + 5);
  const size_t end = text.find_first_of("\r\n", start);
  return text.substr(start, end == std::string::npos ? std::string::npos : end - start);
}
}  // namespace

int main(int argc, char** argv) {
  useParentConsole();
  fs::path isoPath;
  uint64_t maxInstructions = ~0ull;
  std::string callsFile;
  bool headless = false;
  uint64_t dumpEvery = 0, dumpFrom = 0;
  uint64_t statsEvery = 0;  // print where the time went every N frames
  std::vector<Press> script;
  bool interpOnly = false;
  bool software = false;  // draw with the CPU even if the graphics card could
  bool profiling = false;
  uint64_t stopFrame = 0, callsFrom = 0;
  std::vector<uint32_t> peeks;  // words of EE memory to print at the end (debugging)
  struct MemDump {
    uint32_t addr, size;
    std::string file;
  };
  std::vector<MemDump> memDumps;  // EE memory ranges saved to files at the end (debugging)
  fs::path recompileList, recompileOut, recompileOnly, vuBlocksFile, vuOut, iconOut;
  std::string wavFile;  // the sound output, saved at the end
  fs::path replayFile;  // a recorded session to play back (build/recordings/*.txt)
  int scaleOption = 0;  // --scale: over the setting, for this run
  bool attackTurnOption = false, sprintOption = false, filterOption = false;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--max" && i + 1 < argc) {
      maxInstructions = std::stoull(argv[++i]);
    } else if (a == "--calls" && i + 1 < argc) {
      callsFile = argv[++i];
    } else if (a == "--replay" && i + 1 < argc) {
      replayFile = argv[++i];
    } else if (a == "--wav" && i + 1 < argc) {
      wavFile = argv[++i];
    } else if (a == "--headless") {
      headless = true;
    } else if (a == "--dump" && i + 1 < argc) {
      dumpEvery = std::stoull(argv[++i]);
    } else if (a == "--input" && i + 1 < argc) {
      script = parseScript(argv[++i]);
    } else if (a == "--dump-mem" && i + 3 < argc) {  // ADDR SIZE FILE (hex address and size)
      memDumps.push_back({uint32_t(std::stoul(argv[i + 1], nullptr, 16)), uint32_t(std::stoul(argv[i + 2], nullptr, 16)), argv[i + 3]});
      i += 3;
    } else if (a == "--peek" && i + 1 < argc) {
      peeks.push_back(uint32_t(std::stoul(argv[++i], nullptr, 16)));
    } else if (a == "--stats" && i + 1 < argc) {
      statsEvery = std::stoull(argv[++i]);
    } else if (a == "--frames" && i + 1 < argc) {
      stopFrame = std::stoull(argv[++i]);
    } else if (a == "--calls-from" && i + 1 < argc) {
      callsFrom = std::stoull(argv[++i]);
    } else if (a == "--profile") {
      profiling = true;
    } else if (a == "--dump-from" && i + 1 < argc) {
      dumpFrom = std::stoull(argv[++i]);
    } else if (a == "--scale" && i + 1 < argc) {
      scaleOption = std::clamp(std::stoi(argv[++i]), 1, 4);
    } else if (a == "--wide") {
      settings().wide = true;
    } else if (a == "--attack-turn") {
      attackTurnOption = true;
    } else if (a == "--sprint") {
      sprintOption = true;
    } else if (a == "--filter") {
      filterOption = true;
    } else if (a == "--software") {
      software = true;
    } else if (a == "--interp") {
      interpOnly = true;
    } else if (a == "--recompile" && i + 2 < argc) {
      recompileList = argv[++i];
      recompileOut = argv[++i];
    } else if (a == "--vu-recompile" && i + 2 < argc) {
      vuBlocksFile = argv[++i];
      vuOut = argv[++i];
    } else if (a == "--make-icon" && i + 1 < argc) {
      iconOut = argv[++i];
    } else if (a == "--only" && i + 1 < argc) {
      recompileOnly = argv[++i];
    } else if (a == "-h" || a == "--help") {
      usage();
      return 0;
    } else {
      isoPath = a;
    }
  }
  // The disc image: given, else the one the setup or an earlier question saved, else the first .iso
  // here or above, else the player picks it (and it is saved for next time).
  const fs::path exeDir = fs::absolute(argv[0]).parent_path();
  const fs::path settingsFile = (exeDir.filename() == "build" ? exeDir.parent_path() : exeDir) / "codport.ini";
  auto fromUtf8 = [](const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); };
  if (isoPath.empty() && !headless) {
    Settings saved = Settings::defaults();
    saved.load(settingsFile);
    if (!saved.iso.empty() && fs::exists(fromUtf8(saved.iso))) isoPath = fromUtf8(saved.iso);
  }
  if (isoPath.empty()) isoPath = findIso();
  if (isoPath.empty() && !headless) {
    isoPath = askForIso();
    if (!isoPath.empty()) {
      Settings saved = Settings::defaults();
      saved.load(settingsFile);
      const std::u8string chosen = isoPath.u8string();
      saved.iso.assign(chosen.begin(), chosen.end());
      saved.file = settingsFile;
      saved.save();
    }
  }
  if (isoPath.empty()) {
    usage();
    reportError("No disc image of the game was found. Put the ISO of your Castlevania: Curse of Darkness disc (SLES-53755) "
                "in the codport folder or one above it, or run Setup.bat.\n\nNenhuma imagem do jogo encontrada. Coloque a "
                "ISO do seu disco de Castlevania: Curse of Darkness (SLES-53755) na pasta do codport ou em uma acima dela, "
                "ou rode o Setup.bat.");
    return 1;
  }

  std::string error;
  Iso9660 iso;
  if (!iso.open(isoPath, error)) {
    reportError(error);
    return 1;
  }
  const auto cnf = iso.find("SYSTEM.CNF");
  const std::string bootPath = cnf ? bootPathFrom(iso.readFile(*cnf)) : "";
  const auto exe = iso.find(bootPath);
  if (!exe) {
    reportError(isoPath.filename().string() + " is not a PS2 disc image.\n\n" + isoPath.filename().string() +
                " nao e uma imagem de disco de PS2.");
    return 1;
  }
  if (bootPath.find("SLES_537.55") == std::string::npos) {
    reportError("This disc image is not Castlevania: Curse of Darkness for Europe (SLES-53755), the only version this port "
                "runs.\n\nEsta imagem nao e Castlevania: Curse of Darkness europeu (SLES-53755), a unica versao que este port roda.");
    return 1;
  }
  std::printf("disc:  %s (%zu files)\nboot:  %s (%u bytes)\n", isoPath.filename().string().c_str(),
              iso.entries().size(), bootPath.c_str(), exe->size);

  Memory mem;
  ElfImage elf;
  const std::vector<uint8_t> executable = iso.readFile(*exe);
  if (!loadElf(executable, mem, elf, error)) {
    reportError(error);
    return 1;
  }
  for (const auto& seg : elf.segments)
    std::printf("load:  0x%08X  file 0x%06X  mem 0x%06X\n", seg.vaddr, seg.fileSize, seg.memSize);
  std::printf("entry: 0x%08X\n\n", elf.entry);

  if (!iconOut.empty()) {
    if (!makeIcon(iso, iconOut, error)) {
      std::printf("error: %s\n", error.c_str());
      return 1;
    }
    return 0;
  }
  if (!headless && !fs::exists(iconFile())) makeIcon(iso, iconFile(), error);  // for the window

  if (!vuBlocksFile.empty()) {
    std::vector<fs::path> files;  // separated by ';'
    const std::string list = vuBlocksFile.string();
    for (size_t at = 0; at <= list.size();) {
      const size_t end = std::min(list.find(';', at), list.size());
      if (end > at) files.emplace_back(list.substr(at, end - at));
      at = end + 1;
    }
    if (!generateVuCode(files, executable, vuOut, error)) {
      std::printf("error: %s\n", error.c_str());
      return 1;
    }
    return 0;
  }
  if (!recompileList.empty()) {
    if (!generateRecompiledCode(mem, recompileList, recompileOnly, recompileOut, error)) {
      std::printf("error: %s\n", error.c_str());
      return 1;
    }
    return 0;
  }

  Cpu cpu(mem);
  if (!interpOnly) {
    std::vector<rc::Entry> entries;
    registerRecompiled(entries);
    for (const rc::Entry& e : entries) {
      const uint32_t index = (e.pc - Cpu::kRecompBase) >> 2;
      if (cpu.recomp.size() <= index) cpu.recomp.resize(index + 1);
      cpu.recomp[index] = e.fn;
    }
    if (!entries.empty()) std::printf("recompiled code: %zu entry points\n", entries.size());
    installWidescreen(cpu);
    installAttackTurn(cpu);
    installSprint(cpu);
    installMouseCamera(cpu);
    if (!headless) installMinimap(iso);
  }
  std::unordered_map<uint32_t, uint64_t> profile;
  if (profiling) cpu.profile = &profile;
  Kernel kernel(cpu);
  Sif sif(cpu, iso);
  std::vector<int16_t> sound, audio;  // this frame's samples; all of them (--wav)
  if (!wavFile.empty() || !headless) sif.spu->output = &sound;
  // Memory card saves live next to the build folder, so rebuilding never touches them.
  sif.saveDir = (exeDir.filename() == "build" ? exeDir.parent_path() : exeDir) / "memcard";
  sif.clockBase = int64_t(std::time(nullptr));

  // Sessions played in the window are recorded (controller state per frame, with the clock and a
  // copy of the memory card as they were at the start) so they can be replayed exactly.
  struct PadSample {
    uint64_t frame;
    PadInput pad;
  };
  std::vector<PadSample> replay;
  std::ofstream recording;
  const fs::path buildDir = exeDir;
  auto copyCard = [](const fs::path& from, const fs::path& to) {
    std::error_code ec;
    fs::remove_all(to, ec);
    fs::create_directories(to, ec);
    if (fs::exists(from)) fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
  };
  if (!replayFile.empty()) {
    setAttackTurn(attackTurnOption);
    setSprint(sprintOption);
    std::ifstream in(replayFile);
    std::string line;
    while (std::getline(in, line)) {
      std::istringstream ls(line);
      if (line == "attack-turn") {  // the game played differently: the replay must as well
        setAttackTurn(true);
        continue;
      }
      if (line == "sprint") {
        setSprint(true);
        continue;
      }
      if (line.rfind("clock ", 0) == 0) {
        std::string word;
        ls >> word >> sif.clockBase;
        continue;
      }
      PadSample s{};
      unsigned b = 0, lx = 0, ly = 0, rx = 0, ry = 0;
      ls >> s.frame >> std::hex >> b >> lx >> ly >> rx >> ry;
      if (!ls) continue;
      setPad(s.pad, uint16_t(b), 0);
      s.pad.lx = uint8_t(lx), s.pad.ly = uint8_t(ly), s.pad.rx = uint8_t(rx), s.pad.ry = uint8_t(ry);
      float yaw = 0, pitch = 0;  // the mouse's camera turn, on the frames it moved
      if (ls >> std::dec >> yaw >> pitch) s.pad.turnYaw = yaw, s.pad.turnPitch = pitch;
      replay.push_back(s);
    }
    const fs::path card = replayFile.parent_path() / (replayFile.stem().string() + "-memcard");
    copyCard(card, buildDir / "replay-memcard");
    sif.saveDir = buildDir / "replay-memcard";
    std::printf("replaying %zu controller changes from %s\n", replay.size(), replayFile.string().c_str());
  } else if (!headless) {
    // One recording per session, named after its start time, in build/recordings.
    std::tm t{};
    const std::time_t start = std::time_t(sif.clockBase);
    localtime_s(&t, &start);
    const fs::path name = buildDir / "recordings" / std::format("{:04}{:02}{:02}-{:02}{:02}{:02}", t.tm_year + 1900, t.tm_mon + 1,
                                                                t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
    copyCard(sif.saveDir, name.string() + "-memcard");
    recording.open(name.string() + ".txt");
    recording << "clock " << sif.clockBase << "\n";
  }
  cpu.kernel = &kernel;
  kernel.sif = &sif;
  Graphics gfx(mem);
  mem.gfx = &gfx;
  gfx.vu0Micro.setRegs(VuRegs{cpu.s.vf, cpu.s.vi, &cpu.s.vacc});
  kernel.boot(elf.entry, bootPath);

  // Once per VBlank: show the picture, keep the window responsive, keep PS2 time <= real time.
  Window window;
  AudioOut speakers;
  if (!headless) {
    // Settings (keys, controller, window) next to the memory card folder; written once with the
    // defaults so the player can find it.
    const bool existed = fs::exists(settingsFile);
    settings().load(settingsFile);
    if (replayFile.empty()) setAttackTurn(settings().attackTurn), setSprint(settings().sprint);
    if (recording.is_open() && settings().attackTurn) recording << "attack-turn\n";
    if (recording.is_open() && settings().sprint) recording << "sprint\n";
    if (!existed) settings().save();
    window.create(L"Castlevania: Curse of Darkness", settings().windowWidth, settings().windowHeight);
    speakers.open(Spu2::kSampleRate);
  }
  if (!software) {
    std::string why;
    gfx.gs.setTextureFilter(headless ? filterOption : settings().textureFilter);
    if (!headless) onSettingsChanged([&gfx] { gfx.gs.setTextureFilter(settings().textureFilter); });
    if (gfx.useGpu(headless ? nullptr : window.handle(), uint32_t(scaleOption ? scaleOption : settings().scale), why))
      std::printf("drawing on the graphics card\n");
    else
      std::printf("drawing on the CPU: %s\n", why.c_str());
  }
  if (!headless) {  // the FPS counter's "possible": the busiest of these sets the pace
    void* emulation = currentThreadHandle();
    setWorkClocks([&gfx, emulation] {
      return std::vector<double>{threadCpuSeconds(emulation), gfx.vuThreadCpuSeconds(), gfx.gsThreadCpuSeconds(), gfx.gs.gpuBusySeconds()};
    });
  }
  const fs::path dumpDir = exeDir / "frames";
  if (dumpEvery) fs::create_directories(dumpDir);
  std::vector<uint32_t> picture;
  uint64_t shownSerial = 0;
  auto wallStart = std::chrono::steady_clock::now();
  mem.hw.onVblank = [&] {
    const bool loading = !headless && settings().fastLoading && gameIsLoading(mem);
    const uint64_t n = mem.hw.frames;
    sif.vblank();
    samplerFrame(n);
    if (statsEvery && n % statsEvery == 0) {
      struct Snapshot {
        std::chrono::steady_clock::time_point at;
        double cpu, vuCpu, gsCpu;
        uint64_t vu1Nanos, vu1Instructions, waited, primitives, drains;
      };
      static Snapshot last{std::chrono::steady_clock::now(), threadCpuSeconds(), gfx.vuThreadCpuSeconds(), gfx.gsThreadCpuSeconds(), 0, 0, 0, 0, 0};
      const uint64_t drains = gfx.drains;
      gfx.drain();  // for the graphics thread's counters
      gfx.drains = drains;
      const Snapshot now{std::chrono::steady_clock::now(), threadCpuSeconds(), gfx.vuThreadCpuSeconds(), gfx.gsThreadCpuSeconds(), gfx.vu1.runNanos,
                         gfx.vu1.instructions, gfx.gs.throttleNanos, gfx.gs.primitives, drains};
      const double wall = std::chrono::duration<double>(now.at - last.at).count();
      std::printf("frames %llu: %.1f fps, CPU ms/frame: emulation %.1f, VU1 thread %.1f (VU1 %.1f, %.0fk instr), GS thread %.1f (waited for GS workers %.1f); "
                  "%.0f primitives/frame; emulation waited for graphics %.1f times/frame\n",
                  (unsigned long long)n, statsEvery / wall, (now.cpu - last.cpu) * 1000 / statsEvery,
                  (now.vuCpu - last.vuCpu) * 1000 / statsEvery, (now.vu1Nanos - last.vu1Nanos) / 1e6 / statsEvery,
                  (now.vu1Instructions - last.vu1Instructions) / 1e3 / statsEvery, (now.gsCpu - last.gsCpu) * 1000 / statsEvery,
                  (now.waited - last.waited) / 1e6 / statsEvery,
                  double(now.primitives - last.primitives) / statsEvery, double(now.drains - last.drains) / statsEvery);
      std::fflush(stdout);
      last = now;
    }
    if (!sound.empty()) {
      if (!headless && !loading) speakers.push(sound.data(), sound.size());
      if (!wavFile.empty()) audio.insert(audio.end(), sound.begin(), sound.end());
      sound.clear();
    }
    if (stopFrame && n >= stopFrame) cpu.halt(StopReason::Limit, std::format("frame {}", n));
    if (callsFrom && n == callsFrom) {  // only count what runs from here on
      cpu.calls.clear();
      cpu.callOrder.clear();
      if (cpu.profile) cpu.profile->clear();
    }
    if (!script.empty()) {
      uint16_t held = 0;
      uint8_t stick = 0;
      for (const auto& p : script)
        if (n >= p.from && n <= p.to) held |= p.buttons, stick |= p.stick;
      setPad(gPadInput, held, stick);
    } else if (!replay.empty()) {
      static size_t next = 0;
      while (next < replay.size() && replay[next].frame <= n) gPadInput = replay[next++].pad;
    } else if (!headless) {
      applyGamepad(keyboardPad());
    }
    if (recording.is_open()) {
      static PadInput last{};
      static bool first = true;
      const PadInput& p = gPadInput;
      const bool turned = p.turnYaw != 0 || p.turnPitch != 0;
      if (first || turned || p.buttons != last.buttons || p.lx != last.lx || p.ly != last.ly || p.rx != last.rx || p.ry != last.ry) {
        recording << std::dec << n << ' ' << std::hex << p.buttons << ' ' << unsigned(p.lx) << ' ' << unsigned(p.ly) << ' '
                  << unsigned(p.rx) << ' ' << unsigned(p.ry);
        if (turned) recording << std::dec << ' ' << std::format("{} {}", p.turnYaw, p.turnPitch);  // exact (shortest round trip)
        recording << '\n';
        recording.flush();
        last = p, first = false;
      }
    }
    queueCameraTurn(gPadInput.turnYaw, gPadInput.turnPitch);
    gPadInput.turnYaw = gPadInput.turnPitch = 0;
    uint32_t w = 0, h = 0;
    if (dumpEvery && n >= dumpFrom && n % dumpEvery == 0) {  // exact picture of this VBlank
      gfx.vblank(false);
      gfx.drain();
      if (gfx.gs.frame(picture, w, h)) {
        writeBmp((dumpDir / std::format("frame{:05}.bmp", n)).string().c_str(), picture, w, h);
        if (!headless) window.present(picture, w, h);
      }
    } else {  // drawing goes on in the background; show the newest finished picture
      gfx.vblank(!headless);
      if (!headless && gfx.gs.latestFrame(picture, w, h, shownSerial)) window.present(picture, w, h);
    }
    if (!headless) {
      updateMinimap(cpu);
      if (!window.pump()) cpu.halt(StopReason::Exit, "window closed");
      // Real time keeps pace with emulated time (50 or 60 fields per second, as the game chose).
      // Time lost to a stall is not made up: running fast to catch up would push sound faster
      // than it plays (and the excess would be cut), so the pace starts again from now.
      const auto due = wallStart + std::chrono::microseconds(mem.hw.cycles * 1000000 / Hardware::kEeHz);
      const auto now = std::chrono::steady_clock::now();
      if (loading) {  // no waiting: the game does little here but count frames
        if (now < due) wallStart -= due - now;
      } else if (now < due) sleepUntil(due);
      else if (now - due > std::chrono::milliseconds(50)) wallStart += now - due;
    }
  };

  claimFastCore();
  sampleThisThread("emulation");
  const auto start = std::chrono::steady_clock::now();
  cpu.run(maxInstructions);
  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

  std::printf("== stopped: %s\n   %s\n", toString(cpu.stop), cpu.stopDetail.c_str());
  std::printf("   %llu instructions in %.2f s (%.1f MIPS), %llu of them interpreted, %zu distinct functions called\n\n",
              static_cast<unsigned long long>(cpu.executed), seconds, cpu.executed / seconds / 1e6,
              static_cast<unsigned long long>(cpu.interpreted), cpu.callOrder.size());
  gfx.drain();
  gfx.gs.sync();
  std::printf("   CPU time: emulated CPUs (main thread) %.2f s, VU1 thread %.2f s, GS thread %.2f s, whole process %.2f s\n",
              threadCpuSeconds(), gfx.vuThreadCpuSeconds(), gfx.gsThreadCpuSeconds(), processCpuSeconds());
  std::printf("   the emulated CPUs waited for the graphics thread %llu times\n", (unsigned long long)gfx.drains);
  std::printf("   PS2 time: %.2f s, %llu frames\n", double(mem.hw.cycles) / Hardware::kEeHz,
              static_cast<unsigned long long>(mem.hw.frames));
  std::string ints;
  for (int c = 0; c < 15; ++c)
    if (kernel.interrupts[c]) ints += std::format(" cause{}={}", c, kernel.interrupts[c]);
  std::printf("   interrupts delivered:%s\n\n", ints.empty() ? " none" : ints.c_str());

  std::printf("== call stack (innermost last)\n");
  const auto& cs = cpu.s.callstack;
  for (size_t i = cs.size() > 16 ? cs.size() - 16 : 0; i < cs.size(); ++i)
    std::printf("   0x%08X  (returns to 0x%08X)\n", cs[i].first, cs[i].second);

  std::printf("\n== last syscalls\n");
  for (const auto& line : kernel.recentCalls()) std::printf("   %s\n", line.c_str());

  std::printf("\n== last SIF commands\n");
  for (const auto& line : sif.recent) std::printf("   %s\n", line.c_str());
  std::printf("\n== IOP\n");
  for (const auto& line : sif.iopKernel->report()) std::printf("   %s\n", line.c_str());

  gfx.gs.sync();
  std::printf("\n== graphics\n   DMA transfers: VIF1 %llu, GIF %llu; VIF1 commands %llu, VU1 programs %llu; GIF packets %llu\n",
              (unsigned long long)gfx.dmac.transfers[1], (unsigned long long)gfx.dmac.transfers[2],
              (unsigned long long)gfx.vif1.commands, (unsigned long long)gfx.vif1.microCalls,
              (unsigned long long)gfx.gif.packets);
  std::printf("   GS drawing %.2f s (%llu primitives in %llu batches, %llu pixels), VU1 %.2f s (%llu instructions)\n",
              gfx.gs.drawNanos / 1e9, (unsigned long long)gfx.gs.primitives, (unsigned long long)gfx.gs.batches,
              (unsigned long long)gfx.gs.pixels,
              gfx.vu1.runNanos / 1e9, (unsigned long long)gfx.vu1.instructions);
  std::printf("   of which XGKICK (copying the packets out) %.2f s; GS events: SIGNAL %llu, FINISH %llu\n", gfx.kickNanos / 1e9,
              (unsigned long long)gfx.gs.signals, (unsigned long long)gfx.gs.finishes);
  std::printf("   GS workers: drawing");
  for (uint64_t n : gfx.gs.workerDrawNanos) std::printf(" %.2f", n / 1e9);
  std::printf(" s; steps %.2f s; emulation waited %.2f s for them\n", gfx.gs.stepNanos / 1e9, gfx.gs.throttleNanos / 1e9);
  std::printf("   batches waiting for others to finish %llu; steps %llu\n", (unsigned long long)gfx.gs.barrierBatches,
              (unsigned long long)gfx.gs.stepBatches);
  if (!wavFile.empty()) writeWav(wavFile.c_str(), audio, Spu2::kSampleRate);
  vuCollectSave();
  samplerReport();
  std::printf("   IPU: %llu macroblocks; commands BCLR %llu, BDEC %llu, VDEC %llu, FDEC %llu, SETIQ %llu, CSC %llu\n",
              (unsigned long long)gfx.ipu.macroblocks, (unsigned long long)gfx.ipu.commands[0],
              (unsigned long long)gfx.ipu.commands[2], (unsigned long long)gfx.ipu.commands[3],
              (unsigned long long)gfx.ipu.commands[4], (unsigned long long)gfx.ipu.commands[5],
              (unsigned long long)gfx.ipu.commands[7]);
  for (const std::string* f : {&gfx.dmac.fault, &gfx.vif0.fault, &gfx.vif1.fault, &gfx.ipu.fault})
    if (!f->empty()) std::printf("   problem: %s\n", f->c_str());

  if (profiling) {
    std::vector<std::pair<uint32_t, uint64_t>> top(profile.begin(), profile.end());
    std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
    uint64_t total = 0;
    for (const auto& [pc, n] : top) total += n;
    std::printf("\n== profile (64-byte code blocks, share of samples)\n");
    for (size_t i = 0; i < std::min<size_t>(25, top.size()); ++i)
      std::printf("   0x%08X  %5.1f%%\n", top[i].first, 100.0 * double(top[i].second) / double(total));
  }

  std::printf("\n== interrupt handlers\n");
  for (const auto& h : kernel.intcHandlers) std::printf("   INTC cause %u -> 0x%08X (arg 0x%08X)\n", h.cause, h.func, h.arg);
  for (const auto& h : kernel.dmacHandlers) std::printf("   DMAC channel %u -> 0x%08X (arg 0x%08X)\n", h.cause, h.func, h.arg);
  for (uint32_t addr : peeks) std::printf("   peek 0x%08X = 0x%08X\n", addr, mem.read<uint32_t>(addr));
  for (const MemDump& m : memDumps)
    if (const uint8_t* p = mem.hostPtr(m.addr, m.size)) std::ofstream(m.file, std::ios::binary).write(reinterpret_cast<const char*>(p), m.size);

  std::printf("\n== threads\n%s", kernel.describeThreads().c_str());

  std::printf("\n== hardware registers touched (address: reads/writes, last value)\n");
  std::vector<std::pair<uint32_t, Memory::IoStat>> io(mem.io.begin(), mem.io.end());
  std::sort(io.begin(), io.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  for (const auto& [addr, st] : io)
    std::printf("   0x%08X: %llu/%llu, 0x%llX\n", addr, static_cast<unsigned long long>(st.reads),
                static_cast<unsigned long long>(st.writes), static_cast<unsigned long long>(st.value));

  if (!callsFile.empty()) {
    std::ofstream out(callsFile);
    for (uint32_t f : cpu.callOrder) out << std::format("0x{:08X} {}\n", f, cpu.calls[f]);
    std::printf("\nwrote %zu functions to %s\n", cpu.callOrder.size(), callsFile.c_str());
  }
  return 0;
}
