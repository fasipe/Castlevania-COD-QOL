#include "host/settings.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <xinput.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <format>
#include <span>

namespace {
struct Control {
  const char* id;
  const char* label;  // what it does in the game: the manual's "Controls" page, with the default setup
  const char* ps2;    // the button, as the game's own texts call it
  const char* labelEn;
  const char* ps2En;
};
constexpr Control kControlNames[Settings::kControls] = {
    {"select", "Abrir o mapa", "Select", "Open the map", "Select"},
    {"l3", "Correr", "L3", "Sprint", "L3"},
    {"r3", "Sem função no jogo", "R3", "Not used by the game", "R3"},
    {"start", "Abrir o menu", "Start", "Open the menu", "Start"},
    {"cima", "IA do Innocent Devil: cima", "Direcional", "Innocent Devil AI: up", "D-pad"},
    {"direita", "IA do Innocent Devil: direita", "Direcional", "Innocent Devil AI: right", "D-pad"},
    {"baixo", "IA do Innocent Devil: baixo", "Direcional", "Innocent Devil AI: down", "D-pad"},
    {"esquerda", "IA do Innocent Devil: esquerda", "Direcional", "Innocent Devil AI: left", "D-pad"},
    {"l2", "Trocar de alvo", "L2", "Change target", "L2"},
    {"r2", "Travar no alvo", "R2", "Lock on", "R2"},
    {"l1", "Centralizar a câmera", "L1", "Reset the camera", "L1"},
    {"r1", "Defender", "R1", "Guard", "R1"},
    {"triangulo", "Habilidade do Innocent Devil / Voltar", "Triângulo", "Innocent Devil ability / Back", "Triangle"},
    {"bola", "Golpe final / Roubar / Examinar / Mudar de área", "Bola", "Final attack / Steal / Examine / Change area", "Circle"},
    {"x", "Pular / Confirmar", "X (cruz)", "Jump / Confirm", "Cross"},
    {"quadrado", "Atacar", "Quadrado", "Attack", "Square"},
    {"analogico_esq_cima", "Andar: para cima", "Analógico esq.", "Move: up", "Left stick"},
    {"analogico_esq_baixo", "Andar: para baixo", "Analógico esq.", "Move: down", "Left stick"},
    {"analogico_esq_esquerda", "Andar: para a esquerda", "Analógico esq.", "Move: left", "Left stick"},
    {"analogico_esq_direita", "Andar: para a direita", "Analógico esq.", "Move: right", "Left stick"},
    {"analogico_dir_cima", "Câmera: para cima", "Analógico dir.", "Camera: up", "Right stick"},
    {"analogico_dir_baixo", "Câmera: para baixo", "Analógico dir.", "Camera: down", "Right stick"},
    {"analogico_dir_esquerda", "Câmera: para a esquerda", "Analógico dir.", "Camera: left", "Right stick"},
    {"analogico_dir_direita", "Câmera: para a direita", "Analógico dir.", "Camera: right", "Right stick"},
};

struct Named {
  int code;
  const char* name;
};
constexpr Named kKeyNames[] = {
    {VK_BACK, "Backspace"}, {VK_TAB, "Tab"},        {VK_RETURN, "Enter"},     {VK_SHIFT, "Shift"},
    {VK_CONTROL, "Ctrl"},   {VK_MENU, "Alt"},       {VK_ESCAPE, "Esc"},       {VK_SPACE, "Espaco"},
    {VK_PRIOR, "PageUp"},   {VK_NEXT, "PageDown"},  {VK_END, "End"},          {VK_HOME, "Home"},
    {VK_LEFT, "Esquerda"},  {VK_UP, "Cima"},        {VK_RIGHT, "Direita"},    {VK_DOWN, "Baixo"},
    {VK_INSERT, "Insert"},  {VK_DELETE, "Delete"},  {VK_CAPITAL, "CapsLock"}, {VK_OEM_1, "Ç"},
    {VK_OEM_PLUS, "="},     {VK_OEM_COMMA, ","},    {VK_OEM_MINUS, "-"},      {VK_OEM_PERIOD, "."},
    {VK_OEM_2, ";"},        {VK_OEM_3, "'"},        {VK_OEM_4, "´"},          {VK_OEM_5, "]"},
    {VK_OEM_6, "["},        {VK_OEM_7, "~"},        {VK_LSHIFT, "ShiftEsq"},  {VK_RSHIFT, "ShiftDir"},
    {VK_LCONTROL, "CtrlEsq"}, {VK_RCONTROL, "CtrlDir"},
    {VK_LBUTTON, "MouseEsq"}, {VK_RBUTTON, "MouseDir"}, {VK_MBUTTON, "MouseMeio"},
};
constexpr Named kPadNames[] = {
    {XINPUT_GAMEPAD_A, "A"},           {XINPUT_GAMEPAD_B, "B"},          {XINPUT_GAMEPAD_X, "X"},
    {XINPUT_GAMEPAD_Y, "Y"},           {XINPUT_GAMEPAD_LEFT_SHOULDER, "LB"}, {XINPUT_GAMEPAD_RIGHT_SHOULDER, "RB"},
    {Settings::kLeftTrigger, "LT"},    {Settings::kRightTrigger, "RT"},  {XINPUT_GAMEPAD_BACK, "Back"},
    {XINPUT_GAMEPAD_START, "Start"},   {XINPUT_GAMEPAD_LEFT_THUMB, "LS"}, {XINPUT_GAMEPAD_RIGHT_THUMB, "RS"},
    {XINPUT_GAMEPAD_DPAD_UP, "Cima"},  {XINPUT_GAMEPAD_DPAD_DOWN, "Baixo"}, {XINPUT_GAMEPAD_DPAD_LEFT, "Esquerda"},
    {XINPUT_GAMEPAD_DPAD_RIGHT, "Direita"},
};

std::string trim(std::string s) {
  const auto space = [](unsigned char c) { return std::isspace(c) != 0; };
  s.erase(s.begin(), std::find_if_not(s.begin(), s.end(), space));
  s.erase(std::find_if_not(s.rbegin(), s.rend(), space).base(), s.end());
  return s;
}
}  // namespace

