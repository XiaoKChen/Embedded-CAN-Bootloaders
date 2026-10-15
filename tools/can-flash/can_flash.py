#!/usr/bin/env python3
"""Host-side CAN flashing tool for the Arduino UNO R4 Minima CAN bootloader.

Streams a .bin application image to the bootloader over classic CAN 2.0B
(11-bit IDs, DLC 8) using the CAN Update Protocol v1:

    HELLO -> SIZE -> ERASE -> DATA windows -> VERIFY -> GO

Run with no arguments to launch the interactive Textual TUI; pass any flag
(or a firmware path) to run headless exactly as before. See README.md in this
directory for wiring, TUI/CLI usage, and adapter-specific notes.

Requires Python 3.9+, python-can >= 4.0, pyserial >= 3.0, can-isotp >= 2.0, and textual >= 0.60.
"""

# /// script
# requires-python = ">=3.9"
# dependencies = ["python-can>=4.0", "pyserial>=3.0", "can-isotp>=2.0", "textual>=0.60"]
# ///

import argparse
import errno
import os
import struct
import sys
import threading
import time
import zlib

import can

# ---------------------------------------------------------------------------
# CAN Update Protocol v1 constants (frozen -- must match the bootloader)
# ---------------------------------------------------------------------------
PROTOCOL_VERSION = 0x01

DEFAULT_ID_TX = 0x79E          # host -> bootloader
DEFAULT_ID_RX = 0x79F          # bootloader -> host
DEFAULT_BITRATE = 250_000
DEFAULT_RETRIES = 3

# Discovery (--scan): each parked board may listen on its own boot_cmd_id and
# reply on its own boot_reply_id (assigned at provisioning; 0x79E/0x79F if
# unset). --scan INFO-probes a band of candidate cmd IDs while listening on ANY
# reply id, since a board's reply id is arbitrary. Default band + legacy 0x79E.
CAN_ID_MAX = 0x7FF             # 11-bit standard identifier ceiling
DEFAULT_SCAN_FROM = 0x700      # inclusive
DEFAULT_SCAN_TO = 0x71F        # inclusive
SCAN_PROBE_TIMEOUT_S = 0.3     # short per-cmd-id INFO deadline (keeps scan fast)

# Known-device host->boot cmd IDs, provisioned by the ESP-Prog-2 flasher from the
# airform-server-dev BOARDS table (each board's board.rx; ODU/IDU overlap, so the
# unique set is these five). Always probed by --scan so provisioned known boards
# are found without --scan-from/--scan-to (like the legacy 0x79E).
KNOWN_DEVICE_CMD_IDS = (0x01, 0x03, 0x05, 0x07, 0x09)

# TUI defaults: the bare `uv run can_flash.py` launch pre-fills these.
DEFAULT_INTERFACE = "socketcan"
DEFAULT_CHANNEL = "can0"

CMD_HELLO = 0x01
CMD_ERASE = 0x02
CMD_DATA = 0x03
CMD_SIZE = 0x04
CMD_VERIFY = 0x05
CMD_GO = 0x06
CMD_INFO = 0x07                # identify a parked bootloader (5-frame reply)

# INFO reply: 1 header frame + the 32-byte product-name field, 8 bytes/frame.
INFO_NAME_FIELD = 32
INFO_NAME_FRAMES = INFO_NAME_FIELD // 8

REPLY_MAGIC = 0x79             # byte0 of every bootloader reply
ACK = 0x00                     # byte2 == 0x00 means ACK, anything else is a NAK err code

ERR_UNEXPECTED_SEQ = 0x01      # arg = seq to rewind to
ERR_FLASH_FAILURE = 0x02
ERR_CRC_MISMATCH = 0x03
ERR_SIZE_BOUNDS = 0x04
ERR_BAD_STATE = 0x05
ERR_IMAGE_REJECTED = 0x06      # CRC ok, target refused the image (ESP32: not a valid esp32s3 app)

ERR_NAMES = {
    ERR_UNEXPECTED_SEQ: "unexpected sequence number",
    ERR_FLASH_FAILURE: "flash write/erase failure",
    ERR_CRC_MISMATCH: "CRC mismatch",
    ERR_SIZE_BOUNDS: "image size out of bounds",
    ERR_BAD_STATE: "command received in bad state",
    ERR_IMAGE_REJECTED: "image rejected by the target (not a valid app for this chip?)",
}

FRAME_DLC = 8
PAD_BYTE = 0xFF

FLASH_BLOCK_SIZE = 2048        # one DATA window fills exactly one flash block
DATA_BYTES_PER_FRAME = 6       # DATA frame: cmd, seq, then 6 image bytes
FRAMES_PER_WINDOW = (FLASH_BLOCK_SIZE + DATA_BYTES_PER_FRAME - 1) // DATA_BYTES_PER_FRAME  # 342
SEQ_MODULUS = 256

ACK_TIMEOUT_S = 2.0            # ordinary command / window ACK
ERASE_TIMEOUT_S = 15.0         # ERASE wipes 120 blocks and takes seconds
SEND_TIMEOUT_S = 2.0           # give a full local TX queue this long to drain

# A NAK must be noticed before ~256 frames follow the offending one: seq is
# uint8 and a 342-frame window wraps, so frame 256 of a window repeats the
# window-start seq and a deeply buffered resend tail would be silently
# accepted as a fresh window start. A short blocking poll every few frames
# bounds how far the host can run ahead of the replies.
EARLY_POLL_EVERY_FRAMES = 32
EARLY_POLL_TIMEOUT_S = 0.001

# ESP32-S3 updater: one OTA slot of partitions.csv (1.75 MB = 896 windows),
# announced in the HELLO ACK.
EXPECTED_SLOT_SIZE = 0x001C_0000

# ---------------------------------------------------------------------------
# Airform RPC: reboot a RUNNING app into the CAN bootloader (no ACK expected)
# ---------------------------------------------------------------------------
# The A7-IDU-Controller app exposes an Airform route registered as
#   server.add_route("/enter_bootloader", enterCANBootloader)
# whose handler writes the CAN-update handoff magic and calls NVIC_SystemReset()
# WITHOUT sending any response. So this is fire-and-forget: send the request,
# then wait for the board to reboot into the bootloader.
#
# The request is a Uart_Packet, serialized exactly as the Uart_Communication
# library's Packet.cpp Uart_Packet::serialize() does:
#   [header u8=0x55]
#   [address_size u16 BE][address bytes (the route path, no NUL)]
#   [body_size u16 BE][body bytes]
#   [crc u8]
# The CRC is XOR of the header, both body_size bytes, and every body byte --
# the address bytes are NOT part of the CRC (see Packet.cpp calculateCRC()).
# For "/enter_bootloader" with an empty body the packet is 23 bytes, which
# exceeds one 8-byte CAN frame, so it is sent as standard ISO-TP 15765-2
# (First Frame + Consecutive Frames, honoring the board's Flow Control frame).
ENTER_BOOTLOADER_PATH = "/enter_bootloader"
UART_PACKET_HEADER = 0x55            # Uart_Packet::HEADER_VALUE

DEFAULT_BOARD_RX = 0x07              # host -> board CAN ID (IDU Controller RX)
DEFAULT_BOARD_TX = 0x08              # board -> host CAN ID (Flow Control / TX)

ISOTP_SEND_TIMEOUT_S = 2.0          # ISO-TP transfer (FF + FC + CFs) deadline
REBOOT_WAIT_S = 2.0                 # let the MCU reset and the bootloader boot

# --selftest parameters
SELFTEST_CHANNEL = "can_flash_selftest"
SELFTEST_IMAGE_SIZE = 2 * FLASH_BLOCK_SIZE + 1003  # 2 full windows + short final window/frame
SELFTEST_DROP_FRAME = 400      # fake bootloader "loses" this frame once -> forces a rewind NAK
SELFTEST_ERASE_DELAY_S = 0.2

# --selftest scan parameters: fake boards on distinct cmd/reply id pairs (reply
# ids deliberately NOT the legacy 0x79F and outside the probed band, to prove the
# broad listen captures replies on arbitrary ids). The third board sits on a
# known-device cmd id (0x07) reachable only via KNOWN_DEVICE_CMD_IDS, proving the
# default scan covers provisioned known boards without --scan-from/--scan-to.
SELFTEST_SCAN_CHANNEL = "can_flash_scan_selftest"
SELFTEST_SCAN_TIMEOUT_S = 0.2
SELFTEST_SCAN_BOARDS = [
    (0x700, 0x711, "IDU-Controller-A"),
    (0x705, 0x7AB, "Sensor-Node-B"),
    (0x07, 0x7CD, "Known-Device-C"),
]


