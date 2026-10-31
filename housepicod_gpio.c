/* housepicod - A simple home web server for world control through Pico GPIO
 *
 * Copyright 2026, Pascal Martin
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301, USA.
 *
 *
 * housepicod_gpio.c - Access GPIO pins on attached RPi Pico devices
 *
 * SYNOPSYS:
 *
 * const char *housepicod_gpio_initialize (int argc, const char **argv);
 *
 *    Retrieve the configuration and initialize access to the picod devices.
 *    Return 0 on success, and error message otherwise.
 *
 * const char *housepicod_gpio_refresh (void);
 *
 *    Re-evaluate the GPIO setup after the configuration changed.
 *
 * int housepicod_gpio_search (const char *name);
 *
 *    Search for the index of the named point. Returns -1 if not found.
 *
 * int housepicod_gpio_count (void);
 *
 *    Return the number of configured GPIO points available.
 *
 * int housepicod_gpio_set (int point, int state,
 *                           long long pulse, const char *cause);
 *
 *    Set the specified point to the on (1) or off (0) state for the pulse
 *    length specified (in milliseconds). If pulse is 0, the request is to
 *    maintain the GPIO active until a new state is applied. The cause
 *    parameter, if not null, is reproduced in the event.
 *
 *    The GPIO is never maintained active for longer than the pulse limit
 *    specified int the configuration, if any.
 *
 *    Return 1 on success, 0 if the point is not known and -1 on error.
 *
 * void housepicod_gpio_status (ParserContext context, int root);
 *
 *    Populate the context with the list of known points and their values.
 *
 * void housepicod_gpio_fast (int period);
 *
 *    Enable fast scanning for a few seconds. The period is in millisecond
 *    and must be in the 10 to 999 range. Otherwise a period of 0 means
 *    keep using the existing sampling period.
 *
 *    This should be called periodically to maintain fast scanning active.
 *    This is typically called when the client asks for the change history.
 *
 * void housepicod_gpio_changes (long long since,
 *                                ParserContext context, int root);
 *
 *    Populate the context with an history of the input changes that occurred
 *    after the provided millisecond timestamp. Point that have not changed
 *    are ommited. If since is 0, return the complete recent history.
 *
 * void housepicod_gpio_periodic (void);
 *
 *    This function must be called every second. It ends the expired pulses.
 *
 * int housepicod_gpio_same (void);
 *
 *    Return true if something changed compare to the known state.
 *    See the HousePortal's housestate.c module for how this works.
 *
 * int housepicod_gpio_current (void);
 *
 *    Return an identifier of the current state of the GPIO.
 *    See the HousePortal's housestate.c module for how this works.
 */

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <sys/time.h>
#include <errno.h>

#include "echttp.h"
#include "echttp_libc.h"
#include "echttp_hash.h"
#include "echttp_json.h"

#include "houselog.h"
#include "housestate.h"
#include "houseconfig.h"
#include "housecapture.h"

#include "picod.h"
#include "picod_gpio.h"

#include "housepicod.h"
#include "housepicod_gpio.h"
#include "housepicod_memory.h"

#define DEBUG if (echttp_isdebug()) printf

#define HOUSE_GPIO_MODE_INPUT  1
#define HOUSE_GPIO_MODE_OUTPUT 2

// Keep about 6 seconds worth of history, to allow processing periodic requests
// up to 5 seconds aparts with some margin.
//
#define HOUSE_GPIO_PERIOD_DEFAULT 100  // Milliseconds.
#define HOUSE_GPIO_PERIOD_MIN     10   // Milliseconds.
#define HOUSE_GPIO_SCAN_TIMEOUT   15   // Seconds.

#define PICOUIDSIZE 8
#define PICOMAXGPIO 30

#define DEBOUNCE_USEC 2000

struct PicoMap {
    uint8_t uid[PICOUIDSIZE];
    int uidsignature; // Accelerates search.
    int index;        // Self reference.

    char inuse;       // Referenced in the configuration.
    char detected;    // Currently attached to this computer.
    char configured;  // The GPIO have already been set on the Pico

    int points; // First point in the list.