const char* Settings::id(int control) { return kControlNames[control].id; }
const char* Settings::label(int control) { return english() ? kControlNames[control].labelEn : kControlNames[control].label; }
const char* Settings::ps2Name(int control) { return english() ? kControlNames[control].ps2En : kControlNames[control].ps2; }

bool english() { return settings().language == "en"; }

std::string keyLabel(int vk) {
  // As shown: the file's names, in the window's language.
  static constexpr std::pair<int, const char*> kPt[] = {
      {VK_SPACE, "Espaço"}, {VK_LSHIFT, "Shift esq."}, {VK_RSHIFT, "Shift dir."}, {VK_LCONTROL, "Ctrl esq."}, {VK_RCONTROL, "Ctrl dir."},
      {VK_LBUTTON, "Clique esq."}, {VK_RBUTTON, "Clique dir."}, {VK_MBUTTON, "Clique do meio"}};
  static constexpr std::pair<int, const char*> kEn[] = {
      {VK_SPACE, "Space"}, {VK_LEFT, "Left"}, {VK_UP, "Up"}, {VK_RIGHT, "Right"}, {VK_DOWN, "Down"},
      {VK_LSHIFT, "Left Shift"}, {VK_RSHIFT, "Right Shift"}, {VK_LCONTROL, "Left Ctrl"}, {VK_RCONTROL, "Right Ctrl"},
      {VK_LBUTTON, "Left click"}, {VK_RBUTTON, "Right click"}, {VK_MBUTTON, "Middle click"}};
  for (const auto& [code, name] : english() ? std::span<const std::pair<int, const char*>>(kEn) : std::span<const std::pair<int, const char*>>(kPt))
    if (code == vk) return name;
  const std::string name = keyName(vk);
  return english() && name.rfind("Tecla", 0) == 0 ? "Key" + name.substr(5) : name;
}

std::string padLabel(uint16_t input) {
  if (english()) {
    if (input == XINPUT_GAMEPAD_DPAD_UP) return "Up";
    if (input == XINPUT_GAMEPAD_DPAD_DOWN) return "Down";
    if (input == XINPUT_GAMEPAD_DPAD_LEFT) return "Left";
    if (input == XINPUT_GAMEPAD_DPAD_RIGHT) return "Right";
  }
  return padName(input);
}

std::string keyName(int vk) {
  if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) return std::string(1, char(vk));
  if (vk >= VK_F1 && vk <= VK_F24) return std::format("F{}", vk - VK_F1 + 1);
  if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return std::format("Num{}", vk - VK_NUMPAD0);
  for (const Named& k : kKeyNames)
    if (k.code == vk) return k.name;
  return vk ? std::format("Tecla{}", vk) : "";
}

