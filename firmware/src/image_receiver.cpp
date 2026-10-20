#include "image_receiver.h"

#include <stdlib.h>
#include <string.h>

#include "crc32.h"

// Inflate is miniz's tinfl, vendored in src/miniz. The host test build uses
// plain zlib instead.
#ifdef __ZEPHYR__
#include <miniz.h>
#else
#include <zlib.h>
#endif

namespace
{

// Inflates a whole zlib stream into out, which must be exactly outLen bytes.
// Returns false on a corrupt stream or a length that does not match.
bool inflateFrame(const uint8_t *in, uint32_t inLen, uint8_t *out, uint32_t outLen)
{
#ifdef __ZEPHYR__
  // tinfl_decompress_mem_to_mem() would be one call, but it puts an ~11 kB
  // tinfl_decompressor on the stack, too much for the main thread.
  // The low-level entry point lets us heap it. All the input is present and the
  // output buffer is bigger than any back-reference can reach, so one call is
  // enough and no 32 kB dictionary is needed.
  tinfl_decompressor *d = static_cast<tinfl_decompressor *>(malloc(sizeof(tinfl_decompressor)));
  if (d == nullptr)
  {
    return false;
  }
  tinfl_init(d);
  size_t inSize = inLen;
  size_t outSize = outLen;
  const tinfl_status status =
      tinfl_decompress(d, in, &inSize, out, out, &outSize,
                       TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
  free(d);
  return status == TINFL_STATUS_DONE && outSize == outLen;
#else
  uLongf outSize = outLen;
  return uncompress(out, &outSize, in, inLen) == Z_OK && outSize == outLen;
#endif
}

}  // namespace

XferError ImageReceiver::fail(XferError error)
{
  m_state = STATE_ERROR;
  m_error = error;
  return error;
}

void ImageReceiver::setState(XferState state, XferError error)
{
  m_state = state;
  m_error = error;
}

XferError ImageReceiver::begin(const uint8_t *payload, size_t len)
{
  // opcode | magic[4] | ver | flags | w u16 | h u16 | len u32 | crc32 u32
  if (len != BEGIN_PAYLOAD_LEN)
  {
    return fail(ERR_BAD_LENGTH);
  }
  if (memcmp(payload + 1, PROTO_MAGIC, sizeof(PROTO_MAGIC)) != 0)
  {
    return fail(ERR_BAD_MAGIC);
  }
  if (payload[5] != PROTOCOL_VERSION)
  {
    return fail(ERR_BAD_VERSION);
  }

  const uint8_t flags = payload[6];
  const uint16_t width = uint16_t(payload[7]) | (uint16_t(payload[8]) << 8);
  const uint16_t height = uint16_t(payload[9]) | (uint16_t(payload[10]) << 8);
  const uint32_t length = uint32_t(payload[11]) | (uint32_t(payload[12]) << 8)
                          | (uint32_t(payload[13]) << 16) | (uint32_t(payload[14]) << 24);
  const uint32_t declaredCrc = uint32_t(payload[15]) | (uint32_t(payload[16]) << 8)
                               | (uint32_t(payload[17]) << 16) | (uint32_t(payload[18]) << 24);

  // Only full-panel frames are accepted; the client reads the geometry from the
  // INFO characteristic and letterboxes to fit, so this should never trip in
  // normal use.
  if (width != PANEL_WIDTH || height != PANEL_HEIGHT)
  {
    return fail(ERR_BAD_GEOMETRY);
  }
  // The flag picks the frame size: a 2bpp frame is twice a 1bpp one, and a
  // length that does not match the mode is rejected here rather than left for
  // the CRC to discover.
  const bool gray4 = (flags & FLAG_GRAY4) != 0;
  const uint32_t rawLength = gray4 ? PANEL_FRAME_BYTES_GRAY4 : PANEL_FRAME_BYTES;
  if (flags & FLAG_DEFLATE)
  {
    // len is the size of the zlib stream instead. A stream at least as long as
    // the frame it encodes is pointless, so the sender should not have set the
    // flag; rejecting it also bounds the staging buffer below.
    if (length == 0 || length >= rawLength)
    {
      return fail(ERR_BAD_LENGTH);
    }
  }
  else if (length != rawLength)
  {
    return fail(ERR_BAD_LENGTH);
  }

  if (m_buffer == nullptr)
  {
    // Always the larger size: one allocation serves both modes, and the wake
    // cycle ends seconds later anyway.
    m_buffer = static_cast<uint8_t *>(malloc(PANEL_FRAME_BYTES_GRAY4));
    if (m_buffer == nullptr)
    {
      return fail(ERR_NO_MEMORY);
    }
  }

  // Sized per session, so it cannot be kept the way the frame buffer is.
  // Freeing first means a second BEGIN in one session cannot leak it.
  free(m_comp);
  m_comp = nullptr;
  if (flags & FLAG_DEFLATE)
  {
    m_comp = static_cast<uint8_t *>(malloc(length));
    if (m_comp == nullptr)
    {
      return fail(ERR_NO_MEMORY);
    }
  }

  m_flags = flags;
  m_width = width;
  m_height = height;
  m_rawLength = rawLength;
  m_declaredLength = length;
  m_declaredCrc = declaredCrc;
  m_received = 0;
  m_state = STATE_RECEIVING;
  m_error = ERR_NONE;
  return ERR_NONE;
}

XferError ImageReceiver::data(const uint8_t *payload, size_t len)
{
  if (m_state != STATE_RECEIVING)
  {
    return fail(ERR_NO_SESSION);
  }
  if (len <= DATA_HEADER_LEN)
  {
    return fail(ERR_BAD_LENGTH);
  }

  const uint32_t offset = uint32_t(payload[0]) | (uint32_t(payload[1]) << 8)
                          | (uint32_t(payload[2]) << 16) | (uint32_t(payload[3]) << 24);
  const uint32_t chunkLength = uint32_t(len - DATA_HEADER_LEN);

  if (offset > m_declaredLength || chunkLength > m_declaredLength - offset)
  {
    return fail(ERR_OVERFLOW);
  }
  // Re-sending an already-received chunk is fine, but skipping ahead would
  // leave a hole that the CRC would only catch after the whole transfer.
  if (offset > m_received)
  {
    return fail(ERR_INCOMPLETE);
  }

  memcpy(sink() + offset, payload + DATA_HEADER_LEN, chunkLength);
  if (offset + chunkLength > m_received)
  {
    m_received = offset + chunkLength;
  }
  return ERR_NONE;
}

XferError ImageReceiver::commit()
{
  if (m_state != STATE_RECEIVING)
  {
    return fail(ERR_NO_SESSION);
  }
  if (m_received != m_declaredLength)
  {
    return fail(ERR_INCOMPLETE);
  }
  m_state = STATE_VERIFYING;
  m_error = ERR_NONE;
  return ERR_NONE;
}

XferError ImageReceiver::verify()
{
  // The CRC covers what actually crossed the air, so it still catches radio
  // corruption before the inflate has to make sense of it.
  if (crc32(sink(), m_declaredLength) != m_declaredCrc)
  {
    return ERR_CRC_MISMATCH;
  }
  if (m_comp != nullptr)
  {
    if (!inflateFrame(m_comp, m_declaredLength, m_buffer, m_rawLength))
    {
      return ERR_INFLATE;
    }
    free(m_comp);
    m_comp = nullptr;
  }
  return ERR_NONE;
}

void ImageReceiver::abort()
{
  // The buffer itself is deliberately kept: the board deep-sleeps within
  // seconds, which wipes RAM anyway, and reusing it avoids fragmenting the
  // heap under the BLE stack.
  free(m_comp);
  m_comp = nullptr;
  m_received = 0;
  m_rawLength = 0;
  m_declaredLength = 0;
  m_state = STATE_IDLE;
  m_error = ERR_NONE;
}
