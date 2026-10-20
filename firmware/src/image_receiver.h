// Reassembles a packed frame arriving over GATT -- 1bpp black and white, or
// 2bpp four-level greyscale when the sender sets FLAG_GRAY4 -- and checks it
// against the declared length and CRC before anything reaches the panel. With
// FLAG_DEFLATE the bytes on the wire are a zlib stream and verify() inflates
// them into the frame buffer once the CRC has passed.
//
// The BEGIN/DATA/ABORT handlers are cheap enough to run straight from the
// Bluetooth RX thread. commit() only validates the byte count; the CRC pass, the
// inflate and the render are driven from main() via verify(), so the RX thread
// is never blocked on a long scan or an SPI transfer.

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "config.h"

class ImageReceiver
{
public:
  // Handlers for the CTRL/DATA writes. Each returns ERR_NONE on success.
  XferError begin(const uint8_t *payload, size_t len);
  XferError data(const uint8_t *payload, size_t len);

  // Checks that every declared byte arrived. Cheap; safe in a callback.
  XferError commit();

  // Recomputes the CRC over what arrived, then inflates it if the sender
  // compressed it. Both are too slow for a callback: call from loop().
  XferError verify();

  void abort();

  bool hasSession() const { return m_state == STATE_RECEIVING || m_state == STATE_VERIFYING; }
  bool complete() const { return m_state == STATE_VERIFYING; }

  const uint8_t *frame() const { return m_buffer; }
  // The frame the panel gets, which is not what arrived when it was compressed.
  uint32_t frameLength() const { return m_rawLength; }
  uint32_t wireLength() const { return m_declaredLength; }
  uint32_t received() const { return m_received; }
  uint16_t width() const { return m_width; }
  uint16_t height() const { return m_height; }
  // 1bpp frames only; ignored when the frame is 2bpp.
  bool inverted() const { return (m_flags & FLAG_INVERTED) != 0; }
  // 2bpp, four levels, PANEL_FRAME_BYTES_GRAY4 long.
  bool gray4() const { return (m_flags & FLAG_GRAY4) != 0; }
  // The frame arrived as a zlib stream.
  bool deflated() const { return (m_flags & FLAG_DEFLATE) != 0; }

  XferState state() const { return m_state; }
  XferError error() const { return m_error; }
  void setState(XferState state, XferError error = ERR_NONE);

private:
  // Records the failure and returns it, so callers can `return fail(...)`.
  XferError fail(XferError error);

  // Where the incoming bytes land: the compressed staging buffer if there is
  // one, otherwise the frame buffer itself.
  uint8_t *sink() { return m_comp != nullptr ? m_comp : m_buffer; }

  uint8_t *m_buffer = nullptr;  // always the 2bpp size, allocated on the first
                                // BEGIN and kept for the rest of the wake cycle
  uint8_t *m_comp = nullptr;    // compressed bytes when FLAG_DEFLATE is set;
                                // sized per session, freed once inflated
  uint32_t m_rawLength = 0;     // the frame the panel gets: 15000 or 30000
  uint32_t m_declaredLength = 0;  // bytes on the wire, compressed or not
  uint32_t m_declaredCrc = 0;
  uint32_t m_received = 0;
  uint16_t m_width = 0;
  uint16_t m_height = 0;
  uint8_t m_flags = 0;
  XferState m_state = STATE_IDLE;
  XferError m_error = ERR_NONE;
};