int keyFromName(const std::string& name) {
  if (name.empty()) return 0;
  if (name.size() == 1 && std::isalnum(static_cast<unsigned char>(name[0]))) return std::toupper(static_cast<unsigned char>(name[0]));
  for (int vk = 1; vk < 256; ++vk)
    if (keyName(vk) == name) return vk;
  return 0;
}

std::string padName(uint16_t input) {
  for (const Named& p : kPadNames)
    if (p.code == input) return p.name;
  return "";
}

uint16_t padFromName(const std::string& name) {
  for (const Named& p : kPadNames)
    if (name == p.name) return uint16_t(p.code);
  return 0;
}

Settings Settings::defaults() {
  Settings s;
  s.language = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_PORTUGUESE ? "pt" : "en";
  // Keyboard and mouse, for a hand on W/A/S/D and the other on the mouse: W/A/S/D = left stick
  // (Hector moves with it), the mouse turns the camera (and I/J/K/L = right stick), left and right
  // buttons = square and circle (the attack and the final attack), Space = cross, E = triangle,
  // F = R1 (guard), Q = L1, 1 = L2, middle button = R2 (lock on), Shift = L3 (the sprint), Tab =
  // SELECT (the map), Enter = START, arrows = D-pad (the Innocent Devil's AI, and menus).
  s.keys = {VK_TAB, VK_SHIFT, 0,   VK_RETURN, VK_UP, VK_RIGHT, VK_DOWN, VK_LEFT, '1', VK_MBUTTON, 'Q', 'F',
            'E',    VK_RBUTTON, VK_SPACE, VK_LBUTTON, 'W', 'S', 'A', 'D', 'I', 'K', 'J', 'L'};
  // Xbox layout onto the DualShock: A = cross, B = circle, X = square, Y = triangle.
  s.pad = {XINPUT_GAMEPAD_BACK,      XINPUT_GAMEPAD_LEFT_THUMB, XINPUT_GAMEPAD_RIGHT_THUMB, XINPUT_GAMEPAD_START,
           XINPUT_GAMEPAD_DPAD_UP,   XINPUT_GAMEPAD_DPAD_RIGHT, XINPUT_GAMEPAD_DPAD_DOWN,   XINPUT_GAMEPAD_DPAD_LEFT,
           kLeftTrigger,             kRightTrigger,             XINPUT_GAMEPAD_LEFT_SHOULDER, XINPUT_GAMEPAD_RIGHT_SHOULDER,
           XINPUT_GAMEPAD_Y,         XINPUT_GAMEPAD_B,          XINPUT_GAMEPAD_A,           XINPUT_GAMEPAD_X};
  return s;
}

void Settings::load(const std::filesystem::path& from) {
  file = from;
  std::ifstream in(from);
  std::string line, section;
  while (std::getline(in, line)) {
    if (line.rfind("\xEF\xBB\xBF", 0) == 0) line.erase(0, 3);  // saved by an editor with a UTF-8 mark
    line = trim(line);
    if (line.empty() || line[0] == '#') continue;
    if (line.front() == '[' && line.back() == ']') {
      section = line.substr(1, line.size() - 2);
      continue;
    }
    const size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string key = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
    if (section == "disco" && key == "iso") {
      iso = value;
      continue;
    }
    if (section == "janela") {
      if (key == "tela_cheia") fullscreen = value == "1";
      if (key == "tamanho_janela") {
        int w = 0, h = 0;
        if (std::sscanf(value.c_str(), "%dx%d", &w, &h) == 2 && w >= 320 && h >= 240) windowWidth = w, windowHeight = h;
      }
      if (key == "proporcao") wide = value == "16:9";
      if (key == "resolucao") scale = std::clamp(std::atoi(value.c_str()), 1, 4);
      if (key == "mostrar_fps") showFps = value == "1";
      if (key == "interpolacao") interpolation = value == "1";
      if (key == "idioma" && (value == "pt" || value == "en")) language = value;
      continue;
    }
    if (section == "jogo") {
      if (key == "ataque_no_alvo") attackTurn = value == "1";
      if (key == "corrida") sprint = value == "1";
      if (key == "filtro_textura") textureFilter = value == "1";
      if (key == "carregamento_rapido") fastLoading = value == "1";
      if (key == "minimapa") minimap = value == "1";
      if (key == "pacote_texturas") texturePack = value == "1";
      if (key == "salvar_texturas") dumpTextures = value == "1";
      continue;
    }
    if (section == "mouse") {
      if (key == "camera_com_mouse") mouseCamera = value == "1";
      if (key == "sensibilidade") mouseSensitivity = std::clamp(std::atoi(value.c_str()), 1, 10);
      if (key == "inverter_vertical") invertMouseY = value == "1";
      continue;
    }
    for (int c = 0; c < kControls; ++c) {
      if (key != id(c)) continue;
      if (section == "teclado") keys[c] = uint8_t(keyFromName(value));
      if (section == "controle" && c < kButtons) pad[c] = padFromName(value);
    }
  }
}

