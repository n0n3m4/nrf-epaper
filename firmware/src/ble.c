#include "ble.h"

#include <string.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>

// The same UUIDs as SVC_UUID_IMAGE and friends in config.h. Zephyr's
// static GATT table needs them as numbers, so this is the one place the
// protocol is copied rather than shared; change both.
#define EPAPER_UUID(w16) \
  BT_UUID_128_ENCODE(0x69770000u | (w16), 0xb271, 0x453a, 0xa64d, 0xcb31cec14bee)

static const struct bt_uuid_128 s_svc_uuid = BT_UUID_INIT_128(EPAPER_UUID(0x0000));
#define CHR_CTRL BT_UUID_DECLARE_128(EPAPER_UUID(0x0001))
#define CHR_DATA BT_UUID_DECLARE_128(EPAPER_UUID(0x0002))
#define CHR_STATUS BT_UUID_DECLARE_128(EPAPER_UUID(0x0003))
#define CHR_INFO BT_UUID_DECLARE_128(EPAPER_UUID(0x0004))

static K_SEM_DEFINE(s_event, 0, 1);
static struct bt_conn *s_conn;
static volatile bool s_connected;
static int64_t s_last_activity;
static uint8_t s_info[8];
static struct bt_le_adv_param s_adv_param;
static struct bt_data s_sd[1];

static void touch(void)
{
  s_last_activity = k_uptime_get();
  k_sem_give(&s_event);
}

static ssize_t write_ctrl(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                          const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
  if (offset != 0)
  {
    return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
  }
  rx_ctrl(buf, len);
  ble_publish_status();
  touch();
  return len;
}

static ssize_t write_data(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                          const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
  if (offset != 0)
  {
    return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
  }
  if (rx_data(buf, len) != 0)
  {
    // Only report failures; notifying on every chunk would flood the link.
    ble_publish_status();
  }
  touch();
  return len;
}

static ssize_t read_status(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
                           uint16_t len, uint16_t offset)
{
  uint8_t value[6];
  rx_status(value);
  return bt_gatt_attr_read(conn, attr, buf, len, offset, value, sizeof(value));
}

static ssize_t read_info(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
                         uint16_t len, uint16_t offset)
{
  return bt_gatt_attr_read(conn, attr, buf, len, offset, s_info, sizeof(s_info));
}

BT_GATT_SERVICE_DEFINE(
    s_svc,
    BT_GATT_PRIMARY_SERVICE(&s_svc_uuid),
    BT_GATT_CHARACTERISTIC(CHR_CTRL, BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
                           BT_GATT_PERM_WRITE, NULL, write_ctrl, NULL),
    BT_GATT_CHARACTERISTIC(CHR_DATA, BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP,
                           BT_GATT_PERM_WRITE, NULL, write_data, NULL),
    BT_GATT_CHARACTERISTIC(CHR_STATUS, BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY, BT_GATT_PERM_READ,
                           read_status, NULL, NULL),
    BT_GATT_CCC(NULL, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
    BT_GATT_CHARACTERISTIC(CHR_INFO, BT_GATT_CHRC_READ, BT_GATT_PERM_READ, read_info, NULL, NULL));

// Index of the STATUS value attribute in the table above: service, then two
// attributes per characteristic.
#define STATUS_ATTR (&s_svc.attrs[6])

static void on_connected(struct bt_conn *conn, uint8_t err)
{
  if (err != 0)
  {
    return;
  }
  s_conn = bt_conn_ref(conn);
  s_connected = true;
  touch();
}

static void on_disconnected(struct bt_conn *conn, uint8_t reason)
{
  if (s_conn != NULL)
  {
    bt_conn_unref(s_conn);
    s_conn = NULL;
  }
  s_connected = false;
  k_sem_give(&s_event);
}

BT_CONN_CB_DEFINE(s_conn_cbs) = {
    .connected = on_connected,
    .disconnected = on_disconnected,
};

// The 128-bit UUID plus flags fills most of the 31-byte advert, so the name
// goes into the scan response.
static const struct bt_data s_ad[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),
    BT_DATA_BYTES(BT_DATA_UUID128_ALL, EPAPER_UUID(0x0000)),
};

int ble_begin(const char *name, uint16_t adv_min, uint16_t adv_max, const uint8_t info[8])
{
  memcpy(s_info, info, sizeof(s_info));
  s_adv_param = (struct bt_le_adv_param)BT_LE_ADV_PARAM_INIT(BT_LE_ADV_OPT_CONN, adv_min, adv_max, NULL);
  s_sd[0].type = BT_DATA_NAME_COMPLETE;
  s_sd[0].data_len = strlen(name);
  s_sd[0].data = (const uint8_t *)name;
  return bt_enable(NULL);
}

int ble_adv_start(void)
{
  touch();
  return bt_le_adv_start(&s_adv_param, s_ad, ARRAY_SIZE(s_ad), s_sd, ARRAY_SIZE(s_sd));
}

bool ble_connected(void)
{
  return s_connected;
}

void ble_disconnect(void)
{
  if (s_conn != NULL)
  {
    bt_conn_disconnect(s_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
  }
}

bool ble_wait(uint32_t timeout_ms)
{
  return k_sem_take(&s_event, K_MSEC(timeout_ms)) == 0;
}

int64_t ble_last_activity(void)
{
  return s_last_activity;
}

void ble_publish_status(void)
{
  if (s_conn == NULL)
  {
    return;
  }
  uint8_t value[6];
  rx_status(value);
  // -ENOTCONN if the client never subscribed; it can still read STATUS.
  bt_gatt_notify(s_conn, STATUS_ATTR, value, sizeof(value));
}
