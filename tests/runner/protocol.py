"""Binary frame codec shared by runner transports. See tests/protocol/frame.h."""

from dataclasses import dataclass
from enum import IntEnum
import struct
import zlib


MAGIC = b'GBXP'
VERSION = 3
EXECUTION_VERSION = 8
NONCE_SIZE = 8
MAX_PAYLOAD = 8192
RELIABLE = 1
HEADER = struct.Struct('<4sBBHIIII')


class Kind(IntEnum):
    HELLO = 1
    HELLO_REPLY = 2
    PING = 3
    PONG = 4
    DATA = 5
    ACK = 6
    ERROR = 7
    LOG = 8
    RUN = 9
    DONE = 10
    JIT_TEST = 11
    ROM_TEST = 12
    DISPLAY_TEST = 13
    AUDIO_TEST = 14
    ARM7_RUN = 15
    GBA_ROM_TEST = 16


class ProtocolError(ValueError):
    pass


class DeviceResetError(ProtocolError):
    pass


class DeviceError(ProtocolError):
    pass


class UnframedDataError(ProtocolError):
    pass


class FrameCrcError(ProtocolError):
    pass


def is_boot_output(data):
    return b'ESP-ROM' in data or b'rst:' in data or b'boot: ESP-IDF' in data


@dataclass(frozen=True, slots=True)
class Frame:
    kind: int
    flags: int
    session: int
    sequence: int
    payload: bytes


def encode(frame):
    if len(frame.payload) > MAX_PAYLOAD or frame.flags & ~RELIABLE:
        raise ProtocolError('Invalid frame length or flags')
    header = HEADER.pack(MAGIC, VERSION, frame.kind, frame.flags, frame.session,
                         frame.sequence, len(frame.payload), 0)
    crc = zlib.crc32(frame.payload, zlib.crc32(header[:20]))
    return header[:20] + struct.pack('<I', crc) + frame.payload


class Decoder:
    def __init__(self):
        self.buffer = bytearray()
        self.strict = False  # Boot text is allowed only before the handshake.

    def feed(self, data):
        self.buffer.extend(data)

    def pop(self):
        while len(self.buffer) >= len(MAGIC):
            start = self.buffer.find(MAGIC)
            if start != 0:
                if self.strict:
                    # ESP ROM output follows a reset, including after a panic.
                    if is_boot_output(self.buffer):
                        raise DeviceResetError('Device boot output appeared during the run')
                    # A split ROM banner must not be classified as generic noise.
                    if b'ESP-ROM'.startswith(bytes(self.buffer)) or b'\r\nESP-ROM'.startswith(bytes(self.buffer)):
                        return None
                    raise UnframedDataError('Unexpected text inside binary session')
                if start < 0:
                    del self.buffer[:-3]
                    return None
                del self.buffer[:start]
            if len(self.buffer) < HEADER.size:
                return None
            _, version, kind, flags, session, sequence, length, expected_crc = HEADER.unpack_from(self.buffer)
            if version != VERSION:
                raise ProtocolError(f'Transport version mismatch: device {version}, runner {VERSION}')
            if length > MAX_PAYLOAD or flags & ~RELIABLE:
                if self.strict:
                    raise ProtocolError('Invalid frame header')
                del self.buffer[0]
                continue
            end = HEADER.size + length
            if len(self.buffer) < end:
                return None
            payload = bytes(self.buffer[HEADER.size:end])
            crc = zlib.crc32(payload, zlib.crc32(self.buffer[:20]))
            if crc != expected_crc:
                if self.strict:
                    # Only inspect boot text after the frame has failed CRC.
                    # A valid binary payload may contain the same bytes.
                    if is_boot_output(self.buffer):
                        raise DeviceResetError('Device boot output interrupted a frame')
                    raise FrameCrcError('Frame CRC mismatch')
                del self.buffer[0]
                continue
            del self.buffer[:end]
            return Frame(kind, flags, session, sequence, payload)
        return None


def hello_info(payload):
    if len(payload) < NONCE_SIZE + 12:
        raise ProtocolError('Truncated HELLO reply')
    revision, backends, status, cpu_hz, max_payload, window = struct.unpack_from('<HBBIHH', payload, NONCE_SIZE)
    offset = NONCE_SIZE + 12
    texts = []
    for _ in range(4):
        if offset + 2 > len(payload):
            raise ProtocolError('Truncated HELLO string length')
        length, = struct.unpack_from('<H', payload, offset)
        offset += 2
        if offset + length > len(payload):
            raise ProtocolError('Truncated HELLO string')
        texts.append(payload[offset:offset + length].decode('utf-8'))
        offset += length
    # Status bits imply each other: a pack opens only on a mounted card of a ready board.
    if (len(payload) - offset != 194 or status & ~15 or (status & 12 and not status & 2) or
            (status & 2 and not status & 1) or max_payload != MAX_PAYLOAD or not window):
        raise ProtocolError('Invalid HELLO capabilities')
    execution_version, = struct.unpack_from('<H', payload, offset)
    if execution_version != EXECUTION_VERSION:
        raise ProtocolError(f'Execution version mismatch: device {execution_version}, runner {EXECUTION_VERSION}')
    digests = [payload[offset + 2 + 32 * i:offset + 34 + 32 * i].hex() for i in range(6)]
    return {'board': texts[0], 'idf': texts[1], 'firmware': texts[2],
            'preset': texts[3], 'firmware_dirty': texts[2].endswith('-dirty'),
            'firmware_elf_sha256': digests[1], 'execution_version': execution_version,
            'reference_sha256': digests[2], 'manifest_sha256': digests[0],
            'arm7_reference_sha256': digests[4], 'gba_manifest_sha256': digests[3], 'bios_sha256': digests[5],
            'data_ready': bool(status & 4), 'gba_data_ready': bool(status & 8),
            'chip_revision': f'{revision // 100}.{revision % 100}', 'cpu_hz': cpu_hz,
            'backends': backends, 'board_ready': bool(status & 1), 'sd_mounted': bool(status & 2),
            'max_payload': max_payload, 'window': window}
