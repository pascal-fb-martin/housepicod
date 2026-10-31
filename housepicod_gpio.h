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
 * housepicod_gpio.c - Access a GPIO mapped relay board.
 *
 */
const char *housepicod_gpio_initialize (int argc, const char **argv);
const char *housepicod_gpio_refresh (void);

int housepicod_gpio_search (const char *name);
int housepicod_gpio_count (void);

const char *housepicod_gpio_failure (int point);

int housepicod_gpio_get (int point);
int housepicod_gpio_set (int point, int state,
                         long long pulse, const char *cause);

int  housepicod_gpio_same (void);
int  housepicod_gpio_current (void);

void housepicod_gpio_fast (int period);

void housepicod_gpio_status (ParserContext context, int root);
void housepicod_gpio_changes (long long since,
                              ParserContext context, int root);

void housepicod_gpio_periodic (time_t now);

