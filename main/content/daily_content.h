/*
 * daily_content.h
 *
 * Content orchestration: image + quote selection, combined rendering, the
 * daily task (AI generation or SD image+quote), button/Web "next" handling,
 * and the WiFi connect helper.  Extracted from main.c so the entry point stays
 * focused on boot + wakeup dispatch + admin backend + provisioning.
 */
#pragma once

#include <stdbool.h>
#include "config_store.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Store the resolved config (mode/font/refresh_hour).  Call after
 * config_store_load() and before any other content_* call.
 */
void content_init(const thinkink_config_t *cfg);

/**
 * Run the daily task: AI generation (once/day) or SD image+quote.
 */
void content_run_daily_task(void);

/**
 * Button short-press / Web next-prev: show a fresh image + quote.
 */
void content_show_next(void);

/**
 * Handle a pending admin request (ADMIN_REQ_*): next/prev/refresh,
 * display-file, and mode switches.
 */
void content_handle_request(int req);

/**
 * Best-effort WiFi connect (also does a one-time SNTP sync after connecting).
 */
bool content_ensure_wifi(void);

#ifdef __cplusplus
}
#endif
