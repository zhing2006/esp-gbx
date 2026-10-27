"""Runner-side binary link. No test execution or suite judging lives here.

DATA batches are acknowledged explicitly, after the caller accepts/saves them.
Only one control request may be outstanding. A transport error fails the run;
retransmission and execution resume are left to the execution protocol.
The baud argument must match CONFIG_ESP_CONSOLE_UART_BAUDRATE on UART boards;
USB Serial/JTAG does not use it to set the physical transfer rate.
"""

import argparse
from collections import deque
import json
import secrets
import struct
import sys
import time

from protocol import (Decoder, DeviceError, DeviceResetError, Frame, FrameCrcError, Kind, NONCE_SIZE,
                      ProtocolError, RELIABLE, UnframedDataError, encode, hello_info, is_boot_output)

# First line of the LOG frames sent by the profiling test firmware.
PROFILE_MARKER = 'gbx-profile\n'


class Link:
    def __init__(self, port, write_timeout_error=TimeoutError):
        self.port = port
        self.write_timeout_error = write_timeout_error
        self.decoder = Decoder()
        self.session = 0
        self.nonce = b''
        self.received = 0
        self.acknowledged = 0
        self.logs = deque(maxlen=64)
        self.dropped_logs = 0
        self.truncated_logs = 0
        self.profile = []  # Lines of profile frames, kept until the caller takes them.

    @classmethod
    def open(cls, name, baud=460800, reset=False, write_timeout=1):
        import serial

        port = serial.Serial(port=None, baudrate=baud, timeout=0.02, write_timeout=write_timeout,
                             xonxoff=False, rtscts=False, dsrdtr=False)
        port.port = name
        port.dtr = False
        port.rts = False
        try:
            port.open()
            if reset:
                # CH340 EN/BOOT circuit, not a generic USB Serial/JTAG reset.
                port.rts = True
                time.sleep(0.1)
                port.reset_input_buffer()
                port.rts = False
            return cls(port, serial.SerialTimeoutException)
        except OSError as error:
            port.close()
            raise ProtocolError(f'Cannot open serial port {name}: {error}') from error
        except BaseException:
            port.close()
            raise

    def close(self):
        self.port.close()

    def _write(self, frame):
        packet = encode(frame)
        # Keep the port configuration fixed after open(). On macOS, changing
        # pyserial timeouts also reapplies a custom baud rate through IOKit.
        try:
            if self.port.write(packet) != len(packet):
                raise ProtocolError('Short serial write')
        except self.write_timeout_error as error:
            raise TimeoutError(f'Serial write timed out: {error}') from error
        except OSError as error:
            raise ProtocolError(f'Serial write failed: {error}') from error

    def _read(self, deadline):
        while True:
            frame = self.decoder.pop()
            if frame is not None:
                return frame
            if time.perf_counter() >= deadline:
                raise TimeoutError('No complete frame before the deadline')
            try:
                data = self.port.read(min(max(self.port.in_waiting, 1), 16384))
            except OSError as error:
                raise ProtocolError(f'Serial read failed: {error}') from error
            self.decoder.feed(data)

    def connect(self, timeout=10):
        if timeout <= 0:
            raise ValueError('Connection timeout must be positive')
        self.decoder = Decoder()
        self.session = self.received = self.acknowledged = 0
        self.nonce = secrets.token_bytes(NONCE_SIZE)
        self.logs.clear()
        self.dropped_logs = self.truncated_logs = 0
        self.profile.clear()
        self.port.reset_input_buffer()
        request = Frame(Kind.HELLO, 0, 0, 0, self.nonce)
        deadline = time.perf_counter() + timeout
        next_send = 0
        while True:
            now = time.perf_counter()
            if now >= deadline:
                raise TimeoutError('HELLO handshake timed out')
            if now >= next_send:
                if deadline - now >= self.port.write_timeout:
                    self._write(request)
                    now = time.perf_counter()
                    if now >= deadline:
                        raise TimeoutError('HELLO handshake timed out')
                    next_send = now + 0.5
                else:
                    # Wait for an earlier request's reply without allowing a
                    # final serial write to overrun the overall deadline.
                    next_send = deadline
            try:
                frame = self._read(min(deadline, next_send))
            except TimeoutError:
                continue
            if time.perf_counter() >= deadline:
                raise TimeoutError('HELLO handshake timed out')
            if frame.kind != Kind.HELLO_REPLY or frame.payload[:NONCE_SIZE] != self.nonce:
                continue
            if not frame.session or frame.flags or frame.sequence:
                raise ProtocolError('Invalid HELLO reply')
            info = hello_info(frame.payload)
            self.session = frame.session
            self.decoder.strict = True
            return info

    def send(self, kind, payload=b''):
        if not self.session:
            raise ProtocolError('Connect before sending a request')
        self._write(Frame(kind, 0, self.session, 0, payload))

    def receive(self, timeout=10):
        deadline = time.perf_counter() + timeout
        while True:
            try:
                frame = self._read(deadline)
            except (UnframedDataError, FrameCrcError, TimeoutError):
                # The ROM's 115200-baud banner is garbled on a 460800-baud
                # connection. After framing fails, briefly collect the following
                # banner to distinguish a reset from noise. An incomplete DATA
                # frame is inspected only at its deadline: valid binary data
                # can contain boot strings. Never resume this run.
                probe_deadline = min(deadline, time.perf_counter() + 0.5)
                remaining = 4096  # Additional bytes, excluding the failed frame.
                while (not is_boot_output(self.decoder.buffer) and remaining
                       and time.perf_counter() < probe_deadline):
                    try:
                        data = self.port.read(min(max(self.port.in_waiting, 1), remaining))
                        remaining -= len(data)
                        self.decoder.feed(data)
                    except OSError:
                        break
                if is_boot_output(self.decoder.buffer):
                    raise DeviceResetError('Device boot output appeared during the run') from None
                raise
            if frame.session != self.session:
                raise ProtocolError('Device session changed; reconnect required')
            if frame.kind == Kind.DATA:
                if frame.flags != RELIABLE or frame.sequence != self.received + 1:
                    raise ProtocolError('Missing, duplicate or out-of-order DATA batch')
                self.received = frame.sequence
            elif frame.flags or frame.sequence:
                raise ProtocolError('Unexpected control-frame flags or sequence')
            if frame.kind == Kind.HELLO_REPLY:
                # Replies to retries can arrive after connect() has returned.
                if frame.payload[:NONCE_SIZE] != self.nonce:
                    raise ProtocolError('Unexpected HELLO reply')
                continue
            if frame.kind == Kind.LOG:
                if len(frame.payload) < 8:
                    raise ProtocolError('Truncated LOG frame')
                text = frame.payload[8:].decode('utf-8', errors='replace')
                if text.startswith(PROFILE_MARKER):
                    self.profile.extend(text[len(PROFILE_MARKER):].splitlines())
                    continue
                self.dropped_logs, self.truncated_logs = struct.unpack_from('<II', frame.payload)
                self.logs.append(text)
                continue
            if frame.kind == Kind.ERROR:
                raise DeviceError(frame.payload.decode('utf-8', errors='replace'))
            return frame

    def acknowledge(self, frame):
        if (frame.kind != Kind.DATA or frame.session != self.session or
                not self.acknowledged < frame.sequence <= self.received):
            raise ProtocolError('Cannot acknowledge this batch')
        self._write(Frame(Kind.ACK, 0, self.session, frame.sequence, b''))
        self.acknowledged = frame.sequence

    def ping(self, payload=b'', timeout=10):
        self.send(Kind.PING, payload)
        frame = self.receive(timeout)
        if frame.kind != Kind.PONG or frame.payload != payload:
            raise ProtocolError('PONG does not match PING')
        return frame.payload


def main():
    parser = argparse.ArgumentParser(description='Inspect the device link; this does not execute CPU tests.')
    parser.add_argument('port', help='Serial port, such as COM4')
    parser.add_argument('--baud', type=int, default=460800)
    parser.add_argument('--timeout', type=float, default=10, help='Handshake deadline in seconds')
    parser.add_argument('--reset', action='store_true', help='Reset via CH340 RTS/DTR before connecting')
    args = parser.parse_args()
    link = None
    try:
        link = Link.open(args.port, baud=args.baud, reset=args.reset, write_timeout=min(1, args.timeout / 2))
        info = link.connect(args.timeout)
        print(json.dumps(info, indent=2))
        link.ping(timeout=args.timeout)
        print('PING/PONG: OK')
        if not info['board_ready']:
            print('Board initialization failed; SD mounting was not attempted.', file=sys.stderr)
            return 1
        if not info['sd_mounted']:
            print('SD card is not mounted.', file=sys.stderr)
        return 0
    except (OSError, ValueError, ImportError) as error:
        print(f'Device connection failed: {error}', file=sys.stderr)
        return 1
    finally:
        if link is not None:
            link.close()


if __name__ == '__main__':
    raise SystemExit(main())
