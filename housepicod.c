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
 * housepicod.c - Main loop of the housepicod program.
 *
 * SYNOPSYS:
 *
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include "echttp_cors.h"
#include "echttp_json.h"
#include "echttp_static.h"
#include "houseportalclient.h"
#include "housediscover.h"
#include "houselog.h"
#include "houseconfig.h"
#include "housedepositor.h"
#include "housecapture.h"

#include "housepicod.h"
#include "housepicod_gpio.h"
#include "housepicod_memory.h"

static char HostName[256];
static char JsonBuffer[65537];

static const char *pico_status (const char *method, const char *uri,
                                const char *data, int length) {

    if (housepicod_gpio_same ()) return "";

    ParserToken token[1024];
    char pool[65537];

    ParserContext context = echttp_json_start (token, 1024, pool, 65537);

    int root = echttp_json_add_object (context, 0, 0);
    echttp_json_add_string (context, root, "host", HostName);
    echttp_json_add_string (context, root, "proxy", houseportal_server());
    echttp_json_add_integer (context, root, "timestamp", (long long)time(0));
    echttp_json_add_integer (context, root, "latest", housepicod_gpio_current());
    int top = echttp_json_add_object (context, root, "control");

    echttp_json_add_bool (context, top, "history", 1);

    housepicod_gpio_status (context, top);

    const char *error =
        echttp_json_export (context, JsonBuffer, sizeof(JsonBuffer));
    if (error) {
        echttp_error (500, error);
        return "";
    }
    echttp_content_type_json ();
    return JsonBuffer;
}

static const char *pico_set (const char *method, const char *uri,
                             const char *data, int length) {

    const char *point = echttp_parameter_get("point");
    const char *statep = echttp_parameter_get("state");
    const char *pulsep = echttp_parameter_get("pulse");
    const char *cause = echttp_parameter_get("cause");
    int state;
    long long pulse = 0;
    int found = 0;

    if (!point) {
        echttp_error (404, "missing point name");
        return "";
    }
    if (!statep) {
        echttp_error (400, "missing state value");
        return "";
    }
    if ((strcmp(statep, "on") == 0) || (strcmp(statep, "1") == 0)) {
        state = 1;
    } else if ((strcmp(statep, "off") == 0) || (strcmp(statep, "0") == 0)) {
        state = 0;
    } else {
        echttp_error (400, "invalid state value");
        return "";
    }

    if (pulsep) {
        char *endp; // Warning: strtoll() converts 'const char *' to 'char *'..
        pulse = strtoll (pulsep, &endp, 10);
        if (pulse < 0) {
            echttp_error (400, "invalid pulse value");
            return "";
        }
        pulse *= 1000; // Convert internally to milliseconds.

        if (endp[0] == '.') {
            // Read the fractional part up to 3 digits, ignore any more digits
            static int weight[] = {0, 100, 10, 1};
            char c;
            int i;
            for (i = 1; i <= 3; ++i) {
                c = endp[i];
                if ((c < '0') || (c > '9')) break;
                pulse += (c - '0') * weight[i];
            }
            if (i == 4) {
                c = endp[i];
                if ((c >= '5') && (c <= '9')) pulse += 1; // Rounding.
            }
        }
    }

    if (!strcmp (point, "all")) {
       int count = housepicod_gpio_count();
       int i;
       for (i = 0; i < count; ++i) {
           housepicod_gpio_set (i, state, pulse, cause);
       }
       found = 1;
    } else {
       int i = housepicod_gpio_search (point);
       if (i >= 0) {
           housepicod_gpio_set (i, state, pulse, cause);
           found = 1;
       }
    }

    if (! found) {
        echttp_error (404, "invalid point name");
        return "";
    }
    return pico_status (method, uri, data, length);
}

