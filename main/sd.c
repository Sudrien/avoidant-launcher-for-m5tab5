/*
 * sd.c -- see sd.h. The pins, slot and power are the ones
 * feckless-storage-handler-for-m5tab5 uses; that component also needs the
 * USB host up, which a launcher does not, so the mount is repeated here.
 *
 * SPDX-License-Identifier: MIT
 */
#include "sd.h"

#include "driver/sdmmc_host.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "sdmmc_cmd.h"

static const char *TAG = "sd";

/* Slot 0's IOMUX pins. No card-detect or write-protect line. */
#define SD_CLK  GPIO_NUM_43
#define SD_CMD  GPIO_NUM_44
#define SD_D0   GPIO_NUM_39
#define SD_D1   GPIO_NUM_40
#define SD_D2   GPIO_NUM_41
#define SD_D3   GPIO_NUM_42

/* The slot's IO supply is on-chip LDO channel 4. Without it every card
 * reads as absent. */
#define SD_LDO_CHAN 4

static sd_pwr_ctrl_handle_t s_pwr;
static sdmmc_card_t *s_card;

esp_err_t sd_mount(void)
{
    if (s_card) {
        esp_vfs_fat_sdcard_unmount(SD_MOUNT, s_card);
        s_card = NULL;
    }
    if (!s_pwr) {
        const sd_pwr_ctrl_ldo_config_t ldo = { .ldo_chan_id = SD_LDO_CHAN };
        ESP_RETURN_ON_ERROR(sd_pwr_ctrl_new_on_chip_ldo(&ldo, &s_pwr), TAG, "sd ldo");
    }

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;      /* the default is slot 1 */
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
    host.pwr_ctrl_handle = s_pwr;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;
    slot.clk = SD_CLK;
    slot.cmd = SD_CMD;
    slot.d0 = SD_D0;
    slot.d1 = SD_D1;
    slot.d2 = SD_D2;
    slot.d3 = SD_D3;

    const esp_vfs_fat_sdmmc_mount_config_t mnt = {
        .format_if_mount_failed = false,
        .max_files = 4,
        .allocation_unit_size = 16 * 1024,
    };
    esp_err_t err = esp_vfs_fat_sdmmc_mount(SD_MOUNT, &host, &slot, &mnt, &s_card);
    if (err != ESP_OK) {
        s_card = NULL;
        /* a failed mount can leave the host up with its pins taken; the
         * next try would then fail on the pins rather than the card */
        sdmmc_host_deinit();
        ESP_LOGW(TAG, "no card mounted: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "mounted %s, %llu MB", s_card->cid.name,
             (unsigned long long)s_card->csd.capacity * s_card->csd.sector_size / (1024 * 1024));
    return ESP_OK;
}

bool sd_mounted(void) { return s_card != NULL; }

esp_err_t sd_space(uint64_t *total, uint64_t *free_bytes)
{
    if (!s_card) return ESP_ERR_INVALID_STATE;
    return esp_vfs_fat_info(SD_MOUNT, total, free_bytes);
}
