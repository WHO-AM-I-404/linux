// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Minimalistic braille device kernel support.
 *
 * By default, shows console messages on the braille device.
 * Pressing Insert switches to VC browsing.
 *
 * Copyright (C) Samuel Thibault <samuel.thibault@ens-lyon.org>
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/console.h>
#include <linux/notifier.h>
#include <linux/selection.h>
#include <linux/vt_kern.h>
#include <linux/consolemap.h>
#include <linux/keyboard.h>
#include <linux/kbd_kern.h>
#include <linux/input.h>

MODULE_AUTHOR("samuel.thibault@ens-lyon.org");
MODULE_DESCRIPTION("braille device");

/* --- Constants --- */
static const int WIDTH = 40;
static const int BRAILLE_KEY = KEY_INSERT;

enum braille_ctrl {
    SOH = 1,
    STX = 2,
    ETX = 3,
    EOT = 4,
    ENQ = 5,
};

#define BEEP_HIGH 880
#define BEEP_MED 440
#define BEEP_LOW 220

/* --- Module Parameters --- */
static bool sound;
module_param(sound, bool, 0);
MODULE_PARM_DESC(sound, "emit sounds");

/* --- Global State --- */
static u16 console_buf[WIDTH];
static int console_cursor;
static int vc_view_x, vc_view_y, lastvc_x, lastvc_y;
static int console_show = 1;
static int console_newline = 1;
static int lastVC = -1;
static struct console *braille_co;

/* --- Helper Functions --- */
static void beep(unsigned int freq)
{
    if (sound)
        kd_mksound(freq, HZ/10);
}

static void refresh_vc(struct vc_data *vc)
{
    vc_maybe_cursor_moved(vc);
    vc_refresh(vc);
}

static void vc_follow_cursor(struct vc_data *vc)
{
    vc_view_x = vc->state.x - (vc->state.x % WIDTH);
    vc_view_y = vc->state.y;
    lastvc_x = vc->state.x;
    lastvc_y = vc->state.y;
}

/* --- Braille Write --- */
static void braille_write(u16 *buf)
{
    static u16 last_write_buf[WIDTH];
    unsigned char data[1 + 1 + 2*WIDTH + 2 + 1], csum = 0, *data_ptr;
    u16 char_val;
    int i;

    if (!braille_co)
        return;

    if (!memcmp(last_write_buf, buf, WIDTH * sizeof(*buf)))
        return;

    memcpy(last_write_buf, buf, WIDTH * sizeof(*buf));

    data[0] = STX;
    data[1] = '>';
    csum ^= '>';
    data_ptr = &data[2];

    for (i = 0; i < WIDTH; i++) {
        char_val = buf[i];
        if (char_val >= 0x100)
            char_val = '?';
        else if (char_val == 0x00)
            char_val = ' ';
        csum ^= char_val;

        if (char_val <= 0x05) {
            *data_ptr++ = SOH;
            char_val |= 0x40;
        }
        *data_ptr++ = char_val;
    }

    if (csum <= 0x05) {
        *data_ptr++ = SOH;
        csum |= 0x40;
    }
    *data_ptr++ = csum;
    *data_ptr++ = ETX;

    braille_co->write(braille_co, data, data_ptr - data);
}

/* --- Keyboard Handling --- */
static int handle_console_key(struct keyboard_notifier_param *param, struct vc_data *vc)
{
    int ret = NOTIFY_STOP;

    if (param->value == BRAILLE_KEY) {
        console_show = 0;
        beep(BEEP_HIGH);
        vc_follow_cursor(vc);
        refresh_vc(vc);
    } else {
        /* other console key handling can go here */
        ret = NOTIFY_OK;
    }

    return ret;
}

static int handle_vc_key(struct keyboard_notifier_param *param, struct vc_data *vc)
{
    int ret = NOTIFY_STOP;

    switch (param->value) {
    case KEY_INSERT:
        beep(BEEP_MED);
        console_show = 1;
        lastVC = -1;
        braille_write(console_buf);
        break;
    case KEY_LEFT:
        if (vc_view_x > 0) {
            vc_view_x -= WIDTH;
            if (vc_view_x < 0)
                vc_view_x = 0;
        } else if (vc_view_y >= 1) {
            beep(BEEP_HIGH);
            vc_view_y--;
            vc_view_x = vc->vc_cols - WIDTH;
        } else
            beep(BEEP_LOW);
        break;
    case KEY_RIGHT:
        if (vc_view_x + WIDTH < vc->vc_cols) {
            vc_view_x += WIDTH;
        } else if (vc_view_y + 1 < vc->vc_rows) {
            beep(BEEP_HIGH);
            vc_view_y++;
            vc_view_x = 0;
        } else
            beep(BEEP_LOW);
        break;
    case KEY_DOWN:
        if (vc_view_y + 1 < vc->vc_rows)
            vc_view_y++;
        else
            beep(BEEP_LOW);
        break;
    case KEY_UP:
        if (vc_view_y >= 1)
            vc_view_y--;
        else
            beep(BEEP_LOW);
        break;
    case KEY_HOME:
        vc_follow_cursor(vc);
        break;
    case KEY_PAGEUP:
        vc_view_x = 0;
        vc_view_y = 0;
        break;
    case KEY_PAGEDOWN:
        vc_view_x = 0;
        vc_view_y = vc->vc_rows - 1;
        break;
    default:
        ret = NOTIFY_OK;
        break;
    }

    refresh_vc(vc);
    return ret;
}

