#include <string.h>
#include "device/dcd.h"
#include "device/usbd_pvt.h"
#include "hardware/structs/usb.h"
#include "pico/time.h"
#include "pico/unique_id.h"
#include "tusb.h"
#include "port.h"

#ifndef DV3K_FTDI_PRODUCT
#define DV3K_FTDI_PRODUCT "USB-3003"
#endif

#define MPS         64
#define IN_PKTS     8
#define FIFO        1024
#define STR_MANUF   1
#define STR_PRODUCT 2
#define STR_SERIAL  3
#define STR_VERSION 4
#define MANUF       "x0rro"
#define DESC_LEN    (TUD_CONFIG_DESC_LEN + 9 + 7 + 7)

enum { SIO_RESET, SIO_MODEM_CTRL, SIO_SET_FLOW_CTRL, SIO_SET_BAUD_RATE, SIO_SET_DATA, SIO_GET_MODEM_STATUS,
       SIO_SET_EVENT_CHAR, SIO_SET_ERROR_CHAR, SIO_SET_LATENCY_TIMER = 9, SIO_GET_LATENCY_TIMER,
       SIO_SET_BITMODE, SIO_READ_PINS, SIO_READ_EEPROM = 0x90, SIO_WRITE_EEPROM, SIO_ERASE_EEPROM };

#define MODEM_STATUS 0xb1
#define LINE_STATUS  0x60
#define DATA_BREAK   0x4000

static const tusb_desc_device_t desc_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x0403,
    .idProduct = 0x6001,
    .bcdDevice = 0x0600,
    .iManufacturer = STR_MANUF,
    .iProduct = STR_PRODUCT,
    .iSerialNumber = STR_SERIAL,
    .bNumConfigurations = 1,
};

static const uint8_t desc_cfg[DESC_LEN] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, DESC_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 90),
    9, TUSB_DESC_INTERFACE, 0, 0, 2, TUSB_CLASS_VENDOR_SPECIFIC, 0xff, 0xff, STR_VERSION,
    7, TUSB_DESC_ENDPOINT, 0x81, TUSB_XFER_BULK, U16_TO_U8S_LE(MPS), 0,
    7, TUSB_DESC_ENDPOINT, 0x02, TUSB_XFER_BULK, U16_TO_U8S_LE(MPS), 0,
};

static char serial[9];
static const char *const strs[] = { [STR_MANUF] = MANUF, [STR_PRODUCT] = DV3K_FTDI_PRODUCT,
                                    [STR_SERIAL] = serial, [STR_VERSION] = DV3K_VERSION };

static uint16_t eeprom[64];
_Static_assert(0x18 + 2 * (sizeof MANUF + sizeof DV3K_FTDI_PRODUCT + sizeof serial) <= sizeof eeprom - 2,
               "DV3K_FTDI_PRODUCT is too long for the FT232R EEPROM image");
static int eeprom_ready;
static uint8_t ep_in, ep_out, latency = 16;
static uint16_t data_bits;
static uint8_t dtr_rts;
static int boot_pending;
static uint8_t out_pkt[MPS], in_buf[IN_PKTS * MPS + 2];
static uint8_t rxf[FIFO], txf[FIFO];
static uint32_t rxh, rxt, txh, txt;    /* all in the USB poll, so no barriers */
static int out_armed, in_busy;
static uint16_t in_len;
static uint32_t last_in;
static volatile int reconfigured;

static void init_serial(void)
{
    char id[PICO_UNIQUE_BOARD_ID_SIZE_BYTES * 2 + 1];
    if (serial[0])
        return;
    pico_get_unique_board_id_string(id, sizeof id);
    serial[0] = 'X', serial[1] = 'S';
    memcpy(serial + 2, id + strlen(id) - 6, 6);
}

