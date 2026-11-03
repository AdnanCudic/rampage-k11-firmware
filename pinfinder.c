// Minimal USB boot keyboard for the Rampage Radiant K11 (LZ901 = CH555 clone),
// currently built as a matrix pin finder: pressing a key types the pin pair
// it connects as "DdRr" (driven port/bit, read port/bit), e.g. "4002".
//
// Register handling follows WCH's CH555 USB device example. No libraries are
// called from the interrupt: SDCC's mcs51 library is not reentrant.

#include <stdint.h>
#include "ch555.h"

#define EP0_SIZE 8
#define KBD_EP_SIZE 8

// Standard / HID request codes
#define REQ_GET_STATUS 0x00
#define REQ_CLEAR_FEATURE 0x01
#define REQ_SET_FEATURE 0x03
#define REQ_SET_ADDRESS 0x05
#define REQ_GET_DESCRIPTOR 0x06
#define REQ_GET_CONFIGURATION 0x08
#define REQ_SET_CONFIGURATION 0x09
#define REQ_GET_INTERFACE 0x0A
#define REQ_SET_INTERFACE 0x0B
#define HID_GET_IDLE 0x02
#define HID_GET_PROTOCOL 0x03
#define HID_SET_IDLE 0x0A
#define HID_SET_PROTOCOL 0x0B

#define REQ_TYPE_MASK 0x60
#define REQ_TYPE_STANDARD 0x00
#define REQ_TYPE_CLASS 0x20

#define DESC_DEVICE 0x01
#define DESC_CONFIG 0x02
#define DESC_STRING 0x03
#define DESC_HID 0x21
#define DESC_REPORT 0x22

#define LEN_STALL 0xFFFF

static __code const uint8_t device_desc[18] = {
    18, DESC_DEVICE, 0x10, 0x01, // USB 1.1
    0x00, 0x00, 0x00,            // class defined per interface
    EP0_SIZE,
    0xED, 0xFE,                  // VID 0xFEED
    0x12, 0x4B,                  // PID 0x4B12
    0x01, 0x00,                  // bcdDevice
    1, 2, 0,                     // manufacturer, product, no serial
    1,                           // one configuration
};

// Boot keyboard report without LED output: modifiers, reserved, 6 keys.
static __code const uint8_t report_desc[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01,             // Generic Desktop, Keyboard, Application
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7,             // Key codes: LeftControl..Right GUI
    0x15, 0x00, 0x25, 0x01, 0x75, 0x01, 0x95, 0x08, // 8 x 1-bit
    0x81, 0x02,                                     // Input (Data, Var, Abs)
    0x95, 0x01, 0x75, 0x08, 0x81, 0x01,             // reserved byte
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65, // 6 x 8-bit keys
    0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00, // Input (Data, Array)
    0xC0,
};

#define CONFIG_LEN (9 + 9 + 9 + 7)
static __code const uint8_t config_desc[CONFIG_LEN] = {
    9, DESC_CONFIG, CONFIG_LEN, 0, 1, 1, 0, 0x80, 50, // bus powered, 100 mA
    9, 0x04, 0, 0, 1, 0x03, 0x01, 0x01, 0,          // interface 0: HID, boot, keyboard
    9, DESC_HID, 0x11, 0x01, 0, 1, DESC_REPORT, sizeof(report_desc), 0,
    7, 0x05, 0x81, 0x03, KBD_EP_SIZE, 0, 10,        // EP1 IN, interrupt, 10 ms
};

static __code const uint8_t string0[] = {4, DESC_STRING, 0x09, 0x04};
static __code const uint8_t string1[] = {16, DESC_STRING, 'R', 0, 'a', 0, 'm', 0, 'p', 0, 'a', 0, 'g', 0, 'e', 0};
static __code const uint8_t string2[] = {24, DESC_STRING, 'K', 0, '1', 0, '1', 0, ' ', 0, 'm', 0, 'i', 0, 'n', 0,
                                         'i', 0, 'm', 0, 'a', 0, 'l', 0};

static __code const uint8_t *desc_ptr;
static uint16_t desc_left;
static uint8_t setup_req;
static uint8_t new_address;
static volatile uint8_t usb_config;
static uint8_t hid_idle;
static uint8_t hid_protocol = 1;
static volatile uint8_t kbd_ep_busy;

static void clock_24mhz(void)
{
    SAFE_MOD = 0x55;
    SAFE_MOD = 0xAA;
    CLOCK_CFG = CLOCK_CFG & ~MASK_SYS_CK_SEL | 0x05;
    SAFE_MOD = 0x00;
}

