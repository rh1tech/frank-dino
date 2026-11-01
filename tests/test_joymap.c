/*
 * The pads, decoded from the reports their makers document, and the map
 * that sends a pad's controls to the machine's joystick and keys.
 */
#include <stdio.h>
#include <string.h>

#include "joymap.h"
#include "settings.h"
#include "tap.h"

/* What the machine was told, by the map. */
static uint8_t joy;
static uint8_t keys[8];
void spec_joy_set(uint8_t bit, bool pressed) { if (pressed) joy |= (uint8_t)(1u << bit); else joy &= (uint8_t)~(1u << bit); }
void spec_key_set(uint8_t row, uint8_t bit, bool pressed) { if (pressed) keys[row] |= (uint8_t)(1u << bit); else keys[row] &= (uint8_t)~(1u << bit); }

int main(void) {
    uint16_t p;
    GROUP("generic pad");
    {
        const uint8_t r[8] = { 0x00, 0x7F, 0x7F, 0x7F, 0x7F, 0x20, 0x20, 0x00 };   // left, A, Start
        CHECK(pad_from_generic(r, 8, &p), "decodes");
        EQ(p, PAD_BIT(PAD_LEFT) | PAD_BIT(PAD_A) | PAD_BIT(PAD_START), "left + A + Start");
        CHECK(!pad_from_generic(r, 6, &p), "a short report is not one");
    }
    GROUP("DualShock 4");
    {
        // ID 01; LX LY RX RY; hat 6 (W) + Cross; L1 + Options
        const uint8_t r[10] = { 0x01, 0x80, 0x80, 0x80, 0x80, 0x26, 0x21, 0, 0, 0 };
        CHECK(pad_from_ds4(r, 10, &p), "decodes");
        EQ(p, PAD_BIT(PAD_LEFT) | PAD_BIT(PAD_A) | PAD_BIT(PAD_Z) | PAD_BIT(PAD_START), "W hat + Cross + L1 + Options");
        const uint8_t centred[10] = { 0x01, 0x80, 0x80, 0x80, 0x80, 0x08, 0, 0, 0, 0 };
        CHECK(pad_from_ds4(centred, 10, &p) && p == 0, "hat 8 and a centred stick is nothing -- not LEFT");
        CHECK(!pad_from_ds4(r + 1, 9, &p), "without its report ID it is not one");
    }
    GROUP("DualSense");
    {
        // ID 01; LX LY RX RY L2 R2 counter; hat 1 (NE) + Triangle; Create
        const uint8_t r[11] = { 0x01, 0x80, 0x80, 0x80, 0x80, 0, 0, 0x55, 0x81, 0x10, 0 };
        CHECK(pad_from_ds5(r, 11, &p), "decodes");
        EQ(p, PAD_BIT(PAD_UP) | PAD_BIT(PAD_RIGHT) | PAD_BIT(PAD_Y) | PAD_BIT(PAD_SELECT), "NE + Triangle + Create");
    }
    GROUP("XInput");
    CHECK(pad_from_xinput(0x1000 | 0x0010, 0, 0xFF, 0, 20000, &p), "decodes");
    EQ(p, PAD_BIT(PAD_A) | PAD_BIT(PAD_START) | PAD_BIT(PAD_R2) | PAD_BIT(PAD_UP), "A + Start + RT + stick up");
    CHECK(pad_from_xinput(0, 0, 0, -9000, 0, &p) && p == PAD_BIT(PAD_LEFT), "the stick a quarter over is LEFT");
    EQ(pad_kind(0x054C, 0x09CC), PADKIND_DS4, "054C:09CC is a DualShock 4");
    EQ(pad_kind(0x054C, 0x0CE6), PADKIND_DS5, "054C:0CE6 is a DualSense");
    EQ(pad_kind(0x0079, 0x0011), PADKIND_GENERIC, "a DragonRise is generic");

    GROUP("the map");
    memset(&settings, 0, sizeof settings);
    joymap_apply(PAD_BIT(PAD_LEFT) | PAD_BIT(PAD_B));
    EQ(joy, (1u << JOY_BIT_LEFT) | (1u << JOY_BIT_FIRE), "by default left is left and B is fire");
    joymap_apply(0);
    EQ(joy, 0, "and both let go");
    settings.joymap[PAD_X] = JT_KEY + 7 * 5 + 0;          // X -> Space
    joymap_apply(PAD_BIT(PAD_X));
    EQ(keys[7], 0x01, "X mapped to Space presses Space");
    EQ(joy, 0, "and not fire 2");
    joymap_apply(0);
    EQ(keys[7], 0x00, "and lets it go");
    joymap_apply(PAD_BIT(PAD_START));
    EQ(joy, 0, "Start is the menu's, never the machine's");
    settings.joymap[PAD_A] = JT_NONE;
    joymap_apply(PAD_BIT(PAD_A) | PAD_BIT(PAD_B));
    EQ(joy, 1u << JOY_BIT_FIRE, "A set to nothing; B still fires");
    joymap_release();
    EQ(joy, 0, "release lets go of everything");
    return tap_done("joymap");
}