class FlashError(Exception):
    """Fatal protocol or transport error; aborts the flash with exit code 1."""


# ---------------------------------------------------------------------------
# Frame geometry (shared by the host flasher and the selftest fake bootloader)
# ---------------------------------------------------------------------------
def window_count(image_size):
    return (image_size + FLASH_BLOCK_SIZE - 1) // FLASH_BLOCK_SIZE


def window_frame_count(image_size, window):
    block_bytes = min(FLASH_BLOCK_SIZE, image_size - window * FLASH_BLOCK_SIZE)
    return (block_bytes + DATA_BYTES_PER_FRAME - 1) // DATA_BYTES_PER_FRAME


def frame_payload_range(image_size, frame_index):
    """Map a global DATA frame index to (image byte offset, valid byte count).

    Frames never straddle a flash block: the last frame of a full window
    carries only 2 valid bytes (341 * 6 + 2 = 2048).
    """
    window, idx = divmod(frame_index, FRAMES_PER_WINDOW)
    offset = window * FLASH_BLOCK_SIZE + idx * DATA_BYTES_PER_FRAME
    valid = min(
        DATA_BYTES_PER_FRAME,
        (window + 1) * FLASH_BLOCK_SIZE - offset,
        image_size - offset,
    )
    return offset, valid


def window_start_seq(window):
    return (window * FRAMES_PER_WINDOW) % SEQ_MODULUS


def rewind_window(window, n_windows, seq):
    """Map a rewind NAK's seq to the window the bootloader wants resent.

    The bootloader is always filling either our current window or, when our
    ACK for it was lost, the next one -- never anything further away. Window
    start seqs repeat every 128 windows (342 mod 256 = 86, period 128), so
    resolving only against those two keeps the mapping unique for slots of
    any size (an ESP32 OTA slot spans 896 windows). Returns None if neither
    matches."""
    for candidate in (window, window + 1):
        if candidate < n_windows and window_start_seq(candidate) == seq:
            return candidate
    return None


def _pad_frame(data):
    return bytes(data) + bytes([PAD_BYTE]) * (FRAME_DLC - len(data))


def _err_name(err):
    return ERR_NAMES.get(err, "unknown error 0x%02X" % err)


def encode_uart_packet(path, body=b""):
    """Serialize an Airform Uart_Packet, byte-for-byte matching the device's
    Uart_Communication Packet.cpp serialize()/calculateCRC().

    Layout: header(0x55), address_size(u16 BE), address, body_size(u16 BE),
    body, crc. CRC = header XOR both body_size bytes XOR every body byte
    (the address bytes are deliberately excluded, per calculateCRC())."""
    address = path.encode("ascii")
    body = bytes(body)
    body_size = len(body)
    crc = UART_PACKET_HEADER ^ (body_size & 0xFF) ^ ((body_size >> 8) & 0xFF)
    for byte in body:
        crc ^= byte
    return (bytes([UART_PACKET_HEADER])
            + struct.pack(">H", len(address)) + address
            + struct.pack(">H", body_size) + body
            + bytes([crc]))


# ---------------------------------------------------------------------------
# Host-side flasher
# ---------------------------------------------------------------------------
class CanFlasher:
    def __init__(self, bus, id_tx, id_rx, retries, progress=None):
        self.bus = bus
        self.id_tx = id_tx
        self.id_rx = id_rx
        self.retries = retries
        # progress(fraction) is called per accepted window when set; otherwise
        # the inline stdout progress line is used (identical CLI behaviour).
        self.progress = progress

    # -- transport ----------------------------------------------------------
    def _send(self, payload):
        msg = can.Message(
            arbitration_id=self.id_tx,
            is_extended_id=False,
            data=_pad_frame(payload),
        )
        # Bursting a 342-frame window outruns the bus: socketcan surfaces the
        # backpressure either as ENOBUFS (qdisc full) or as python-can's
        # "Transmit buffer full" (socket sndbuf full -- bus.send() without a
        # timeout polls writability with ZERO wait). Pass a real timeout and
        # retry both transient cases until the queue drains.
        deadline = time.monotonic() + SEND_TIMEOUT_S
        while True:
            try:
                self.bus.send(msg, timeout=max(0.0, deadline - time.monotonic()))
                return
            except can.CanOperationError as exc:
                text = str(exc).lower()
                transient = (getattr(exc, "error_code", None) == errno.ENOBUFS
                             or "buffer full" in text
                             or "no buffer space" in text)
                if not transient:
                    raise
                if time.monotonic() >= deadline:
                    raise FlashError(
                        "local CAN TX queue stayed full for %.0f s -- frames "
                        "are not leaving the interface (bus unpowered, wiring, "
                        "or termination?); check state/errors with: "
                        "ip -details -statistics link show <channel>"
                        % SEND_TIMEOUT_S)
                time.sleep(0.002)

    def _wait_reply(self, cmd, timeout):
        """Wait for an ACK/NAK echoing `cmd`. Returns (err, arg, raw) or None on timeout."""
        deadline = time.monotonic() + timeout
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return None
            msg = self.bus.recv(remaining)
            if msg is None:
                return None
            if msg.arbitration_id != self.id_rx or msg.is_error_frame or msg.is_remote_frame:
                continue
            if msg.dlc != FRAME_DLC:
                raise FlashError(
                    "bootloader reply with bad DLC %d (protocol requires 8)" % msg.dlc)
            data = bytes(msg.data)
            if data[0] != REPLY_MAGIC or data[1] != cmd:
                continue  # stale echo from a retried command, or foreign traffic
            return data[2], data[3], data

    def _command(self, payload, name, timeout=ACK_TIMEOUT_S):
        """Send an idempotent command, retrying on timeout. NAK is fatal."""
        cmd = payload[0]
        for _ in range(self.retries + 1):
            self._send(payload)
            reply = self._wait_reply(cmd, timeout)
            if reply is None:
                continue
            err, arg, raw = reply
            if err != ACK:
                raise FlashError("%s rejected: %s (arg=0x%02X)" % (name, _err_name(err), arg))
            return raw
        raise FlashError("no reply to %s after %d attempts" % (name, self.retries + 1))

    # -- protocol steps ------------------------------------------------------
    def hello(self):
        """Handshake. Returns the announced application slot size in bytes."""
        raw = self._command(bytes([CMD_HELLO, PROTOCOL_VERSION]), "HELLO")
        boot_version = raw[3]
        slot_size = struct.unpack_from("<I", raw, 4)[0]
        if boot_version != PROTOCOL_VERSION:
            raise FlashError(
                "bootloader speaks protocol v%d, this tool speaks v%d"
                % (boot_version, PROTOCOL_VERSION))
        return slot_size

    def send_size(self, image_size):
        self._command(
            bytes([CMD_SIZE, PAD_BYTE]) + struct.pack("<I", image_size), "SIZE")

    def erase(self):
        self._command(bytes([CMD_ERASE]), "ERASE", timeout=ERASE_TIMEOUT_S)

    def send_image(self, image):
        size = len(image)
        n_windows = window_count(size)
        started = time.monotonic()
        window = 0
        attempts = 0
        while window < n_windows:
            if attempts > self.retries:
                raise FlashError(
                    "DATA window %d not accepted after %d attempts" % (window, attempts))
            reply = self._send_window(image, window)
            if reply is None:
                reply = self._wait_reply(CMD_DATA, ACK_TIMEOUT_S)
            if reply is None:
                # ACK lost or the whole window lost: resend. If the bootloader
                # HAD accepted it, the resend draws a NAK 0x01 pointing at the
                # NEXT window's start seq and the rewind below advances us.
                attempts += 1
                self._drain()
                continue
            err, arg, _ = reply
            if err == ACK:
                first = window * FRAMES_PER_WINDOW
                last_seq = (first + window_frame_count(size, window) - 1) % SEQ_MODULUS
                if arg != last_seq:
                    attempts += 1  # stale ACK from an earlier resend; resync
                    self._drain()
                    continue
                window += 1
                attempts = 0
                self._report_progress(size, window, started)
            elif err == ERR_UNEXPECTED_SEQ:
                # The bootloader NAKs every frame after a seq gap; flush the
                # burst before resending or we would misread stale NAKs.
                self._drain()
                target = rewind_window(window, n_windows, arg)
                if target is None:
                    raise FlashError(
                        "bootloader asked to rewind to unknown seq 0x%02X" % arg)
                attempts = 0 if target > window else attempts + 1
                window = target
            elif err == ERR_FLASH_FAILURE:
                # Bootloader discarded the window and stays in transfer: resend.
                attempts += 1
                self._drain()
            else:
                raise FlashError("DATA window %d rejected: %s" % (window, _err_name(err)))
        if self.progress is None:
            sys.stdout.write("\n")

    def verify(self, crc32):
        self._command(
            bytes([CMD_VERIFY, PAD_BYTE]) + struct.pack("<I", crc32), "VERIFY")

    def go(self):
        self._command(bytes([CMD_GO]), "GO")

    def query_info(self, timeout=ACK_TIMEOUT_S):
        """Ask a parked bootloader to identify itself (INFO, CMD 0x07).

        Reply is a header frame {0x79, 0x07, name_len, proto, FF...} followed by
        the 32-byte product-name field, 8 bytes/frame, 0x00-padded past name_len.
        Returns (name, name_len, protocol_version)."""
        for _ in range(self.retries + 1):
            self._drain()
            self._send(bytes([CMD_INFO]))
            header = self._wait_reply(CMD_INFO, timeout)
            if header is None:
                continue
            name_len, proto, _ = header  # byte2 = name_len, byte3 = protocol ver
            name = bytearray()
            for _ in range(INFO_NAME_FRAMES):
                msg = self.bus.recv(timeout)
                if (msg is None or msg.arbitration_id != self.id_rx
                        or msg.is_error_frame or msg.is_remote_frame
                        or msg.dlc != FRAME_DLC):
                    break
                name.extend(bytes(msg.data))
            if len(name) != INFO_NAME_FIELD or name_len > INFO_NAME_FIELD:
                continue  # short/corrupt burst -- retry the whole INFO exchange
            return name[:name_len].decode("ascii", "replace"), name_len, proto
        raise FlashError("no INFO reply after %d attempts" % (self.retries + 1))

    # -- helpers -------------------------------------------------------------
    def _send_window(self, image, window):
        """Send every DATA frame of one window, polling for an early reply
        after each frame -- the bootloader NAKs immediately on a seq gap, so
        aborting the window early avoids flooding its 4-deep RX FIFO with
        frames it will reject anyway. Returns the reply or None."""
        first = window * FRAMES_PER_WINDOW
        count = window_frame_count(len(image), window)
        for i, frame_index in enumerate(range(first, first + count)):
            offset, valid = frame_payload_range(len(image), frame_index)
            self._send(bytes([CMD_DATA, frame_index % SEQ_MODULUS])
                       + image[offset:offset + valid])
            # Block briefly every EARLY_POLL_EVERY_FRAMES frames so pending
            # NAKs are actually delivered (see the constant's comment).
            yield_now = (i % EARLY_POLL_EVERY_FRAMES) == EARLY_POLL_EVERY_FRAMES - 1
            reply = self._poll_data_reply(EARLY_POLL_TIMEOUT_S if yield_now else 0)
            if reply is not None:
                return reply
        return None

    def _poll_data_reply(self, timeout):
        """Short-poll for a DATA ACK/NAK. Returns (err, arg, raw) or None."""
        msg = self.bus.recv(timeout)
        if (msg is None or msg.arbitration_id != self.id_rx
                or msg.is_error_frame or msg.is_remote_frame
                or msg.dlc != FRAME_DLC):
            return None
        data = bytes(msg.data)
        if data[0] != REPLY_MAGIC or data[1] != CMD_DATA:
            return None
        return data[2], data[3], data

    def _drain(self, quiet_s=0.15):
        """Flush queued replies (e.g. the per-frame NAK burst after a seq gap)
        until the RX side has been quiet for `quiet_s`."""
        while self.bus.recv(quiet_s) is not None:
            pass

    def _report_progress(self, size, windows_done, started):
        done = min(windows_done * FLASH_BLOCK_SIZE, size)
        if self.progress is not None:
            self.progress(done / size)
            return
        elapsed = time.monotonic() - started
        sys.stdout.write("\r  %5.1f%%  %d/%d bytes  %.1fs"
                         % (done * 100.0 / size, done, size, elapsed))
        sys.stdout.flush()