static void usb_init(void)
{
    IE_USB = 0;
    USB_CTRL = 0;
    D0_EP0RES = UEP_R_RES_ACK | UEP_T_RES_NAK;
    D0_EP_MOD = bUX_DEV_EN;
    D0_EP1RES = bUEP_X_AUTO_TOG | UEP_X_RES_NAK;
    D0_ADDR = 0;
    HB_ADDR = 0x7F; // forward hub-address traffic straight to sub-device 0
    USB_IF = 0xFF;
    USB_IE = bUX_IE_TRANSFER | bUX_IE_BUS_RST;
    USB_CTRL = bUX_DP_PU_EN;
    IE_USB = 1;
}

// Functions called from the USB interrupt must not share overlaid locals
// with main-loop functions.
#pragma save
#pragma nooverlay

// Load the next EP0 IN chunk of the pending descriptor; returns its length.
static uint8_t ep0_load_chunk(void)
{
    uint8_t n = desc_left > EP0_SIZE ? EP0_SIZE : (uint8_t)desc_left;
    uint8_t i;
    for (i = 0; i < n; i++)
        pD0_EP0_BUF[i] = desc_ptr[i];
    desc_ptr += n;
    desc_left -= n;
    return n;
}

static uint16_t handle_setup(void)
{
    uint8_t type = pD0_EP0_BUF[0];
    uint8_t value_l = pD0_EP0_BUF[2];
    uint8_t value_h = pD0_EP0_BUF[3];
    uint16_t want = pD0_EP0_BUF[6] | ((uint16_t)pD0_EP0_BUF[7] << 8);
    uint16_t len = 0;

    setup_req = pD0_EP0_BUF[1];

    if ((type & REQ_TYPE_MASK) == REQ_TYPE_CLASS) {
        switch (setup_req) {
        case HID_SET_IDLE:
            hid_idle = value_h;
            return 0;
        case HID_SET_PROTOCOL:
            hid_protocol = value_l;
            return 0;
        case HID_GET_IDLE:
            pD0_EP0_BUF[0] = hid_idle;
            return 1;
        case HID_GET_PROTOCOL:
            pD0_EP0_BUF[0] = hid_protocol;
            return 1;
        default:
            return LEN_STALL;
        }
    }
    if ((type & REQ_TYPE_MASK) != REQ_TYPE_STANDARD)
        return LEN_STALL;

    switch (setup_req) {
    case REQ_GET_DESCRIPTOR:
        switch (value_h) {
        case DESC_DEVICE:
            desc_ptr = device_desc;
            len = sizeof(device_desc);
            break;
        case DESC_CONFIG:
            desc_ptr = config_desc;
            len = sizeof(config_desc);
            break;
        case DESC_HID:
            desc_ptr = config_desc + 18;
            len = 9;
            break;
        case DESC_REPORT:
            desc_ptr = report_desc;
            len = sizeof(report_desc);
            break;
        case DESC_STRING:
            if (value_l == 0) {
                desc_ptr = string0;
                len = sizeof(string0);
            } else if (value_l == 1) {
                desc_ptr = string1;
                len = sizeof(string1);
            } else if (value_l == 2) {
                desc_ptr = string2;
                len = sizeof(string2);
            } else {
                return LEN_STALL;
            }
            break;
        default:
            return LEN_STALL;
        }
        desc_left = len < want ? len : want;
        return ep0_load_chunk();

    case REQ_SET_ADDRESS:
        new_address = value_l; // applied after the status stage
        return 0;
    case REQ_GET_CONFIGURATION:
        pD0_EP0_BUF[0] = usb_config;
        return 1;
    case REQ_SET_CONFIGURATION:
        usb_config = value_l;
        D0_EP1RES = bUEP_X_AUTO_TOG | UEP_X_RES_NAK; // restart at DATA0
        kbd_ep_busy = 0;
        return 0;
    case REQ_GET_INTERFACE:
        pD0_EP0_BUF[0] = 0;
        return 1;
    case REQ_GET_STATUS:
        pD0_EP0_BUF[0] = 0;
        pD0_EP0_BUF[1] = 0;
        return 2;
    case REQ_SET_INTERFACE:
    case REQ_SET_FEATURE:
    case REQ_CLEAR_FEATURE:
        return 0;
    default:
        return LEN_STALL;
    }
}

