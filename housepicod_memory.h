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
 * housepicod.h - A mechanism to record GPIO changes of state.
 */
void housepicod_memory_reset (int count, int rate);
int  housepicod_memory_add   (const char *name);
void housepicod_memory_store (long long timestamp, int index, int state);
void housepicod_memory_done  (long long timestamp);
void housepicod_memory_history (long long since,
                                ParserContext context, int root);
void housepicod_memory_background (time_t now);