def build_bus_kwargs(interface, channel, bitrate):
    """Assemble can.Bus kwargs, omitting interface/channel so python-can's own
    config file can supply them when the caller leaves them blank."""
    kwargs = {"bitrate": bitrate}
    if interface:
        kwargs["interface"] = interface
    if channel:
        kwargs["channel"] = channel
    return kwargs


def flash_image(bus, image, id_tx, id_rx, retries, send_go, log=print, progress=None):
    """Run the full update flow. Returns (crc32, slot_size).

    `log(text)` receives each status line (defaults to print, so the CLI output
    is unchanged); `progress(fraction)`, when given, receives the 0..1 DATA
    progress instead of the inline stdout progress line (used by the TUI)."""
    flasher = CanFlasher(bus, id_tx, id_rx, retries, progress=progress)

    slot_size = flasher.hello()
    log("HELLO ok: protocol v%d, app slot %d bytes" % (PROTOCOL_VERSION, slot_size))
    if slot_size != EXPECTED_SLOT_SIZE:
        log("  note: announced slot differs from the expected 0x%08X"
            % EXPECTED_SLOT_SIZE)
    if len(image) > slot_size:
        raise FlashError("image is %d bytes but the app slot is only %d bytes"
                         % (len(image), slot_size))

    flasher.send_size(len(image))
    log("SIZE ok: %d bytes" % len(image))

    log("ERASE... (up to %.0f s)" % ERASE_TIMEOUT_S)
    flasher.erase()
    log("ERASE ok")

    log("DATA: %d windows of up to %d bytes"
        % (window_count(len(image)), FLASH_BLOCK_SIZE))
    flasher.send_image(image)

    crc32 = zlib.crc32(image) & 0xFFFFFFFF
    flasher.verify(crc32)
    log("VERIFY ok: CRC32 0x%08X" % crc32)

    if send_go:
        flasher.go()
        log("GO sent -- application starting")
    else:
        log("GO skipped (--no-go)")
    return crc32, slot_size


# ---------------------------------------------------------------------------
# Discovery: find every parked board, each on its own boot_cmd_id/boot_reply_id
# ---------------------------------------------------------------------------
def build_scan_ids(scan_from, scan_to):
    """Candidate cmd IDs to probe: the [scan_from, scan_to] band plus the legacy
    0x79E and the known-device cmd IDs (always included), sorted and de-duplicated."""
    return sorted(set(range(scan_from, scan_to + 1))
                  | {DEFAULT_ID_TX} | set(KNOWN_DEVICE_CMD_IDS))


def probe_info(bus, cmd_id, timeout):
    """Send one INFO (CMD 0x07) on cmd_id and listen BROADLY for the 5-frame
    reply on ANY arbitration id -- a parked board's reply id is arbitrary.

    A hit is a header frame {0x79, 0x07, name_len, proto, FF, FF, FF, FF} on some
    id, followed by 4 frames on that SAME id carrying the 32-byte product-name
    field. Returns (reply_id, name, name_len, proto) or None on timeout / a
    malformed reply. The bus must be opened WITHOUT a single reply-id filter."""
    while bus.recv(0.0) is not None:
        pass  # flush stale traffic before probing this id

    bus.send(can.Message(arbitration_id=cmd_id, is_extended_id=False,
                         data=_pad_frame(bytes([CMD_INFO]))))

    deadline = time.monotonic() + timeout
    reply_id = name_len = proto = None
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            return None
        msg = bus.recv(remaining)
        if msg is None:
            return None
        if (msg.is_error_frame or msg.is_remote_frame or msg.dlc != FRAME_DLC):
            continue
        data = bytes(msg.data)
        if data[0] == REPLY_MAGIC and data[1] == CMD_INFO:
            reply_id, name_len, proto = msg.arbitration_id, data[2], data[3]
            break

    name = bytearray()
    for _ in range(INFO_NAME_FRAMES):
        remaining = deadline - time.monotonic()
        msg = bus.recv(remaining if remaining > 0 else 0.0)
        if (msg is None or msg.arbitration_id != reply_id
                or msg.is_error_frame or msg.is_remote_frame
                or msg.dlc != FRAME_DLC):
            return None  # incomplete/foreign burst -- treat this id as no-hit
        name.extend(bytes(msg.data))
    if len(name) != INFO_NAME_FIELD or name_len > INFO_NAME_FIELD:
        return None
    return reply_id, name[:name_len].decode("ascii", "replace"), name_len, proto


