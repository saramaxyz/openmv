# main.py — Sarama AE3 minimal relay firmware (single file)
#
# Does exactly one thing: turn on camera + microphone + IMU and stream them
# to the relay over TCP using the 0xDEADBEEF framing from
# docs/contracts/tcp-streaming.md:
#   0x01 video (JPEG)   0x04 audio (PCM16 mono)
#   0x05 distance (IMU-derived)   0x06 identity (dog name + device uuid)
#
# ponytail: single-file firmware by design — no SD, no BLE, no OTA, no
# deepsleep. Split into modules when a second feature actually lands.

import gc
import json
import os
import struct
import sys
import time
import asyncio

import machine
import network
import sensor
import audio
import imu as imu_mod

# ── Config ────────────────────────────────────────────────────────────────────

FIRMWARE_VERSION = "3.0.0-fr-fr-single"
SETTINGS_FILE = "/flash/settings.json"
RELAY_HOST_DEFAULT = "137.184.118.166"
RELAY_PORT_DEFAULT = 8554

JPEG_QUALITY = 65
FRAME_INTERVAL_MS = 100          # ~10 fps
CAMERA_SETTLE_MS = 2000

AUDIO_RATE = 16000
AUDIO_GAIN_DB = 30
AUDIO_SAMPLES = 512              # ~32 ms per callback
AUDIO_QUEUE_MAX = 64

IMU_POLL_MS = 20                 # 50 Hz; step detection needs no more
STEP_THRESHOLD_MG = 180
STEP_MIN_INTERVAL_MS = 300
STEP_LENGTH_MM = 650
DISTANCE_SEND_MS = 5000

WIFI_TIMEOUT_MS = 20000
RECONNECT_MIN_MS = 3000
RECONNECT_MAX_MS = 30000
IDENTITY_REFRESH_MS = 60000
HEALTH_INTERVAL_MS = 30000

MAGIC = 0xDEADBEEF
TYPE_VIDEO = 0x01
TYPE_AUDIO = 0x04
TYPE_DISTANCE = 0x05
TYPE_IDENTITY = 0x06
AUDIO_FORMAT_PCM16 = 0x01

# ── Protocol (CRC-16/MODBUS + packet builders) ────────────────────────────────

def crc16(data):
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            if crc & 1:
                crc = (crc >> 1) ^ 0xA001
            else:
                crc >>= 1
    return crc


def build_video_header(frame_num, rel_ms, width, height, payload_len):
    """24-byte video header (incl CRC); JPEG payload is written separately."""
    header = struct.pack(
        "<IIIHHBBI",
        MAGIC,
        frame_num & 0xFFFFFFFF,
        rel_ms & 0xFFFFFFFF,
        width & 0xFFFF,
        height & 0xFFFF,
        TYPE_VIDEO,
        0,
        payload_len & 0xFFFFFFFF,
    )
    return header + struct.pack("<H", crc16(header))


def build_audio_header(payload_len):
    """12-byte audio header (incl CRC); PCM payload is written separately."""
    header = struct.pack(
        "<IBBHH",
        MAGIC,
        TYPE_AUDIO,
        AUDIO_FORMAT_PCM16,
        AUDIO_RATE & 0xFFFF,
        payload_len & 0xFFFF,
    )
    return header + struct.pack("<H", crc16(header))


def build_distance_packet(rel_ms, session_mm, total_mm):
    data = struct.pack(
        "<IBIII",
        MAGIC,
        TYPE_DISTANCE,
        int(session_mm) & 0xFFFFFFFF,
        int(total_mm) & 0xFFFFFFFF,
        rel_ms & 0xFFFFFFFF,
    )
    return data + struct.pack("<H", crc16(data))


def build_identity_packet(dog_name, device_uuid):
    dog = dog_name.encode("ascii", "ignore")[:255]
    uuid = str(device_uuid or "").strip().upper().encode("ascii", "ignore")[:255]
    data = struct.pack("<IBBBB", MAGIC, TYPE_IDENTITY, len(dog), len(uuid), 0xA5)
    data += dog + uuid
    return data + struct.pack("<H", crc16(data))


# ── Settings (/flash/settings.json) ───────────────────────────────────────────

def _device_uuid_from_hardware():
    """Return the stable, printable hardware identity used by registration."""
    try:
        return "".join("{:02X}".format(byte) for byte in machine.unique_id())
    except Exception:
        return "UNKNOWN000"