    // The following items make the precalculated Pico configuration.
    int pincount; // Number of pins
    int pins[PICOMAXGPIO]; // The list of GPIO pin numbers used.
    int dir[PICOMAXGPIO];
    int level[PICOMAXGPIO];
};

#define PICODEVICESMAX 128    // A realistic maximum number of USB devices
static struct PicoMap *PicoDevices[PICODEVICESMAX] = {0};
static int             PicoDevicesCount = 0;

struct GpioMap {
    const char *name;
    const char *gear;
    const char *desc;
    unsigned int signature; // Accelerates search.
    uint8_t uid[PICOUIDSIZE];
    unsigned int uidsignature; // Accelerates search.
    int pico;  // Reference to parent Pico device (per UID)
    int next;

    int mode;
    int gpio;
    int on;
    int pulse;  // Maximum pulse time supported by the gear, in milliseconds.
    int failed; // Failure already detected, avoid logging the same error.
    int state;
    int commanded;
    long long deadline; // in milliseconds

    int history; // Index of this input point in the history.
};

static struct GpioMap *Gpios = 0;
static int GpioCount = 0;

static int *InputIndex = 0;
static int InputCount = 0;

static time_t    GpioFastScanEnd = 0; // Fastscan is on a timer.
static int       GpioHasActivePulse = 0;

static int LiveGpioState = -1;
static int PicodCapture = -1;

static long long housepicod_gpio_timestamp (void) {

    struct timeval now;
    gettimeofday (&now, 0);
    return (1000LL * now.tv_sec) + now.tv_usec / 1000;
}

static void housepicod_gpio_tap (const char *action, const char *device,
                                 const uint8_t *data, int length) {

    if (!housecapture_active (PicodCapture)) return;

    char buffer[1024];
    int cursor = 0;
    while (length-- > 0) {
        cursor += snprintf (buffer+cursor, sizeof(buffer)-cursor,
                            "%02x", *data++);
    }
    const char *sep = strrchr (device, '/');
    if (sep) device = sep + 1;
    housecapture_record (PicodCapture, device, action, "%s", buffer);
}

static struct PicoMap *housepicod_gpio_find_pico (const uint8_t *uid, int *empty) {

    if (empty) *empty = -1;

    struct PicoMap *dev;
    unsigned int signature = picod_signature (uid, PICOUIDSIZE);

    // This could be accelerated with a hash, but:
    // - the echttp hash index uses a string as key, while the UID is binary.
    // - it is not expected that an actual configuration will use more than
    //   a handful of Pico devices.
    int i;
    for (i = 0; i < PicoDevicesCount; ++i) {
        dev = PicoDevices[i];
        if (!dev) {
            if (empty && (*empty < 0)) *empty = i;
            continue;
        }
        if (dev->uidsignature != signature) continue; // Faster
        if (memcmp (dev->uid, uid, PICOUIDSIZE)) continue; // Slower
        return dev;
    }
    return 0;
}

static struct PicoMap *housepicod_gpio_add_pico (const uint8_t *uid) {

    int empty;
    struct PicoMap *dev = housepicod_gpio_find_pico (uid, &empty);
    if (dev) {
        dev->inuse = 1;
        return dev; // This was a known Pico device, nothing to update.
    }

    // New Pico device.
    if (empty < 0){
        if (PicoDevicesCount >= PICODEVICESMAX) return 0; // Ignore.
        empty = PicoDevicesCount++; // New empty entry.
    }
    if (! PicoDevices[empty])
        PicoDevices[empty] = malloc (sizeof(struct PicoMap));
    dev = PicoDevices[empty];

    dev->inuse = 1;
    dev->detected = 0;
    dev->configured = 0;
    memcpy (dev->uid, uid, PICOUIDSIZE);
    dev->uidsignature = picod_signature (uid, PICOUIDSIZE);;
    dev->index = empty;
    dev->points = -1;
    dev->pincount = 0;
    return dev;
}

static int housepicod_gpio_pico_present (int index) {

    struct PicoMap *dev = PicoDevices[index];
    if (!dev || !dev->inuse || !dev->detected) return 0;
    return 1;
}

