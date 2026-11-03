// Minimal USB boot keyboard firmware for the Rampage Radiant K11
// (LZ901 = CH555 clone). pinfinder.c is the matrix-mapping variant.
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
    7, 0x05, 0x81, 0x03, KBD_EP_SIZE, 0, 1,         // EP1 IN, interrupt, 1 ms
};

static __code const uint8_t string0[] = {4, DESC_STRING, 0x09, 0x04};
static __code const uint8_t string1[] = {16, DESC_STRING, 'R', 0, 'a', 0, 'm', 0, 'p', 0, 'a', 0, 'g', 0, 'e', 0};
static __code const uint8_t string2[] = {24, DESC_STRING, 'R', 0, 'a', 0, 'd', 0, 'i', 0, 'a', 0, 'n', 0, 't', 0,
                                         ' ', 0, 'K', 0, '1', 0, '1', 0};

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
// Keyboard matrix (mapped with pinfinder.c)
//
// 14 columns are driven low one at a time: P0.0-P0.7, P3.0-P3.5.
// 5 rows are read back: P4.1-P4.5 (low = key pressed, diodes on every key).

#define NCOLS 14
#define NROWS 5
#define ROW_SHIFT 1 // rows start at P4.1

// Keycodes (USB HID usage page 0x07)
#define ____ 0x00
#define KC_A 0x04
#define KC_B 0x05
#define KC_C 0x06
#define KC_D 0x07
#define KC_E 0x08
#define KC_F 0x09
#define KC_G 0x0A
#define KC_H 0x0B
#define KC_I 0x0C
#define KC_J 0x0D
#define KC_K 0x0E
#define KC_L 0x0F
#define KC_M 0x10
#define KC_N 0x11
#define KC_O 0x12
#define KC_P 0x13
#define KC_Q 0x14
#define KC_R 0x15
#define KC_S 0x16
#define KC_T 0x17
#define KC_U 0x18
#define KC_V 0x19
#define KC_W 0x1A
#define KC_X 0x1B
#define KC_Y 0x1C
#define KC_Z 0x1D
#define KC_1 0x1E
#define KC_2 0x1F
#define KC_3 0x20
#define KC_4 0x21
#define KC_5 0x22
#define KC_6 0x23
#define KC_7 0x24
#define KC_8 0x25
#define KC_9 0x26
#define KC_0 0x27
#define KC_ENT 0x28
#define KC_ESC 0x29
#define KC_BSPC 0x2A
#define KC_TAB 0x2B
#define KC_SPC 0x2C
#define KC_MINS 0x2D
#define KC_EQL 0x2E
#define KC_LBRC 0x2F
#define KC_RBRC 0x30
#define KC_BSLS 0x31
#define KC_SCLN 0x33
#define KC_QUOT 0x34
#define KC_GRV 0x35
#define KC_COMM 0x36
#define KC_DOT 0x37
#define KC_SLSH 0x38
#define KC_CAPS 0x39
#define KC_F1 0x3A
#define KC_F2 0x3B
#define KC_F3 0x3C
#define KC_F4 0x3D
#define KC_F5 0x3E
#define KC_F6 0x3F
#define KC_F7 0x40
#define KC_F8 0x41
#define KC_F9 0x42
#define KC_F10 0x43
#define KC_F11 0x44
#define KC_F12 0x45
#define KC_PSCR 0x46
#define KC_DEL 0x4C
#define KC_RGHT 0x4F
#define KC_LEFT 0x50
#define KC_DOWN 0x51
#define KC_UP 0x52
#define KC_APP 0x65
#define KC_LCTL 0xE0
#define KC_LSFT 0xE1
#define KC_LALT 0xE2
#define KC_LGUI 0xE3
#define KC_RCTL 0xE4
#define KC_RSFT 0xE5
#define KC_RALT 0xE6
#define KC_FN 0xFF  // layer key, never sent
#define KC_RGB 0xFE // hold + 1..9 sets LED brightness, + 0 turns LEDs off

static __code const uint8_t keymap[2][NROWS][NCOLS] = {
    // Base layer
    {
        {KC_ESC,  KC_1,    KC_2,    KC_3,   KC_4,    KC_5,  KC_6,   KC_7,    KC_8,    KC_9,   KC_0,    KC_MINS, KC_EQL,  KC_BSPC},
        {KC_TAB,  KC_Q,    KC_W,    KC_E,   KC_R,    KC_T,  KC_Y,   KC_U,    KC_I,    KC_O,   KC_P,    KC_LBRC, KC_RBRC, ____},
        {KC_CAPS, KC_A,    KC_S,    KC_D,   KC_F,    KC_G,  KC_H,   KC_J,    KC_K,    KC_L,   KC_SCLN, KC_QUOT, KC_ENT,  KC_BSLS},
        {KC_LSFT, KC_Z,    KC_X,    KC_C,   KC_V,    KC_B,  KC_N,   KC_M,    KC_COMM, KC_DOT, KC_SLSH, KC_RSFT, ____,    ____},
        {KC_LCTL, KC_LGUI, KC_LALT, KC_SPC, KC_RALT, KC_FN, KC_RGB, KC_RCTL, ____,    ____,   ____,    ____,    ____,    ____},
    },
    // Fn layer (____ falls through to the base layer)
    {
        {KC_GRV,  KC_F1,   KC_F2,   KC_F3,  KC_F4,   KC_F5, KC_F6,  KC_F7,   KC_F8,   KC_F9,   KC_F10,  KC_F11,  KC_F12,  KC_DEL},
        {____,    ____,    ____,    ____,   ____,    ____,  ____,   ____,    KC_UP,   ____,    KC_PSCR, ____,    ____,    ____},
        {____,    ____,    ____,    ____,   ____,    ____,  ____,   KC_LEFT, KC_DOWN, KC_RGHT, ____,    ____,    ____,    ____},
        {____,    ____,    ____,    ____,   ____,    ____,  ____,   ____,    ____,    ____,    ____,    ____,    ____,    ____},
        {____,    ____,    ____,    ____,   ____,    ____,  ____,   ____,    ____,    ____,    ____,    ____,    ____,    ____},
    },
};

