// Compile-time configuration for the BLE e-paper display.

#pragma once

#include <stddef.h>
#include <stdint.h>

// Panel wiring is in app.overlay.

// ---------------------------------------------------------------------------
// Panel geometry. The GDEY042T81 is natively 400x300 landscape, so a packed
// 1bpp frame is exactly 50 bytes per row x 300 rows = 15000 bytes.
// ---------------------------------------------------------------------------
static const uint16_t PANEL_WIDTH  = 400;
static const uint16_t PANEL_HEIGHT = 300;
static const uint16_t PANEL_STRIDE = (PANEL_WIDTH + 7) / 8;
static const uint32_t PANEL_FRAME_BYTES = uint32_t(PANEL_STRIDE) * PANEL_HEIGHT;

// The same panel in 4-level greyscale: 2 bits per pixel, so 100 bytes per row
// and 30000 for a frame. Whether a panel can do this at all depends on its
// film, not the controller; see the "Grayscale" section of README.md.
static const uint16_t PANEL_STRIDE_GRAY4 = PANEL_WIDTH / 4;
static const uint32_t PANEL_FRAME_BYTES_GRAY4 = uint32_t(PANEL_STRIDE_GRAY4) * PANEL_HEIGHT;

// ---------------------------------------------------------------------------
// Advertising. Continuous, connectable, 1.0-1.5 s on air (0.625 ms units):
// the SoftDevice Controller sleeps the CPU and the crystal between events, so
// only the events cost; see the top of main.cpp.
// ---------------------------------------------------------------------------
static const uint16_t ADV_INTERVAL_MIN = 1600;
static const uint16_t ADV_INTERVAL_MAX = 2400;

// Reported in INFO as sleep_seconds, the client's hint for how long the panel
// may go unheard. There is no sleep any more; the field is kept for the wire.
static const uint32_t DEEP_SLEEP_SECONDS  = 60;

// A client that connects and then stalls would otherwise hold the session
// open forever.
static const uint32_t SESSION_TIMEOUT_MS = 60000;

// Grace period after the final status notification so it reaches the client
// before we tear the link down.
static const uint32_t NOTIFY_FLUSH_MS = 300;

// ---------------------------------------------------------------------------
// GAP / GATT identity. Vendor-specific 128-bit UUIDs; no manufacturer data.
// ---------------------------------------------------------------------------
#define DEVICE_NAME "EPaper-BLE"

#define SVC_UUID_IMAGE  "69770000-b271-453a-a64d-cb31cec14bee"
#define CHR_UUID_CTRL   "69770001-b271-453a-a64d-cb31cec14bee"
#define CHR_UUID_DATA   "69770002-b271-453a-a64d-cb31cec14bee"
#define CHR_UUID_STATUS "69770003-b271-453a-a64d-cb31cec14bee"
#define CHR_UUID_INFO   "69770004-b271-453a-a64d-cb31cec14bee"

// ---------------------------------------------------------------------------
// Wire protocol. Kept in one place so the Python client and the firmware
// cannot drift apart.
// ---------------------------------------------------------------------------
static const uint8_t PROTOCOL_VERSION = 1;

// BEGIN header magic, "EPD1".
static const uint8_t PROTO_MAGIC[4] = {'E', 'P', 'D', '1'};

enum CtrlOpcode : uint8_t
{
  OP_BEGIN  = 0x01,
  OP_COMMIT = 0x02,
  OP_ABORT  = 0x03,
  OP_CLEAR  = 0x04,
};

// BEGIN payload: opcode | magic[4] | ver | flags | w u16 | h u16 | len u32 | crc32 u32
static const size_t BEGIN_PAYLOAD_LEN = 19;

// DATA payload: offset u32 | bytes...
static const size_t DATA_HEADER_LEN = 4;

// BEGIN flags.
static const uint8_t FLAG_INVERTED = 1 << 0;  // a set bit means white, not black
static const uint8_t FLAG_GRAY4    = 1 << 1;  // 4 colours (2bpp), not 2 (1bpp)
// DATA carries a zlib stream of the frame rather than the frame itself, so the
// BEGIN len and crc32 describe the compressed bytes. The uncompressed size is
// whatever FLAG_GRAY4 implies, so the header needs no second length field.
static const uint8_t FLAG_DEFLATE  = 1 << 2;

enum XferState : uint8_t
{
  STATE_IDLE      = 0,
  STATE_RECEIVING = 1,
  STATE_VERIFYING = 2,
  STATE_RENDERING = 3,
  STATE_DONE      = 4,
  STATE_ERROR     = 5,
};

enum XferError : uint8_t
{
  ERR_NONE          = 0,
  ERR_BAD_MAGIC     = 1,
  ERR_BAD_VERSION   = 2,
  ERR_BAD_GEOMETRY  = 3,
  ERR_BAD_LENGTH    = 4,
  ERR_CRC_MISMATCH  = 5,
  ERR_OVERFLOW      = 6,
  ERR_NO_SESSION    = 7,
  ERR_NO_MEMORY     = 8,
  ERR_INCOMPLETE    = 9,
  ERR_INFLATE       = 10,
};