static int housepicod_gpio_store (int point, int state) {

    if (point < 0 || point > GpioCount) return 0;
    if (Gpios[point].state == state) return 0;

    DEBUG ("Point %s has new state %d\n", Gpios[point].name, state);
    Gpios[point].state = state;
    return 1;
}

static void housepicod_gpio_read_response (int index, void *data,
                                           int status, const uint8_t *response) {

    if (status) return; // No data when an error is returned.

    const uint8_t *uid = picod_uid (index);
    struct PicoMap *dev = housepicod_gpio_find_pico (uid, 0);
    if (!dev) return;

    long long timestamp = housepicod_gpio_timestamp ();
    int levels[30];
    picod_gpio_read_levels (response, levels);

    int changed = 0;
    int point;
    for (point = dev->points; point >= 0; point = Gpios[point].next) {
        struct GpioMap *gpio = Gpios + point;
        int level = levels[gpio->gpio];
        int state = (level == gpio->on)?1:0;

        if (housepicod_gpio_store (point, state)) {
            if (GpioFastScanEnd && (gpio->history >= 0))
                housepicod_memory_store (timestamp, gpio->history, state);
            changed = 1;
        }
    }
    if (changed) housestate_changed (LiveGpioState);
    if (GpioFastScanEnd) housepicod_memory_done (timestamp);
}

static void housepicod_gpio_checkpulses (long long timestamp) {

    if (!GpioHasActivePulse) return;
    GpioHasActivePulse = 0; // Unless one is still active.

    int i;
    for (i = GpioCount-1; i >= 0; --i) {
        if (Gpios[i].deadline <= 0) continue;
        if (Gpios[i].mode != HOUSE_GPIO_MODE_OUTPUT) continue;
        if (!timestamp || (Gpios[i].deadline <= timestamp)) {
            housepicod_gpio_set (i, 1 - Gpios[i].commanded, -1, 0);
            continue;
        }
        GpioHasActivePulse = 1;
    }
}

static void housepicod_gpio_alert (int index, void *data,
                                   int status, const uint8_t *response) {

    int count = picod_gpio_alert_count (response);
    if (count <= 0) return;

    const uint8_t *uid = picod_uid (index);
    struct PicoMap *dev = housepicod_gpio_find_pico (uid, 0);
    if (!dev) return;

    long long timestamp = housepicod_gpio_timestamp ();
    uint32_t base = picod_gpio_alert_timestamp (count-1, response);

    int i;
    int changed = 0;
    for (i = 0; i < count; ++i) {
        uint32_t levels = picod_gpio_alert_levels (i, response);
        uint32_t tick = picod_gpio_alert_timestamp (i, response);
        uint32_t delta = (base - tick) / 1000; // Play games with overflow rules
        long long adjusted = timestamp - delta;

        int point = dev->points;
        for (point = dev->points; point >= 0; point = Gpios[point].next) {
            int level = (levels >> Gpios[point].gpio) & 1;
            int state = (level == Gpios[point].on)?1:0;

            if (housepicod_gpio_store (point, state)) {
                int history = Gpios[point].history;
                if (GpioFastScanEnd && (history >= 0))
                    housepicod_memory_store (adjusted, history, state);
                changed = 1;
            }
        }
    }
    if (changed) housestate_changed (LiveGpioState);
    if (GpioFastScanEnd) housepicod_memory_done (timestamp);

    housepicod_gpio_checkpulses (timestamp);
}

static void housepicod_gpio_wakeup (int fd, int mode) {

    if (mode & 2)
        if (!picod_transmit (fd))
            echttp_listen (fd, 1, housepicod_gpio_wakeup, 1);

    if (mode & 1) picod_receive (fd);
}

static void housepicod_gpio_submit (int index) {

    picod_submit (index);
    if (picod_busy (index)) {
        int fd = picod_descriptor (index);
        if (fd > 0) echttp_listen (fd, 3, housepicod_gpio_wakeup, 1);
    }
}

