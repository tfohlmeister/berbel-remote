#!/usr/bin/env python3
"""Fake Berbel hood: a BLE central a Berbel remote will pair and talk to.

In the Berbel protocol the remote is the peripheral and the hood is the
central, so reading a remote out means playing the hood. This tool finds a
remote, bonds with it (legacy Just Works), subscribes to the button
characteristic and records every code it sends. It can also write status
frames back on f004f001, which the remote mirrors on its own LEDs.

Runs on Linux/BlueZ. See README.md for the adapter preparation it needs.
"""

import argparse
import asyncio
import json
import os
import signal
import sys
from datetime import datetime
from pathlib import Path

from bleak import BleakClient, BleakScanner

from hood import (ADV_SERVICE_UUID, AFTERRUN_SECONDS, BATTERY_CHAR_UUID,
                  BERBEL_SERVICE_UUID, BUTTON_CHAR_UUID, CHAR_ALIASES,
                  COVER_TRAVEL_SECONDS, STATUS_CHAR_UUID, Hood, State)
from dbus_fast import BusType
from dbus_fast.aio import MessageBus
from dbus_fast.service import ServiceInterface, method

AGENT_PATH = "/berbel/fakehood/agent"



# --------------------------------------------------------------------------
# Known button codes, for a readable live log. Source of truth is the repo's
# berbel_button_map.json; unknown codes are printed raw, which is the point.
# --------------------------------------------------------------------------

def load_known_codes():
    for parent in [Path(__file__).resolve().parent, *Path(__file__).resolve().parents]:
        candidate = parent / "berbel_button_map.json"
        if candidate.is_file():
            data = json.loads(candidate.read_text())
            return {
                int(key, 16): f"{entry['label']} ({entry['description']})"
                for key, entry in data.get("buttons", {}).items()
            }
    return {}


KNOWN_CODES = load_known_codes()


# --------------------------------------------------------------------------
# Logging
# --------------------------------------------------------------------------

class Logger:
    """Writes every event to stdout as a line and to a JSONL file."""

    def __init__(self, path):
        self.file = open(path, "a", buffering=1, encoding="utf-8") if path else None

    def __call__(self, event, **fields):
        record = {
            "ts": datetime.now().astimezone().isoformat(timespec="milliseconds"),
            "event": event,
            **fields,
        }
        if self.file:
            self.file.write(json.dumps(record, ensure_ascii=False) + "\n")
        print(self.format(record), flush=True)

    @staticmethod
    def format(record):
        clock = record["ts"][11:23]
        event = record["event"]
        rest = {k: v for k, v in record.items() if k not in ("ts", "event")}
        if event == "button":
            data = rest["data"]
            pretty = " ".join(data[i:i + 2] for i in range(0, len(data), 2))
            code = rest.get("code")
            if code == 0:
                meaning = "release"
            else:
                meaning = KNOWN_CODES.get(code, "UNKNOWN CODE")
                meaning = f"0x{code:02X}  {meaning}"
            return f"{clock}  BUTTON   {pretty:<12} {meaning}"
        if event == "label":
            return f"{clock}  ----     {rest['text']}"
        detail = "  ".join(f"{k}={v}" for k, v in rest.items())
        return f"{clock}  {event.upper():<8} {detail}"

    def close(self):
        if self.file:
            self.file.close()


# --------------------------------------------------------------------------
# Pairing agent: NoInputNoOutput, so BlueZ completes Just Works unattended
# --------------------------------------------------------------------------

class NoInputNoOutputAgent(ServiceInterface):
    def __init__(self, log):
        super().__init__("org.bluez.Agent1")
        self.log = log

    @method()
    def Release(self):
        self.log("agent", call="Release")

    @method()
    def RequestPinCode(self, device: "o") -> "s":  # noqa: F821
        self.log("agent", call="RequestPinCode", device=device)
        return "0000"

    @method()
    def DisplayPinCode(self, device: "o", pincode: "s"):  # noqa: F821
        self.log("agent", call="DisplayPinCode", device=device, pincode=pincode)

    @method()
    def RequestPasskey(self, device: "o") -> "u":  # noqa: F821
        self.log("agent", call="RequestPasskey", device=device)
        return 0

    @method()
    def DisplayPasskey(self, device: "o", passkey: "u", entered: "q"):  # noqa: F821
        self.log("agent", call="DisplayPasskey", device=device, passkey=passkey)

    @method()
    def RequestConfirmation(self, device: "o", passkey: "u"):  # noqa: F821
        self.log("agent", call="RequestConfirmation", device=device, passkey=passkey)

    @method()
    def RequestAuthorization(self, device: "o"):  # noqa: F821
        self.log("agent", call="RequestAuthorization", device=device)

    @method()
    def AuthorizeService(self, device: "o", uuid: "s"):  # noqa: F821
        self.log("agent", call="AuthorizeService", device=device, uuid=uuid)

    @method()
    def Cancel(self):
        self.log("agent", call="Cancel")


