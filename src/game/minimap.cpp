#include "game/minimap.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <mutex>
#include <optional>
#include <unordered_map>

#include "ee/cpu.h"
#include "game/widescreen.h"
#include "host/settings.h"
#include "iso9660.h"

namespace {
// The game's tables, found through the map screen (FUN_0051d7c0 and what it calls).
constexpr uint32_t kMapTable = 0x009527B8;     // -> areas: +10 count, +12 list of {id, rooms, _, -> rooms}
constexpr uint32_t kAreaFile = 0x00952CE0;     // area -> file with its room models
constexpr uint32_t kFileEntries = 0x00938C08;  // file -> {u16 _, u16 sector in FILEDATA.BIN; sectors << 8}
constexpr uint32_t kFirstFloor = 0x009527F0;   // area -> floor, when the player's cell gives none
constexpr uint32_t kAreaMapItem = 0x009526E8;  // area -> the item that reveals its map
constexpr uint32_t kRevealLists = 0x009526A0;  // area -> per room {-> cells, count; count < 0: all}
constexpr uint32_t kRoomOffsetX = 0x00952EB0, kRoomOffsetZ = 0x00952EC0, kRoomAngle = 0x00952EA0;  // by rotation
constexpr uint32_t kCellSeen = 0x0093CCC4;     // -> a byte per cell, bit 0
constexpr uint32_t kStateFlags = 0x0093CC10;   // -> words (FUN_0043EC98, flag class 4)
constexpr uint32_t kRoomFlags = 0x0093CC78;    // -> words (class 6)
constexpr uint32_t kRoomInfo = 0x0094470C;     // -> +12: per area (12 bytes) +8: per room (20 bytes)
constexpr uint32_t kItemRanges = 0x0094AB30;   // 13 of {kind, first, end, capacity}
constexpr uint32_t kInventory = 0x00AEF800;
constexpr uint32_t kGameMode = 0x00AED100, kModeInfo = 0x0093CD98;
// Where the player is. kPlace: area, room. kCell: the part of the room, two 16-bit numbers.
constexpr uint32_t kPlace = 0x00AED050, kCell = 0x00AED080;
constexpr uint32_t kPlayer = 0x00B06880, kPosition = 0x00B06790, kYaw = 0x00B067AC;
constexpr float kPositionToMap = 0.08f;  // FUN_00520460
// When the game is being played. Checked against whole sessions, frame by frame with their pictures:
// the status display's update (statusDisplayUpdates, not during loads, title and so on; the display itself
// also fades out while exploring, so its alpha says nothing), the player's input not cut off
// (FUN_0045FEA8: story scenes, save points), the tasks not paused (FUN_007986D8: menus, the map
// screen, messages; hits pause them for 2 frames), and these event flags: bit 15 in doors and
// loading, bit 17 before a movie.
constexpr uint32_t kInputOff = 0x00B065C8, kTaskState = 0x00BA0968, kEventFlags = 0x00AED030;
constexpr uint32_t kEventsNotPlayable = 1u << 15 | 1u << 17;
constexpr int kPauseFrames = 3;       // a pause longer than a hit's
constexpr int kSettleFrames = 24;     // play for this long first: going from a menu to another has a few frames of it
constexpr float kFadeIn = 1.0f / 12, kFadeOut = 1.0f / 6;  // per frame

constexpr uint32_t rgba(uint32_t r, uint32_t g, uint32_t b) { return r | g << 8 | b << 16 | 0xFF000000u; }
constexpr uint32_t kGrey = rgba(128, 128, 128), kWhite = rgba(255, 255, 255), kBlue = rgba(0, 0, 255);
constexpr uint32_t kRed = rgba(255, 0, 0), kGreen = rgba(0, 255, 0);

template <class T>
T fromBytes(const std::vector<uint8_t>& data, size_t at) {
  T v{};
  if (at + sizeof(T) <= data.size()) std::memcpy(&v, data.data() + at, sizeof(T));
  return v;
}

struct Model {
  struct Object {
    int node;
    std::vector<std::array<float, 2>> triangles;  // three corners each, in map units
  };
  std::vector<Object> objects;
  std::vector<int16_t> parents;  // of each node
};

// A room's model: a matrix for all of it (offset at +8), and objects of VU packets. Corners are in
// UNPACK V3-32 (fixed point, 1/16), and
// triangle strips in the V4-16 unpack to the list above the vertices: each number is the corner's
// VU address (4 per corner, from 4), its top bit meaning "no triangle at this corner".
Model parseModel(const std::vector<uint8_t>& d) {
  Model m;
  const size_t matrix = fromBytes<uint32_t>(d, 8);
  auto cell = [&](int column, int row) { return fromBytes<float>(d, matrix + size_t(column) * 16 + size_t(row) * 4); };
  const uint32_t count = fromBytes<uint32_t>(d, 0x20);
  size_t at = fromBytes<uint32_t>(d, 0x24);
  if (count > 4096) return m;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t size = fromBytes<uint32_t>(d, at);
    if (size == 0 || at + size > d.size()) break;
    Model::Object object{int(fromBytes<uint32_t>(d, at + 0x40) >> 24), {}};
    std::vector<std::array<float, 2>> corners;
    std::vector<uint16_t> strip;
    for (size_t p = at + 0xC0, end = at + size; p + 4 <= end;) {
      const uint32_t code = fromBytes<uint32_t>(d, p);
      const uint32_t command = code >> 24, num = (code >> 16) & 0xFF;
      p += 4;
      if (command >= 0x60) {
        const uint32_t components = ((command >> 2) & 3) + 1, bits = 32u >> (command & 3);
        const size_t bytes = (num * components * bits + 31) / 32 * 4;
        if (components == 3 && bits == 32 && corners.empty()) {
          for (uint32_t k = 0; k < num; ++k) {
            const float x = float(fromBytes<int32_t>(d, p + k * 12)) / 16.0f, y = float(fromBytes<int32_t>(d, p + k * 12 + 4)) / 16.0f;
            const float z = float(fromBytes<int32_t>(d, p + k * 12 + 8)) / 16.0f;
            corners.push_back({cell(0, 0) * x + cell(1, 0) * y + cell(2, 0) * z + cell(3, 0), cell(0, 2) * x + cell(1, 2) * y + cell(2, 2) * z + cell(3, 2)});
          }
        } else if (components == 4 && bits == 16 && (code & 0x3FF) >= 4) {
          for (uint32_t k = 0; k < num * 4; ++k) strip.push_back(fromBytes<uint16_t>(d, p + k * 2));
        }
        p += bytes;
      } else if (command == 0x20) {
        p += 4;
      } else if (command == 0x30 || command == 0x31) {
        p += 16;
      }
    }
    const std::array<float, 2>* a = nullptr;
    const std::array<float, 2>* b = nullptr;
    for (const uint16_t v : strip) {
      const size_t k = (v & 0x7FFFu) / 4;
      if (k == 0 || k > corners.size()) continue;
      const std::array<float, 2>* c = &corners[k - 1];
      if (!(v & 0x8000u) && a && b && *a != *b && *b != *c && *a != *c) {
        object.triangles.push_back(*a), object.triangles.push_back(*b), object.triangles.push_back(*c);
      }
      a = b, b = c;
    }
    m.objects.push_back(std::move(object));
    at += size;
  }
  const uint32_t nodes = fromBytes<uint32_t>(d, 0x40);
  const uint32_t table = fromBytes<uint32_t>(d, 0x34);
  if (table != 0 && nodes <= 4096) {
    const size_t list = fromBytes<uint32_t>(d, table);
    for (uint32_t k = 0; k < nodes; ++k) m.parents.push_back(fromBytes<int16_t>(d, list + k * 2));
  }
  return m;
}