def scan_boards(bus, cmd_ids, timeout=SCAN_PROBE_TIMEOUT_S):
    """INFO-probe every candidate cmd id, one at a time (so at most one board can
    answer each), and return the parked boards found, in cmd-id order:
    [(cmd_id, reply_id, name, name_len, proto), ...]. The bus must be opened for
    BROAD listening (no single reply-id filter)."""
    boards = []
    for cmd_id in cmd_ids:
        hit = probe_info(bus, cmd_id, timeout)
        if hit is not None:
            reply_id, name, name_len, proto = hit
            boards.append((cmd_id, reply_id, name, name_len, proto))
    return boards


def print_scan_table(boards, scan_from, scan_to, total_ids):
    print("Scanned %d candidate cmd IDs (band 0x%03X..0x%03X plus legacy 0x%03X); "
          "found %d parked board(s)."
          % (total_ids, scan_from, scan_to, DEFAULT_ID_TX, len(boards)))
    if not boards:
        print("No board answered INFO. Check power, wiring, bitrate, and that the "
              "boards are parked in the bootloader.")
        return
    print()
    print("  %-3s %-32s %-8s %-9s %s" % ("#", "NAME", "CMD_ID", "REPLY_ID", "PROTO"))
    for i, (cmd_id, reply_id, name, _, proto) in enumerate(boards, 1):
        print("  %-3d %-32s 0x%03X    0x%03X     v%d"
              % (i, name, cmd_id, reply_id, proto))
    print()
    prog = os.path.basename(sys.argv[0]) or "can_flash.py"
    print("Flash one with:  %s app.bin --id-tx 0x%03X --id-rx 0x%03X "
          "[--interface ... --channel ...]"
          % (prog, boards[0][0], boards[0][1]))


def enter_bootloader(bus, board_rx, board_tx):
    """Reboot a RUNNING app into the CAN bootloader via the Airform
    '/enter_bootloader' RPC, sent as standard ISO-TP.

    The device's handler resets the MCU and never replies, so this is
    fire-and-forget: it returns once the ISO-TP transfer (First Frame ->
    Flow Control -> Consecutive Frames) has been acknowledged at the
    transport layer. It does NOT wait for an application-level response."""
    try:
        import isotp
    except ImportError:
        raise FlashError(
            "the --enter-bootloader pre-step needs the 'can-isotp' package; "
            "run this tool with 'uv run' (deps are declared inline) or "
            "'pip install can-isotp'")

    payload = encode_uart_packet(ENTER_BOOTLOADER_PATH)
    # txid = the board's RX id (host->board); rxid = the board's TX id, where
    # its Flow Control frame arrives. tx_padding pads every frame to 8 bytes,
    # matching the library's fixed-DLC-8 frames.
    addr = isotp.Address(isotp.AddressingMode.Normal_11bits,
                         txid=board_rx, rxid=board_tx)
    stack = isotp.CanStack(bus, address=addr,
                           params={"blocking_send": True, "tx_padding": 0x00})
    stack.start()
    try:
        stack.send(payload, send_timeout=ISOTP_SEND_TIMEOUT_S)
    except isotp.BlockingSendFailure as exc:
        raise FlashError(
            "ISO-TP send of %s to board RX 0x%X failed (%s) -- no Flow Control "
            "from the board: wrong --board-rx/--board-tx, or the app isn't "
            "running the /enter_bootloader route?"
            % (ENTER_BOOTLOADER_PATH, board_rx, exc))
    finally:
        stack.stop()


# ---------------------------------------------------------------------------
# Selftest: in-process fake bootloader on python-can's 'virtual' interface
# ---------------------------------------------------------------------------
class FakeBootloader(threading.Thread):
    """Protocol v1 bootloader simulation over a bytearray 'flash'.

    Once per run it pretends to lose DATA frame `drop_frame` and answers the
    enclosing window with NAK 0x01, exercising the host's rewind path.
    """

    def __init__(self, channel, id_host, id_boot,
                 slot_size=EXPECTED_SLOT_SIZE, drop_frame=SELFTEST_DROP_FRAME,
                 name="UNO R4 Minima"):
        super().__init__(name="fake-bootloader", daemon=True)
        self.bus = can.Bus(interface="virtual", channel=channel)
        self.bus.set_filters([{"can_id": id_host, "can_mask": 0x7FF, "extended": False}])
        self.id_boot = id_boot
        self.slot_size = slot_size
        self.drop_frame = drop_frame
        self.product_name = name
        self.flash = bytearray([PAD_BYTE]) * slot_size

        self.image_size = None
        self.rewind_naks_sent = 0
        self.verify_ok = False
        self.go_received = False

        self._state = "idle"
        self._expect_frame = 0      # next global DATA frame index expected
        self._window = 0
        self._block_buf = bytearray(FLASH_BLOCK_SIZE)
        self._drop_done = False
        self._stop_requested = threading.Event()  # do not shadow Thread._stop

    def stop(self):
        self._stop_requested.set()
        self.join(timeout=2.0)
        self.bus.shutdown()

    def run(self):
        while not self._stop_requested.is_set():
            msg = self.bus.recv(0.1)
            if msg is None or msg.dlc != FRAME_DLC:
                continue
            self._handle(bytes(msg.data))

    # -- internals -----------------------------------------------------------
    def _reply(self, cmd, err, tail=b""):
        self.bus.send(can.Message(
            arbitration_id=self.id_boot,
            is_extended_id=False,
            data=_pad_frame(bytes([REPLY_MAGIC, cmd, err]) + tail),
        ))

    def _handle(self, data):
        cmd = data[0]
        if cmd == CMD_HELLO:
            self._state = "hello"
            self._reply(CMD_HELLO, ACK,
                        bytes([PROTOCOL_VERSION]) + struct.pack("<I", self.slot_size))
        elif cmd == CMD_SIZE:
            if self._state not in ("hello", "sized"):
                self._reply(CMD_SIZE, ERR_BAD_STATE)
                return
            size = struct.unpack_from("<I", data, 2)[0]
            if size == 0 or size > self.slot_size:
                self._reply(CMD_SIZE, ERR_SIZE_BOUNDS)
                return
            self.image_size = size
            self._state = "sized"
            self._reply(CMD_SIZE, ACK)
        elif cmd == CMD_ERASE:
            if self._state not in ("sized", "data"):
                self._reply(CMD_ERASE, ERR_BAD_STATE)
                return
            time.sleep(SELFTEST_ERASE_DELAY_S)  # stand-in for the 120-block erase
            self.flash[:] = bytes([PAD_BYTE]) * self.slot_size
            self._expect_frame = 0
            self._window = 0
            self._state = "data"
            self._reply(CMD_ERASE, ACK)
        elif cmd == CMD_DATA:
            if self._state != "data":
                self._reply(CMD_DATA, ERR_BAD_STATE)
                return
            self._handle_data_frame(data)
        elif cmd == CMD_VERIFY:
            if self._state != "flashed":
                self._reply(CMD_VERIFY, ERR_BAD_STATE)
                return
            expected = struct.unpack_from("<I", data, 2)[0]
            actual = zlib.crc32(bytes(self.flash[:self.image_size])) & 0xFFFFFFFF
            if actual != expected:
                self._reply(CMD_VERIFY, ERR_CRC_MISMATCH)
                return
            self.verify_ok = True
            self._state = "verified"
            self._reply(CMD_VERIFY, ACK)
        elif cmd == CMD_GO:
            if self._state != "verified":
                self._reply(CMD_GO, ERR_BAD_STATE)
                return
            self.go_received = True
            self._reply(CMD_GO, ACK)
        elif cmd == CMD_INFO:
            self._reply_info()

    def _reply_info(self):
        """INFO reply: header frame {0x79, 0x07, name_len, proto, FF...} then the
        32-byte product-name field, 8 bytes/frame, 0x00-padded past the name."""
        name = self.product_name.encode("ascii")[:INFO_NAME_FIELD]
        field = name + bytes(INFO_NAME_FIELD - len(name))
        self._reply(CMD_INFO, len(name), bytes([PROTOCOL_VERSION]))
        for i in range(INFO_NAME_FRAMES):
            self.bus.send(can.Message(
                arbitration_id=self.id_boot, is_extended_id=False,
                data=field[i * FRAME_DLC:(i + 1) * FRAME_DLC]))

    def _handle_data_frame(self, data):
        if self._expect_frame == self.drop_frame and not self._drop_done:
            self._drop_done = True  # pretend the frame never arrived
            return
        seq = data[1]
        window_first = self._window * FRAMES_PER_WINDOW
        if seq != self._expect_frame % SEQ_MODULUS:
            # Firmware behaviour: NAK immediately on EVERY out-of-order frame,
            # arg = the window's first seq; the window buffer is discarded and
            # retransmission is expected from that seq.
            self.rewind_naks_sent += 1
            self._expect_frame = window_first
            self._reply(CMD_DATA, ERR_UNEXPECTED_SEQ,
                        bytes([window_first % SEQ_MODULUS]))
            return
        offset, valid = frame_payload_range(self.image_size, self._expect_frame)
        block_offset = offset - self._window * FLASH_BLOCK_SIZE
        self._block_buf[block_offset:block_offset + valid] = data[2:2 + valid]
        self._expect_frame += 1

        frames_expected = window_frame_count(self.image_size, self._window)
        if self._expect_frame < window_first + frames_expected:
            return  # mid-window frames get no reply
        last_seq = (self._expect_frame - 1) % SEQ_MODULUS
        block_offset = self._window * FLASH_BLOCK_SIZE
        block_bytes = min(FLASH_BLOCK_SIZE, self.image_size - block_offset)
        self.flash[block_offset:block_offset + block_bytes] = self._block_buf[:block_bytes]
        self._window += 1
        if self._window == window_count(self.image_size):
            self._state = "flashed"
        self._reply(CMD_DATA, ACK, bytes([last_seq]))