static const char *pico_history (const char *method, const char *uri,
                                 const char *data, int length) {

    const char *syncpar = echttp_parameter_get("sync");
    const char *sincepar = echttp_parameter_get("since");
    const char *periodpar = echttp_parameter_get("period");

    int sync = 0;
    if (syncpar) sync = atoi(syncpar);
    else if (echttp_parameter_present("sync")) sync = 1; // default value.

    long long since = 0;
    if (sincepar) since = atoll(sincepar);

    int period = 0;
    if (periodpar) period = atoi (periodpar);
    housepicod_gpio_fast (period);

    ParserToken token[20480];
    char pool[65537];

    ParserContext context = echttp_json_start (token, 20480, pool, 65537);

    int root = echttp_json_add_object (context, 0, 0);
    echttp_json_add_string (context, root, "host", HostName);
    echttp_json_add_integer (context, root, "timestamp", (long long)time(0));
    int top = echttp_json_add_object (context, root, "control");

    int container = echttp_json_add_object (context, top, "history");
    housepicod_memory_history (since, context, container);

    if (sync) {
        container = echttp_json_add_object (context, top, "status");
        housepicod_gpio_status (context, container);
    }

    const char *error =
        echttp_json_export (context, JsonBuffer, sizeof(JsonBuffer));
    if (error) {
        echttp_error (500, error);
        return "";
    }
    echttp_content_type_json ();
    return JsonBuffer;
}

static const char *pico_config (const char *method, const char *uri,
                                const char *data, int length) {

    if (strcmp ("GET", method) == 0) {
        echttp_content_type_json ();
        return houseconfig_current();
    }

    if (strcmp ("POST", method) == 0) {
        const char *error = houseconfig_update (data, "USER CHANGE");
        if (error) {
            echttp_error (400, error);
        }
        return houseconfig_current();
    }

    echttp_error (400, "invalid state value");
    return "";
}

static void pico_background (int fd, int mode) {

    time_t now = time(0);

    houseportal_background (now);
    housepicod_gpio_periodic (now);
    housediscover (now);
    houselog_background (now);
    houseconfig_background (now);
    housedepositor_periodic (now);
    housepicod_memory_background (now);
    housecapture_background (now);
}

static void pico_protect (const char *method, const char *uri) {
    echttp_cors_protect(method, uri);
}

int main (int argc, const char **argv) {

    const char *error;

    // These strange statements are to make sure that fds 0 to 2 are
    // reserved, since this application might output some errors.
    // 3 descriptors are wasted if 0, 1 and 2 are already open. No big deal.
    //
    open ("/dev/null", O_RDONLY);
    dup(open ("/dev/null", O_WRONLY));

    gethostname (HostName, sizeof(HostName));

    echttp_default ("-http-service=dynamic");

    argc = echttp_open (argc, argv);
    if (echttp_dynamic_port()) {
        static const char *path[] = {"control:/picod"};
        houseportal_initialize (argc, argv);
        houseportal_declare (echttp_port(4), path, 1);
    }
    housediscover_initialize (argc, argv);
    houselog_initialize ("picod", argc, argv);
    housedepositor_initialize (argc, argv);

    error = houseconfig_initialize
                ("picod", housepicod_gpio_refresh, argc, argv);
    if (error) {
        houselog_trace
            (HOUSE_FAILURE, "CONFIG", "Cannot load configuration: %s\n", error);
    }
    housecapture_initialize ("/picod", argc, argv);

    error = housepicod_gpio_initialize (argc, argv);
    if (error) {
        houselog_trace
            (HOUSE_FAILURE, "CONFIG", "Cannot configure GPIO: %s\n", error);
    }

    echttp_cors_allow_method("GET");
    echttp_protect (0, pico_protect);

    echttp_route_uri ("/picod/status",  pico_status);
    echttp_route_uri ("/picod/set",     pico_set);
    echttp_route_uri ("/picod/history", pico_history);

    echttp_route_uri ("/picod/config", pico_config);

    echttp_static_route ("/", "/usr/local/share/house/public");
    echttp_background (&pico_background);

    houselog_event ("SERVICE", "picod", "STARTED", "ON %s", houselog_host());
    echttp_loop();
}