async def register_agent(log):
    """Register a NoInputNoOutput agent and make it the default."""
    bus = await MessageBus(bus_type=BusType.SYSTEM).connect()
    bus.export(AGENT_PATH, NoInputNoOutputAgent(log))
    introspection = await bus.introspect("org.bluez", "/org/bluez")
    obj = bus.get_proxy_object("org.bluez", "/org/bluez", introspection)
    manager = obj.get_interface("org.bluez.AgentManager1")
    await manager.call_register_agent(AGENT_PATH, "NoInputNoOutput")
    await manager.call_request_default_agent(AGENT_PATH)
    log("agent", call="registered", capability="NoInputNoOutput")
    return bus


# --------------------------------------------------------------------------
# Session state, shared between the BLE loop and the control socket
# --------------------------------------------------------------------------

async def handle_control(state, reader, writer):
    try:
        while True:
            line = await reader.readline()
            if not line:
                return
            try:
                reply = await run_command(state, line.decode().strip())
            except Exception as exc:  # a bad command must not kill the session
                reply = f"ERR {type(exc).__name__}: {exc}"
            writer.write((reply + "\n").encode())
            await writer.drain()
    finally:
        writer.close()


async def run_command(state, line):
    if not line:
        return "OK"
    parts = line.split(maxsplit=1)
    command = parts[0].lower()
    rest = parts[1] if len(parts) > 1 else ""

    if command == "label":
        state.log("label", text=rest)
        return "OK"

    if command == "quit":
        state.stop.set()
        return "OK stopping"

    if command == "state":
        connected = state.client is not None and state.client.is_connected
        return (f"OK connected={connected} address={state.address} "
                f"emulate={state.emulate} hood: {state.hood.describe()}")

    client = state.client
    if client is None or not client.is_connected:
        return "ERR not connected"

    if command in ("status", "button", "write"):
        if command == "write":
            target, _, payload = rest.partition(" ")
        else:
            target, payload = command, rest
        uuid = CHAR_ALIASES.get(target.lower(), target)
        data = bytes.fromhex(payload.replace(" ", ""))
        await client.write_gatt_char(uuid, data, response=False)
        state.log("write", char=uuid, data=data.hex())
        return f"OK wrote {len(data)} bytes"

    if command == "press":
        if not state.emulate:
            return "ERR not running with --emulate"
        state.react(int(rest, 16))
        return f"OK reacted to 0x{int(rest, 16):02X}"

    if command == "profile":
        path = rest.strip() or "profile.json"
        written = await dump_profile(state, path)
        return f"OK {written} -> {path}"

    if command == "sync":
        sync = Hood.sync_frame(state.frame_length)
        await client.write_gatt_char(STATUS_CHAR_UUID, sync, response=False)
        state.log("status", data=sync.hex(), reason="sync on request")
        return "OK sync sent"

    if command == "push":
        await state.push_status("requested")
        return f"OK {state.hood.frame(state.frame_length).hex()}"

    if command == "read":
        uuid = CHAR_ALIASES.get(rest.lower(), rest)
        value = await client.read_gatt_char(uuid)
        state.log("read", char=uuid, data=value.hex())
        return f"OK {value.hex()}"

    if command == "info":
        lines = []
        for service in client.services:
            lines.append(f"service {service.uuid}")
            for char in service.characteristics:
                props = ",".join(char.properties)
                lines.append(f"  char {char.uuid} [{props}]")
        return "OK " + " | ".join(lines)

    return f"ERR unknown command: {command}"


# --------------------------------------------------------------------------
# BLE
# --------------------------------------------------------------------------

async def find_remote(args, state, log):
    """Scan until a remote shows up, by address or by its service data."""
    found = asyncio.get_running_loop().create_future()
    seen = set()

    def callback(device, advert):
        if found.done():
            return
        if args.address:
            match = device.address.upper() == args.address.upper()
        else:
            match = ADV_SERVICE_UUID in advert.service_data
        if match:
            state.advertisement = {
                "local_name": advert.local_name,
                "rssi": advert.rssi,
                "tx_power": advert.tx_power,
                "service_uuids": list(advert.service_uuids),
                "service_data": {k: v.hex() for k, v in advert.service_data.items()},
                "manufacturer_data": {str(k): v.hex()
                                      for k, v in advert.manufacturer_data.items()},
            }
            found.set_result(device)
        elif ADV_SERVICE_UUID in advert.service_data and device.address not in seen:
            seen.add(device.address)
            log("seen", address=device.address, rssi=advert.rssi,
                service_data=advert.service_data[ADV_SERVICE_UUID].hex())

    log("scanning", filter=args.address or ADV_SERVICE_UUID)
    async with BleakScanner(detection_callback=callback, adapter=args.adapter):
        try:
            return await asyncio.wait_for(found, timeout=args.scan_timeout)
        except asyncio.TimeoutError:
            log("scan_timeout", seconds=args.scan_timeout)
            return None