static void housepicod_gpio_configure_pico (struct PicoMap *dev)  {

    dev->configured = 0;
    if(!dev->detected) return; // Will be configured later.

    picod_reset (dev->uid);
    if (!dev->inuse) return; // No need to configure it.

    // Setup the GPIO pins mode on that new device and read the initial
    // state. If the default (i.e. off) state is 1, then the pull-up resistor
    // is enabled: assume an open drain circuit.
    // The Pico configuration is built as a single large (around 330 bytes)
    // packet, and the only response expected is to the final read command.
    //
    int index = picod_start (dev->uid);
    if (index < 0) return; // Not connected.

    picod_gpio_direction
        (index, dev->pincount, dev->pins, dev->dir, dev->level, 0, 0);
    picod_gpio_pull (index, dev->pincount, dev->pins, dev->level, 0, 0, 0);
    int i;
    for (i = dev->pincount-1; i >= 0; --i) {
        picod_gpio_debounce (index, dev->pins[i], DEBOUNCE_USEC, 0, 0);
    }
    picod_gpio_alert
        (index, dev->pincount, dev->pins, 0, housepicod_gpio_alert, 0);
    picod_gpio_read (index, housepicod_gpio_read_response, 0);
    housepicod_gpio_submit (index);

    // No poll is needed: just wait for the initial read response
    // and subsequent alerts.

    dev->configured = 1;
}

static void housepicod_gpio_refresh_pico (void) {

    // Update the list of detected Pico devices.
    int i;
    for (i = 0; i < PicoDevicesCount; ++i) {
        if (PicoDevices[i]) PicoDevices[i]->detected = 0;
    }
    int count = picod_count ();
    while (count--) {
        const uint8_t *uid = picod_uid (count);
        if (!uid) continue;
        struct PicoMap *dev = housepicod_gpio_find_pico (uid, 0);
        if (!dev) continue; // Ignore devices never present in a configuration

        dev->detected = 1;
    }

    // Prune out the Pico devices that are neither referenced in
    // the current configuration and have not been detected.
    // Those Pico device that remain are forcefully reconfigured to match
    // the new list of points.
    for (i = 0; i < PicoDevicesCount; ++i) {
        struct PicoMap *dev = PicoDevices[i];
        if (!dev) continue;
        if (!dev->inuse && !dev->detected) {
            PicoDevices[i] = 0;
            free (dev);
            continue;
        }
        housepicod_gpio_configure_pico (dev);
    }
}

static int housepicod_gpio_to_mode (const char *text) {

   // Default is output for compatibility with previous versions.
   if (!text || !text[0]) return HOUSE_GPIO_MODE_OUTPUT;

   if (strsame (text, "out")) return HOUSE_GPIO_MODE_OUTPUT;
   if (strsame (text, "output")) return HOUSE_GPIO_MODE_OUTPUT;
   if (strsame (text, "in")) return HOUSE_GPIO_MODE_INPUT;
   if (strsame (text, "input")) return HOUSE_GPIO_MODE_INPUT;

   return HOUSE_GPIO_MODE_INPUT; // Safer, no short circuit.
}

static const char *housepicod_gpio_from_mode (int point) {

    if (point < 0 || point > GpioCount) return 0;
    switch (Gpios[point].mode) {
    case HOUSE_GPIO_MODE_OUTPUT: return "output";
    case HOUSE_GPIO_MODE_INPUT:  return "input";
    }
    return ""; // Safe.
}

static int PicoRegisteredFd[PICODEVICESMAX] = {0};

static void housepicod_gpio_detect (void) {

    // Reset I/O registration and detection. This will be reevaluated
    // when walking the list of Pico devices later.
    int i;
    for (i = 0; i < PICODEVICESMAX; ++i) {
        if (PicoRegisteredFd[i] > 0) {
            echttp_listen (PicoRegisteredFd[i], 0, 0, 0);
            PicoRegisteredFd[i] = 0;
        }
    }

    struct PicoMap *dev;
    for (i = 0; i < PicoDevicesCount; ++i) {
        dev = PicoDevices[i];
        if (dev) dev->detected = 0;
    }

    int count = picod_count ();
    while (count-- > 0) {
        const uint8_t *uid = picod_uid (count);
        if (uid) {
            dev = housepicod_gpio_find_pico (uid, 0);
            if (dev) {
                // Reconfigure the devices that have appeared and are
                // referenced in the current configuration.
                // There is no point in reconfiguring all Pico devices
                // periodically..
                if (dev->inuse && (!dev->configured))
                    housepicod_gpio_configure_pico (dev);
                dev->detected = 1;
            }
        }

        int fd = picod_descriptor (count);
        if (fd > 0) {
            int mode = (picod_busy (count))?3:1;
            echttp_listen (fd, mode, housepicod_gpio_wakeup, 1);
            PicoRegisteredFd[count] = fd;
        }
    }

    for (i = 0; i < PicoDevicesCount; ++i) {
        dev = PicoDevices[i];
        if (dev && !dev->detected) dev->configured = 0;
    }
}