def check_enter_bootloader_vector():
    """Golden vector: the encoded '/enter_bootloader' Uart_Packet must match,
    byte for byte, what Uart_Communication's Packet.cpp would deserialize.

      55            header (0x55)
      00 11         address_size = 17 (u16 BE)
      2F..72        "/enter_bootloader" (17 ASCII bytes)
      00 00         body_size = 0 (u16 BE)
      55            crc = 0x55 ^ 0x00 ^ 0x00 (empty body)
    """
    expected = bytes([
        0x55, 0x00, 0x11,
        0x2F, 0x65, 0x6E, 0x74, 0x65, 0x72, 0x5F, 0x62, 0x6F, 0x6F,
        0x74, 0x6C, 0x6F, 0x61, 0x64, 0x65, 0x72,
        0x00, 0x00, 0x55,
    ])
    actual = encode_uart_packet(ENTER_BOOTLOADER_PATH)
    if actual != expected:
        raise FlashError(
            "selftest FAILED: /enter_bootloader Uart_Packet encoding mismatch\n"
            "  expected %s\n  actual   %s" % (expected.hex(" "), actual.hex(" ")))
    print("selftest: /enter_bootloader Uart_Packet golden vector OK (%d bytes)"
          % len(actual))


def check_rewind_window_beyond_128():
    """Window start seqs repeat every 128 windows; a rewind must still resolve
    to the current or next window in a slot as large as the ESP32 OTA slot."""
    n_windows = window_count(EXPECTED_SLOT_SIZE)
    for window in (0, 127, 128, 130, n_windows - 2):
        same = rewind_window(window, n_windows, window_start_seq(window))
        nxt = rewind_window(window, n_windows, window_start_seq(window + 1))
        if same != window or nxt != window + 1:
            raise FlashError("selftest FAILED: rewind for window %d resolved to "
                             "%r/%r" % (window, same, nxt))
    if rewind_window(n_windows - 1, n_windows, window_start_seq(n_windows)) is not None:
        raise FlashError("selftest FAILED: rewind past the last window accepted")
    print("selftest: rewind resolution OK across %d windows" % n_windows)


def run_selftest(retries):
    import random

    check_enter_bootloader_vector()
    check_rewind_window_beyond_128()

    print("selftest: %d-byte image, virtual bus, forced frame drop at frame %d"
          % (SELFTEST_IMAGE_SIZE, SELFTEST_DROP_FRAME))
    image = bytes(random.Random(0xB007CAFE).randrange(256)
                  for _ in range(SELFTEST_IMAGE_SIZE))

    fake = FakeBootloader(SELFTEST_CHANNEL, DEFAULT_ID_TX, DEFAULT_ID_RX)
    fake.start()
    bus = can.Bus(interface="virtual", channel=SELFTEST_CHANNEL)
    bus.set_filters([{"can_id": DEFAULT_ID_RX, "can_mask": 0x7FF, "extended": False}])
    try:
        crc32, slot_size = flash_image(
            bus, image, DEFAULT_ID_TX, DEFAULT_ID_RX, retries, send_go=True)
    finally:
        fake.stop()
        bus.shutdown()

    def check(condition, message):
        if not condition:
            raise FlashError("selftest FAILED: " + message)

    check(slot_size == EXPECTED_SLOT_SIZE, "announced slot size mismatch")
    check(fake.image_size == len(image), "SIZE not registered by fake bootloader")
    check(bytes(fake.flash[:len(image)]) == image, "flashed bytes differ from the image")
    check(all(b == PAD_BYTE for b in fake.flash[len(image):len(image) + 64]),
          "bytes past the image end were written")
    check(fake.rewind_naks_sent >= 1, "rewind path was not exercised")
    check(fake.verify_ok, "VERIFY did not succeed on the fake bootloader")
    check(crc32 == (zlib.crc32(image) & 0xFFFFFFFF), "host-side CRC mismatch")
    check(fake.go_received, "GO was not received")
    print("selftest PASS: image verified in fake flash, rewind path exercised")

    run_scan_selftest()
    run_tui_smoke_test()


def run_scan_selftest():
    """Bring up two fake bootloaders on distinct cmd/reply id pairs over the
    virtual bus and assert --scan's broad listen finds BOTH (correct name, cmd,
    reply) while ignoring a decoy cmd id that no board answers."""
    print("selftest: scan discovery with %d fake boards on distinct cmd/reply IDs"
          % len(SELFTEST_SCAN_BOARDS))
    fakes = [FakeBootloader(SELFTEST_SCAN_CHANNEL, cmd, reply, name=name)
             for cmd, reply, name in SELFTEST_SCAN_BOARDS]
    for fake in fakes:
        fake.start()
    bus = can.Bus(interface="virtual", channel=SELFTEST_SCAN_CHANNEL)
    # Broad listen: NO single reply-id filter -- reply ids here are arbitrary.
    try:
        # Probe with the DEFAULT scan set (no --scan-from/--scan-to overrides):
        # the two band boards fall in 0x700..0x71F while the known-device board on
        # cmd 0x07 is reachable ONLY via KNOWN_DEVICE_CMD_IDS -- so finding it
        # proves the default scan covers provisioned known boards. Empty in-band
        # ids (e.g. 0x702) sit among the probes, so a clean count rules out
        # false positives.
        probe_ids = build_scan_ids(DEFAULT_SCAN_FROM, DEFAULT_SCAN_TO)
        boards = scan_boards(bus, probe_ids, timeout=SELFTEST_SCAN_TIMEOUT_S)
    finally:
        for fake in fakes:
            fake.stop()
        bus.shutdown()

    def check(condition, message):
        if not condition:
            raise FlashError("selftest FAILED: " + message)

    found = {(cmd, reply, name) for cmd, reply, name, _, _ in boards}
    for cmd, reply, name in SELFTEST_SCAN_BOARDS:
        check((cmd, reply, name) in found,
              "scan did not find %r on cmd 0x%X -> reply 0x%X" % (name, cmd, reply))
    check(len(boards) == len(SELFTEST_SCAN_BOARDS),
          "scan reported %d boards, expected %d (in-band false-positive?)"
          % (len(boards), len(SELFTEST_SCAN_BOARDS)))
    default_ids = set(build_scan_ids(DEFAULT_SCAN_FROM, DEFAULT_SCAN_TO))
    check(DEFAULT_ID_TX in default_ids,
          "default scan set omits the legacy 0x%03X cmd id" % DEFAULT_ID_TX)
    check(set(KNOWN_DEVICE_CMD_IDS) <= default_ids,
          "default scan set omits the known-device cmd IDs")
    print("selftest PASS: default scan found all boards with correct "
          "name/cmd/reply, including the known-device cmd id, no false positives")