static void init_eeprom(void)
{
    uint8_t *b = (uint8_t *)eeprom;
    uint16_t sum = 0xaaaa;
    unsigned off = 0x18, i, k;
    init_serial();
    memset(eeprom, 0, sizeof eeprom);
    eeprom[1] = desc_device.idVendor, eeprom[2] = desc_device.idProduct, eeprom[3] = desc_device.bcdDevice;
    b[0x08] = 0xa0, b[0x09] = 45;
    b[0x0a] = 0x08;
    eeprom[6] = 0x0200;
    for (i = STR_MANUF; i <= STR_SERIAL; i++) {
        size_t n = strlen(strs[i]);
        b[0x0e + 2 * (i - 1)] = (uint8_t)(off | 0x80);
        b[0x0f + 2 * (i - 1)] = (uint8_t)(2 * n + 2);
        b[off++] = (uint8_t)(2 * n + 2), b[off++] = TUSB_DESC_STRING;
        for (k = 0; k < n; k++)
            b[off++] = (uint8_t)strs[i][k], b[off++] = 0;
    }
    for (i = 0; i < 63; i++) {
        sum ^= eeprom[i];
        sum = (uint16_t)(sum << 1 | sum >> 15);
    }
    eeprom[63] = sum;
    eeprom_ready = 1;
}

const uint8_t *tud_descriptor_device_cb(void) { return (const uint8_t *)&desc_device; }

const uint8_t *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return desc_cfg;
}

const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    static uint16_t d[64];
    const char *s;
    uint8_t n;
    (void)langid;
    init_serial();
    if (index == 0) {
        d[1] = 0x0409;
        n = 1;
    } else {
        if (index >= sizeof strs / sizeof strs[0] || !strs[index])
            return NULL;
        s = strs[index];
        for (n = 0; n < 63 && s[n]; n++)
            d[1 + n] = (uint8_t)s[n];
    }
    d[0] = (uint16_t)(TUSB_DESC_STRING << 8 | (2 * n + 2));
    return d;
}

static void arm_out(void)
{
    if (!out_armed && ep_out && FIFO - (rxh - rxt) >= MPS && usbd_edpt_xfer(0, ep_out, out_pkt, MPS))
        out_armed = 1;
}

uint32_t port_read(uint8_t *p, uint32_t n)
{
    uint32_t i;
    if (n > rxh - rxt)
        n = rxh - rxt;
    for (i = 0; i < n; i++)
        p[i] = rxf[rxt++ & (FIFO - 1)];
    arm_out();
    return n;
}

uint32_t port_write_room(void) { return FIFO - (txh - txt); }

void port_write(const uint8_t *p, uint32_t n)
{
    while (n--)
        txf[txh++ & (FIFO - 1)] = *p++;
}

static void reopen(void)
{
    const uint8_t *ep = desc_cfg + TUD_CONFIG_DESC_LEN + 9;
    reconfigured = 0;
    dcd_edpt_close_all(0);
    dcd_edpt_open(0, (const tusb_desc_endpoint_t *)ep);
    dcd_edpt_open(0, (const tusb_desc_endpoint_t *)(ep + 7));
    if (out_armed)
        dcd_edpt_xfer(0, ep_out, out_pkt, MPS);
    if (in_busy)
        dcd_edpt_xfer(0, ep_in, in_buf, in_len);
}

void port_event(uint32_t event)
{
    const volatile uint8_t *s = usb_dpram->setup_packet;
    if (event == DCD_EVENT_SETUP_RECEIVED && ep_in && s[0] == 0x00 && s[1] == TUSB_REQ_SET_CONFIGURATION &&
        s[2] == desc_cfg[5])
        reconfigured = 1;
}

void port_flush(void)
{
    uint32_t len = 0, k, pkts;
    if (reconfigured)
        reopen();
    if (in_busy || !ep_in)
        return;
    if (txh == txt && time_us_32() - last_in < 1000u * (latency ? latency : 1))
        return;
    for (pkts = 0; pkts < IN_PKTS && txh != txt; pkts++) {
        in_buf[len++] = MODEM_STATUS, in_buf[len++] = LINE_STATUS;
        for (k = 0; k < MPS - 2 && txh != txt; k++)
            in_buf[len++] = txf[txt++ & (FIFO - 1)];
    }
    if (len % MPS == 0)
        in_buf[len++] = MODEM_STATUS, in_buf[len++] = LINE_STATUS;
    if (usbd_edpt_xfer(0, ep_in, in_buf, (uint16_t)len)) {
        in_busy = 1, in_len = (uint16_t)len;
        last_in = time_us_32();
    }
}

void port_discard(void) { txt = txh; }

void port_discard_rx(void)
{
    rxt = rxh;
    arm_out();
}

static void ftdi_init(void) {}

static void ftdi_reset(uint8_t rhport)
{
    (void)rhport;
    ep_in = ep_out = 0;
    out_armed = in_busy = 0, reconfigured = 0;
    rxh = rxt = txh = txt = 0;
    latency = 16, data_bits = 0, dtr_rts = 0, boot_pending = 0;
}

