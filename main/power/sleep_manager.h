/*
 * sleep_manager.h
 *
 * Low-power sleep orchestration.
 *
 * The device spends most of its time in light sleep, waking on:
 *   - RTC timer        (daily refresh at config.json's refresh_hour)
 *   - GPIO48 button    (opens the admin backend for 15 min)
 *
 * GPIO48 is NOT an RTC GPIO (ESP32-S3 RTC GPIOs are 0..21), so deep sleep's
 * EXT0/EXT1 wakeup can't use it.  Light sleep's `gpio_wakeup_enable` works on
 * any GPIO, which is why we use light sleep here (~130µA + regulator 60µA).
 *
 * On wake we call esp_restart() so the app boots clean — light sleep leaves
 * SPI/FATFS/FreeType state in RAM whose backing peripherals are powered off,
 * so a clean boot avoids re-initialising all of that by hand.
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SLEEP_BUTTON_GPIO      48
#define ADMIN_TIMEOUT_SEC      (15 * 60)

/* Wakeup source, preserved across esp_restart() via RTC memory (esp_restart
   loses the normal esp_sleep wakeup cause, so we stash it before rebooting). */
typedef enum {
    SLEEP_WAKE_NONE = 0,   /* power-on / reset — not a light-sleep wakeup */
    SLEEP_WAKE_TIMER,      /* RTC timer fired (daily refresh) */
    SLEEP_WAKE_GPIO,       /* GPIO48 button pressed */
} sleep_wake_cause_t;

/*
 * Return the wakeup source for this boot (see sleep_wake_cause_t).  Call in
 * app_main to decide the daily-task vs admin-backend dispatch.
 */
sleep_wake_cause_t sleep_manager_get_wake_cause(void);   /* admin backend auto-off */

/*
 * Configure both wakeup sources and enter light sleep.  This function does
 * NOT return on a timer/GPIO wake — it calls esp_restart() to reboot clean.
 *
 *   refresh_hour — hour of day (0-23) for the daily wake.  If the RTC has no
 *                  valid time base (never synced), we fall back to a fixed
 *                  24h timer.
 */
esp_err_t sleep_manager_enter(int refresh_hour);

/*
 * Compute microseconds until the next occurrence of `hour` (0-23), assuming
 * the current time is `now` (unix seconds).  Returns the sleep duration in µs.
 */
uint64_t sleep_manager_us_until_next_hour(uint32_t now, int hour);

#ifdef __cplusplus
}
#endif