class Minimap {
public:
  explicit Minimap(const Iso9660& iso) : iso_(iso) {}

  void update(Cpu& c);
  MinimapView view(std::vector<MinimapVertex>& triangles, uint64_t have) {
    std::lock_guard lock(mutex_);
    if (have != view_.version) triangles = mesh_;
    return view_;
  }

private:
  bool playable() const;
  uint8_t u8(uint32_t a) const { const uint8_t* p = mem_->hostPtr(a, 1); return p ? *p : 0; }
  int8_t s8(uint32_t a) const { return int8_t(u8(a)); }
  int16_t s16(uint32_t a) const { const uint8_t* p = mem_->hostPtr(a, 2); int16_t v = 0; if (p) std::memcpy(&v, p, 2); return v; }
  uint32_t u32(uint32_t a) const { const uint8_t* p = mem_->hostPtr(a, 4); uint32_t v = 0; if (p) std::memcpy(&v, p, 4); return v; }
  float f32(uint32_t a) const { return std::bit_cast<float>(u32(a)); }

  uint32_t roomsOf(int area, int& count) const;
  void loadArea(int area);
  int currentFloor(int area, int room, uint32_t rooms, int count) const;
  bool hasAreaMap(int area) const;
  bool cellRevealed(uint32_t cell, int area, int room, bool hasMap) const;
  bool stateFlag() const;
  bool roomFlag(int area, int room) const;
  uint32_t roomColour(int area, int room) const;
  uint64_t signature(int area, int floor, uint32_t rooms, int count, bool hasMap) const;
  void build(int area, int floor, uint32_t rooms, int count, bool hasMap, std::vector<MinimapVertex>& out) const;
  void publish(const MinimapView& view);

