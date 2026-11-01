/*
 * The SMUC card: its 24LC16 driven bit by bit the way ProfROM's driver
 * drives it through #FFBA, and the decode that keeps it off other ports.
 */
#include <stdio.h>
#include <string.h>

#include "smuc.h"
#include "settings.h"
#include "spectrum.h"
#include "tap.h"

extern bool stub_beta_paged;

/* SYS bits: D6 SCL, D4 SDA. */
static void line(const int scl, const int sda) { nvram24_write((uint8_t)((scl ? 0x40 : 0) | (sda ? 0x10 : 0))); }

static void start(void) { line(1, 1); line(1, 0); line(0, 0); }
static void stop(void)  { line(0, 0); line(1, 0); line(1, 1); }

/* A byte out, MSB first; returns whether the chip ACKed (SDA low on the 9th clock). */
static int send(const uint8_t b) {
    for (int i = 7; i >= 0; i--) { const int bit = (b >> i) & 1; line(0, bit); line(1, bit); line(0, bit); }
    line(0, 1); line(1, 1);
    const int ack = !(nvram24_read() & 0x40);
    line(0, 1);
    return ack;
}

static uint8_t recv(const int ack) {
    uint8_t b = 0;
    for (int i = 0; i < 8; i++) {
        line(0, 1); line(1, 1);
        b = (uint8_t)((b << 1) | ((nvram24_read() >> 6) & 1));
    }
    line(0, !ack); line(1, !ack); line(0, !ack);
    return b;
}

int main(void) {
    GROUP("24LC16");
    smuc_init();
    start();
    CHECK(send(0xA4), "the device address with page 2 is ACKed");      // write, page 2
    CHECK(send(0x10), "the word address is ACKed");
    CHECK(send(0x5A), "a data byte is ACKed");
    CHECK(send(0xC3), "and the next");
    stop();
    EQ(nvram24_mem()[0x210], 0x5A, "byte written at page 2, #10");
    EQ(nvram24_mem()[0x211], 0xC3, "and the next one after it");

    start(); send(0xA4); send(0x10);                    // set the address...
    start(); CHECK(send(0xA5), "a read from page 2 is ACKed");          // ...and read
    EQ(recv(1), 0x5A, "reads back the first byte");
    EQ(recv(0), 0xC3, "and the second");
    stop();

    start();
    CHECK(!send(0x50), "a device address that is not #Ax is not answered");
    stop();

    GROUP("decode");
    spec_machine = MACHINE_ZS1024;
    uint8_t v = 0;
    stub_beta_paged = false; spec_port_1ffd = 0;
    CHECK(!smuc_port_in(0x5FBA, &v), "the card is silent outside TR-DOS and SYSEN");
    spec_port_1ffd = 0x02;
    CHECK(smuc_port_in(0x5FBA, &v) && v == 0x57, "#5FBA is the version with SYSEN up");
    CHECK(smuc_port_in(0x5FBE, &v) && v == 0x17, "#5FBE the revision");
    CHECK(smuc_port_in(0xFFBE, &v) && v == 0x00, "the ATA status is 0 with no drive -- not busy");
    CHECK(!smuc_port_in(0x7FFD, &v), "#7FFD is not the card's");
    CHECK(!smuc_port_in(0x5FFA, &v), "A6 set is not the card's");
    spec_machine = MACHINE_128K;
    CHECK(!smuc_port_in(0x5FBA, &v), "a 128K has no SMUC");
    return tap_done("smuc");
}