async def dump_profile(state, path):
    """Record everything a central can see of the connected peripheral.

    The point is to outlive the hardware: with this file in the repository, an
    emulator can be diffed against the real remote long after the remote has
    gone back. Values are raw hex plus, where it decodes cleanly, text.
    """
    client = state.client
    profile = {
        "recorded": datetime.now().astimezone().isoformat(timespec="seconds"),
        "address": state.address,
        "advertisement_as_bluez_saw_it": dict(
            state.advertisement or {},
            caveat="BlueZ's view of the device, not the raw PDU: it fills "
                   "local_name and service_uuids from the GATT database as "
                   "well, so trust raw_advertising for what actually goes "
                   "over the air."),
        "status_frame_bytes": state.frame_length,
        "services": [],
    }

    for service in client.services:
        entry = {"uuid": service.uuid, "description": service.description,
                 "characteristics": []}
        for char in service.characteristics:
            item = {
                "uuid": char.uuid,
                "handle": char.handle,
                "description": char.description,
                "properties": list(char.properties),
                "descriptors": [
                    {"uuid": d.uuid, "handle": d.handle} for d in char.descriptors
                ],
            }
            if "read" in char.properties:
                try:
                    value = await client.read_gatt_char(char)
                except Exception as exc:
                    item["read_error"] = f"{type(exc).__name__}: {exc}"
                else:
                    # Decoding belongs outside the read: most of these
                    # characteristics are binary, and a System ID that is not
                    # text is a successful read, not a failed one.
                    item["value_hex"] = bytes(value).hex()
                    text = bytes(value).decode("utf-8", "replace")
                    if value and text.isprintable() and "\ufffd" not in text:
                        item["value_text"] = text
            entry["characteristics"].append(item)
        profile["services"].append(entry)

    # The raw advertising PDU and the remote's name are captured by hand with
    # btmon, which this cannot do. Overwriting a profile that already has them
    # would discard the half of the file the hardware is no longer around to
    # replace, so carry those keys over instead.
    try:
        with open(path, encoding="utf-8") as handle:
            previous = json.load(handle)
    except (OSError, ValueError):
        previous = {}
    for key in ("remote", "raw_advertising"):
        if key in previous:
            profile.setdefault(key, previous[key])

    with open(path, "w", encoding="utf-8") as handle:
        json.dump(profile, handle, indent=2, ensure_ascii=False, sort_keys=False)
        handle.write("\n")

    chars = sum(len(s["characteristics"]) for s in profile["services"])
    state.log("profile", path=path, services=len(profile["services"]),
              characteristics=chars)
    return f"{len(profile['services'])} services, {chars} characteristics"


def on_button(state):
    def callback(_characteristic, data):
        code = data[0] if data else None
        state.log("button", data=bytes(data).hex(), code=code)
        if state.emulate and code:
            state.react(code)
    return callback


async def session(device, args, state):
    log = state.log
    disconnected = asyncio.Event()

    client = BleakClient(
        device,
        disconnected_callback=lambda _c: disconnected.set(),
        timeout=args.connect_timeout,
        adapter=args.adapter,
    )

    await client.connect()
    state.client = client
    state.address = device.address
    log("connected", address=device.address, name=device.name)

    try:
        if args.unpair:
            try:
                await client.unpair()
                log("unpaired", address=device.address)
            except Exception as exc:
                log("unpair_failed", error=str(exc))

        if args.pair != "never":
            try:
                await client.pair()
                log("paired", address=device.address)
            except Exception as exc:
                log("pair_failed", error=f"{type(exc).__name__}: {exc}")
                if args.pair == "always":
                    raise

        for service in client.services:
            for char in service.characteristics:
                log("gatt", service=service.uuid, char=char.uuid,
                    props=",".join(char.properties))

        await client.start_notify(BUTTON_CHAR_UUID, on_button(state))
        log("subscribed", char=BUTTON_CHAR_UUID)

        if state.emulate:
            # The remote sizes its own status buffer, and the Skyline Edge
            # models use 13 bytes where the others use 9. Read it rather
            # than assume, so the frames we write are the shape it expects.
            try:
                probe = await client.read_gatt_char(STATUS_CHAR_UUID)
                if len(probe) in Hood.FRAME_LENGTHS:
                    state.frame_length = len(probe)
                log("frame_length", bytes=state.frame_length, probe=probe.hex())
            except Exception as exc:
                log("frame_length_unknown", error=f"{type(exc).__name__}: {exc}",
                    assuming=state.frame_length)

            # A hood opens with a sync packet of all 0x11, then its real state.
            sync = Hood.sync_frame(state.frame_length)
            await client.write_gatt_char(STATUS_CHAR_UUID, sync, response=False)
            log("status", data=sync.hex(), reason="sync on connect")
            await state.push_status("initial state")

        try:
            level = await client.read_gatt_char(BATTERY_CHAR_UUID)
            log("battery", percent=level[0])
        except Exception as exc:
            log("battery_unavailable", error=f"{type(exc).__name__}: {exc}")

        waiter = asyncio.create_task(disconnected.wait())
        stopper = asyncio.create_task(state.stop.wait())
        await asyncio.wait({waiter, stopper}, return_when=asyncio.FIRST_COMPLETED)
        for task in (waiter, stopper):
            task.cancel()
    finally:
        state.client = None
        if client.is_connected:
            await client.disconnect()
        log("disconnected", address=device.address)