void usb_isr(void) __interrupt(INT_NO_USB)
{
    uint8_t flags, status;
    uint16_t len;
    uint8_t i;

    for (;;) {
        flags = USB_IF;
        if (flags & bUX_IF_D0_TRANS) {
            status = D0_STATUS;
            if ((status & (bUXS_SETUP_ACT | MASK_UXS_TOKEN)) == (bUXS_SETUP_ACT | UXS_TOKEN_FREE)) {
                // SETUP: reply starts with DATA1
                D0_EP0RES = bUEP_R_TOG | bUEP_T_TOG | UEP_R_RES_ACK | UEP_T_RES_ACK;
                len = handle_setup();
                if (len == LEN_STALL) {
                    setup_req = 0xFF;
                    D0_EP0RES = bUEP_R_TOG | bUEP_T_TOG | UEP_R_RES_STALL | UEP_T_RES_STALL;
                    // brief hold so the stall is seen, then re-arm for the next SETUP
                    for (i = 255; i; i--)
                        ;
                    D0_EP0RES = UEP_R_RES_ACK | UEP_T_RES_NAK;
                } else {
                    D0_EP0T_L = (uint8_t)len;
                }
            } else {
                switch (status & (MASK_UXS_TOKEN | MASK_UXS_ENDP)) {
                case UXS_TOKEN_IN | 0:
                    if (setup_req == REQ_GET_DESCRIPTOR) {
                        D0_EP0T_L = ep0_load_chunk();
                        D0_EP0RES ^= bUEP_T_TOG;
                    } else {
                        if (setup_req == REQ_SET_ADDRESS)
                            D0_ADDR = new_address;
                        D0_EP0T_L = 0;
                        D0_EP0RES = UEP_R_RES_ACK | UEP_T_RES_NAK;
                    }
                    break;
                case UXS_TOKEN_OUT | 0:
                    // status stage of an IN transfer
                    D0_EP0RES = UEP_R_RES_ACK | UEP_T_RES_NAK;
                    break;
                case UXS_TOKEN_IN | 1:
                    D0_EP1T_L = 0;
                    D0_EP1RES = D0_EP1RES & ~MASK_UEP_X_RES | UEP_X_RES_NAK;
                    kbd_ep_busy = 0;
                    break;
                default:
                    break;
                }
            }
            D0_STATUS = 0; // clears bUX_IF_D0_TRANS
        } else if (flags & bUX_IF_BUS_RST) {
            D0_EP0RES = UEP_R_RES_ACK | UEP_T_RES_NAK;
            D0_EP1RES = bUEP_X_AUTO_TOG | UEP_X_RES_NAK;
            D0_ADDR = 0;
            HB_ADDR = 0x7F;
            usb_config = 0;
            kbd_ep_busy = 0;
            USB_IF = bUX_IF_BUS_RST;
        } else if (flags) {
            USB_IF = flags & (bUX_IF_SUSPEND | bUX_IF_FIFO_OV);
            D0_STATUS = 0;
            if (!(USB_IF & (bUX_IF_D0_TRANS | bUX_IF_BUS_RST)))
                break;
        } else {
            break;
        }
    }
}
#pragma restore

// ---------------------------------------------------------------------------
// Keyboard reports

#define KC_SPACE 0x2C
#define KC_ENTER 0x28

static void send_key(uint8_t keycode)
{
    uint8_t i;
    if (!usb_config)
        return;
    while (kbd_ep_busy)
        ;
    for (i = 0; i < KBD_EP_SIZE; i++)
        pUSB_BUF_DEV0[UX_EP1_ADDR + i] = 0;
    pUSB_BUF_DEV0[UX_EP1_ADDR + 2] = keycode;
    kbd_ep_busy = 1;
    D0_EP1T_L = KBD_EP_SIZE;
    D0_EP1RES = D0_EP1RES & ~MASK_UEP_X_RES | UEP_X_RES_ACK;
}

static void type_key(uint8_t keycode)
{
    send_key(keycode);
    send_key(0); // release
}

static void type_digit(uint8_t d)
{
    type_key(d == 0 ? 0x27 : 0x1D + d);
}

// ---------------------------------------------------------------------------
// Matrix pin finder

// Every GPIO bonded out on the LQFP48 package except the USB pins (P6.x).
// Pin code = port * 10 + bit, as typed.
#define NPINS 42
static __code const uint8_t pin_port[NPINS] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2,
    2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3, 4, 4, 4, 4, 4, 4, 4, 4, 7, 7,
};
static __code const uint8_t pin_bit[NPINS] = {
    0, 1, 2, 3, 4, 5, 6, 7, 0, 1, 2, 3, 4, 5, 6, 7, 0, 1, 2, 3, 4,
    5, 6, 7, 0, 1, 2, 3, 4, 5, 6, 7, 0, 1, 2, 3, 4, 5, 6, 7, 0, 1,
};

// Snapshot of all port inputs: P0..P4, then P7.0/P7.1 in bits 0/1.
typedef struct {
    uint8_t p[6];
} ports_t;

// Per driven pin: which inputs read low with no key pressed.
static __xdata ports_t baseline[NPINS];
static __xdata uint8_t drivable[NPINS];