/* --- Main Keyboard Notifier --- */
static int keyboard_notifier_call(struct notifier_block *blk,
                                  unsigned long code, void *_param)
{
    struct keyboard_notifier_param *param = _param;
    struct vc_data *vc = param->vc;
    int ret = NOTIFY_OK;

    if (!param->down)
        return ret;

    switch (code) {
    case KBD_KEYCODE:
        if (console_show)
            ret = handle_console_key(param, vc);
        else
            ret = handle_vc_key(param, vc);
        break;
    case KBD_POST_KEYSYM:
        {
            unsigned char type = KTYP(param->value) - 0xf0;
            if (type == KT_SPEC) {
                unsigned char val = KVAL(param->value);
                int on_off = -1;
                switch (val) {
                case KVAL(K_CAPS): on_off = vt_get_leds(fg_console, VC_CAPSLOCK); break;
                case KVAL(K_NUM):  on_off = vt_get_leds(fg_console, VC_NUMLOCK); break;
                case KVAL(K_HOLD): on_off = vt_get_leds(fg_console, VC_SCROLLOCK); break;
                }
                if (on_off == 1) beep(BEEP_HIGH);
                else if (on_off == 0) beep(BEEP_MED);
            }
        }
        break;
    case KBD_UNBOUND_KEYCODE:
    case KBD_UNICODE:
    case KBD_KEYSYM:
        /* Unused */
        break;
    }

    return ret;
}

static struct notifier_block keyboard_notifier_block = {
    .notifier_call = keyboard_notifier_call,
};

/* --- VT Notifier --- */
static int vt_notifier_call(struct notifier_block *blk,
                            unsigned long code, void *_param)
{
    struct vt_notifier_param *param = _param;
    struct vc_data *vc = param->vc;

    switch (code) {
    case VT_WRITE:
        {
            unsigned char c = param->c;
            if (vc->vc_num != fg_console)
                break;

            switch (c) {
            case '\b':
            case 127:
                if (console_cursor > 0) {
                    console_cursor--;
                    console_buf[console_cursor] = ' ';
                }
                break;
            case '\n': case '\v': case '\f': case '\r':
                console_newline = 1;
                break;
            case '\t': c = ' '; fallthrough;
            default:
                if (c < 32) break;
                if (console_newline) {
                    memset(console_buf, 0, sizeof(console_buf));
                    console_cursor = 0;
                    console_newline = 0;
                }
                if (console_cursor == WIDTH)
                    memmove(console_buf, &console_buf[1], (WIDTH-1) * sizeof(*console_buf));
                else
                    console_cursor++;
                console_buf[console_cursor-1] = c;
                break;
            }
            if (console_show) braille_write(console_buf);
            else refresh_vc(vc);
        }
        break;
    case VT_UPDATE:
        if (console_show) {
            if (vc->vc_num != lastVC) {
                lastVC = vc->vc_num;
                memset(console_buf, 0, sizeof(console_buf));
                console_cursor = 0;
                braille_write(console_buf);
            }
        } else refresh_vc(vc);
        break;
    default:
        break;
    }

    return NOTIFY_OK;
}

static struct notifier_block vt_notifier_block = {
    .notifier_call = vt_notifier_call,
};

/* --- Console Registration --- */
int braille_register_console(struct console *console, int index,
                             char *console_options, char *braille_options)
{
    int ret;

    if (!console_options)
        console_options = "57600o8";

    if (braille_co)
        return -ENODEV;

    if (console->setup) {
        ret = console->setup(console, console_options);
        if (ret != 0)
            return ret;
    }

    console->flags |= CON_ENABLED;
    console->index = index;
    braille_co = console;

    register_keyboard_notifier(&keyboard_notifier_block);
    register_vt_notifier(&vt_notifier_block);

    return 1;
}

int braille_unregister_console(struct console *console)
{
    if (braille_co != console)
        return -EINVAL;

    unregister_keyboard_notifier(&keyboard_notifier_block);
    unregister_vt_notifier(&vt_notifier_block);
    braille_co = NULL;
    return 1;
}