async def run(args):
    log = Logger(args.log)
    state = State(log, emulate=args.emulate,
                  afterrun_seconds=args.afterrun_seconds)

    loop = asyncio.get_running_loop()
    for sig in (signal.SIGINT, signal.SIGTERM):
        loop.add_signal_handler(sig, state.stop.set)

    bus = await register_agent(log)

    if os.path.exists(args.socket):
        os.unlink(args.socket)
    server = await asyncio.start_unix_server(
        lambda r, w: handle_control(state, r, w), path=args.socket)
    log("control", socket=args.socket)

    try:
        while not state.stop.is_set():
            device = await find_remote(args, state, log)
            if device is None:
                if args.once:
                    break
                continue
            try:
                await session(device, args, state)
            except Exception as exc:
                log("session_failed", error=f"{type(exc).__name__}: {exc}")
            if args.once:
                break
    finally:
        server.close()
        await server.wait_closed()
        if os.path.exists(args.socket):
            os.unlink(args.socket)
        bus.disconnect()
        log("stopped")
        log.close()


async def scan(args):
    """List everything advertising, marking anything that looks like a remote."""
    log = Logger(args.log)
    seen = {}

    def callback(device, advert):
        berbel = ADV_SERVICE_UUID in advert.service_data
        if not berbel and not args.all:
            return
        line = (device.address, device.name, advert.rssi // 10)
        if seen.get(device.address) == line:
            return
        seen[device.address] = line
        log("advert",
            address=device.address,
            name=device.name,
            rssi=advert.rssi,
            berbel=berbel,
            service_data={k: v.hex() for k, v in advert.service_data.items()},
            manufacturer_data={k: v.hex() for k, v in advert.manufacturer_data.items()})

    async with BleakScanner(detection_callback=callback, adapter=args.adapter):
        await asyncio.sleep(args.seconds)
    log.close()


async def control(args):
    reader, writer = await asyncio.open_unix_connection(args.socket)
    writer.write((" ".join(args.command) + "\n").encode())
    await writer.drain()
    print((await reader.readline()).decode().rstrip())
    writer.close()
    await writer.wait_closed()


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--adapter", default="hci0")
    parser.add_argument("--log", default=None, help="JSONL log file")
    sub = parser.add_subparsers(dest="mode", required=True)

    p_scan = sub.add_parser("scan", help="look for advertising remotes")
    p_scan.add_argument("--seconds", type=float, default=20)
    p_scan.add_argument("--all", action="store_true", help="show every advertiser")

    p_run = sub.add_parser("run", help="connect, bond and record buttons")
    p_run.add_argument("--address", default=None, help="remote MAC, else match by service data")
    p_run.add_argument("--socket", default="/tmp/fakehood.sock")
    p_run.add_argument("--pair", choices=("auto", "always", "never"), default="auto")
    p_run.add_argument("--unpair", action="store_true", help="drop an existing bond first")
    p_run.add_argument("--once", action="store_true", help="do not reconnect after a drop")
    p_run.add_argument("--emulate", action="store_true",
                       help="act as a hood: answer every press with a status frame")
    p_run.add_argument("--afterrun-seconds", type=float, default=AFTERRUN_SECONDS,
                       help="run-on time; the real hood uses 600")
    p_run.add_argument("--scan-timeout", type=float, default=60)
    p_run.add_argument("--connect-timeout", type=float, default=30)

    p_ctl = sub.add_parser("ctl", help="send a command to a running fakehood")
    p_ctl.add_argument("--socket", default="/tmp/fakehood.sock")
    p_ctl.add_argument("command", nargs="+")

    args = parser.parse_args()
    handler = {"scan": scan, "run": run, "ctl": control}[args.mode]
    try:
        asyncio.run(handler(args))
    except KeyboardInterrupt:
        sys.exit(130)


if __name__ == "__main__":
    main()
