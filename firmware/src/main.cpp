// Battery-powered BLE e-paper display on a Pro Micro nRF52840 driving a 400x300
// SSD1683 panel.
//
// It advertises all the time, slowly, instead of a 2 s burst every 60 s with
// deep sleep in between: the SoftDevice Controller sleeps the CPU and the crystal
// between advert events, so an advert costs only its ~25 uC and the rest is
// System ON idle at a few uA. Continuous 2 s adverts are ~16 uA average, less
// than a 4 s window every minute would cost, and the panel is connectable at
// any moment instead of for 3 % of the time. main() runs once, forever.

#include <zephyr/drivers/flash.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/kernel.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/printk.h>
#include <nrfx.h>

#include "ble.h"
#include "config.h"
#include "epd.h"
#include "image_receiver.h"

static ImageReceiver s_rx;
static volatile bool s_clearRequested;

// --- the receiver, as seen from the Bluetooth RX thread ----------------------

extern "C" void rx_ctrl(const uint8_t *payload, size_t len)
{
  if (len < 1)
  {
    s_rx.setState(STATE_ERROR, ERR_BAD_LENGTH);
    return;
  }
  switch (payload[0])
  {
    case OP_BEGIN: s_rx.begin(payload, len); break;
    case OP_COMMIT: s_rx.commit(); break;
    case OP_ABORT: s_rx.abort(); break;
    case OP_CLEAR:
      // The panel is only ever touched from the main thread.
      s_rx.abort();
      s_rx.setState(STATE_RENDERING, ERR_NONE);
      s_clearRequested = true;
      break;
    default: s_rx.setState(STATE_ERROR, ERR_BAD_MAGIC); break;
  }
}

extern "C" int rx_data(const uint8_t *payload, size_t len)
{
  return s_rx.data(payload, len) == ERR_NONE ? 0 : -1;
}

extern "C" void rx_status(uint8_t out[6])
{
  const uint32_t received = s_rx.received();
  out[0] = uint8_t(s_rx.state());
  out[1] = uint8_t(s_rx.error());
  out[2] = uint8_t(received & 0xFF);
  out[3] = uint8_t((received >> 8) & 0xFF);
  out[4] = uint8_t((received >> 16) & 0xFF);
  out[5] = uint8_t((received >> 24) & 0xFF);
}

// --- the "painted" flag --------------------------------------------------------

// Whether this device has ever been given an image, so a reset, a reflash or a
// battery swap never repaints the splash over what the panel is showing. One
// word at the start of the storage partition, written once in the device's
// life; the NVS subsystem would be a lot of code for a single bit.
static const uint32_t PAINTED_MAGIC = 0x44544E50;  // "PNTD"
static const struct device *const s_flash = FIXED_PARTITION_DEVICE(storage_partition);
static const off_t s_flagOffset = FIXED_PARTITION_OFFSET(storage_partition);
static bool s_everPainted;

static bool readPainted()
{
  uint32_t value = 0;
  return flash_read(s_flash, s_flagOffset, &value, sizeof(value)) == 0 && value == PAINTED_MAGIC;
}

static void markPainted()
{
  if (s_everPainted)
  {
    return;
  }
  flash_erase(s_flash, s_flagOffset, 4096);  // one nRF52 page
  flash_write(s_flash, s_flagOffset, &PAINTED_MAGIC, sizeof(PAINTED_MAGIC));
  s_everPainted = true;
}

// --- one connection --------------------------------------------------------------