static void read_ports(ports_t *s)
{
    s->p[0] = P0;
    s->p[1] = P1;
    s->p[2] = P2;
    s->p[3] = P3;
    s->p[4] = P4;
    s->p[5] = (P7 >> 4) & 0x03;
}

static uint8_t port_index(uint8_t port)
{
    return port == 7 ? 5 : port;
}

static void pins_idle(void)
{
    // open-drain with pull-up: writing 1 releases a pin, 0 pulls it low
    P4_LED_KEY = 0;
    P0_MOD_OC = 0xFF;
    P0_DIR_PU = 0xFF;
    P1_MOD_OC = 0xFF;
    P1_DIR_PU = 0xFF;
    P2_MOD_OC = 0xFF;
    P2_DIR_PU = 0xFF;
    P3_MOD_OC = 0xFF;
    P3_DIR_PU = 0xFF;
    P4_MOD_OC = 0xFF;
    P4_DIR_PU = 0xFF;
    P0 = P1 = P2 = P3 = P4 = 0xFF;
    P7 = (P7 & 0xF0) | 0x03; // P7.0/P7.1 input with pull-up
}

static void drive(uint8_t n, uint8_t low)
{
    uint8_t mask = 1 << pin_bit[n];
    switch (pin_port[n]) {
    case 0: P0 = low ? P0 & ~mask : P0 | mask; break;
    case 1: P1 = low ? P1 & ~mask : P1 | mask; break;
    case 2: P2 = low ? P2 & ~mask : P2 | mask; break;
    case 3: P3 = low ? P3 & ~mask : P3 | mask; break;
    case 4: P4 = low ? P4 & ~mask : P4 | mask; break;
    default:
        // P7: bits 3:2 direction, 1:0 output / pull-up
        if (low)
            P7 = (P7 & ~mask) | (mask << 2);
        else
            P7 = (P7 & ~(mask << 2)) | mask;
        break;
    }
}

static void settle(void)
{
    volatile uint8_t k;
    for (k = 0; k < 60; k++)
        ;
}

// Drive pin n low briefly and capture all inputs.
static void probe(uint8_t n, ports_t *s)
{
    drive(n, 1);
    settle();
    read_ports(s);
    drive(n, 0);
    settle();
}

static void take_baseline(void)
{
    ports_t idle, s;
    uint8_t n, i, round;

    settle();
    read_ports(&idle);
    for (n = 0; n < NPINS; n++) {
        // a pin already low at rest is strapped or driven externally: never drive it
        drivable[n] = (idle.p[port_index(pin_port[n])] >> pin_bit[n]) & 1;
        for (i = 0; i < 6; i++)
            baseline[n].p[i] = 0;
    }
    for (round = 0; round < 8; round++) {
        for (n = 0; n < NPINS; n++) {
            if (!drivable[n])
                continue;
            probe(n, &s);
            for (i = 0; i < 6; i++)
                baseline[n].p[i] |= ~s.p[i];
        }
    }
}

#define MAX_PAIRS 6
static uint8_t pair_drive[MAX_PAIRS];
static uint8_t pair_read[MAX_PAIRS];

// One full scan; returns the number of pin pairs newly connected vs baseline.
static uint8_t scan(void)
{
    ports_t s;
    uint8_t n, m, idx, low, found = 0;

    for (n = 0; n < NPINS; n++) {
        if (!drivable[n])
            continue;
        probe(n, &s);
        for (m = 0; m < NPINS; m++) {
            if (m == n)
                continue;
            idx = port_index(pin_port[m]);
            low = ((~s.p[idx] & ~baseline[n].p[idx]) >> pin_bit[m]) & 1;
            if (low && found < MAX_PAIRS) {
                pair_drive[found] = n;
                pair_read[found] = m;
                found++;
            }
        }
    }
    return found;
}

static void type_pin(uint8_t n)
{
    type_digit(pin_port[n]);
    type_digit(pin_bit[n]);
}

void main(void)
{
    uint8_t found, last = 0, stable = 0, reported = 0, i;

    clock_24mhz();
    pins_idle();
    take_baseline();
    usb_init();
    EA = 1;

    for (;;) {
        found = scan();
        // require 3 identical scans before reporting (debounce)
        if (found == last) {
            if (stable < 3)
                stable++;
        } else {
            stable = 0;
            last = found;
        }
        if (stable == 3 && found && !reported) {
            for (i = 0; i < found; i++) {
                type_pin(pair_drive[i]);
                type_pin(pair_read[i]);
                type_key(KC_SPACE);
            }
            type_key(KC_ENTER);
            reported = 1;
        }
        if (stable == 3 && !found)
            reported = 0;
    }
}