static void matrix_init(void)
{
    // Columns (P0, P3.0-P3.5) and rows (P4.1-P4.5): open-drain with pull-up,
    // released high. Other pins keep their reset state.
    P4_LED_KEY = 0;
    P0_MOD_OC = 0xFF;
    P0_DIR_PU = 0xFF;
    P0 = 0xFF;
    P3_MOD_OC |= 0x3F;
    P3_DIR_PU |= 0x3F;
    P3 |= 0x3F;
    P4_MOD_OC |= 0x3E;
    P4_DIR_PU |= 0x3E;
    P4 |= 0x3E;
    // green (P2.1-P2.5) and blue (P1.1-P1.5) LED rows: open-drain, released
    P2_MOD_OC |= 0x3E;
    P2_DIR_PU |= 0x3E;
    P2 |= 0x3E;
    P1_MOD_OC |= 0x3E;
    P1_DIR_PU |= 0x3E;
    P1 |= 0x3E;
}

// Column c low: value for P0 and P3 (P3.6/P3.7 stay high).
static __code const uint8_t col_p0[NCOLS] = {0xFE, 0xFD, 0xFB, 0xF7, 0xEF, 0xDF, 0xBF, 0x7F,
                                            0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static __code const uint8_t col_p3[NCOLS] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                            0xFE, 0xFD, 0xFB, 0xF7, 0xEF, 0xDF};

// ---------------------------------------------------------------------------
// RGB LEDs
//
// The LEDs share the matrix: a column's line low powers that column's LEDs
// (through Q1-Q15), and each row has a red, green and blue cathode line:
// P4.1-P4.5 (red), P2.1-P2.5 (green), P1.1-P1.5 (blue). P4 in LED mode +
// push-pull sinks a current-limited amount; P2/P1 have no on-chip limit and
// rely on the board's own resistors.
//
// LED current is switched once per column slot (~40 us, ~24 kHz) so the
// board's inductor doesn't sing in the audible range.

#define LED_SLOT 72 // Timer0 ticks (0.5 us) per column; on-time is a share of this
#define LED_ROWS 0x3E

#define LED_RED 0x01
#define LED_GREEN 0x02
#define LED_BLUE 0x04

static __code const uint8_t led_on_ticks[10] = {0, 1, 2, 3, 5, 8, 13, 22, 36, 60};
static uint8_t led_level = 3;
static uint8_t led_colors = LED_RED;

static void timer_init(void)
{
    ET0 = 0;
    T2MOD &= ~bT0_CLK;              // Fsys/12 = 2 MHz
    TMOD = (TMOD & 0xF0) | 0x01;    // timer0 16-bit
}

static void timer_wait(uint8_t ticks)
{
    if (!ticks)
        return;
    TR0 = 0;
    TH0 = 0xFF;
    TL0 = (uint8_t)(0 - ticks);
    TF0 = 0;
    TR0 = 1;
    while (!TF0)
        ;
    TR0 = 0;
}

static void led_rows_on(void)
{
    if (led_colors & LED_RED) {
        P4_LED_KEY |= LED_ROWS; // current-limited sink first
        P4 &= ~LED_ROWS;
        P4_MOD_OC &= ~LED_ROWS; // push-pull
    }
    if (led_colors & LED_GREEN)
        P2 &= ~LED_ROWS;
    if (led_colors & LED_BLUE)
        P1 &= ~LED_ROWS;
}

static void led_rows_off(void)
{
    P4_MOD_OC |= LED_ROWS; // back to open-drain before releasing high
    P4 |= LED_ROWS;
    P4_LED_KEY &= ~LED_ROWS;
    P2 |= LED_ROWS;
    P1 |= LED_ROWS;
}

// bit r set = key (r, c) pressed
static uint8_t raw[NCOLS], prev_raw[NCOLS], debounced[NCOLS];

