# housepicod - A simple home web server for world control through Pico GPIO
#
# Copyright 2026, Pascal Martin
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor,
# Boston, MA  02110-1301, USA.
#
# WARNING
#
# This Makefile depends on echttp and houseportal (dev) being installed.

prefix=/usr/local
SHARE=$(prefix)/share/house

INSTALL=/usr/bin/install

HAPP=housepicod
HCAT=automation

EXTRADOC=/var/lib/house/note/extra

# Application build. --------------------------------------------

OBJS= housepicod.o housepicod_gpio.o housepicod_memory.o

# all: libpicod.a picodtst picodcmd housepicod
all: housepicod

clean:
	rm -f *.o housepicod
	rm -rf build

rebuild: clean all

%.o: %.c
	gcc -c -Wall -Os -g -o $@ $<

housepicod: $(OBJS)
	gcc -Os -o housepicod $(OBJS) -lpicod -lhouseportal -lechttp -lssl -lcrypto -lmagic -lrt

# Distribution agnostic file installation -----------------------

install-ui: install-preamble
	$(INSTALL) -m 0755 -d $(DESTDIR)$(SHARE)/public/picod
	$(INSTALL) -m 0644 public/* $(DESTDIR)$(SHARE)/public/picod

install-runtime: install-preamble
	$(INSTALL) -m 0755 -s housepicod $(DESTDIR)$(prefix)/bin
	touch $(DESTDIR)/etc/default/housepicod

install-app: install-ui install-runtime

uninstall-app:
	rm -rf $(DESTDIR)$(SHARE)/public/picod
	rm -f $(DESTDIR)$(prefix)/bin/housepicod

purge-app:

purge-config:
	rm -f $(DESTDIR)/etc/house/picod.config
	rm -f $(DESTDIR)/etc/default/housepicod

# Build a private Debian package. -------------------------------

install-package: install-ui install-runtime install-systemd

debian-package: debian-package-generic

# System installation. ------------------------------------------

include $(SHARE)/install.mak