  const Iso9660& iso_;
  Memory* mem_ = nullptr;
  std::optional<Iso9660::Entry> fileData_;
  int loadedArea_ = -1;
  std::unordered_map<uint32_t, Model> models_;
  uint64_t shownSignature_ = 0;
  uint64_t seenUpdates_ = 0;
  int quietFrames_ = 1000, pausedFrames_ = 0, playingFrames_ = 0;
  float alpha_ = 0;
  MinimapView shown_;  // kept for the fade out
  // Shared with the renderer.
  std::mutex mutex_;
  MinimapView view_;
  std::vector<MinimapVertex> mesh_;
};

uint32_t Minimap::roomsOf(int area, int& count) const {
  const uint32_t table = u32(kMapTable);
  if (table == 0) return 0;
  const int areas = s16(table + 10);
  const uint32_t list = u32(table + 12);
  for (int i = 0; i < areas; ++i) {
    if (s8(list + i * 8) == area) {
      count = s8(list + i * 8 + 1);
      return u32(list + i * 8 + 4);
    }
  }
  return 0;
}

// The area's file: a table of models, then the models one after the other on 128-byte boundaries.
void Minimap::loadArea(int area) {
  loadedArea_ = area;
  models_.clear();
  if (!fileData_) fileData_ = iso_.find("FILEDATA.BIN");
  const uint32_t file = u32(kAreaFile + area * 4);
  if (!fileData_ || file == 0) return;
  const uint32_t entry = u32(kFileEntries + file * 4);
  const uint32_t sector = u32(entry) >> 16, bytes = u32(entry + 4) * 8;
  if (bytes == 0 || bytes > (32u << 20)) return;
  std::vector<uint8_t> pack((bytes + 2047) / 2048 * 2048);
  if (!iso_.readSectors(fileData_->lba + sector, uint32_t(pack.size() / 2048), pack.data())) return;
  const uint32_t count = fromBytes<uint32_t>(pack, 0x10), table = fromBytes<uint32_t>(pack, 0x14);
  if (count > 4096) return;
  size_t at = (size_t(table) + count * 16 + 0x7F) & ~size_t(0x7F);
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t id = fromBytes<uint32_t>(pack, table + i * 16), size = fromBytes<uint32_t>(pack, table + i * 16 + 12);
    if (at + size > pack.size()) break;
    models_[id] = parseModel(std::vector<uint8_t>(pack.begin() + ptrdiff_t(at), pack.begin() + ptrdiff_t(at + size)));
    at = (at + size + 0x7F) & ~size_t(0x7F);
  }
}

