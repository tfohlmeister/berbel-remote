# Viewing the firmware log in Home Assistant

The firmware can mirror its whole serial log to MQTT. Turn on the **Remote Log**
switch on the device and every line the firmware prints is published to
`berbel/hood/log`. That covers the case this exists for: a hood-mounted ESP32
that drops its BLE link at 3 in the morning, with no laptop attached.

The switch alone only gets the lines onto the broker. This page adds a readable
view in Home Assistant.

## What you get out of the box

MQTT auto-discovery already creates two entities:

| Entity | Type | Notes |
|--------|------|-------|
| Remote Log | Switch | Turns the mirroring on and off. Retained, so it survives a reboot. |
| Log | Sensor | The most recent log line. **Disabled by default** in Home Assistant. |

The sensor is disabled on purpose: while remote logging is on it changes several
times a minute, and every change is written to the recorder database. Enable it
under *Settings → Devices & Services → MQTT → Berbel Hood* when you want the
last line on a dashboard or in the history graph.

## Rolling log view

For an actual log you want the last N lines together, not just the newest one.
A trigger-based template sensor collects them into an attribute.

Save this as `packages/berbel_log.yaml` in your Home Assistant config directory
(requires `homeassistant: packages: !include_dir_named packages/`), or paste the
`template:` block into your `configuration.yaml`:

```yaml
# Berbel hood: rolling view of the firmware log published on berbel/hood/log.
# Lines only arrive while the "Remote Log" switch on the device is on.

template:
  - trigger:
      - trigger: mqtt
        topic: berbel/hood/log
    sensor:
      - name: Berbel Log History
        unique_id: berbel_log_history
        icon: mdi:text-box-outline
        device_class: timestamp
        state: "{{ now().isoformat() }}"
        attributes:
          lines: >
            {% set prev = this.attributes.get('lines', []) if this is not none else [] %}
            {{ (prev + [now().strftime('%H:%M:%S') ~ '  ' ~ trigger.payload])[-40:] }}
```

Reload it under *Developer Tools → YAML → Template entities*, no restart needed.
The sensor's state is the time of the last line; the `lines` attribute holds the
last 40 lines with a timestamp prefix.

One limitation to know about: the template appends to the value it last wrote, so
a burst of lines arriving back to back can render before the previous state is
visible and overwrite each other instead of appending. Single lines and the usual
trickle are fine, but if you are chasing something that logs a burst, read the
topic directly as shown at the bottom of this page.

Then add a Markdown card to a dashboard:

```yaml
type: markdown
title: Berbel Log
content: |
  {% set lines = state_attr('sensor.berbel_log_history', 'lines') %}
  {% if lines %}
  ```
  {{ lines | join('\n') }}
  ```
  {% else %}
  No log received. Turn on the **Remote Log** switch.
  {% endif %}
```

Which looks like this:

```
08:44:02  [BLE] Hood connected!
08:44:02  [BLE] Peer 84:f7:xx:xx:xx:xx, encrypted=0 bonded=0
08:44:03  [BLE] Authentication complete: encrypted=1 bonded=1
08:44:30  [SYS] Free heap: 137524 bytes, BLE: connected, WiFi: connected
17:44:02  [BLE] Disconnect: HCI 0x08 - supervision timeout, link lost
17:44:02  [BLE] Hood disconnected
17:44:02  [BLE] Advertising started
```

## If you leave it running for days

Every line is one state change, and the 40-line attribute is stored with it. That
is fine for a debugging session but adds up over a week. Either switch **Remote
Log** off again when you are done, or exclude the sensor from the recorder:

```yaml
recorder:
  exclude:
    entities:
      - sensor.berbel_log_history
      - sensor.berbel_hood_log
```

## Reading it without Home Assistant

The topic is plain text, so a terminal works just as well:

```bash
mosquitto_sub -h <broker> -u <user> -P <pass> -t 'berbel/hood/log'
```
