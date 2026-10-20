#include "epd.h"

#include <string.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/regulator.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/kernel.h>
#include <zephyr/pm/device.h>
#include <zephyr/sys/printk.h>

#include "epd_gray_lut.h"
#include "font5x7.h"

#define PANEL_WIDTH 400
#define PANEL_HEIGHT 300
#define FRAME_1BPP (PANEL_WIDTH / 8 * PANEL_HEIGHT)   // 15000
#define FRAME_2BPP (PANEL_WIDTH / 4 * PANEL_HEIGHT)   // 30000

#define BUSY_TIMEOUT_MS 10000

static const struct spi_dt_spec s_spi =
    SPI_DT_SPEC_GET(DT_NODELABEL(epd), SPI_OP_MODE_MASTER | SPI_WORD_SET(8) | SPI_TRANSFER_MSB);
static const struct gpio_dt_spec s_dc = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), dc_gpios);
static const struct gpio_dt_spec s_rst = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), reset_gpios);
static const struct gpio_dt_spec s_busy = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), busy_gpios);
static const struct device *const s_vcc = DEVICE_DT_GET(DT_NODELABEL(vcc_ext));
#define VCC_STARTUP_MS (DT_PROP(DT_NODELABEL(vcc_ext), startup_delay_us) / 1000)

// One 1bpp plane, in RAM because EasyDMA cannot read flash. It is the splash
// framebuffer, the inverted copy of a 1bpp frame, and each 2bpp plane in turn.
static uint8_t s_plane[FRAME_1BPP];
static bool s_awake;

// --- bus ------------------------------------------------------------------

static void xfer(const uint8_t *buf, size_t len)
{
  const struct spi_buf b = {.buf = (void *)buf, .len = len};
  const struct spi_buf_set set = {.buffers = &b, .count = 1};
  spi_write_dt(&s_spi, &set);  // CS is held for the whole buffer
}

static void cmd(uint8_t c)
{
  gpio_pin_set_dt(&s_dc, 0);
  xfer(&c, 1);
}

static void data(const uint8_t *buf, size_t len)
{
  gpio_pin_set_dt(&s_dc, 1);
  xfer(buf, len);
}

// Command followed by up to four data bytes.
static void cmdn(uint8_t c, const uint8_t *d, size_t n)
{
  uint8_t copy[4];
  memcpy(copy, d, n);
  cmd(c);
  data(copy, n);
}
#define CMD(c, ...) cmdn((c), (const uint8_t[]){__VA_ARGS__}, sizeof((const uint8_t[]){__VA_ARGS__}))

// BUSY is high while the controller is working. Bounded: a panel that never
// releases BUSY must not hold a battery-powered board awake.
static bool wait_busy(void)
{
  const int64_t start = k_uptime_get();
  while (gpio_pin_get_dt(&s_busy) > 0)
  {
    if (k_uptime_get() - start > BUSY_TIMEOUT_MS)
    {
      printk("epd: BUSY timeout\n");
      return false;
    }
    k_msleep(10);
  }
  return true;
}

// --- power ------------------------------------------------------------------

// The panel is unpowered between refreshes: SSD1683 datasheet fig. 9-1 ends a
// refresh with "turn off VCI supply" once BUSY is low, and every paint already
// starts from a hardware reset and a full init. The order is the trap. Its
// inputs are rated -0.5 .. VDDIO+0.5 V, so with VCI off any line held high
// feeds the controller through its ESD diodes (the WeAct module has only 100R
// in series). So: supply first, then drive the lines; let every line go, then
// cut the supply. Released lines are inputs with the buffer disconnected, which
// neither drives the panel nor leaks on the nRF side.
static void power_on(void)
{
  regulator_enable(s_vcc);
  k_msleep(VCC_STARTUP_MS);  // datasheet: 10 ms after VCI
  gpio_pin_configure_dt(&s_spi.config.cs.gpio, GPIO_OUTPUT_INACTIVE);
  gpio_pin_configure_dt(&s_dc, GPIO_OUTPUT_INACTIVE);
  gpio_pin_configure_dt(&s_rst, GPIO_OUTPUT_INACTIVE);  // inactive = not in reset
  gpio_pin_configure_dt(&s_busy, GPIO_INPUT);
  pm_device_action_run(s_spi.bus, PM_DEVICE_ACTION_RESUME);  // SCK, MOSI back
}

static void power_off(void)
{
  // The sleep pinctrl state (low-power-enable) releases SCK and MOSI.
  pm_device_action_run(s_spi.bus, PM_DEVICE_ACTION_SUSPEND);
  gpio_pin_configure_dt(&s_spi.config.cs.gpio, GPIO_DISCONNECTED);
  gpio_pin_configure_dt(&s_dc, GPIO_DISCONNECTED);
  gpio_pin_configure_dt(&s_rst, GPIO_DISCONNECTED);
  gpio_pin_configure_dt(&s_busy, GPIO_DISCONNECTED);
  regulator_disable(s_vcc);
}