int Minimap::currentFloor(int area, int room, uint32_t rooms, int count) const {
  const int a = s16(kCell), b = s16(kCell + 2);
  for (int i = 0; i < count; ++i) {
    const uint32_t r = rooms + i * 0x18;
    if (s8(r + 8) != room) continue;
    const uint32_t cells = u32(r + 16);
    for (int k = 0, n = s8(r + 9); k < n; ++k) {
      const uint32_t cell = cells + k * 16;
      if (s8(cell + 5) == a && s8(cell + 4) == b) return s8(cell + 6);
    }
  }
  return s16(kFirstFloor + area * 4);
}

// FUN_0051C878 and FUN_0045D3D8: the area's map in the inventory (or the game mode gives them all).
bool Minimap::hasAreaMap(int area) const {
  const int item = int(u32(kAreaMapItem + area * 4));
  if (item == 0) return false;
  if (int(u32(kModeInfo + int(u32(kGameMode)) * 8)) == 2) return true;
  for (int i = 0; i < 13; ++i) {
    const uint32_t range = kItemRanges + i * 16;
    if (item < int(u32(range + 4)) || item >= int(u32(range + 8))) continue;
    static constexpr std::array<int, 11> offsets = {-1, 0, 0x15C, 0x1E4, 600, 0x2C4, 0x350, 0x370, 0x430, -1, 0x484};
    const int kind = int(u32(range));
    if (kind <= 0 || kind >= int(offsets.size()) || offsets[size_t(kind)] < 0) return false;
    const uint32_t list = kInventory + offsets[size_t(kind)];
    const int capacity = int(u32(kItemRanges + kind * 16 + 12));
    for (int k = 0; k < capacity; ++k) {
      const int id = s16(list + k * 4);
      if (id == 0) return false;
      if (id == item) return s16(list + k * 4 + 2) != 0;
    }
    return false;
  }
  return false;
}

// FUN_0051C8D0: the map item shows this cell.
bool Minimap::cellRevealed(uint32_t cell, int area, int room, bool hasMap) const {
  if (!hasMap) return false;
  const uint32_t lists = u32(kRevealLists + area * 4);
  if (lists == 0) return false;
  const uint32_t list = u32(lists + room * 8);
  const int n = int(u32(lists + room * 8 + 4));
  if (n < 0) return true;
  for (int i = 0; i < n; ++i)
    if (s16(list + i * 4) == s8(cell + 5) && s16(list + i * 4 + 2) == s8(cell + 4)) return true;
  return false;
}

bool Minimap::stateFlag() const { return (u32(u32(kStateFlags) + 4) >> 10) & 1; }

bool Minimap::roomFlag(int area, int room) const {
  const uint32_t info = u32(u32(u32(kRoomInfo) + 12) + area * 12 + 8);
  const uint32_t id = uint16_t(s16(info + room * 20 + 12));
  return (u32(u32(kRoomFlags) + (id & 0x3FFF) * 4) >> 27) & 1;
}

// FUN_004547A0, and the colours FUN_0051F6A8 gives each state.
uint32_t Minimap::roomColour(int area, int room) const {
  const uint32_t state = u32(u32(u32(u32(kRoomInfo) + 12) + area * 12 + 8) + room * 20);
  return state == 0x490 ? kRed : state == 0x491 ? kGreen : kBlue;
}

// All that decides the triangles: they are built again only when it changes.
uint64_t Minimap::signature(int area, int floor, uint32_t rooms, int count, bool hasMap) const {
  uint64_t h = 0xCBF29CE484222325ull;
  auto mix = [&h](uint64_t v) { h = (h ^ v) * 0x100000001B3ull; };
  mix(uint64_t(area)), mix(uint64_t(floor)), mix(hasMap), mix(stateFlag());
  const uint32_t seen = u32(kCellSeen);
  for (int i = 0; i < count; ++i) {
    const uint32_t r = rooms + i * 0x18;
    const uint32_t cells = u32(r + 16);
    for (int k = 0, n = s8(r + 9); k < n; ++k) mix(u8(seen + (u32(cells + k * 16) & 0x3FFF)) & 1);
    if (s8(r + 10) > 0) mix(roomFlag(area, s8(r + 8)));
  }
  return h;
}