static uint16_t ftdi_open(uint8_t rhport, tusb_desc_interface_t const *itf, uint16_t max_len)
{
    uint16_t len = sizeof(tusb_desc_interface_t) + 2 * sizeof(tusb_desc_endpoint_t);
    if (itf->bInterfaceClass != TUSB_CLASS_VENDOR_SPECIFIC || itf->bNumEndpoints != 2 || max_len < len)
        return 0;
    if (!usbd_open_edpt_pair(rhport, tu_desc_next(itf), 2, TUSB_XFER_BULK, &ep_out, &ep_in))
        return 0;
    arm_out();
    return len;
}

static bool ftdi_xfer_cb(uint8_t rhport, uint8_t ep, xfer_result_t result, uint32_t n)
{
    uint32_t i;
    (void)rhport, (void)result;
    if (ep == ep_out) {
        for (i = 0; i < n; i++)
            rxf[rxh++ & (FIFO - 1)] = out_pkt[i];
        out_armed = 0;
        arm_out();
    } else if (ep == ep_in) {
        in_busy = 0;
        last_in = time_us_32();
        port_flush();
    }
    return true;
}

static bool ftdi_itf_control(uint8_t rhport, uint8_t stage, tusb_control_request_t const *r)
{
    (void)rhport, (void)stage, (void)r;
    return false;
}

static const usbd_class_driver_t ftdi_driver = {
    .name = "FTDI", .init = ftdi_init, .reset = ftdi_reset, .open = ftdi_open,
    .control_xfer_cb = ftdi_itf_control, .xfer_cb = ftdi_xfer_cb,
};

const usbd_class_driver_t *usbd_app_driver_get_cb(uint8_t *count)
{
    *count = 1;
    return &ftdi_driver;
}

static uint32_t baud(uint16_t value, uint16_t index)
{
    static const uint8_t eighths[8] = { 0, 4, 2, 1, 3, 5, 6, 7 };
    uint32_t div = value & 0x3fff, frac = eighths[(value >> 14 | (index & 1) << 2) & 7];
    if (div == 0 && frac == 0)
        return 3000000;
    if (div == 1 && frac == 0)
        return 2000000;
    return 24000000 / (8 * div + frac);
}

bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage, tusb_control_request_t const *r)
{
    static uint8_t buf[64];
    uint16_t v = r->wValue;
    uint8_t lines;
    if (stage == CONTROL_STAGE_ACK) {
        if (boot_pending)
            host_bootsel();
        return true;
    }
    if (stage != CONTROL_STAGE_SETUP)
        return true;
    if (r->bmRequestType_bit.direction == TUSB_DIR_IN) {
        uint16_t n = r->wLength < sizeof buf ? r->wLength : sizeof buf;
        memset(buf, 0, n);
        if (r->bRequest == SIO_GET_MODEM_STATUS)
            buf[0] = MODEM_STATUS, buf[1] = LINE_STATUS;
        else if (r->bRequest == SIO_GET_LATENCY_TIMER)
            buf[0] = latency;
        else if (r->bRequest == SIO_READ_PINS)
            buf[0] = 0xff;
        else if (r->bRequest == SIO_READ_EEPROM && n >= 2) {
            if (!eeprom_ready)
                init_eeprom();
            buf[0] = (uint8_t)eeprom[r->wIndex & 63], buf[1] = (uint8_t)(eeprom[r->wIndex & 63] >> 8);
        }
        return tud_control_xfer(rhport, r, buf, n);
    }
    switch (r->bRequest) {
    case SIO_RESET:
        host_drop(0);
        break;
    case SIO_MODEM_CTRL:
        lines = (uint8_t)((dtr_rts & ~(v >> 8)) | (v & v >> 8 & 3));
        if ((dtr_rts & 3) && !(lines & 3))
            host_drop(1);
        dtr_rts = lines & 3;
        break;
    case SIO_SET_BAUD_RATE:
        boot_pending = baud(v, r->wIndex) == 1200;
        break;
    case SIO_SET_DATA:
        if ((v & DATA_BREAK) && !(data_bits & DATA_BREAK))
            host_reset();
        data_bits = v;
        break;
    case SIO_SET_LATENCY_TIMER:
        latency = (uint8_t)v;
        break;
    default:
        break;
    }
    return tud_control_status(rhport, r);
}