static void housepicod_gpio_scanner (int fd, int mode) {
    housepicod_gpio_checkpulses (housepicod_gpio_timestamp());
}

void housepicod_gpio_enablefast (int duration) {

    if (InputCount <= 0) return; // Nothing to enable anyway.

    if (!GpioFastScanEnd) { // Protection against self reset.
        housepicod_memory_reset (InputCount, DEBOUNCE_USEC/1000);
        int i;
        for (i = 0; i < GpioCount; ++i) Gpios[i].history = -1;
        for (i = 0; i < InputCount; ++i) {
            int point = InputIndex[i];
            Gpios[point].history = housepicod_memory_add (Gpios[point].name);
        }
    }
    // Extend fast scanning for some more time.
    time_t end = time(0) + duration;
    if (GpioFastScanEnd < end) GpioFastScanEnd = end;
}

void housepicod_gpio_fast (int period) {
    // This implementation ignore the requested period. That period
    // is a sampling period and picod is the one doing the sampling:
    // the sampling is always the fastet possible (3ms period, as
    // defined by the debounce timer).

   housepicod_gpio_enablefast (HOUSE_GPIO_SCAN_TIMEOUT);
}

static void housepicod_gpio_slow (void) {

    GpioFastScanEnd = 0;
}

static const char *housepicod_gpio_set_uid (int index, int point) {

    const char *ascii = houseconfig_string (point, ".uid");
    if (!ascii) return "missing UID";
    if (strlen (ascii) != 2 * PICOUIDSIZE) return "invalid UID length";

    uint8_t *uid = Gpios[index].uid;
    int i;
    for (i = 0; i < PICOUIDSIZE; ++i, ascii += 2) {
        uid[i] = hextobyte (ascii);
    }
    Gpios[index].uidsignature = picod_signature (uid, PICOUIDSIZE);
    return 0;
}

