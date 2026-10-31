/*
 * limport.h -- Wi-Fi networks from M5Launcher, into the list Avoidant and
 * the apps it runs share (0045).
 *
 * Apps built for the feckless ABI get their saved networks from here
 * (feckless_net_start(), the "defeatist" namespace), so a network
 * M5Launcher knew has to be entered once, here, not once per app.
 * M5Launcher keeps /config.conf on the card with its passwords encrypted
 * under a key built into its firmware (limport_core.h); Avoidant has
 * taken the factory slot where that firmware was, so the key is looked
 * for in any .bin on the card or the drive instead -- the one M5Launcher
 * was installed from.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

typedef enum {
    LIMPORT_DONE = 0,      /* networks were saved (maybe none new) */
    LIMPORT_NO_CONFIG,     /* no config.conf with a wifi list on any volume */
    LIMPORT_NO_NETWORKS,   /* a config.conf, with nothing to import in it */
    LIMPORT_NO_KEY,        /* networks, and no .bin that holds their key */
    LIMPORT_NO_MEM,
} limport_result_t;

/*
 * Look for /sd/config.conf and /usb/config.conf, find the key in a .bin
 * on the same volume or the other (two folders deep, 64 KB to 8 MB),
 * decrypt and save each network with wifistore_save(). Takes seconds:
 * it reads every .bin that carries M5Launcher's mark. Holds work_lock()
 * and the volumes meanwhile. Never logs a passphrase.
 */
limport_result_t limport_run(int *imported, int *skipped);