// Power, then hardware reset then software reset, waiting out BUSY after each
// -- the two waits GxEPD2 skips and the nRF sketch this was checked against
// does not.
static void wake(void)
{
  power_on();
  gpio_pin_set_dt(&s_rst, 0);
  k_msleep(10);
  gpio_pin_set_dt(&s_rst, 1);
  k_msleep(10);
  gpio_pin_set_dt(&s_rst, 0);
  k_msleep(10);
  wait_busy();
  cmd(0x12);  // SWRESET
  k_msleep(10);
  wait_busy();
  s_awake = true;
}

// Data entry X+ Y+, full window, counters at the origin. Repeated before every
// plane because the counters advance as the plane is written.
static void set_ram_area(void)
{
  CMD(0x11, 0x03);
  CMD(0x44, 0x00, 0x31);              // RAM X range, in bytes: 0..49
  CMD(0x45, 0x00, 0x00, 0x2B, 0x01);  // RAM Y range: 0..299
  CMD(0x4E, 0x00);
  CMD(0x4F, 0x00, 0x00);
}

// --- 1bpp, the controller's own waveform -----------------------------------

// Streams one value into both RAM planes, without a second frame buffer.
static void fill_planes(uint8_t value)
{
  uint8_t chunk[256];
  memset(chunk, value, sizeof(chunk));
  const uint8_t planes[2] = {0x26, 0x24};
  for (int p = 0; p < 2; p++)
  {
    set_ram_area();
    cmd(planes[p]);
    for (uint32_t sent = 0; sent < FRAME_1BPP; sent += sizeof(chunk))
    {
      data(chunk, MIN(sizeof(chunk), FRAME_1BPP - sent));
    }
  }
}

// GxEPD2's _Update_Full: the controller's own waveform, then power off.
static void refresh_full(void)
{
  CMD(0x21, 0x40, 0x00);  // bypass RED
  CMD(0x22, 0xF7);
  cmd(0x20);
  wait_busy();
}

// GxEPD2's GDEY042T81 init. Both RAM planes are written, which is what a clean
// full refresh needs to avoid ghosting.
//
// The clear to white first is not redundant: GxEPD2 runs clearScreen() before
// the first image write after init(), and the original firmware re-initialised
// it on every wake, so the panel always flashed to white and then to the image.
// Same look here.
static void render_planes_1bpp(void)
{
  CMD(0x01, 0x2B, 0x01, 0x00);  // MUX 300
  CMD(0x3C, 0x01);              // border waveform
  CMD(0x18, 0x80);              // internal temperature sensor
  fill_planes(0xFF);
  refresh_full();
  set_ram_area();
  cmd(0x26);
  data(s_plane, FRAME_1BPP);
  set_ram_area();
  cmd(0x24);
  data(s_plane, FRAME_1BPP);
  refresh_full();
  epd_sleep();
}

void epd_render_1bpp(const uint8_t *frame, bool inverted)
{
  wake();
  // The controller's convention is 1 = white; ours is 1 = black unless the
  // sender flagged the image as inverted. Safe when frame == s_plane.
  for (uint32_t i = 0; i < FRAME_1BPP; i++)
  {
    s_plane[i] = inverted ? frame[i] : (uint8_t)~frame[i];
  }
  render_planes_1bpp();
}

void epd_clear(void)
{
  wake();
  CMD(0x01, 0x2B, 0x01, 0x00);
  CMD(0x3C, 0x01);
  CMD(0x18, 0x80);
  fill_planes(0xFF);
  refresh_full();
  epd_sleep();
}

// --- 2bpp, Waveshare's greyscale waveform ------------------------------------

// Waveshare's epd4in2_V2 greyscale init. Loading the LUT replaces the
// panel's built-in monochrome waveform, so greyscale has no temperature
// compensation.
static void init_gray4(void)
{
  uint8_t lut[sizeof(LUT_4GRAY)];
  memcpy(lut, LUT_4GRAY, sizeof(lut));  // EasyDMA needs it in RAM

  CMD(0x21, 0x00, 0x00);              // display update control
  CMD(0x3C, 0x03);                    // border waveform
  CMD(0x0C, 0x8B, 0x9C, 0xA4, 0x0F);  // booster soft start
  cmd(0x32);                          // waveform LUT
  data(lut, 227);
  cmdn(0x3F, lut + 227, 1);
  cmdn(0x03, lut + 228, 1);  // gate voltage
  cmdn(0x04, lut + 229, 3);  // source voltage
  cmdn(0x2C, lut + 232, 1);  // VCOM
  set_ram_area();
  wait_busy();
}