def _normalize_identity(settings):
    """Keep the single-file and established registration settings shapes aligned."""
    if not isinstance(settings, dict):
        settings = {}
    identity = settings.get("identity")
    device = settings.get("device")
    identity = dict(identity) if isinstance(identity, dict) else {}
    device = dict(device) if isinstance(device, dict) else {}

    dog_name = device.get("dog_name") or identity.get("dog_name") or "Unknown"
    device_uuid = device.get("device_uuid") or identity.get("device_uuid") or _device_uuid_from_hardware()
    dog_name = str(dog_name)
    device_uuid = str(device_uuid).strip().upper()

    identity["dog_name"] = dog_name
    identity["device_uuid"] = device_uuid
    device["dog_name"] = dog_name
    device["device_uuid"] = device_uuid
    settings["identity"] = identity
    settings["device"] = device
    return settings


def load_settings():
    try:
        with open(SETTINGS_FILE, "r") as f:
            return _normalize_identity(json.loads(f.read()))
    except Exception:
        return {}


def create_default_settings():
    """Create settings with one stable identity usable before WiFi connects."""
    return _normalize_identity({
        "wifi": {"known_networks": []},
        "relay": {},
    })


def save_settings(settings):
    """Persist settings atomically while preserving both supported identity shapes."""
    settings = _normalize_identity(settings)
    tmp_path = SETTINGS_FILE + ".tmp"
    try:
        with open(tmp_path, "w") as f:
            f.write(json.dumps(settings))
        try:
            os.remove(SETTINGS_FILE)
        except OSError:
            pass
        os.rename(tmp_path, SETTINGS_FILE)
        return True
    except Exception as exc:
        print("[SETTINGS] save failed: {}".format(exc))
        try:
            os.remove(tmp_path)
        except OSError:
            pass
        return False


def get_device_identity(state=None, settings=None):
    """Compatibility API consumed by provisioning and watcher registration flows."""
    if settings is None and state is not None:
        settings = state.get("settings")
    if settings is None:
        settings = create_default_settings()
    identity = _normalize_identity(settings)["identity"]
    return identity["dog_name"], identity["device_uuid"]


def install_collar_logic_compatibility():
    """Expose the registration API without restoring the legacy firmware module."""
    try:
        import types
        compat = types.ModuleType("collar_logic")
    except Exception:
        class Compat:
            pass
        compat = Compat()
    compat.load_settings = load_settings
    compat.create_default_settings = create_default_settings
    compat.save_settings = save_settings
    compat.get_device_identity = get_device_identity
    sys.modules["collar_logic"] = compat
    return compat


def load_wifi_credentials(settings):
    """Highest-priority known network, or (None, None)."""
    networks = settings.get("wifi", {}).get("known_networks", [])
    if not networks:
        return (None, None)
    networks.sort(key=lambda n: n.get("priority", 50))
    return (networks[0].get("ssid", ""), networks[0].get("password", ""))


def load_relay_endpoint(settings):
    """relay.host must be a routable LAN/WAN IP; 0.0.0.0 is a bind address."""
    host, port = RELAY_HOST_DEFAULT, RELAY_PORT_DEFAULT
    relay = settings.get("relay", {})
    if isinstance(relay, dict):
        candidate = str(relay.get("host", "") or relay.get("ip", "")).strip()
        if candidate and candidate != "0.0.0.0":
            host = candidate
        try:
            p = int(relay.get("port", port))
            if 0 < p < 65536:
                port = p
        except Exception:
            pass
    return (host, port)


def load_identity(settings, wlan):
    identity = _normalize_identity(settings).get("identity", {})
    dog_name = identity.get("dog_name", "Unknown")
    device_uuid = identity.get("device_uuid", "")
    if not device_uuid and wlan is not None:
        try:
            device_uuid = "".join("{:02X}".format(b) for b in wlan.config("mac"))
        except Exception:
            device_uuid = _device_uuid_from_hardware()
    return (dog_name, device_uuid)


# ── Hardware init ─────────────────────────────────────────────────────────────

def camera_init():
    sensor.reset()
    sensor.set_pixformat(sensor.RGB565)
    sensor.set_framesize(sensor.QVGA)
    sensor.skip_frames(time=CAMERA_SETTLE_MS)
    gc.collect()
    return (sensor.width(), sensor.height())


def audio_init():
    try:
        audio.init(channels=1, frequency=AUDIO_RATE, gain_db=AUDIO_GAIN_DB,
                   samples=AUDIO_SAMPLES)
    except TypeError:
        # older builds have no samples kwarg
        audio.init(channels=1, frequency=AUDIO_RATE, gain_db=AUDIO_GAIN_DB)
    gc.collect()


def imu_init():
    try:
        whoami = imu_mod.__read_reg(0x0F)
        if whoami != 0x6A:
            print("[IMU] WARNING: WHO_AM_I=0x{:02X}, expected 0x6A".format(whoami))
    except Exception as e:
        print("[IMU] WARNING: could not read WHO_AM_I: {}".format(e))


