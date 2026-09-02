# Remote logging

Diagnosing a dropped BLE link needs the log from the moment it happened. The
ESP32 usually sits behind or above the hood, so attaching a laptop for the hours
or days until the next dropout is rarely practical.

The firmware can therefore mirror everything it prints to the serial console to
the MQTT topic `berbel/hood/log`, and Home Assistant shows those lines in the
history of a sensor.

## Turning it on

It takes two steps, and both are needed. The first one starts the mirroring, the
second one makes the lines visible in Home Assistant.

**1. Switch on Remote Log.** MQTT auto-discovery creates a **Remote Log** switch
under the Berbel Hood device. Turning it on makes the firmware publish its log.
The setting is retained on the broker and restored on boot, so it stays on across
a reboot, which is exactly the event you usually want logged.

**2. Enable the Log entity.** Discovery also creates a **Log** sensor whose state
is the most recent line, but it ships **disabled**. Enable it under
*Settings → Devices & Services → MQTT → Berbel Hood*, open the entity, then
*Settings → Enabled*. Home Assistant re-subscribes within about half a minute.

It is disabled by default on purpose: while logging is on the sensor changes
several times a minute, and every change is written to the recorder database. If
it were enabled out of the box, every user would pay for a feature almost nobody
has switched on.

## Reading it

Open the **Log** entity and look at its history. Each line is one state change
with a timestamp, so the entity's own activity view is the log view. The sensor
sets `force_update`, so two identical lines in a row still show up as two
entries rather than being collapsed into one.

Without Home Assistant, the topic is plain text:

```bash
mosquitto_sub -h <broker> -u <user> -P <pass> -t 'berbel/hood/log'
```

That is also the better option when you expect a burst of lines, since it has no
recorder and no state machine in between.

## What the lines look like

Every line carries the subsystem that wrote it:

| Prefix | What it covers |
|--------|----------------|
| `[BLE]` | Connects, disconnects with the decoded reason code, pairing result, advertising |
| `[HOOD]` | Raw 9-byte status packets from the hood |
| `[MQTT]` | Broker connect, incoming commands, state restore |
| `[CMD]` / `[BTN]` | Queued and sent button codes |
| `[SYS]` | Free heap plus BLE and WiFi state, every 30 s |
| `[WiFi]` / `[OTA]` / `[MAC]` | Startup and update plumbing |
| `[LOG]` | The logging itself, including dropped lines |

A healthy reconnect reads like this:

```
[BLE] Hood connected!
[BLE] Peer 84:f7:xx:xx:xx:xx, encrypted=0 bonded=0
[BLE] Authentication complete: encrypted=1 bonded=1
[HOOD] Status (9 bytes): 11 11 11 11 11 11 11 11 11
[HOOD] Sync packet ignored
```

`encrypted=1 bonded=1` means the bond from pairing was resumed. A disconnect
names its cause:

```
[BLE] Disconnect: HCI 0x08 - supervision timeout, link lost
[BLE] Hood disconnected
[BLE] Advertising started
```

`HCI 0x08` is a lost radio link, `0x13` means the hood hung up on purpose, and
`0x05` or `0x06` mean the bond is broken and the hood will refuse to reconnect
until it is paired again.

## Limits worth knowing

- **240 characters per line.** Longer lines are cut, which keeps them under Home
  Assistant's 255 character limit for a sensor state.
- **24 queued lines.** Log lines are written from the BLE task and published from
  the main loop, so they pass through a queue. If MQTT is down or the firmware
  logs faster than it can publish, the oldest lines are dropped and a
  `[LOG] N lines dropped, queue was full` note is published once the queue drains.
- **Nothing before MQTT is up.** Lines written during boot are only kept if
  logging is already enabled at that point, see below.

## Logging from the first boot

The retained switch is only read once the broker connection is up, so the
earliest boot lines are lost. To capture those too, set the compile-time default
in `config.h`:

```c
#define REMOTE_LOG_DEFAULT true
```

Logging is then on from the first line, and the queue holds the startup output
until MQTT connects.

## Turning it off

Switch **Remote Log** off. The firmware flushes whatever is still queued before
going quiet, so the lines leading up to the moment you switched off are not lost.

If you leave logging on for days, keep an eye on the recorder database, or
exclude the sensor from it:

```yaml
recorder:
  exclude:
    entities:
      - sensor.berbel_hood_log
```
