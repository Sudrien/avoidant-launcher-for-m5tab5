/* main/boxns.c on the host (u-avoidant-0004). SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <string.h>

#include "boxns.h"

static int failed;

static void expect(const char *file, const char *want)
{
    char out[BOXNS_LEN + 1];
    memset(out, 'x', sizeof out);
    const bool ok = boxns_for(file, out);
    const bool good = want ? (ok && strcmp(out, want) == 0) : (!ok && out[0] == '\0');
    if (!good) {
        printf("FAIL %s: got %s \"%s\", want %s\n", file ? file : "(null)", ok ? "true" : "false", out,
               want ? want : "none");
        failed++;
    }
}

int main(void)
{
    expect("defeatist_boxed.app.elf", "defeatist");
    expect("defeatist_boxwin.app.elf", "defeatist");
    expect("spurious_reader.app.elf", "spurious");
    expect("Lothesome.app.elf", "lothesome");
    expect("jinxed.app.elf", "jinxed");
    expect("abc123", "abc123");
    expect("fifteen_chars", "fifteen");
    expect("exactly15charsx.app.elf", "exactly15charsx");
    expect("sixteen16charsxx.app.elf", NULL);       /* too long */
    expect("_x.app.elf", NULL);                      /* empty */
    expect(".app.elf", NULL);
    expect("", NULL);
    expect(NULL, NULL);
    expect("my-app.app.elf", NULL);                  /* another character */
    expect("caf\xc3\xa9.app.elf", NULL);
    expect("wifistore.app.elf", NULL);               /* kept */
    expect("WiFiStore.app.elf", NULL);
    expect("duplicitous.app.elf", NULL);
    expect("avoidant_x.app.elf", NULL);
    expect("feckless_https.app.elf", NULL);
    expect("fecklessmail.app.elf", NULL);
    expect("phy.app.elf", NULL);
    expect("nvs.net80211", NULL);
    expect("esp32thing.app.elf", NULL);
    expect("wifi2.app.elf", NULL);
    expect("misc.app.elf", NULL);
    expect("physics.app.elf", NULL);                 /* the prefix's cost, said in boxns.c */
    expect("avoidantx.app.elf", "avoidantx");        /* a name, not a prefix */
    if (failed) {
        printf("%d FAILED\n", failed);
        return 1;
    }
    printf("boxns: all passed\n");
    return 0;
}