def run_tui_smoke_test():
    """Headless check (no terminal) that the TUI mounts with its key widgets,
    and that bare invocation / --tui route to the TUI instead of headless."""
    import asyncio

    from textual.widgets import Button, Input, ProgressBar, RichLog, Switch

    # Arg routing: no args -> TUI; --tui flag -> TUI; a firmware path -> headless.
    if parse_args(["--tui"]).tui is not True:
        raise FlashError("selftest FAILED: --tui did not set the TUI route")
    if parse_args(["app.bin"]).tui is not False:
        raise FlashError("selftest FAILED: a firmware path wrongly set the TUI route")

    async def _drive():
        app = FlasherApp()
        async with app.run_test() as pilot:
            for wid, kind in (
                ("#interface_input", Input), ("#channel_input", Input),
                ("#bitrate_input", Input), ("#bin_input", Input),
                ("#board_rx_input", Input), ("#board_tx_input", Input),
                ("#enter_bl_switch", Switch), ("#no_go_switch", Switch),
                ("#identify", Button), ("#flash", Button), ("#browse_bin", Button),
                ("#progress", ProgressBar), ("#log", RichLog),
            ):
                app.query_one(wid, kind)
            if app.query_one("#interface_input", Input).value != DEFAULT_INTERFACE:
                raise FlashError("selftest FAILED: interface input default wrong")
            if app.query_one("#channel_input", Input).value != DEFAULT_CHANNEL:
                raise FlashError("selftest FAILED: channel input default wrong")
            await pilot.pause()

    asyncio.run(_drive())
    print("selftest: TUI mounts headless with all key widgets; arg routing OK")


# ---------------------------------------------------------------------------
# Textual TUI (bare `uv run can_flash.py` launches this)
# ---------------------------------------------------------------------------
from pathlib import Path

from textual import work
from textual.app import App, ComposeResult
from textual.containers import Horizontal, Vertical
from textual.screen import ModalScreen
from textual.widgets import (
    Button,
    DirectoryTree,
    Footer,
    Header,
    Input,
    Label,
    ProgressBar,
    RichLog,
    Static,
    Switch,
)


class _BinDirectoryTree(DirectoryTree):
    """Directory tree showing only directories and .bin files matching a filter."""

    name_filter = ""

    def filter_paths(self, paths):
        needle = self.name_filter.lower()
        return [
            p for p in paths
            if (p.is_dir() and not p.name.startswith("."))
            or (p.suffix.lower() == ".bin" and needle in p.name.lower())
        ]


class BinPickerScreen(ModalScreen):
    """Modal file dialog: type a path, browse the tree, filter .bin files by name."""

    BINDINGS = [("escape", "cancel", "Cancel")]

    CSS = """
    BinPickerScreen { align: center middle; }
    #picker_dialog {
        width: 90;
        max-width: 95%;
        height: 80%;
        background: $panel;
        border: round $primary;
        padding: 1 2;
    }
    #picker_dialog Input { margin: 0 0 1 0; }
    #picker_tree { height: 1fr; border: round $accent; }
    #picker_buttons { height: auto; align: right middle; padding: 1 0 0 0; }
    #picker_buttons Button { margin: 0 0 0 1; }
    """

    def __init__(self, start):
        super().__init__()
        self._start = start
        self._chosen = None

    def compose(self):
        with Vertical(id="picker_dialog"):
            yield Input(value=str(self._start), placeholder="directory to browse", id="picker_path")
            yield Input(placeholder="filter .bin file names...", id="picker_filter")
            yield _BinDirectoryTree(self._start, id="picker_tree")
            with Horizontal(id="picker_buttons"):
                yield Button("Select", id="picker_select", variant="primary", disabled=True)
                yield Button("Cancel", id="picker_cancel")

    def on_mount(self):
        self.query_one("#picker_dialog", Vertical).border_title = "Select firmware .bin"

    def on_input_submitted(self, event):
        if event.input.id != "picker_path":
            return
        new_root = Path(event.value).expanduser()
        if new_root.is_dir():
            self.query_one(_BinDirectoryTree).path = new_root
        else:
            self.notify("Not a directory: %s" % new_root, severity="error")

    def on_input_changed(self, event):
        if event.input.id != "picker_filter":
            return
        tree = self.query_one(_BinDirectoryTree)
        tree.name_filter = event.value
        tree.reload()

    def on_directory_tree_file_selected(self, event):
        self._chosen = event.path
        self.query_one("#picker_select", Button).disabled = False

    def on_button_pressed(self, event):
        if event.button.id == "picker_select":
            self.dismiss(self._chosen)
        elif event.button.id == "picker_cancel":
            self.dismiss(None)

    def action_cancel(self):
        self.dismiss(None)