// What FUN_0051F6A8 and FUN_0051F3B8 do to a room's model: colours from the seen cells, and the
// parts of other floors or not found yet hidden.
void Minimap::build(int area, int floor, uint32_t rooms, int count, bool hasMap, std::vector<MinimapVertex>& out) const {
  const uint32_t seenBytes = u32(kCellSeen);
  for (int i = 0; i < count; ++i) {
    const uint32_t r = rooms + i * 0x18;
    const auto model = models_.find(u32(r + 12));
    if (model == models_.end()) continue;
    const Model& m = model->second;
    const int room = s8(r + 8), cells = s8(r + 9), rules = s8(r + 10);
    const size_t nodes = m.parents.size();
    std::vector<uint8_t> hidden(nodes, 0);
    std::vector<uint32_t> colour(nodes, 0);
    auto forTree = [&](int node, auto&& apply) {
      if (node < 0 || size_t(node) >= nodes) return;
      std::vector<uint8_t> in(nodes, 0);
      in[size_t(node)] = 1;
      apply(size_t(node));
      for (size_t k = 0; k < nodes; ++k) {  // parents come before their children
        const int p = m.parents[k];
        if (p >= 0 && size_t(p) < nodes && in[size_t(p)] && !in[k]) in[k] = 1, apply(k);
      }
    };
    auto paint = [&](int node, uint32_t c) { forTree(node, [&](size_t k) { colour[k] = c; }); };
    auto hide = [&](int node) { forTree(node, [&](size_t k) { hidden[k] = 1; }); };
    const uint32_t ruleList = u32(r + 20);
    for (int k = 0; k < rules; ++k) {
      const uint32_t e = ruleList + k * 4;
      if (u8(e) != 0) continue;
      const bool value = s8(e + 2) == 1 ? roomFlag(area, room) : stateFlag();
      if (s8(e + 3) == int(!value)) hide(s8(e + 1));
    }
    const uint32_t cellList = u32(r + 16);
    const uint32_t floorColour = roomColour(area, room);
    for (int k = 0; k < cells; ++k) {
      const uint32_t cell = cellList + k * 16;
      const bool seen = (u8(seenBytes + (u32(cell) & 0x3FFF)) & 1) != 0;
      const bool revealed = cellRevealed(cell, area, room, hasMap);
      if (seen) {
        paint(s8(cell + 10), floorColour);
        paint(s8(cell + 11), kWhite);
        paint(s8(cell + 13), kWhite);
        paint(s8(cell + 12), kWhite);
      } else if (revealed) {
        paint(s8(cell + 10), kGrey);
        paint(s8(cell + 11), kGrey);
        paint(s8(cell + 13), kGrey);
      }
      if (s8(cell + 9) >= 0 && !(s8(cell + 6) == floor && (seen || revealed))) hide(s8(cell + 9));
      if (s8(cell + 12) >= 0 && revealed) hide(s8(cell + 12));
    }
    const int rotation = s16(r + 4) & 3;
    const float angle = f32(kRoomAngle + rotation * 4), cs = std::cos(angle), sn = std::sin(angle);
    const float px = -float(s16(r)) * 15.999999f + f32(kRoomOffsetX + rotation * 4);
    const float pz = float(s16(r + 2)) * 15.999999f + f32(kRoomOffsetZ + rotation * 4);
    for (const Model::Object& object : m.objects) {
      if (object.node < 0 || size_t(object.node) >= nodes) continue;
      if (hidden[size_t(object.node)] || colour[size_t(object.node)] == 0) continue;
      for (const auto& t : object.triangles) {
        const float u = t[0], w = t[1];
        out.push_back({-(u * cs + w * sn + px), -u * sn + w * cs + pz, colour[size_t(object.node)]});
      }
    }
  }
}