// One LED frame over all columns; with read_keys, also samples the matrix.
// A key read holds its column low for only a few instructions: the row drops
// through the key's diode at once, before the column's LED transistor turns
// on, so pressed keys don't flash. During the LED phase all rows are already
// low, so a pressed key adds no current there either.
static void matrix_scan(uint8_t read_keys)
{
    uint8_t c, rows, on = led_colors ? led_on_ticks[led_level] : 0;
    for (c = 0; c < NCOLS; c++) {
        if (read_keys) {
            P0 = col_p0[c];
            P3 = col_p3[c];
            __asm
                nop
                nop
                nop
                nop
                nop
                nop
            __endasm; // ~0.25 us for the port input to follow
            rows = P4;
            P0 = 0xFF;
            P3 = 0xFF;
            raw[c] = (~rows >> ROW_SHIFT) & ((1 << NROWS) - 1);
        }
        if (on) {
            P0 = col_p0[c];
            P3 = col_p3[c];
            led_rows_on();
            timer_wait(on);
            led_rows_off();
            P0 = 0xFF;
            P3 = 0xFF;
        }
        timer_wait(LED_SLOT - on); // also lets the rows recover before the next column
    }
}

// ---------------------------------------------------------------------------
// Keyboard reports

static uint8_t report[KBD_EP_SIZE], last_report[KBD_EP_SIZE];
static uint8_t rgb_colour_keys; // R/G/B keys held during the last RGB-key scan

static void send_report(void)
{
    uint8_t i;
    if (!usb_config)
        return;
    while (kbd_ep_busy)
        if (!usb_config)
            return;
    for (i = 0; i < KBD_EP_SIZE; i++)
        pUSB_BUF_DEV0[UX_EP1_ADDR + i] = report[i];
    kbd_ep_busy = 1;
    D0_EP1T_L = KBD_EP_SIZE;
    D0_EP1RES = D0_EP1RES & ~MASK_UEP_X_RES | UEP_X_RES_ACK;
}

static void build_report(void)
{
    uint8_t r, c, kc, n = 2, fn = 0, rgb = 0;

    for (r = 0; r < KBD_EP_SIZE; r++)
        report[r] = 0;

    // Fn and RGB first, so the layer is known before keys are resolved
    for (c = 0; c < NCOLS; c++)
        for (r = 0; r < NROWS; r++)
            if (debounced[c] >> r & 1) {
                if (keymap[0][r][c] == KC_FN)
                    fn = 1;
                else if (keymap[0][r][c] == KC_RGB)
                    rgb = 1;
            }

    // RGB held: number row sets brightness, R/G/B toggle colours; nothing is sent
    if (rgb) {
        for (c = 1; c <= 10; c++)
            if (debounced[c] & 1)
                led_level = c == 10 ? 0 : c;
        r = (debounced[4] >> 1 & 1) * LED_RED      // R: row P4.2, col P0.4
            | (debounced[5] >> 2 & 1) * LED_GREEN  // G: row P4.3, col P0.5
            | (debounced[5] >> 3 & 1) * LED_BLUE;  // B: row P4.4, col P0.5
        led_colors ^= r & ~rgb_colour_keys; // toggle on press only
        rgb_colour_keys = r;
        return;
    }
    rgb_colour_keys = 0;

    for (c = 0; c < NCOLS; c++) {
        for (r = 0; r < NROWS; r++) {
            if (!(debounced[c] >> r & 1))
                continue;
            kc = fn ? keymap[1][r][c] : ____;
            if (kc == ____)
                kc = keymap[0][r][c];
            if (kc == ____ || kc == KC_FN || kc == KC_RGB)
                continue;
            if (kc >= KC_LCTL && kc <= 0xE7)
                report[0] |= 1 << (kc - KC_LCTL);
            else if (n < KBD_EP_SIZE)
                report[n++] = kc;
        }
    }
}

#define DEBOUNCE_READS 4 // key reads (every KEY_READ_FRAMES LED frames)
#define KEY_READ_FRAMES 4

void main(void)
{
    uint8_t c, i, frame = 0, stable = 0, changed;

    clock_24mhz();
    timer_init();
    matrix_init();
    usb_init();
    EA = 1;

    for (;;) {
        if (++frame < KEY_READ_FRAMES) {
            matrix_scan(0);
            continue;
        }
        frame = 0;
        matrix_scan(1);

        // accept a new matrix state once it reads the same DEBOUNCE_READS times
        changed = 0;
        for (c = 0; c < NCOLS; c++)
            if (raw[c] != prev_raw[c])
                changed = 1;
        for (c = 0; c < NCOLS; c++)
            prev_raw[c] = raw[c];
        if (changed)
            stable = 0;
        else if (stable < DEBOUNCE_READS && ++stable == DEBOUNCE_READS)
            for (c = 0; c < NCOLS; c++)
                debounced[c] = raw[c];

        build_report();
        changed = 0;
        for (i = 0; i < KBD_EP_SIZE; i++)
            if (report[i] != last_report[i])
                changed = 1;
        // never wait for the host here: that would stall the LED scan
        if (changed && usb_config && !kbd_ep_busy) {
            send_report();
            for (i = 0; i < KBD_EP_SIZE; i++)
                last_report[i] = report[i];
        }
    }
}