class FlasherApp(App):
    """Flash a .bin over CAN to the UNO R4 Minima bootloader (Protocol v1)."""

    TITLE = "CAN Flasher"
    SUB_TITLE = "UNO R4 Minima CAN bootloader  -  Protocol v1"

    CSS = """
    Screen { background: $surface; }
    #panels { height: auto; padding: 1 2 0 2; }
    .panel {
        width: 1fr;
        height: auto;
        margin: 0 1 0 0;
        padding: 1 2;
        background: $panel;
        border: round $primary;
    }
    .panel:last-of-type { margin: 0; }
    #device_status { height: auto; padding: 0 0 1 0; }
    .ok { color: $success; text-style: bold; }
    .warn { color: $warning; text-style: bold; }
    .err { color: $error; text-style: bold; }
    .row { height: auto; padding: 1 0 0 0; }
    .row Label { padding: 1 1 0 0; width: auto; }
    .row Button { margin: 0 1 0 0; }
    .row Switch { margin: 0 1 0 0; }
    .row Input { width: 1fr; }
    #bin_input { width: 1fr; margin: 0 1 0 0; }
    #footer_row { height: auto; padding: 1 2; align: left middle; }
    #progress { width: 1fr; margin: 0 2 0 0; }
    RichLog {
        height: 1fr;
        margin: 0 2 1 2;
        border: round $accent;
        background: $surface-darken-1;
        padding: 0 1;
    }
    """

    BINDINGS = [
        ("i", "identify", "Identify"),
        ("f", "flash", "Flash"),
        ("b", "browse", "Browse .bin"),
        ("q", "quit", "Quit"),
    ]

    def __init__(self, interface=None, channel=None, bitrate=DEFAULT_BITRATE,
                 firmware=None, id_tx=DEFAULT_ID_TX, id_rx=DEFAULT_ID_RX,
                 retries=DEFAULT_RETRIES, board_rx=DEFAULT_BOARD_RX,
                 board_tx=DEFAULT_BOARD_TX, no_go=False):
        super().__init__()
        self._interface = interface or DEFAULT_INTERFACE
        self._channel = channel or DEFAULT_CHANNEL
        self._bitrate = bitrate
        self._firmware = firmware or ""
        self._id_tx = id_tx
        self._id_rx = id_rx
        self._retries = retries
        self._board_rx = board_rx
        self._board_tx = board_tx
        self._no_go = no_go

    def compose(self):
        yield Header()
        with Horizontal(id="panels"):
            with Vertical(id="bus_panel", classes="panel"):
                yield Static("Device: not identified", id="device_status")
                with Horizontal(classes="row"):
                    yield Label("Interface:")
                    yield Input(value=self._interface, id="interface_input")
                with Horizontal(classes="row"):
                    yield Label("Channel:")
                    yield Input(value=self._channel, id="channel_input")
                with Horizontal(classes="row"):
                    yield Label("Bitrate:")
                    yield Input(value=str(self._bitrate), id="bitrate_input")
                with Horizontal(classes="row"):
                    yield Button("Identify", id="identify", variant="primary")

            with Vertical(id="fw_panel", classes="panel"):
                with Horizontal(classes="row"):
                    yield Label("Firmware (.bin):")
                    yield Input(value=self._firmware, placeholder="path to .bin", id="bin_input")
                    yield Button("Browse", id="browse_bin")
                with Horizontal(classes="row"):
                    yield Label("Board RX:")
                    yield Input(value="0x%02X" % self._board_rx, id="board_rx_input")
                    yield Label("Board TX:")
                    yield Input(value="0x%02X" % self._board_tx, id="board_tx_input")
                with Horizontal(classes="row"):
                    yield Switch(value=False, id="enter_bl_switch")
                    yield Label("Enter bootloader first (reboot running app)")
                with Horizontal(classes="row"):
                    yield Switch(value=self._no_go, id="no_go_switch")
                    yield Label("Skip GO (flash + verify only)")

        with Horizontal(id="footer_row"):
            yield ProgressBar(total=100, show_eta=False, id="progress")
            yield Button("Flash", id="flash", variant="success")
            yield Button("Quit", id="quit", variant="error")
        yield RichLog(id="log", markup=True, highlight=True)
        yield Footer()

    def on_mount(self):
        self.query_one("#bus_panel").border_title = "CAN bus"
        self.query_one("#fw_panel").border_title = "Firmware & target"
        self.query_one("#log", RichLog).border_title = "Log"
        self._log("Ready. Set the bus, pick a .bin, then [b]Flash[/b]. "
                  "[b]Identify[/b] queries a parked bootloader (CMD_INFO 0x07).")

    # ----- input helpers -----------------------------------------------------

    def _bus_kwargs(self):
        interface = self.query_one("#interface_input", Input).value.strip()
        channel = self.query_one("#channel_input", Input).value.strip()
        bitrate = int(self.query_one("#bitrate_input", Input).value.strip(), 0)
        return build_bus_kwargs(interface, channel, bitrate)

    # ----- actions -----------------------------------------------------------

    def action_identify(self):
        try:
            bus_kwargs = self._bus_kwargs()
        except ValueError:
            self._log("[red]Bitrate must be an integer.[/red]")
            return
        self._busy(True)
        self.query_one("#device_status", Static).set_classes("warn")
        self.query_one("#device_status", Static).update("Device: identifying...")
        self._log("[b]Identify[/b] on %s..." % bus_kwargs)
        self.identify_worker(bus_kwargs, self._id_tx, self._id_rx, self._retries)

    def action_flash(self):
        bin_path = self.query_one("#bin_input", Input).value.strip()
        if not bin_path:
            self._log("[red]Enter a firmware .bin path (or use Browse).[/red]")
            return
        try:
            image = load_image(bin_path)
        except FlashError as exc:
            self._log("[red]%s[/red]" % exc)
            return
        if len(image) > EXPECTED_SLOT_SIZE:
            self._log("[red]Image is %d bytes, larger than the %d-byte app slot.[/red]"
                      % (len(image), EXPECTED_SLOT_SIZE))
            return
        try:
            bus_kwargs = self._bus_kwargs()
            board_rx = int(self.query_one("#board_rx_input", Input).value.strip(), 0)
            board_tx = int(self.query_one("#board_tx_input", Input).value.strip(), 0)
        except ValueError:
            self._log("[red]Bitrate and board RX/TX must be integers.[/red]")
            return
        enter_bl = self.query_one("#enter_bl_switch", Switch).value
        send_go = not self.query_one("#no_go_switch", Switch).value
        self._busy(True)
        self._set_progress(0)
        self._log("[b]Flashing[/b] %s (%d bytes)..." % (bin_path, len(image)))
        self.flash_worker(image, bus_kwargs, self._id_tx, self._id_rx, self._retries,
                          send_go, enter_bl, board_rx, board_tx)

    def action_browse(self):
        current = self.query_one("#bin_input", Input).value
        start = Path(current).expanduser().parent if current else Path.cwd()
        if not start.is_dir():
            start = Path.cwd()

        def apply_choice(chosen):
            if chosen is not None:
                self.query_one("#bin_input", Input).value = str(chosen)

        self.push_screen(BinPickerScreen(start), apply_choice)

    def on_button_pressed(self, event):
        handler = {
            "identify": self.action_identify,
            "flash": self.action_flash,
            "browse_bin": self.action_browse,
            "quit": self.exit,
        }.get(event.button.id)
        if handler:
            handler()

    # ----- workers (off the UI thread) --------------------------------------

    @work(thread=True, exclusive=True, group="can")
    def identify_worker(self, bus_kwargs, id_tx, id_rx, retries):
        try:
            bus = can.Bus(**bus_kwargs)
            bus.set_filters([{"can_id": id_rx, "can_mask": 0x7FF, "extended": False}])
            try:
                flasher = CanFlasher(bus, id_tx, id_rx, retries)
                name, name_len, proto = flasher.query_info()
            finally:
                bus.shutdown()
            self.call_from_thread(
                self._identify_done, True,
                "protocol v%d, product name (%d chars): %r" % (proto, name_len, name))
        except (FlashError, can.CanError) as exc:
            self.call_from_thread(self._identify_done, False, str(exc))
        except Exception as exc:  # noqa: BLE001
            self.call_from_thread(self._identify_done, False, repr(exc))

    @work(thread=True, exclusive=True, group="can")
    def flash_worker(self, image, bus_kwargs, id_tx, id_rx, retries,
                     send_go, enter_bl, board_rx, board_tx):
        def log(message):
            self.call_from_thread(self._log, message)

        def progress(fraction):
            self.call_from_thread(self._set_progress, fraction * 100.0)

        try:
            if enter_bl:
                log("Rebooting running app into the bootloader via %s "
                    "(board RX 0x%X, TX 0x%X)..." % (ENTER_BOOTLOADER_PATH, board_rx, board_tx))
                reboot_bus = can.Bus(**bus_kwargs)
                reboot_bus.set_filters(
                    [{"can_id": board_tx, "can_mask": 0x7FF, "extended": False}])
                try:
                    enter_bootloader(reboot_bus, board_rx, board_tx)
                finally:
                    reboot_bus.shutdown()
                log("RPC sent (no ack expected); waiting %.1f s for reboot..." % REBOOT_WAIT_S)
                time.sleep(REBOOT_WAIT_S)

            bus = can.Bus(**bus_kwargs)
            bus.set_filters([{"can_id": id_rx, "can_mask": 0x7FF, "extended": False}])
            try:
                crc32, _ = flash_image(bus, image, id_tx, id_rx, retries,
                                       send_go=send_go, log=log, progress=progress)
            finally:
                bus.shutdown()
            self.call_from_thread(
                self._flash_done, True,
                "Done: %d bytes, CRC32 0x%08X" % (len(image), crc32))
        except (FlashError, can.CanError) as exc:
            self.call_from_thread(self._flash_done, False, "error: %s" % exc)
        except Exception as exc:  # noqa: BLE001
            self.call_from_thread(self._flash_done, False, repr(exc))

    # ----- UI updates (main thread) -----------------------------------------

    def _identify_done(self, ok, message):
        self._busy(False)
        status = self.query_one("#device_status", Static)
        if ok:
            status.set_classes("ok")
            status.update("Device: [b]%s[/b]" % message)
            self._log("[green]INFO: %s[/green]" % message)
        else:
            status.set_classes("err")
            status.update("Device: [b]no reply[/b]")
            self._log("[yellow]Identify: %s[/yellow]" % message)

    def _flash_done(self, ok, message):
        self._busy(False)
        if ok:
            self._set_progress(100)
            self._log("[green]%s[/green]" % message)
        else:
            self._log("[red]%s[/red]" % message)

    def _busy(self, busy):
        for bid in ("identify", "flash"):
            self.query_one("#%s" % bid, Button).disabled = busy

    def _set_progress(self, value):
        self.query_one("#progress", ProgressBar).update(progress=value)

    def _log(self, message):
        self.query_one("#log", RichLog).write(message)