void Settings::save() const {
  if (file.empty()) return;
  std::ofstream out(file);
  out << "# Configuracoes do port. Tudo isto tambem muda com F1 dentro do jogo.\n"
         "# Teclado: letras, numeros, F1-F24, Num0-Num9, Enter, Espaco, Backspace, Tab, Esc, Shift, Ctrl,\n"
         "# Alt, Cima, Baixo, Esquerda, Direita... Controle (Xbox): A, B, X, Y, LB, RB, LT, RT, Back, Start,\n"
         "# LS, RS, Cima, Baixo, Esquerda, Direita. Vazio = nenhuma.\n\n";
  out << "[disco]\n# A imagem do disco do jogo (SLES-53755); vazio = o primeiro .iso desta pasta ou de uma acima\niso=" << iso
      << "\n\n";
  out << "[janela]\ntela_cheia=" << (fullscreen ? 1 : 0)
      << "\n# Tamanho da janela fora da tela cheia, em pixels (em tela cheia vale o do monitor)\ntamanho_janela=" << windowWidth << 'x'
      << windowHeight << "\nproporcao=" << (wide ? "16:9" : "4:3")
      << "\n# Resolucao do desenho: 1 = a do PS2; 2, 3 ou 4 = tantas vezes mais nitida (pesa mais na placa de\n"
         "# video)\nresolucao="
      << scale << "\n# Quadros por segundo no canto superior direito (F9 liga e desliga)\nmostrar_fps=" << (showFps ? 1 : 0)
      << "\n# Interpolação: quadros feitos entre os do jogo, em telas de 100 Hz ou mais (atrasa a imagem um pouco)\ninterpolacao="
      << (interpolation ? 1 : 0)
      << "\n# Idioma das opcoes e controles (F1): pt ou en / Language of the options and controls (F1): pt or en\nidioma=" << language
      << "\n\n[jogo]\n# Com um inimigo travado, os ataques viram para ele\nataque_no_alvo=" << (attackTurn ? 1 : 0)
      << "\n# L3 (apertar o analogico esquerdo) liga e desliga a corrida mais rapida\ncorrida=" << (sprint ? 1 : 0)
      << "\n# Texturas suavizadas como no PS2; 0 = pixels duros\nfiltro_textura=" << (textureFilter ? 1 : 0)
      << "\n# Portas e telas de Loading sem esperar o tempo real; a musica e o som da porta somem\ncarregamento_rapido=" << (fastLoading ? 1 : 0)
      << "\n# Mapa no canto da tela (F8 liga e desliga)\nminimapa=" << (minimap ? 1 : 0)
      << "\n# Pacote de texturas: as imagens de textures\\replace no lugar das do jogo (vale ao abrir o jogo)\npacote_texturas="
      << (texturePack ? 1 : 0)
      << "\n# Salva as texturas do jogo em textures\\dump, para fazer um pacote (vale ao abrir o jogo)\nsalvar_texturas="
      << (dumpTextures ? 1 : 0)
      << "\n\n[mouse]\n# O mouse gira a camera (o analogico direito); Esc solta o cursor, um clique na janela prende de novo\n"
         "camera_com_mouse=" << (mouseCamera ? 1 : 0)
      << "\n# De 1 a 10\nsensibilidade=" << mouseSensitivity
      << "\n# 1 = empurrar o mouse para a frente olha para baixo\ninverter_vertical=" << (invertMouseY ? 1 : 0)
      << "\n\n[teclado]\n";
  for (int c = 0; c < kControls; ++c) out << id(c) << '=' << keyName(keys[c]) << '\n';
  out << "\n[controle]\n";
  for (int c = 0; c < kButtons; ++c) out << id(c) << '=' << padName(pad[c]) << '\n';
}

Settings& settings() {
  static Settings s = Settings::defaults();
  return s;
}