// Packs one 1bpp plane out of the 2bpp frame into s_plane. `high` picks which
// bit of each pixel's level goes into it. The low bit feeds 0x24 and the high
// bit 0x26, which looks backwards and is the one thing Waveshare's own two
// demos disagree about: their Arduino epd4in2_V2 sketch maps the middle codes
// the other way round, their Raspberry Pi EPD_4in2_V2.c the way it is here.
// Only this way are the levels monotonic -- measured on the panel, the other
// mapping paints light grey where dark belongs. Do not "fix" it back.
static void pack_plane(const uint8_t *frame, bool high)
{
  for (uint32_t i = 0, o = 0; i < FRAME_2BPP; i += 2, o++)
  {
    // Inverted on the way out: the controller reads 1 as white, we store 1 as
    // ink, exactly as the 1bpp path does.
    const uint8_t a = (uint8_t)~frame[i];
    const uint8_t b = (uint8_t)~frame[i + 1];
    s_plane[o] = high ? (uint8_t)((grayHighNibble(a) << 4) | grayHighNibble(b))
                      : (uint8_t)((grayLowNibble(a) << 4) | grayLowNibble(b));
  }
}

void epd_render_gray4(const uint8_t *frame)
{
  wake();
  init_gray4();

  cmd(0x24);
  pack_plane(frame, false);
  data(s_plane, FRAME_1BPP);

  set_ram_area();
  cmd(0x26);
  pack_plane(frame, true);
  data(s_plane, FRAME_1BPP);

  CMD(0x22, 0xCF);  // greyscale update sequence
  cmd(0x20);
  wait_busy();
  epd_sleep();
}

// --- splash -------------------------------------------------------------------

#define SCALE 2
#define CHAR_W (6 * SCALE)
#define LINE_H (8 * SCALE + 6)

static void draw_char(int x0, int y0, char c)
{
  if (c < FONT5X7_FIRST || c > FONT5X7_LAST)
  {
    return;
  }
  const uint8_t *glyph = FONT5X7[c - FONT5X7_FIRST];
  for (int col = 0; col < 5; col++)
  {
    for (int row = 0; row < 7; row++)
    {
      if (!((glyph[col] >> row) & 1))
      {
        continue;
      }
      for (int dy = 0; dy < SCALE; dy++)
      {
        for (int dx = 0; dx < SCALE; dx++)
        {
          const int x = x0 + col * SCALE + dx;
          const int y = y0 + row * SCALE + dy;
          if (x >= 0 && x < PANEL_WIDTH && y >= 0 && y < PANEL_HEIGHT)
          {
            s_plane[y * (PANEL_WIDTH / 8) + x / 8] |= (uint8_t)(0x80 >> (x % 8));
          }
        }
      }
    }
  }
}

void epd_splash(const char *text)
{
  int lines = 1;
  for (const char *p = text; *p; p++)
  {
    lines += *p == '\n';
  }
  memset(s_plane, 0x00, FRAME_1BPP);  // 1 = ink, inverted by epd_render_1bpp

  int y = (PANEL_HEIGHT - lines * LINE_H) / 2;
  const char *line = text;
  while (true)
  {
    const char *end = strchr(line, '\n');
    const int len = end ? (int)(end - line) : (int)strlen(line);
    int x = (PANEL_WIDTH - len * CHAR_W) / 2;
    for (int i = 0; i < len; i++, x += CHAR_W)
    {
      draw_char(x, y, line[i]);
    }
    if (!end)
    {
      break;
    }
    line = end + 1;
    y += LINE_H;
  }
  epd_render_1bpp(s_plane, false);
}

// --- sleep ------------------------------------------------------------------

void epd_sleep(void)
{
  if (!s_awake)
  {
    return;
  }
  // Deep sleep, then the supply. Both refresh sequences above already end with
  // analog and clock off, so this is belt and braces before VCI goes.
  CMD(0x10, 0x01);
  k_msleep(100);
  power_off();
  s_awake = false;
}

// The regulator is boot-on, so the panel is powered while the SPI driver drives
// CS high at init; this is the first point that can release the lines and cut it.
int epd_init(void)
{
  if (!spi_is_ready_dt(&s_spi) || !device_is_ready(s_vcc) || !gpio_is_ready_dt(&s_dc) ||
      !gpio_is_ready_dt(&s_rst) || !gpio_is_ready_dt(&s_busy))
  {
    return -ENODEV;
  }
  power_off();
  return 0;
}
