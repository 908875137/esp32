/*
 * events.h
 *
 * Application-level event definitions for ThinkINK EPD.
 * Modules enqueue events here; main.c dispatches them.
 *
 * Reserved for future expansion:
 *   - BUTTON_SHORT_PRESS   → regenerate image
 *   - BUTTON_LONG_PRESS    → enter provisioning
 *   - AI_PIPELINE_DONE     → ready for next cycle
 *   - WIFI_CONNECTED       → on connection established
 *   - WIFI_DISCONNECTED    → on connection lost
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* --- Event IDs --- */
typedef enum {
    EVENT_NONE = 0,
    EVENT_BUTTON_SHORT_PRESS,
    EVENT_BUTTON_LONG_PRESS,
    EVENT_WIFI_CONNECTED,
    EVENT_WIFI_DISCONNECTED,
    EVENT_AI_PIPELINE_DONE,
    EVENT_AI_PIPELINE_ERROR,
    EVENT_CUSTOM,
    EVENT_MAX
} thinkink_event_id_t;

/* --- Event payload --- */
typedef struct {
    thinkink_event_id_t id;
    int                 arg_int;
    void               *arg_ptr;
} thinkink_event_t;

/* --- Public API (to be implemented in events.c later) --- */

#ifdef __cplusplus
}
#endif
