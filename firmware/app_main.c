/**
 * @file app_main.c
 * @brief MegaWifi Audio Subsystem
 *
 * Copyright (c) 2026 Mike Wolak <mikewolak@gmail.com>
 * All rights reserved.
 *
 * Part of the MegaWifi MOD Player project.
 * https://github.com/mikewolak/MegaWifiModPlayer
 */
#include <nvs.h>
#include <nvs_flash.h>
#include "sdkconfig.h"
#include "led.h"
#include "util.h"
#include "megawifi.h"

#if CONFIG_MW_AUDIO_ENABLE
#include "mixer.h"
#include "mod_player.h"

/* Embedded test MOD — linked by CMake EMBED_FILES */
extern const uint8_t test_mod_start[] asm("_binary_test_mod_start");
extern const uint8_t test_mod_end[]   asm("_binary_test_mod_end");
#endif

void app_main(void)
{
	// Initialize NVS.
	esp_err_t err = nvs_flash_init();
	if (err == ESP_ERR_NVS_NO_FREE_PAGES) {
		ESP_ERROR_CHECK(nvs_flash_erase());
		err = nvs_flash_init();
	}
	ESP_ERROR_CHECK( err );

	// UART2 is used for logging.
	LOGI("=== MeGaWiFi firmware version %d.%d-%s ===",
			MW_FW_VERSION_MAJOR, MW_FW_VERSION_MINOR, MW_FW_VARIANT);
	LOGI("            doragasu, 2016 ~ 2024\n");
	// Power the LED on
	led_init();
	led_on();
#if CONFIG_MW_AUDIO_ENABLE
	// Initialize mixer (PWM ISR starts, outputs silence)
	mixer_init();
	// Initialize MOD player: CH6=left, CH7=right
	// Playback starts when Genesis sends MW_CMD_AUD_PLAY
	mod_player_init(6, 7);
#endif
	// Initialize MeGaWiFi system and FSM
	if (MwInit()) {
		abort();
	}
	LOGI("Init done!");
}
