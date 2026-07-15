#!/usr/bin/env python3
"""Host-side CAN flashing tool for the Arduino UNO R4 Minima CAN bootloader.

Streams a .bin application image to the bootloader over classic CAN 2.0B
(11-bit IDs, DLC 8) using the CAN Update Protocol v1:

    HELLO -> SIZE -> ERASE -> DATA windows -> VERIFY -> GO

See README.md in this directory for wiring and adapter-specific usage.

Requires Python 3.9+ and python-can >= 4.0.
"""

# /// script
# requires-python = ">=3.9"
# dependencies = ["python-can>=4.0"]
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

CMD_HELLO = 0x01
CMD_ERASE = 0x02
CMD_DATA = 0x03
CMD_SIZE = 0x04
CMD_VERIFY = 0x05
CMD_GO = 0x06

REPLY_MAGIC = 0x79             # byte0 of every bootloader reply
ACK = 0x00                     # byte2 == 0x00 means ACK, anything else is a NAK err code

ERR_UNEXPECTED_SEQ = 0x01      # arg = seq to rewind to
ERR_FLASH_FAILURE = 0x02
ERR_CRC_MISMATCH = 0x03
ERR_SIZE_BOUNDS = 0x04
ERR_BAD_STATE = 0x05

ERR_NAMES = {
    ERR_UNEXPECTED_SEQ: "unexpected sequence number",
    ERR_FLASH_FAILURE: "flash write/erase failure",
    ERR_CRC_MISMATCH: "CRC mismatch",
    ERR_SIZE_BOUNDS: "image size out of bounds",
    ERR_BAD_STATE: "command received in bad state",
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

EXPECTED_SLOT_SIZE = 0x0003_B800  # 243,712 bytes announced in the HELLO ACK

# --selftest parameters
SELFTEST_CHANNEL = "can_flash_selftest"
SELFTEST_IMAGE_SIZE = 2 * FLASH_BLOCK_SIZE + 1003  # 2 full windows + short final window/frame
SELFTEST_DROP_FRAME = 400      # fake bootloader "loses" this frame once -> forces a rewind NAK
SELFTEST_ERASE_DELAY_S = 0.2


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


def _pad_frame(data):
    return bytes(data) + bytes([PAD_BYTE]) * (FRAME_DLC - len(data))


def _err_name(err):
    return ERR_NAMES.get(err, "unknown error 0x%02X" % err)


# ---------------------------------------------------------------------------
# Host-side flasher
# ---------------------------------------------------------------------------
class CanFlasher:
    def __init__(self, bus, id_tx, id_rx, retries):
        self.bus = bus
        self.id_tx = id_tx
        self.id_rx = id_rx
        self.retries = retries

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
        # Window start seqs are unique across the transfer: consecutive starts
        # differ by 342 mod 256 = 86, which has period 128 windows, and the
        # app slot holds at most 119 blocks -- so a rewind seq maps to exactly
        # one window.
        seq_to_window = {(w * FRAMES_PER_WINDOW) % SEQ_MODULUS: w
                         for w in range(n_windows)}
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
                self._print_progress(size, window, started)
            elif err == ERR_UNEXPECTED_SEQ:
                # The bootloader NAKs every frame after a seq gap; flush the
                # burst before resending or we would misread stale NAKs.
                self._drain()
                target = seq_to_window.get(arg)
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
        sys.stdout.write("\n")

    def verify(self, crc32):
        self._command(
            bytes([CMD_VERIFY, PAD_BYTE]) + struct.pack("<I", crc32), "VERIFY")

    def go(self):
        self._command(bytes([CMD_GO]), "GO")

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

    @staticmethod
    def _print_progress(size, windows_done, started):
        done = min(windows_done * FLASH_BLOCK_SIZE, size)
        elapsed = time.monotonic() - started
        sys.stdout.write("\r  %5.1f%%  %d/%d bytes  %.1fs"
                         % (done * 100.0 / size, done, size, elapsed))
        sys.stdout.flush()


def flash_image(bus, image, id_tx, id_rx, retries, send_go):
    """Run the full update flow. Returns (crc32, slot_size)."""
    flasher = CanFlasher(bus, id_tx, id_rx, retries)

    slot_size = flasher.hello()
    print("HELLO ok: protocol v%d, app slot %d bytes" % (PROTOCOL_VERSION, slot_size))
    if slot_size != EXPECTED_SLOT_SIZE:
        print("  note: announced slot differs from the expected 0x%08X"
              % EXPECTED_SLOT_SIZE)
    if len(image) > slot_size:
        raise FlashError("image is %d bytes but the app slot is only %d bytes"
                         % (len(image), slot_size))

    flasher.send_size(len(image))
    print("SIZE ok: %d bytes" % len(image))

    print("ERASE... (up to %.0f s)" % ERASE_TIMEOUT_S)
    flasher.erase()
    print("ERASE ok")

    print("DATA: %d windows of up to %d bytes"
          % (window_count(len(image)), FLASH_BLOCK_SIZE))
    flasher.send_image(image)

    crc32 = zlib.crc32(image) & 0xFFFFFFFF
    flasher.verify(crc32)
    print("VERIFY ok: CRC32 0x%08X" % crc32)

    if send_go:
        flasher.go()
        print("GO sent -- application starting")
    else:
        print("GO skipped (--no-go)")
    return crc32, slot_size


# ---------------------------------------------------------------------------
# Selftest: in-process fake bootloader on python-can's 'virtual' interface
# ---------------------------------------------------------------------------
class FakeBootloader(threading.Thread):
    """Protocol v1 bootloader simulation over a bytearray 'flash'.

    Once per run it pretends to lose DATA frame `drop_frame` and answers the
    enclosing window with NAK 0x01, exercising the host's rewind path.
    """

    def __init__(self, channel, id_host, id_boot,
                 slot_size=EXPECTED_SLOT_SIZE, drop_frame=SELFTEST_DROP_FRAME):
        super().__init__(name="fake-bootloader", daemon=True)
        self.bus = can.Bus(interface="virtual", channel=channel)
        self.bus.set_filters([{"can_id": id_host, "can_mask": 0x7FF, "extended": False}])
        self.id_boot = id_boot
        self.slot_size = slot_size
        self.drop_frame = drop_frame
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


def run_selftest(retries):
    import random

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
    parser.add_argument("--selftest", action="store_true",
                        help="run the full flow against an in-process fake "
                             "bootloader on a virtual bus (no hardware)")
    args = parser.parse_args(argv)
    if not args.selftest and not args.firmware:
        parser.error("firmware image is required unless --selftest is given")
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
    args = parse_args(argv)
    try:
        if args.selftest:
            run_selftest(args.retries)
            return 0

        image = load_image(args.firmware)
        bus_kwargs = {"bitrate": args.bitrate}
        if args.interface:
            bus_kwargs["interface"] = args.interface
        if args.channel:
            bus_kwargs["channel"] = args.channel
        started = time.monotonic()
        bus = can.Bus(**bus_kwargs)
        bus.set_filters([{"can_id": args.id_rx, "can_mask": 0x7FF, "extended": False}])
        try:
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