def wifi_connect(ssid, password):
    """Connect STA. Returns wlan on success, None on timeout."""
    wlan = network.WLAN(network.STA_IF)
    wlan.active(True)
    wlan.connect(ssid, password)
    start = time.ticks_ms()
    while not wlan.isconnected():
        if time.ticks_diff(time.ticks_ms(), start) > WIFI_TIMEOUT_MS:
            print("[WiFi] connect timeout")
            return None
        time.sleep_ms(200)
    try:
        print("[WiFi] connected, IP: {}".format(wlan.ipconfig("addr4")))
    except Exception:
        print("[WiFi] connected")
    return wlan


# ── Network send path ─────────────────────────────────────────────────────────

def close_writer(st):
    w = st["writer"]
    st["writer"] = None
    if w is not None:
        try:
            w.close()
        except Exception:
            pass


async def send_packet(st, header, payload=None):
    """Serialized socket write. Returns False (and disconnects) on error."""
    if st["writer"] is None:
        return False
    async with st["lock"]:
        w = st["writer"]
        if w is None:
            return False
        try:
            w.write(header)
            if payload:
                w.write(payload)
            await w.drain()
            return True
        except Exception as e:
            print("[STREAMER] send error: {}".format(e))
            close_writer(st)
            return False


async def net_task(st):
    """Own WiFi link + relay connection + identity refresh, with backoff."""
    delay = RECONNECT_MIN_MS
    while True:
        wlan = st["wlan"]
        if wlan is None or not wlan.isconnected():
            close_writer(st)
            print("[WiFi] reconnecting to '{}'".format(st["ssid"]))
            st["wlan"] = wifi_connect(st["ssid"], st["password"])
            if st["wlan"] is None:
                await asyncio.sleep_ms(delay)
                delay = min(delay * 2, RECONNECT_MAX_MS)
                continue

        if st["writer"] is None:
            try:
                reader, writer = await asyncio.open_connection(st["host"], st["port"])
                st["writer"] = writer
                delay = RECONNECT_MIN_MS
                print("[STREAMER] connected to {}:{}".format(st["host"], st["port"]))
                await send_packet(st, build_identity_packet(st["dog_name"], st["uuid"]))
                st["identity_at"] = time.ticks_ms()
            except OSError as e:
                print("[STREAMER] connect failed: {}".format(e))
                await asyncio.sleep_ms(delay)
                delay = min(delay * 2, RECONNECT_MAX_MS)
                continue
        elif time.ticks_diff(time.ticks_ms(), st["identity_at"]) >= IDENTITY_REFRESH_MS:
            if await send_packet(st, build_identity_packet(st["dog_name"], st["uuid"])):
                st["identity_at"] = time.ticks_ms()

        await asyncio.sleep_ms(500)


# ── Producer tasks ────────────────────────────────────────────────────────────

def rel_ms(st):
    return time.ticks_diff(time.ticks_ms(), st["boot"]) & 0xFFFFFFFF


async def video_task(st):
    """Capture, JPEG-compress, send. One frame in memory at a time."""
    next_t = time.ticks_ms()
    while True:
        if st["writer"] is not None:
            img = sensor.snapshot()
            jpeg = img.compress(quality=JPEG_QUALITY)
            buf = jpeg.bytearray()
            header = build_video_header(st["frame_num"], rel_ms(st),
                                        img.width(), img.height(), len(buf))
            if await send_packet(st, header, buf):
                st["frame_num"] = (st["frame_num"] + 1) & 0xFFFFFFFF
                st["frames_sent"] += 1
            del buf, jpeg, img
            gc.collect()
        next_t = time.ticks_add(next_t, FRAME_INTERVAL_MS)
        wait = time.ticks_diff(next_t, time.ticks_ms())
        if wait <= 0:
            next_t = time.ticks_ms()
        await asyncio.sleep_ms(wait if wait > 0 else 0)


def make_audio_callback(st):
    q = st["audio_q"]

    def callback(pcmbuf):
        # DMA reuses pcmbuf between callbacks — copy before queueing.
        try:
            pcm = bytes(pcmbuf)
            if len(q) < AUDIO_QUEUE_MAX:
                q.append(pcm)
            else:
                st["audio_drops"] += 1
        except Exception:
            pass

    return callback


async def audio_task(st):
    """Drain the audio callback queue into the socket."""
    q = st["audio_q"]
    while True:
        if not q:
            await asyncio.sleep_ms(5)
            continue
        pcm = q.pop(0)  # ponytail: O(n) pop on a <=64 item list is fine
        if st["writer"] is not None:
            if await send_packet(st, build_audio_header(len(pcm)), pcm):
                st["audio_sent"] += 1
        del pcm
        await asyncio.sleep_ms(0)


