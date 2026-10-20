// Everything that touches the panel: an SSD1683 over SPI, driven directly for
// both bit depths. The panel is unpowered except while it is being painted:
// every render powers it, paints, and cuts its supply again.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Releases the lines and cuts the panel's supply. Returns a negative errno if
// the bus, a pin or the regulator is missing.
int epd_init(void);

// Paints a packed 1bpp full-panel frame (MSB first, 50 bytes per row).
// `inverted` selects the bit convention: false means a set bit is black.
void epd_render_1bpp(const uint8_t *frame, bool inverted);

// Paints a packed 2bpp frame, levels 0 white .. 3 black, 100 bytes per row.
// About four seconds: the greyscale waveform is longer than the monochrome one.
void epd_render_gray4(const uint8_t *frame);

// Blanks the panel to white.
void epd_clear(void);

// Centred multi-line text at 2x the 5x7 font; '\n' starts a new line.
void epd_splash(const char *text);

// Deep sleep and power off, if the panel was woken. The image is retained
// without power.
void epd_sleep(void);

#ifdef __cplusplus
}
#endif
