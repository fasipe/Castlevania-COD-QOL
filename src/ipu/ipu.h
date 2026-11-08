#pragma once
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include "gs/dmac.h"

// The IPU: the PS2's MPEG-2 decoding unit. The game's movie player (libmpeg) parses the stream
// itself and has the IPU do the bit-level work: variable- and fixed-length decoding (VDEC/FDEC),
// whole macroblocks to IDCT output (BDEC), and YCbCr to RGB conversion (CSC). Input arrives
// through DMA channel 4 into an 8-qword FIFO, output leaves through DMA channel 3.
class Ipu : public DmaDevice {
public:
  explicit Ipu(Dmac& dmac);

  bool read(uint32_t phys, uint64_t& value);
  bool write(uint32_t phys, uint64_t value);
  bool read128(uint32_t phys, void* qword);         // output FIFO
  bool write128(uint32_t phys, const void* qword);  // input FIFO
  void dmaStarted(int channel) override;

  uint64_t commands[16] = {};
  uint64_t macroblocks = 0;
  std::string fault;  // first unsupported request, for the run report

private:
  struct Starved {};  // thrown when a command needs input that has not arrived yet

  // Bitstream reader over the input FIFO, most significant bit first.
  uint32_t peek(int bits);  // bits <= 32
  uint32_t get(int bits);
  void skip(int bits) { get(bits); }
  int32_t getSigned(int bits);
  void need(int bits);
  void fill(uint32_t aheadQw);
  uint32_t aheadQw() const;

  void run();
  void execute();
  void bdec(uint32_t cmd);
  void csc(uint32_t cmd);
  int decodeBlock(int16_t* coef, int block, bool intra);
  int vlc(const struct VlcTable& t);
  void output(const void* data, uint32_t bytes);
  void drain();
  void updateTop();

  Dmac& dmac_;
  std::vector<uint8_t> in_;  // FIFO contents, whole qwords from the start of the current one
  uint64_t bit_ = 0;         // read position in `in_`, in bits
  // Output FIFO: bytes appended at the back and taken from the front.
  struct ByteQueue {
    std::vector<uint8_t> data;
    size_t head = 0;
    size_t size() const { return data.size() - head; }
    bool empty() const { return size() == 0; }
    void clear() { data.clear(), head = 0; }
    const uint8_t* front() const { return data.data() + head; }
    void pop(size_t n) {
      head += n;
      if (head >= data.size()) clear();
      else if (head >= 65536) data.erase(data.begin(), data.begin() + ptrdiff_t(head)), head = 0;
    }
  } out_;
  uint32_t cmd_ = 0, data_ = 0, top_ = 0;
  bool pending_ = false, topValid_ = false;
  uint32_t ctrl_ = 0;     // IDP, AS, IVF, QST, MP1, PCT (bits 16-26)
  uint32_t cbp_ = 0;
  bool ecd_ = false, scd_ = false;
  uint32_t cscLeft_ = 0;  // macroblocks still to convert in the current CSC
  int dcPred_[3] = {};
  uint8_t iq_[2][64] = {};  // intra / non-intra matrices in natural order
  uint16_t vqclut_[16] = {};
  uint32_t th0_ = 0, th1_ = 0;
};