def run_tui(interface=None, channel=None, bitrate=DEFAULT_BITRATE, firmware=None,
            id_tx=DEFAULT_ID_TX, id_rx=DEFAULT_ID_RX, retries=DEFAULT_RETRIES,
            board_rx=DEFAULT_BOARD_RX, board_tx=DEFAULT_BOARD_TX, no_go=False):
    """Launch the interactive TUI. Returns 0 when the app exits."""
    FlasherApp(interface=interface, channel=channel, bitrate=bitrate,
               firmware=firmware, id_tx=id_tx, id_rx=id_rx, retries=retries,
               board_rx=board_rx, board_tx=board_tx, no_go=no_go).run()
    return 0


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------
def _int_any_base(text):
    return int(text, 0)


def parse_args(argv):
    parser = argparse.ArgumentParser(
        description="Flash a .bin application image over CAN to the "
                    "UNO R4 Minima CAN bootloader (protocol v1).",
        epilog="Examples:  %(prog)s app.bin --interface slcan --channel COM5 | "
               "%(prog)s app.bin --interface socketcan --channel can0 | "
               "%(prog)s --selftest")
    parser.add_argument("firmware", nargs="?",
                        help="application image (.bin); not needed with --selftest")
    parser.add_argument("--interface",
                        help="python-can interface (socketcan, slcan, gs_usb, ...); "
                             "falls back to the python-can config if omitted")
    parser.add_argument("--channel",
                        help="python-can channel (can0, COM5, /dev/ttyACM0, ...)")
    parser.add_argument("--bitrate", type=int, default=DEFAULT_BITRATE,
                        help="CAN bitrate in bit/s (default %(default)s)")
    parser.add_argument("--id-tx", type=_int_any_base, default=DEFAULT_ID_TX,
                        help="host->bootloader CAN ID (default 0x%03X)" % DEFAULT_ID_TX)
    parser.add_argument("--id-rx", type=_int_any_base, default=DEFAULT_ID_RX,
                        help="bootloader->host CAN ID (default 0x%03X)" % DEFAULT_ID_RX)
    parser.add_argument("--retries", type=int, default=DEFAULT_RETRIES,
                        help="retry limit per protocol step (default %(default)s)")
    parser.add_argument("--no-go", action="store_true",
                        help="flash and verify but do not send GO")
    parser.add_argument("--info", action="store_true",
                        help="ask the parked bootloader for its product name "
                             "(INFO, CMD 0x07) and exit; no firmware needed")
    parser.add_argument("--scan", action="store_true",
                        help="discover ALL parked boards -- INFO-probe a band of "
                             "candidate cmd IDs while listening on ANY reply id "
                             "(each board may have its own cmd/reply IDs) -- print "
                             "a table and exit; no firmware needed")
    parser.add_argument("--scan-from", type=_int_any_base, default=DEFAULT_SCAN_FROM,
                        help="first cmd ID to probe with --scan, inclusive "
                             "(default 0x%03X); 0x%03X is always included too"
                             % (DEFAULT_SCAN_FROM, DEFAULT_ID_TX))
    parser.add_argument("--scan-to", type=_int_any_base, default=DEFAULT_SCAN_TO,
                        help="last cmd ID to probe with --scan, inclusive "
                             "(default 0x%03X)" % DEFAULT_SCAN_TO)
    parser.add_argument("--enter-bootloader", action="store_true",
                        help="first reboot a RUNNING app into the CAN "
                             "bootloader via the Airform /enter_bootloader RPC "
                             "(ISO-TP), then flash; with no firmware, just "
                             "reboot into the bootloader and exit")
    parser.add_argument("--board-rx", type=_int_any_base, default=DEFAULT_BOARD_RX,
                        help="target board Airform RX CAN ID for the "
                             "/enter_bootloader RPC (host->board, default 0x%02X)"
                             % DEFAULT_BOARD_RX)
    parser.add_argument("--board-tx", type=_int_any_base, default=DEFAULT_BOARD_TX,
                        help="target board Airform TX CAN ID (board->host Flow "
                             "Control, default 0x%02X)" % DEFAULT_BOARD_TX)
    parser.add_argument("--selftest", action="store_true",
                        help="run the full flow against an in-process fake "
                             "bootloader on a virtual bus (no hardware)")
    parser.add_argument("--tui", action="store_true",
                        help="launch the interactive Textual TUI (also the "
                             "default when the tool is run with no arguments)")
    args = parser.parse_args(argv)
    if (not args.selftest and not args.info and not args.scan
            and not args.enter_bootloader and not args.tui and not args.firmware):
        parser.error("firmware image is required unless --selftest, --info, "
                     "--scan, --enter-bootloader, or --tui is given")
    if args.info and args.enter_bootloader:
        parser.error("--info and --enter-bootloader cannot be combined")
    if args.scan and (args.info or args.enter_bootloader or args.firmware):
        parser.error("--scan lists all parked boards and exits; run it alone, "
                     "then flash a specific board with --id-tx/--id-rx")
    if not (0 <= args.scan_from <= args.scan_to <= CAN_ID_MAX):
        parser.error("--scan-from/--scan-to must be 11-bit CAN IDs (0..0x%03X) "
                     "with scan-from <= scan-to" % CAN_ID_MAX)
    return args


def load_image(path):
    if not os.path.isfile(path):
        raise FlashError("firmware image not found: %s" % path)
    with open(path, "rb") as handle:
        image = handle.read()
    if not image:
        raise FlashError("firmware image is empty: %s" % path)
    return image


def main(argv=None):
    if argv is None:
        argv = sys.argv[1:]
    # Bare invocation -> interactive TUI; any flag/firmware -> headless as before.
    if not argv:
        return run_tui()
    args = parse_args(argv)
    if args.tui:
        return run_tui(interface=args.interface, channel=args.channel,
                       bitrate=args.bitrate, firmware=args.firmware,
                       id_tx=args.id_tx, id_rx=args.id_rx, retries=args.retries,
                       board_rx=args.board_rx, board_tx=args.board_tx,
                       no_go=args.no_go)
    try:
        if args.selftest:
            run_selftest(args.retries)
            return 0

        image = None if (args.info or args.scan or not args.firmware) else load_image(args.firmware)
        bus_kwargs = build_bus_kwargs(args.interface, args.channel, args.bitrate)

        if args.scan:
            scan_ids = build_scan_ids(args.scan_from, args.scan_to)
            print("Scanning %d candidate cmd IDs on %s (up to %.1f s each)..."
                  % (len(scan_ids), bus_kwargs, SCAN_PROBE_TIMEOUT_S))
            bus = can.Bus(**bus_kwargs)
            # Broad listen: NO single reply-id filter -- reply ids are arbitrary.
            try:
                boards = scan_boards(bus, scan_ids)
            finally:
                bus.shutdown()
            print_scan_table(boards, args.scan_from, args.scan_to, len(scan_ids))
            return 0

        if args.enter_bootloader:
            print("Rebooting running app into the bootloader via %s "
                  "(board RX 0x%X, TX 0x%X)..."
                  % (ENTER_BOOTLOADER_PATH, args.board_rx, args.board_tx))
            reboot_bus = can.Bus(**bus_kwargs)
            reboot_bus.set_filters(
                [{"can_id": args.board_tx, "can_mask": 0x7FF, "extended": False}])
            try:
                enter_bootloader(reboot_bus, args.board_rx, args.board_tx)
            finally:
                reboot_bus.shutdown()
            print("  RPC sent (no ack expected); waiting %.1f s for reboot..."
                  % REBOOT_WAIT_S)
            time.sleep(REBOOT_WAIT_S)
            if image is None:
                print("Bootloader entry requested; no firmware to flash.")
                return 0

        started = time.monotonic()
        bus = can.Bus(**bus_kwargs)
        bus.set_filters([{"can_id": args.id_rx, "can_mask": 0x7FF, "extended": False}])
        try:
            if args.info:
                flasher = CanFlasher(bus, args.id_tx, args.id_rx, args.retries)
                name, name_len, proto = flasher.query_info()
                print("INFO: protocol v%d, product name (%d chars): %r"
                      % (proto, name_len, name))
                return 0
            crc32, _ = flash_image(bus, image, args.id_tx, args.id_rx,
                                   args.retries, send_go=not args.no_go)
        finally:
            bus.shutdown()
        elapsed = time.monotonic() - started
        print("Done: %d bytes, CRC32 0x%08X, %.1f s" % (len(image), crc32, elapsed))
        return 0
    except (FlashError, can.CanError) as exc:
        print("error: %s" % exc, file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("\naborted", file=sys.stderr)
        return 130


if __name__ == "__main__":
    sys.exit(main())