// Returns when the session is over: rendered, cleared, timed out or dropped.
static void serveSession()
{
  while (ble_connected())
  {
    if (s_clearRequested)
    {
      s_clearRequested = false;
      printk("clear\n");
      epd_clear();
      markPainted();  // a deliberate blank must not bring the splash back
      s_rx.setState(STATE_DONE, ERR_NONE);
      ble_publish_status();
      return;
    }

    if (s_rx.complete())
    {
      // The CRC pass, the inflate and the refresh all belong here, never in a
      // GATT callback.
      ble_publish_status();
      const XferError error = s_rx.verify();
      if (error != ERR_NONE)
      {
        printk("verify failed: error %d\n", int(error));
        s_rx.setState(STATE_ERROR, error);
        ble_publish_status();
        continue;  // stay connected so the client can retry with a fresh BEGIN
      }
      printk("rendering %u bytes, %d colours%s\n", unsigned(s_rx.frameLength()),
             s_rx.gray4() ? 4 : 2, s_rx.deflated() ? " (deflate)" : "");
      s_rx.setState(STATE_RENDERING);
      ble_publish_status();
      if (s_rx.gray4())
      {
        epd_render_gray4(s_rx.frame());
      }
      else
      {
        epd_render_1bpp(s_rx.frame(), s_rx.inverted());
      }
      markPainted();
      s_rx.setState(STATE_DONE);
      ble_publish_status();
      printk("rendered\n");
      return;
    }

    // A client that connects and then stalls would otherwise hold the radio on
    // until the battery dies.
    const int64_t idle = k_uptime_get() - ble_last_activity();
    if (idle > SESSION_TIMEOUT_MS)
    {
      printk("session idle in state %d, dropping\n", int(s_rx.state()));
      s_rx.setState(STATE_ERROR, ERR_INCOMPLETE);
      ble_publish_status();
      return;
    }
    ble_wait(uint32_t(SESSION_TIMEOUT_MS - idle));
  }
}

int main()
{
  // Why we are here. Read before anything can reset again: a reset every boot
  // is invisible without it. The bits are Zephyr's RESET_* flags (pin 1,
  // software 2, brownout 4, POR 8, watchdog 16, debug 32, ... lockup 256).
  uint32_t cause = 0;
  hwinfo_get_reset_cause(&cause);
  hwinfo_clear_reset_cause();
  printk("\nboot, reset cause 0x%x, UICR NFCPINS %08x PSELRESET %08x %08x, "
         "advertising every %u-%u ms\n",
         unsigned(cause), unsigned(NRF_UICR->NFCPINS), unsigned(NRF_UICR->PSELRESET[0]),
         unsigned(NRF_UICR->PSELRESET[1]), unsigned(ADV_INTERVAL_MIN * 5 / 8),
         unsigned(ADV_INTERVAL_MAX * 5 / 8));

  s_everPainted = readPainted();
  if (epd_init() != 0)
  {
    printk("epd: bus or pins not ready\n");
  }

  // Geometry and protocol version, so the client need not hardcode them. bpp is
  // the deepest mode accepted, selected per frame by BEGIN's FLAG_GRAY4.
  const uint8_t info[8] = {
      PROTOCOL_VERSION,
      uint8_t(PANEL_WIDTH & 0xFF),
      uint8_t(PANEL_WIDTH >> 8),
      uint8_t(PANEL_HEIGHT & 0xFF),
      uint8_t(PANEL_HEIGHT >> 8),
      2,
      uint8_t(DEEP_SLEEP_SECONDS & 0xFF),
      uint8_t(DEEP_SLEEP_SECONDS >> 8),
  };
  const int err = ble_begin(DEVICE_NAME, ADV_INTERVAL_MIN, ADV_INTERVAL_MAX, info);
  if (err != 0)
  {
    printk("bt_enable failed: %d\n", err);
    return 0;
  }

  if (!s_everPainted)
  {
    // Only ever shown on a device that has not been sent an image yet.
    epd_splash(DEVICE_NAME "\nready\n \nsend an image");
  }

  ble_update_battery();
  int64_t nextBattery = k_uptime_get() + BATTERY_PERIOD_MS;
  while (true)
  {
    s_rx.abort();
    s_clearRequested = false;
    // BT_LE_ADV_OPT_CONN does not restart after a disconnect, hence every loop.
    // A failure here would otherwise be a silent, radio-off "not found".
    const int adv = ble_adv_start();
    if (adv != 0)
    {
      printk("adv start failed: %d\n", adv);
      k_sleep(K_SECONDS(5));
      continue;
    }

    // Only a connection ends the wait; the timeout is the battery's cadence.
    // Never measured mid-session, so a panel refresh does not sag the reading.
    while (!ble_connected())
    {
      const int64_t now = k_uptime_get();
      if (now >= nextBattery)
      {
        ble_update_battery();
        nextBattery = now + BATTERY_PERIOD_MS;
      }
      ble_wait(uint32_t(nextBattery - now));
    }

    serveSession();
    // Give the last status notification time to reach the client before the
    // link goes away.
    k_msleep(NOTIFY_FLUSH_MS);
    ble_disconnect();
    for (int i = 0; i < 20 && ble_connected(); i++)
    {
      ble_wait(100);
    }
    epd_sleep();
  }
  return 0;
}
