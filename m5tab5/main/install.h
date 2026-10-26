/*
 * install.h -- copy a .bin from the card into app2 and boot it.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

/* The screen and the serial console both reach the card and the flash;
 * whichever is working holds this. */
void work_lock(void);
void work_unlock(void);

/* Called as the copy goes, from the caller's task. */
typedef void (*install_progress_fn)(size_t done, size_t total, void *ctx);

/*
 * Write the app in `path` (on /sd or /usb) to app2. Takes either
 *   - an app image (starts with 0xE9), as `idf.py build` leaves in
 *     build/<project>.bin and Launcher's own .bins are, or
 *   - a merged image from 0x0 (`idf.py merge-bin`, the projects'
 *     _standalone.bin): its partition table at 0x8000 says where its
 *     first app starts, and only that app is copied.
 * Either way the image is checked for the ESP32-P4 and verified after
 * the write (esp_ota_end). On failure a reason is left in err.
 * Takes work_lock() itself.
 */
esp_err_t install_from_file(const char *path, install_progress_fn cb, void *ctx,
                            char *err, size_t err_len);

/* app2 holds a valid image. name (may be NULL) gets the file it came
 * from, or the image's own project name if that was not recorded. */
bool install_present(char *name, size_t name_len);

/* Boot app2 once: mark it to boot, then restart. The reset after that
 * comes back here (sdkconfig.defaults says why). Returns only on error. */
esp_err_t install_boot(void);
