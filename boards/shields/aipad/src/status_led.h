#pragma once

/* status_led has no public API: it is a self-contained module compiled in
 * only when CONFIG_AIPAD_STATUS_LED is set. This header exists so the
 * translation unit has something to include and is never empty. */
