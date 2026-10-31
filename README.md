# HousePicod

A web server to control GPIO pins on connected RPi Pico devices.

## Overview

This is a web server designed to provide access to GPIO pins on connected RPi Pico devices. This project depends on [libpicod](https://github.com/pascal-fb-martin/libpicod), [echttp](https://github.com/pascal-fb-martin/echttp) and [houseportal](https://github.com/pascal-fb-martin/houseportal).

This application is a variant of [HouseRelay](https://github.com/pascal-fb-martin/housepicod) and intentionally presents a compatible web API.

The primary intent is to support a distributed network of GPIO devices, to avoid pulling electric wires from one side of the home to the other: a small set of RPi Pico boards are installed, each board located close to existing wiring or near the equipment to control. Each RPi Pico board is attached to a small computer (e.g. a Raspberry Pi Zero W or a mini PC). The typical use is to control sprinklers, a garage door, etc. By using such a micro-service architecture, the application may run anywhere in the home and still access all the devices, regardless of their locations.

The secondary intent is to share access to the GPIO pins of the RPI Pico devices between multiple independent applications: sprinkler system, garage door controller, etc. Each application controls only those devices that it is configured for. A `gear` configuration attribute allows an application to discover which points it wants to control.

A third intent is to share access to digital inputs, e.g. reed relay status. Since the overhead of web polling is relatively high and the timing of HTTP requests is not really accurate, this server stores a limited history of input changes, which clients can access at a lower rate. The sampling rate of inputs is typically 100ms and about 6 seconds worth of the most recent changes is kept available. Since these are input points, multiple applications may access them simultanously.

This way RPi Pico boards may be installed at convenient points across the home, and be accessed by separate applications independently of the relays or applications physical locations.

## Installation.

* Install the OpenSSL development package(s).
* Install [echttp](https://github.com/pascal-fb-martin/echttp).
* Install [houseportal](https://github.com/pascal-fb-martin/houseportal).
* Install [libpicod](https://github.com/pascal-fb-martin/libpicod).
* Clone this GitHub repository.
* make
* sudo make install

It is also possible to create a Debian package. See section "Debian Packaging".

## Hardware

The typical hardware supported by this applications are relay boards controlled through 5V TTL digital pins. These are wired to the digital I/O pins of a Raspberry Pi Pico. Depending on the model, these board are either controlled using open-drain outputs (active low) or 3-state outputs (active high).

The hardware interface is configured through the HouseDepot file "<group>/picod.json", or through the local file /etc/house/picod.json if local storage is enabled.

> [!NOTE]
> Multiple HousePicod services running on separate computers will share the same configuration. Which points are listed by each service depends on which RPi Pico devices are detected locally, as identified by the RPi Pico UID. The configuration contains the total list of points for the entire House group.

A typical exemple of configuration is:

```
{
    "picod" : {
        "points" : [
            {
                "name" : "relay1",
                "uid" : "deadbeefdeadbeef",
                "gpio" : 4,
                "mode" : "output",
                "on" : 0,
                "gear" : "valve",
                "pulse" : 700,
                "connection" : 1,
                "description": "Relay 1"
            },
            {
                "name" : "relay2",
                "uid" : "deadbeefdeadbeef",
                "gpio" : 17,
                "mode" : "output",
                "on" : 0,
                "gear" : "valve",
                "connection" : 2,
                "description": "Relay 2"
            }
        ]
    }
}
```

The uid item must match the UID returned by one RPi Pico device attached to the local computer. The gpio item must match the GPIO pin number on the RPi Pico (this also matches the usual Raspberry Pi naming convention for I/O pin names, e.g. GPIO4, GPIO17).

The mode can be `input` or `output`. If the item is missing, the mode defaults to `output`. All control requests that target an input point are ignored.

If `on` is 0, the point is configured as open-drain with pull-up enabled, the `on` command sets the output to 0, and the `off` command sets the output to 1.

If `on` is 1, an output point is configured as push-pull, the `on` command sets the output to 1, and the `off` command sets the output to 0.

The `gear` attribute can be used by applications to filter which control points to show on their user interface. Typical values are `valve` (irrigation), `light` and `track` (train detectors on tracks).

The `pulse` attribute is optional. If present, it represents a duration in milliseconds and sets a limit to how long the control may be maintained active (regardless of the request). The intent of this attribute is to protect gears that would be damaged if maintained active for an excessive duration, for example a coil under high current. If this attribute is set, any request with no pulse, or with a pulse value greater than this limit, with be silently interpreted as a request with this pulse limit's duration.

This pulse limit is not exact as it is sensitive to the polling period, and that polling period depends on the requested accuracy. As a general rule, a limit in thousands of milliseconds should be set to 1 second less than the actual gear limit, a limit in hundreds of milliseconds should be set to 50 milliseconds less than the actual gear limit, and any more precise limit should be set to 10 milliseconds less than the actual gear limit. for example a pulse limit of 6000 milliseconds may cause an actual pulse anywhere between 5 and 7 seconds, while a pulse limit of 6001 milliseconds may produce an actual pulse anywhere between 5991 and 6011 milliseconds.

> [!WARNING]
> There are ways to overcome this protection mechanism, for example by repeating the request at a high frequency. Use common sense.

The connection and description items are informational. The connection item can be used to match the markings on the relays motherboard. The description item can be used to store any useful comment about this point's purpose or special properties.

## Web API

This program implements the [House control API](https://github.com/pascal-fb-martin/houseportal/blob/master/controlapi.md), including the sequence of changes extension.

The server is also capable of serving static pages, location in /usr/share/house/public/picod. The URL of each page must start with /picod.

## Debian Packaging

The provided Makefile supports building private Debian packages. These are _not_ official packages:

- They do not follow all Debian policies.

- They are not built using Debian standard conventions and tools.

- The packaging is not separate from the upstream sources, and there is
  no source package.

To build a Debian package, use the `debian-package` target:

```
make debian-package
```