async def imu_task(st):
    """Poll accel at 50 Hz, count steps, send a distance packet every 5 s."""
    last_step = -STEP_MIN_INTERVAL_MS
    last_send = 0
    while True:
        now = rel_ms(st)
        try:
            ax, ay, az = imu_mod.acceleration_mg()
            mag = (ax * ax + ay * ay + az * az) ** 0.5
            if abs(mag - 1000) >= STEP_THRESHOLD_MG and \
                    now - last_step >= STEP_MIN_INTERVAL_MS:
                last_step = now
                st["session_mm"] += STEP_LENGTH_MM
                st["total_mm"] += STEP_LENGTH_MM
        except Exception as e:
            st["last_error"] = "imu: {}".format(e)

        if now - last_send >= DISTANCE_SEND_MS and st["writer"] is not None:
            pkt = build_distance_packet(now, st["session_mm"], st["total_mm"])
            if await send_packet(st, pkt):
                st["distance_sent"] += 1
                last_send = now
        await asyncio.sleep_ms(IMU_POLL_MS)


async def health_task(st):
    while True:
        await asyncio.sleep_ms(HEALTH_INTERVAL_MS)
        gc.collect()
        print("[HEALTH] frames={} audio={} distance={} drops={} mem_free={} err={}".format(
            st["frames_sent"], st["audio_sent"], st["distance_sent"],
            st["audio_drops"], gc.mem_free(), st["last_error"]))


async def supervise(coro_factory, name, delay_ms):
    """Restart a crashed task after a short delay, forever."""
    while True:
        try:
            await coro_factory()
        except asyncio.CancelledError:
            raise
        except Exception as e:
            print("[TASK:{}] crashed: {}".format(name, e))
            try:
                sys.print_exception(e)
            except Exception:
                pass
        gc.collect()
        await asyncio.sleep_ms(delay_ms)


# ── Entry point ───────────────────────────────────────────────────────────────

async def main():
    print("[BOOT] fr_fr single-file firmware v{}".format(FIRMWARE_VERSION))
    settings = load_settings()
    ssid, password = load_wifi_credentials(settings)
    host, port = load_relay_endpoint(settings)
    print("[BOOT] relay {}:{} wifi={}".format(host, port,
                                              "configured" if ssid else "MISSING"))
    if not ssid:
        print("[BOOT] no WiFi credentials in settings.json; idle")
        while True:
            await asyncio.sleep_ms(60000)

    st = {
        "boot": time.ticks_ms(),
        "ssid": ssid,
        "password": password,
        "host": host,
        "port": port,
        "wlan": None,
        "writer": None,
        "lock": asyncio.Lock(),
        "identity_at": 0,
        "dog_name": "Unknown",
        "uuid": "",
        "audio_q": [],
        "frame_num": 0,
        "frames_sent": 0,
        "audio_sent": 0,
        "distance_sent": 0,
        "audio_drops": 0,
        "session_mm": 0,
        "total_mm": 0,  # ponytail: RAM only; persist to flash when lifetime totals matter
        "last_error": None,
    }

    print("[CAM] init...")
    w, h = camera_init()
    print("[CAM] ready: {}x{}".format(w, h))
    print("[AUDIO] init...")
    audio_init()
    print("[AUDIO] ready")
    print("[IMU] init...")
    imu_init()
    print("[IMU] ready")

    # WiFi up front so identity can use the MAC; net_task retries afterwards.
    st["wlan"] = wifi_connect(ssid, password)
    st["dog_name"], st["uuid"] = load_identity(settings, st["wlan"])
    print("[IDENTITY] dog='{}' uuid='{}'".format(st["dog_name"], st["uuid"]))

    audio.start_streaming(make_audio_callback(st))
    print("[AUDIO] streaming started")

    tasks = [
        asyncio.create_task(supervise(lambda: net_task(st), "net", 3000)),
        asyncio.create_task(supervise(lambda: video_task(st), "video", 1000)),
        asyncio.create_task(supervise(lambda: audio_task(st), "audio", 500)),
        asyncio.create_task(supervise(lambda: imu_task(st), "imu", 1000)),
        asyncio.create_task(health_task(st)),
    ]
    print("[MAIN] all tasks started")
    try:
        while True:
            await asyncio.sleep_ms(1000)
    finally:
        try:
            audio.stop_streaming()
        except Exception:
            pass
        for t in tasks:
            try:
                t.cancel()
            except Exception:
                pass
        close_writer(st)


if __name__ == "__main__":
    install_collar_logic_compatibility()
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("[SYS] stopped by user")
    except Exception as e:
        print("[SYS] fatal: {}".format(e))
        try:
            sys.print_exception(e)
        except Exception:
            pass
