"""protocol.py -- shared wire protocol for the Kineo WSL camera bridge.

Simple, explicit, versioned framing so messages can never be confused by
TCP packet boundaries: every message is a fixed 10-byte header followed
by exactly `payload_len` bytes.

Header (10 bytes, all integers little-endian):
    magic        4 bytes   b"KCB1"
    version      1 byte    protocol version (currently 1)
    msg_type     1 byte    one of MSG_*
    payload_len  4 bytes   uint32, length of the payload that follows

Control messages (HELLO/OPEN/CONFIGURE/START/STOP/CLOSE/STATUS/ERROR)
carry a UTF-8 JSON payload -- easy to extend without changing the framing.
A request and its response reuse the same msg_type; the response payload
is a JSON object with at least {"ok": bool} and, on failure, {"error": str}.

FRAME messages (server -> client only, while streaming is active) carry a
fixed 33-byte binary frame header followed by exactly `data_len` raw
pixel bytes:
    frame_id      8 bytes  uint64, monotonically increasing
    width         4 bytes  uint32
    height        4 bytes  uint32
    pixel_format  4 bytes  uint32 (PFNC value, e.g. Mono8 = 0x01080001)
    data_len      4 bytes  uint32, exact byte length of the pixel payload
    timestamp_ns  8 bytes  uint64
    status        1 byte   0 = OK, 1 = incomplete/bad frame
"""
import json
import socket
import struct

MAGIC = b"KCB1"
VERSION = 1

MSG_HELLO = 1
MSG_OPEN = 2
MSG_CONFIGURE = 3
MSG_START = 4
MSG_STOP = 5
MSG_CLOSE = 6
MSG_STATUS = 7
MSG_FRAME = 8
MSG_ERROR = 9

MSG_NAMES = {
    MSG_HELLO: "HELLO", MSG_OPEN: "OPEN", MSG_CONFIGURE: "CONFIGURE",
    MSG_START: "START", MSG_STOP: "STOP", MSG_CLOSE: "CLOSE",
    MSG_STATUS: "STATUS", MSG_FRAME: "FRAME", MSG_ERROR: "ERROR",
}

_HEADER = struct.Struct("<4sBBI")
_FRAME_HEADER = struct.Struct("<QIIIIQB")

FRAME_HEADER_SIZE = _FRAME_HEADER.size  # 33
HEADER_SIZE = _HEADER.size  # 10


class ProtocolError(Exception):
    pass


def recv_exact(sock: socket.socket, n: int) -> bytes:
    """Read exactly n bytes or raise ConnectionError on EOF/disconnect."""
    chunks = []
    remaining = n
    while remaining > 0:
        chunk = sock.recv(remaining)
        if not chunk:
            raise ConnectionError(f"connection closed while reading {n} bytes ({remaining} remaining)")
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


def send_message(sock: socket.socket, msg_type: int, payload: bytes, lock=None) -> None:
    header = _HEADER.pack(MAGIC, VERSION, msg_type, len(payload))
    data = header + payload
    if lock is not None:
        with lock:
            sock.sendall(data)
    else:
        sock.sendall(data)


def send_json(sock: socket.socket, msg_type: int, obj: dict, lock=None) -> None:
    send_message(sock, msg_type, json.dumps(obj).encode("utf-8"), lock=lock)


def recv_message(sock: socket.socket):
    """Returns (msg_type, payload_bytes). Raises ConnectionError on
    disconnect, ProtocolError on a bad magic/version."""
    header = recv_exact(sock, HEADER_SIZE)
    magic, version, msg_type, payload_len = _HEADER.unpack(header)
    if magic != MAGIC:
        raise ProtocolError(f"bad magic {magic!r}, expected {MAGIC!r}")
    if version != VERSION:
        raise ProtocolError(f"unsupported protocol version {version}, expected {VERSION}")
    payload = recv_exact(sock, payload_len) if payload_len else b""
    return msg_type, payload


def recv_json(sock: socket.socket):
    msg_type, payload = recv_message(sock)
    obj = json.loads(payload.decode("utf-8")) if payload else {}
    return msg_type, obj


def pack_frame_header(frame_id: int, width: int, height: int, pixel_format: int,
                       data_len: int, timestamp_ns: int, status: int) -> bytes:
    return _FRAME_HEADER.pack(frame_id, width, height, pixel_format, data_len, timestamp_ns, status)


def unpack_frame_header(buf: bytes):
    frame_id, width, height, pixel_format, data_len, timestamp_ns, status = _FRAME_HEADER.unpack(buf)
    return {
        "frame_id": frame_id, "width": width, "height": height,
        "pixel_format": pixel_format, "data_len": data_len,
        "timestamp_ns": timestamp_ns, "status": status,
    }