const char *housepicod_gpio_refresh (void) {

    housepicod_gpio_checkpulses (0); // Cancel any ongoing pulse.

    int i;
    for (i = 0; i < PicoDevicesCount; ++i) {
        struct PicoMap *dev = PicoDevices[i];
        if (!dev) continue;
        dev->inuse = 0;
        dev->pincount = 0;
        dev->points = -1;
    }

    for (i = 0; i < GpioCount; ++i) {
        Gpios[i].name = 0;
        Gpios[i].signature = 0;
    }

    housepicod_gpio_slow (); // No history until requested.

    int picod = houseconfig_array (0, ".picod.points");
    if (picod < 0) return "cannot find points array";

    int oldcount = GpioCount;
    GpioCount = houseconfig_array_length (picod);
    if (GpioCount <= 0) return "no point found";
    DEBUG ("found %d points\n", GpioCount);

    if (GpioCount > oldcount) {
        if (Gpios) free(Gpios);
        Gpios = calloc(GpioCount, sizeof(struct GpioMap));
        if (!Gpios) return "no more memory";

        if (InputIndex) free(InputIndex);
        InputIndex = calloc (GpioCount, sizeof(int));
        if (!InputIndex) return "no more memory";
    }
    InputCount = 0;

    int count = 0;
    int *list = calloc (GpioCount, sizeof(int));
    houseconfig_enumerate (picod, list, GpioCount);
    for (i = 0; i < GpioCount; ++i) {
        int point = houseconfig_object (list[i], 0);
        if (point <= 0) continue;
        const char *name = houseconfig_string (point, ".name");
        if ((!name) || (!name[0])) continue;

        Gpios[count].name = name;
        Gpios[count].gear = houseconfig_string (point, ".gear");
        Gpios[count].mode =
            housepicod_gpio_to_mode (houseconfig_string (point, ".mode"));
        Gpios[count].desc = houseconfig_string (point, ".description");
        const char *error = housepicod_gpio_set_uid (count, point);
        if (error) return error;
        Gpios[count].gpio = houseconfig_integer (point, ".gpio");
        Gpios[count].on  = houseconfig_integer (point, ".on") & 1;

        Gpios[count].pulse = houseconfig_positive (point, ".pulse");

        Gpios[count].signature = echttp_hash_signature (Gpios[count].name);
        Gpios[count].state = 0;
        Gpios[count].commanded = 0;
        Gpios[count].deadline = 0;
        Gpios[count].failed = 0;

        if (Gpios[count].mode == HOUSE_GPIO_MODE_INPUT)
            InputIndex[InputCount++] = count;

        // Link all GPIO points to their Pico device.
        struct PicoMap *dev = housepicod_gpio_add_pico (Gpios[count].uid);
        if (dev) {
            int j = dev->pincount++;
            if (j < PICOMAXGPIO) {
                Gpios[count].next = dev->points;
                dev->points = count;
                Gpios[count].pico = dev->index;
                dev->pins[j] = Gpios[count].gpio;
                dev->dir[j] = (Gpios[count].mode == HOUSE_GPIO_MODE_OUTPUT);
                dev->level[j] = 1 - Gpios[count].on;
            }
        }

        DEBUG ("found point %s, gpio %d, on %d %s\n", Gpios[count].name, Gpios[count].gpio, Gpios[count].on, Gpios[count].desc);
        count += 1;
    }
    free (list);
    if (count != GpioCount) {
        houselog_trace (HOUSE_FAILURE, "GPIO",
                        "Ignoring %d invalid points\n", GpioCount-count);
        if (GpioCount <= 0) return "no valid point found";
        GpioCount = count; // Adjust the count to include valid entries only.
    }

    // The list of points changed: remove all references to the old names
    // and erase the existing history.
    housepicod_memory_reset (InputCount, DEBOUNCE_USEC/1000);

    // Now that the points configuration has been retrieved,
    // initialize the access to the GPIO.
    //
    housepicod_gpio_refresh_pico ();
    housepicod_gpio_detect (); // To register the file descriptors.

    return 0;
}

int housepicod_gpio_search (const char *name) {
    int i;
    unsigned int signature = echttp_hash_signature (name);
    for (i = 0; i < GpioCount; ++i) {
       if (Gpios[i].signature != signature) continue; // Faster than strsame()
       if (!strsame (name, Gpios[i].name)) continue;
       if (housepicod_gpio_pico_present (Gpios[i].pico)) return i;
    }
    return -1;
}

int housepicod_gpio_count (void) {
    return GpioCount;
}

