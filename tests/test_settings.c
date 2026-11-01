/*
 * The settings file, and the invariants it cannot be trusted to hold.
 *
 * The menu keeps interfaces that cannot coexist apart by turning one
 * off when the other goes on. A card does not: it carries whatever was
 * written to it, by an older firmware or by hand. So the load path has
 * to resolve those pairs before they reach the port handlers, where
 * the winner would be decided by the order of the ifs.
 */
#include <stdio.h>
#include <string.h>

#include "settings.h"
#include "spectrum.h"
#include "tap.h"

static void write_cfg(const char *body) {
    FILE *f = fopen("frank-dino.cfg", "wb");   /* the shim drops the leading / */
    fputs(body, f);
    fclose(f);
}

int main(void) {
    GROUP("defaults");
    settings_defaults();
    EQ(settings.betadisk && settings.esxdos, 0, "the defaults do not fight");

    GROUP("round trip");
    write_cfg("# a comment\nmachine 3\nay_vol 7\n  saa 1\n");
    CHECK(settings_load(), "a config file loads");
    /* key and value are separated by whitespace, not by an = */
    EQ(settings.machine, 3, "machine came back");
    EQ(settings.ay_vol, 7, "ay_vol came back");
    EQ(settings.saa, 1, "saa came back");

    GROUP("invariants");
    /* The pair that matters: #E3/#E7/#EB all alias the Beta Disk's
     * system register, and both interfaces page in on a fetch in
     * #3D00-#3DFF. */
    write_cfg("betadisk 1\nesxdos 1\n");
    CHECK(settings_load(), "a config naming both interfaces loads");
    CHECK(!(settings.betadisk && settings.esxdos),
          "but only one of the Beta Disk and DivMMC survives it");
    EQ(settings.betadisk, 1, "and it is the one named first");

    settings_defaults();
    settings.betadisk = 1; settings.esxdos = 1;
    settings_validate();
    CHECK(!(settings.betadisk && settings.esxdos), "validate resolves it directly");

    GROUP("junk");
    write_cfg("machine 200\nnonsense\n\nay_vol\nno_such_key 4\n");
    CHECK(settings_load(), "a damaged config still loads");
    CHECK(settings.machine < MACHINE_COUNT, "and leaves a machine that exists (%u)",
          settings.machine);

    remove("frank-dino.cfg");
    return tap_done("settings");
}
