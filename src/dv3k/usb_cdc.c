#include "pico/unique_id.h"
#include "tusb.h"
#include "port.h"

#define STR_MANUF   1
#define STR_PRODUCT 2
#define STR_SERIAL  3
#define STR_CDC     4
#define DESC_LEN    (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN)

static const tusb_desc_device_t desc_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x2e8a,
    .idProduct = 0x0009,
    .bcdDevice = 0x0100,
    .iManufacturer = STR_MANUF,
    .iProduct = STR_PRODUCT,
    .iSerialNumber = STR_SERIAL,
    .bNumConfigurations = 1,
};

static const uint8_t desc_cfg[DESC_LEN] = {
    TUD_CONFIG_DESCRIPTOR(1, 2, 0, DESC_LEN, 0, 250),
    TUD_CDC_DESCRIPTOR(0, STR_CDC, 0x81, 8, 0x02, 0x82, 64),
};

static char serial[PICO_UNIQUE_BOARD_ID_SIZE_BYTES * 2 + 1];
static const char *const strs[] = { [STR_MANUF] = "x0rro", [STR_PRODUCT] = "xombe-stick", [STR_SERIAL] = serial,
                                    [STR_CDC] = DV3K_VERSION };

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
    if (!serial[0])
        pico_get_unique_board_id_string(serial, sizeof serial);
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

uint32_t port_read(uint8_t *p, uint32_t n) { return tud_cdc_available() ? tud_cdc_read(p, n) : 0; }
uint32_t port_write_room(void) { return tud_cdc_write_available(); }
void port_write(const uint8_t *p, uint32_t n) { tud_cdc_write(p, n); }
void port_flush(void) { tud_cdc_write_flush(); }
void port_discard(void) { tud_cdc_write_clear(); }
void port_discard_rx(void) { tud_cdc_read_flush(); }
void port_event(uint32_t event) { (void)event; }

void tud_cdc_line_state_cb(uint8_t itf, bool dtr, bool rts)
{
    (void)itf, (void)rts;
    if (!dtr)
        host_drop(1);
}

void tud_cdc_line_coding_cb(uint8_t itf, cdc_line_coding_t const *coding)
{
    (void)itf;
    if (coding->bit_rate == 1200)
        host_bootsel();
}

void tud_cdc_send_break_cb(uint8_t itf, uint16_t ms)
{
    (void)itf;
    if (ms)
        host_reset();
}