int housepicod_gpio_set (int point, int state,
                         long long pulse, const char *cause) {

    if (point < 0 || point > GpioCount) return 0;

    // Silently ignore control requests on points that are not output.
    // This is not considered as an error.
    if (Gpios[point].mode != HOUSE_GPIO_MODE_OUTPUT) return 1;

    if (!housepicod_gpio_pico_present (Gpios[point].pico)) return 0;

    time_t now = time(0);
    const char *namedstate = state?"ON":"OFF";

    // Limit the active time of the control if the configuration specifies
    // a maximum pulse value.
    int max = Gpios[point].pulse;
    if (max > 0) {
        if ((pulse == 0) || (pulse > max)) pulse = max;
    }

    if (echttp_isdebug()) {
        if (pulse)
            printf ("set %s to %s at %lld (pulse %lld ms)\n", Gpios[point].name, namedstate, (long long)now, pulse);
        else
            printf ("set %s to %s at %lld\n", Gpios[point].name, namedstate, (long long)now);
    }

    int index = picod_start (Gpios[point].uid);
    if (index < 0) {
        houselog_event ("GPIO", Gpios[point].name, namedstate,
                        "CONTROL FAILED: DEVICE NOT AVAILABLE");
    } else {
        int pin = Gpios[point].gpio;
        int level = state?Gpios[point].on:(1-Gpios[point].on);
        const char *error = picod_gpio_write (index, 1, &pin, &level, 0, 0);
        if (error)
            houselog_event ("GPIO", Gpios[point].name, namedstate,
                            "CONTROL FAILED: %s", error);
        housepicod_gpio_submit (index);
    }

    char comment[256];
    if (cause)
        snprintf (comment, sizeof(comment), " (%s)", cause);
    else
        comment[0] = 0;

    if (pulse > 0) {

        long long s = pulse / 1000;
        int ms = (int) (pulse % 1000);
        if (ms) {
            Gpios[point].deadline = housepicod_gpio_timestamp() + pulse;
            houselog_event ("GPIO", Gpios[point].name, namedstate,
                            "FOR %lld.%03d SECONDS%s", s, ms, comment);
        } else {
            Gpios[point].deadline = (time(0) + s) * 1000;
            houselog_event ("GPIO", Gpios[point].name, namedstate,
                            "FOR %lld SECONDS%s", s, comment);
        }
        GpioHasActivePulse = 1;

    } else if (pulse < 0) {

        Gpios[point].deadline = 0;
        houselog_event ("GPIO", Gpios[point].name, namedstate,
                        "END OF PULSE");
    } else {

        Gpios[point].deadline = 0;
        houselog_event ("GPIO", Gpios[point].name, namedstate,
                        "LATCHED%s", comment);
    }
    Gpios[point].commanded = state;
    housestate_changed (LiveGpioState);
    return 1;
}

void housepicod_gpio_status (ParserContext context, int root) {

    int i;
    for (i = 0; i < GpioCount; ++i) {
       if (!housepicod_gpio_pico_present (Gpios[i].pico))
           continue; // Don't report point not present on this computer.

       const char *mode = housepicod_gpio_from_mode(i);
       const char *status = Gpios[i].state?"on":"off";
       const char *commanded = Gpios[i].commanded?"on":"off";

       int point = echttp_json_add_object (context, root, Gpios[i].name);
       if (mode) echttp_json_add_string (context, point, "mode", mode);
       echttp_json_add_string (context, point, "state", status);
       if ((Gpios[i].mode == HOUSE_GPIO_MODE_OUTPUT) &&
           (!strsame (status, commanded)))
           echttp_json_add_string (context, point, "command", commanded);
       if (Gpios[i].deadline > 0) {
           echttp_json_add_integer
               (context, point, "pulse", Gpios[i].deadline / 1000);
       }
       if (Gpios[i].gear && (Gpios[i].gear[0] != 0))
           echttp_json_add_string (context, point, "gear", Gpios[i].gear);
    }
}

void housepicod_gpio_periodic (time_t now) {

    picod_background (now);
    housepicod_gpio_detect ();

    if (GpioFastScanEnd) {
        // If there is no changes request for much more than the stored
        // history and no pulse to maintain, disable fast scan: there is
        // no need  for it anymore.
        if (now > GpioFastScanEnd) housepicod_gpio_slow ();
    }

    if (!GpioFastScanEnd) { // May have have been reset above..
        // Check for pulses here if there is no active fast scan.
        // Otherwise the fast scan takes care of the pulses.
        housepicod_gpio_checkpulses (now * 1000); // Pulses in milliseconds.
    }
}

int housepicod_gpio_same (void) {
    return housestate_same (LiveGpioState);
}

int housepicod_gpio_current (void) {
    return housestate_current (LiveGpioState);
}

const char *housepicod_gpio_initialize (int argc, const char **argv) {

    PicodCapture  = housecapture_register ("PICOD");

    LiveGpioState = housestate_declare ("live");

    picod_spy (housepicod_gpio_tap);
    const char *error =  picod_initialize (argc, argv);;
    if (error) return error;

    picod_listen (housepicod_gpio_detect);
    echttp_fastscan (housepicod_gpio_scanner, 50000); // To handle pulses

    if (houseconfig_active()) return housepicod_gpio_refresh ();
    return picod_initialize (argc, argv);;
}