void Minimap::publish(const MinimapView& view) {
  std::lock_guard lock(mutex_);
  const uint64_t version = view_.version;
  view_ = view;
  view_.version = version;
}

bool Minimap::playable() const {
  const uint32_t player = u32(kPlayer);
  if (player == 0 || s16(player + uint32_t(s16(player + 0x144)) * 2 + 0x146) != 2) return false;  // FUN_00458320
  return quietFrames_ <= 2 && pausedFrames_ <= kPauseFrames && (u32(kInputOff) & 3) == 0 && !(u32(kEventFlags) & kEventsNotPlayable);
}

void Minimap::update(Cpu& c) {
  mem_ = &c.mem;
  quietFrames_ = statusDisplayUpdates() != seenUpdates_ ? 0 : quietFrames_ + 1;
  seenUpdates_ = statusDisplayUpdates();
  pausedFrames_ = (u32(kTaskState) & 1) ? 0 : pausedFrames_ + 1;
  // Out of play the map fades from where it was; it comes back only with somewhere to show.
  auto fadeOut = [this] {
    alpha_ = std::max(0.0f, alpha_ - kFadeOut);
    shown_.visible = alpha_ > 0, shown_.alpha = alpha_;
    publish(shown_);
  };
  if (!settings().minimap) alpha_ = 0;
  playingFrames_ = playable() ? playingFrames_ + 1 : 0;
  if (!settings().minimap || playingFrames_ == 0 || (alpha_ == 0 && playingFrames_ < kSettleFrames)) return fadeOut();
  MinimapView view;
  const int area = s8(kPlace), room = s8(kPlace + 1);
  int count = 0;
  const uint32_t rooms = roomsOf(area, count);
  if (rooms == 0 || count <= 0) return fadeOut();
  uint32_t current = 0;
  for (int i = 0; i < count; ++i)
    if (s8(rooms + i * 0x18 + 8) == room) current = rooms + i * 0x18;
  if (current == 0) return fadeOut();
  if (area != loadedArea_) {
    loadArea(area);
    shownSignature_ = 0;
  }
  const int floor = currentFloor(area, room, rooms, count);
  const bool hasMap = hasAreaMap(area);
  const uint64_t signatureNow = signature(area, floor, rooms, count, hasMap);
  if (signatureNow != shownSignature_) {
    shownSignature_ = signatureNow;
    std::vector<MinimapVertex> mesh;
    build(area, floor, rooms, count, hasMap, mesh);
    std::lock_guard lock(mutex_);
    mesh_ = std::move(mesh);
    ++view_.version;
  }
  // FUN_00520460: the player's position is in the room's own coordinates.
  const int rotation = s16(current + 4) & 3;
  const float angle = f32(kRoomAngle + rotation * 4), cs = std::cos(angle), sn = std::sin(angle);
  const float u = f32(kPosition) * kPositionToMap, w = f32(kPosition + 8) * kPositionToMap;
  const float yaw = f32(kYaw), du = std::sin(yaw), dw = std::cos(yaw);
  view.x = -(u * cs + w * sn - float(s16(current)) * 15.999999f + f32(kRoomOffsetX + rotation * 4));
  view.z = -u * sn + w * cs + float(s16(current + 2)) * 15.999999f + f32(kRoomOffsetZ + rotation * 4);
  view.dirX = -(du * cs + dw * sn);
  view.dirZ = -du * sn + dw * cs;
  alpha_ = std::min(1.0f, alpha_ + kFadeIn);
  view.alpha = alpha_;
  view.visible = true;
  shown_ = view;
  publish(view);
}

Minimap* gMinimap = nullptr;
}  // namespace

void installMinimap(const Iso9660& iso) {
  static Minimap minimap(iso);
  gMinimap = &minimap;
}

void updateMinimap(Cpu& cpu) {
  if (gMinimap) gMinimap->update(cpu);
}

MinimapView minimapView(std::vector<MinimapVertex>& triangles, uint64_t have) {
  return gMinimap ? gMinimap->view(triangles, have) : MinimapView{};
}
