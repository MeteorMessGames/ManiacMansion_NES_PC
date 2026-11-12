/* update_check.h - ask GitHub once at launch whether a newer release exists.
 * Runs in a background thread; nothing is sent except a plain GET to the public
 * releases endpoint with a generic User-Agent. Disable with "UpdateCheck = 0" in config.ini. */
#pragma once
#include <stddef.h>

/* Start the check (no-op when enabled == 0 or already started). current = "1.1.0". */
void update_check_start(const char *current, int enabled);
/* Returns 1 once, with the newer tag (e.g. "v1.2.0") in out, when a newer release was found. */
int  update_check_poll(char *out, size_t n);
