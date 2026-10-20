// Drives ImageReceiver with frames produced by the Python client, so the two
// sides of the wire format are checked against each other. Runs on the host,
// no board required: see test/run.sh.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>
#include "epd_gray_lut.h"
#include "image_receiver.h"

static std::vector<uint8_t> readFile(const char* p) {
  FILE* f = fopen(p, "rb");
  std::vector<uint8_t> v;
  uint8_t b[4096]; size_t n;
  while ((n = fread(b, 1, sizeof(b), f)) > 0) v.insert(v.end(), b, b + n);
  fclose(f);
  return v;
}

// Streams a payload in 249-byte DATA writes, the size a 256-byte MTU allows.
static XferError stream(ImageReceiver& rx, const std::vector<uint8_t>& data) {
  const size_t chunk = 249;
  for (size_t off = 0; off < data.size(); off += chunk) {
    const size_t n = std::min(chunk, data.size() - off);
    std::vector<uint8_t> pkt(4 + n);
    const uint32_t offset = uint32_t(off);
    memcpy(pkt.data(), &offset, 4);
    memcpy(pkt.data() + 4, data.data() + off, n);
    const XferError e = rx.data(pkt.data(), pkt.size());
    if (e != ERR_NONE) return e;
  }
  return ERR_NONE;
}

static int failures = 0;
static void check(bool ok, const char* what) {
  printf("%s %s\n", ok ? "  ok  " : "  FAIL", what);
  if (!ok) failures++;
}

int main(int, char** argv) {
  auto begin = readFile(argv[1]);   // 19-byte BEGIN built by the client
  auto frame = readFile(argv[2]);   // 15000-byte packed image
  auto grayBegin = readFile(argv[3]);  // the same, with FLAG_GRAY4 set
  auto grayFrame = readFile(argv[4]);  // 30000-byte 2bpp image
  auto zBegin = readFile(argv[5]);      // FLAG_DEFLATE, len = the zlib stream
  auto zFrame = readFile(argv[6]);      // the 1bpp image, zlib-compressed
  auto zGrayBegin = readFile(argv[7]);  // FLAG_DEFLATE | FLAG_GRAY4
  auto zGrayFrame = readFile(argv[8]);  // the 2bpp image, zlib-compressed
  auto zBadBegin = readFile(argv[9]);   // a valid CRC over an invalid stream
  auto zBadFrame = readFile(argv[10]);

  // --- happy path -------------------------------------------------------
  {
    ImageReceiver rx;
    check(rx.begin(begin.data(), begin.size()) == ERR_NONE, "BEGIN accepted");
    check(rx.width() == 400 && rx.height() == 300, "geometry parsed");
    check(rx.frameLength() == 15000, "length parsed");
    check(!rx.inverted(), "flags parsed");

    const size_t chunk = 249;
    for (size_t off = 0; off < frame.size(); off += chunk) {
      const size_t n = std::min(chunk, frame.size() - off);
      std::vector<uint8_t> pkt(4 + n);
      pkt[0] = off & 0xFF; pkt[1] = (off >> 8) & 0xFF;
      pkt[2] = (off >> 16) & 0xFF; pkt[3] = (off >> 24) & 0xFF;
      memcpy(pkt.data() + 4, frame.data() + off, n);
      if (rx.data(pkt.data(), pkt.size()) != ERR_NONE) { check(false, "DATA chunk"); break; }
    }
    check(rx.received() == 15000, "all bytes received");
    check(rx.commit() == ERR_NONE, "COMMIT accepted");
    check(rx.complete(), "state is VERIFYING");
    check(rx.verify() == ERR_NONE, "CRC verifies");
    check(memcmp(rx.frame(), frame.data(), frame.size()) == 0, "frame reassembled byte-exact");
  }

  // --- a single flipped bit must be caught ------------------------------
  {
    ImageReceiver rx;
    rx.begin(begin.data(), begin.size());
    std::vector<uint8_t> pkt(4 + frame.size(), 0);
    memcpy(pkt.data() + 4, frame.data(), frame.size());
    pkt[4] ^= 0x01;
    rx.data(pkt.data(), pkt.size());
    rx.commit();
    check(rx.verify() == ERR_CRC_MISMATCH, "flipped bit -> CRC_MISMATCH");
  }

  // --- protocol errors ---------------------------------------------------
  {
    ImageReceiver rx;
    check(rx.commit() == ERR_NO_SESSION, "COMMIT without BEGIN -> NO_SESSION");

    auto bad = begin; bad[1] = 'X';
    check(rx.begin(bad.data(), bad.size()) == ERR_BAD_MAGIC, "bad magic -> BAD_MAGIC");

    bad = begin; bad[5] = 99;
    check(rx.begin(bad.data(), bad.size()) == ERR_BAD_VERSION, "bad version -> BAD_VERSION");

    bad = begin; bad[7] = 128; bad[8] = 0;  // width 128 instead of 400
    check(rx.begin(bad.data(), bad.size()) == ERR_BAD_GEOMETRY, "wrong size -> BAD_GEOMETRY");

    check(rx.begin(begin.data(), begin.size() - 1) == ERR_BAD_LENGTH, "short BEGIN -> BAD_LENGTH");
  }
  {
    ImageReceiver rx;
    rx.begin(begin.data(), begin.size());
    // one byte past the end of the frame
    std::vector<uint8_t> pkt(4 + 8, 0);
    uint32_t off = 14999;
    memcpy(pkt.data(), &off, 4);
    check(rx.data(pkt.data(), pkt.size()) == ERR_OVERFLOW, "past end -> OVERFLOW");
  }
  {
    ImageReceiver rx;
    rx.begin(begin.data(), begin.size());
    std::vector<uint8_t> pkt(4 + 8, 0);
    uint32_t off = 4096;  // skips ahead, leaving a hole
    memcpy(pkt.data(), &off, 4);
    check(rx.data(pkt.data(), pkt.size()) == ERR_INCOMPLETE, "gap -> INCOMPLETE");
  }
  {
    ImageReceiver rx;
    rx.begin(begin.data(), begin.size());
    std::vector<uint8_t> pkt(4 + 100, 0);
    rx.data(pkt.data(), pkt.size());
    check(rx.commit() == ERR_INCOMPLETE, "short image -> INCOMPLETE");
  }
  // re-sending a chunk must not corrupt the byte count
  {
    ImageReceiver rx;
    rx.begin(begin.data(), begin.size());
    std::vector<uint8_t> pkt(4 + 100, 0);
    memcpy(pkt.data() + 4, frame.data(), 100);
    rx.data(pkt.data(), pkt.size());
    rx.data(pkt.data(), pkt.size());
    check(rx.received() == 100, "re-sent chunk does not double-count");
  }

  // --- 4 colours --------------------------------------------------------
  {
    ImageReceiver rx;
    check(rx.begin(grayBegin.data(), grayBegin.size()) == ERR_NONE, "2bpp BEGIN accepted");
    check(rx.gray4(), "FLAG_GRAY4 parsed");
    check(rx.frameLength() == 30000, "2bpp length parsed");

    const size_t chunk = 249;
    for (size_t off = 0; off < grayFrame.size(); off += chunk) {
      const size_t n = std::min(chunk, grayFrame.size() - off);
      std::vector<uint8_t> pkt(4 + n);
      const uint32_t offset = uint32_t(off);
      memcpy(pkt.data(), &offset, 4);
      memcpy(pkt.data() + 4, grayFrame.data() + off, n);
      if (rx.data(pkt.data(), pkt.size()) != ERR_NONE) { check(false, "2bpp chunk"); break; }
    }
    check(rx.received() == 30000, "all 2bpp bytes received");
    check(rx.commit() == ERR_NONE, "2bpp COMMIT accepted");
    check(rx.verify() == ERR_NONE, "2bpp CRC verifies");
    check(memcmp(rx.frame(), grayFrame.data(), grayFrame.size()) == 0, "2bpp frame reassembled");
  }
  // The flag and the length have to agree: neither size is accepted in the
  // other mode, so a client that sets one without the other is caught at BEGIN
  // rather than after streaming 30 kB.
  {
    ImageReceiver rx;
    std::vector<uint8_t> hdr = grayBegin;
    hdr[6] = 0;  // 30000 bytes, but claiming 1bpp
    check(rx.begin(hdr.data(), hdr.size()) == ERR_BAD_LENGTH, "2bpp length without the flag");
  }
  {
    ImageReceiver rx;
    std::vector<uint8_t> hdr = begin;
    hdr[6] = FLAG_GRAY4;  // 15000 bytes, but claiming 2bpp
    check(rx.begin(hdr.data(), hdr.size()) == ERR_BAD_LENGTH, "1bpp length with the flag");
  }

  // --- deflate ----------------------------------------------------------
  // The CRC covers the compressed bytes, so radio corruption is still caught
  // before the inflate; what the panel gets is the inflated frame, which has
  // to come back byte-identical to the one the client packed.
  {
    ImageReceiver rx;
    check(rx.begin(zBegin.data(), zBegin.size()) == ERR_NONE, "deflate BEGIN accepted");
    check(rx.deflated(), "FLAG_DEFLATE parsed");
    check(rx.frameLength() == 15000, "frameLength is the uncompressed size");
    check(rx.wireLength() == zFrame.size(), "wireLength is what crosses the air");
    check(rx.wireLength() < 7500, "fixture really does compress");
    check(stream(rx, zFrame) == ERR_NONE, "deflate chunks accepted");
    check(rx.commit() == ERR_NONE, "deflate COMMIT accepted");
    check(rx.verify() == ERR_NONE, "deflate CRC verifies and inflates");
    check(memcmp(rx.frame(), frame.data(), frame.size()) == 0, "inflated frame is byte-exact");
  }
  {
    ImageReceiver rx;
    check(rx.begin(zGrayBegin.data(), zGrayBegin.size()) == ERR_NONE, "2bpp deflate BEGIN");
    check(rx.deflated() && rx.gray4(), "both flags parsed");
    check(rx.frameLength() == 30000, "2bpp uncompressed size");
    check(stream(rx, zGrayFrame) == ERR_NONE, "2bpp deflate chunks accepted");
    check(rx.commit() == ERR_NONE, "2bpp deflate COMMIT accepted");
    check(rx.verify() == ERR_NONE, "2bpp deflate verifies and inflates");
    check(memcmp(rx.frame(), grayFrame.data(), grayFrame.size()) == 0, "2bpp inflated byte-exact");
  }
  // A stream that passes the CRC but is not valid deflate must fail cleanly
  // rather than render whatever happened to be in the buffer.
  {
    ImageReceiver rx;
    check(rx.begin(zBadBegin.data(), zBadBegin.size()) == ERR_NONE, "bad-stream BEGIN accepted");
    check(stream(rx, zBadFrame) == ERR_NONE, "bad-stream chunks accepted");
    check(rx.commit() == ERR_NONE, "bad-stream COMMIT accepted");
    check(rx.verify() == ERR_INFLATE, "corrupt stream rejected as INFLATE");
  }
  // With the flag set, len is the compressed size, so it has to be shorter
  // than the frame -- and non-zero.
  {
    ImageReceiver rx;
    std::vector<uint8_t> hdr = zBegin;
    const uint32_t full = 15000;
    memcpy(hdr.data() + 11, &full, 4);
    check(rx.begin(hdr.data(), hdr.size()) == ERR_BAD_LENGTH, "deflate len == frame size");
  }
  {
    ImageReceiver rx;
    std::vector<uint8_t> hdr = zBegin;
    const uint32_t none = 0;
    memcpy(hdr.data() + 11, &none, 4);
    check(rx.begin(hdr.data(), hdr.size()) == ERR_BAD_LENGTH, "deflate len == 0");
  }

  // --- 2bpp plane packing -----------------------------------------------
  // One byte of four pixels, ink-side up: white, light, dark, black. After the
  // driver's inversion the controller sees 0b11 0b10 0b01 0b00. The low bits
  // go to plane 0x24 and the high bits to 0x26 -- the mapping that makes the
  // levels monotonic on this panel.
  {
    const uint8_t px = uint8_t((0 << 6) | (1 << 4) | (2 << 2) | 3);
    const uint8_t sent = uint8_t(~px);
    check(grayLowNibble(sent) == 0xA, "plane 0x24: white+dark are 1");
    check(grayHighNibble(sent) == 0xC, "plane 0x26: white+light are 1");
    check(grayLowNibble(uint8_t(~0x00)) == 0xF, "all white -> plane 0x24 all 1");
    check(grayHighNibble(uint8_t(~0xFF)) == 0x0, "all black -> plane 0x26 all 0");
    check(PANEL_FRAME_BYTES_GRAY4 == 30000, "2bpp frame is 30000 bytes");
  }

  printf(failures ? "\n%d FAILED\n" : "\nall passed\n", failures);
  return failures ? 1 : 0;
}
