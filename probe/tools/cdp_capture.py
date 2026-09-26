# cdp_capture.py — minimal, pure-stdlib Chrome DevTools Protocol client to
# passively capture WebSocket frames from an Electron app's renderer,
# specifically the ws://localhost:9002 connection to KineoDeviceService.
#
# Read-only: enables the Network domain and logs webSocketFrameSent/Received
# events. Does not modify Kineo, does not inject/patch any JS, does not send
# any command to the app itself beyond standard CDP introspection.
#
# Usage: python cdp_capture.py <duration_seconds> <output_jsonl_path>
import json
import socket
import struct
import base64
import hashlib
import os
import sys
import time
import urllib.request

CDP_HOST = "localhost"
CDP_PORT = 9222


def list_targets():
    with urllib.request.urlopen(f"http://{CDP_HOST}:{CDP_PORT}/json") as r:
        return json.loads(r.read().decode())


def ws_connect(ws_url):
    # ws_url like ws://localhost:9222/devtools/page/<id>
    assert ws_url.startswith("ws://")
    rest = ws_url[len("ws://"):]
    host_port, path = rest.split("/", 1)
    path = "/" + path
    if ":" in host_port:
        host, port = host_port.split(":")
        port = int(port)
    else:
        host, port = host_port, 80
    sock = socket.create_connection((host, port), timeout=10)
    key = base64.b64encode(os.urandom(16)).decode()
    req = (
        f"GET {path} HTTP/1.1\r\n"
        f"Host: {host}:{port}\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        f"Sec-WebSocket-Key: {key}\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n"
    )
    sock.sendall(req.encode())
    resp = b""
    while b"\r\n\r\n" not in resp:
        resp += sock.recv(4096)
    if b"101" not in resp.split(b"\r\n", 1)[0]:
        raise RuntimeError(f"WS handshake failed: {resp[:200]!r}")
    return sock


def ws_send_text(sock, text):
    payload = text.encode()
    mask = os.urandom(4)
    masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
    length = len(payload)
    if length < 126:
        header = struct.pack("!BB", 0x81, 0x80 | length)
    elif length < 65536:
        header = struct.pack("!BBH", 0x81, 0x80 | 126, length)
    else:
        header = struct.pack("!BBQ", 0x81, 0x80 | 127, length)
    sock.sendall(header + mask + masked)


class WSReader:
    """Buffered reader that yields complete text-frame payloads (handles
    fragmentation of the underlying TCP stream, not WS message fragmentation
    -- CDP doesn't fragment messages in practice for our use)."""
    def __init__(self, sock):
        self.sock = sock
        self.buf = b""

    def _fill(self, n):
        while len(self.buf) < n:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("socket closed")
            self.buf += chunk

    def read_frame(self):
        self._fill(2)
        b0, b1 = self.buf[0], self.buf[1]
        opcode = b0 & 0x0F
        masked = (b1 & 0x80) != 0
        length = b1 & 0x7F
        offset = 2
        if length == 126:
            self._fill(offset + 2)
            length = struct.unpack("!H", self.buf[offset:offset+2])[0]
            offset += 2
        elif length == 127:
            self._fill(offset + 8)
            length = struct.unpack("!Q", self.buf[offset:offset+8])[0]
            offset += 8
        mask_key = b""
        if masked:
            self._fill(offset + 4)
            mask_key = self.buf[offset:offset+4]
            offset += 4
        self._fill(offset + length)
        payload = self.buf[offset:offset+length]
        self.buf = self.buf[offset+length:]
        if masked:
            payload = bytes(b ^ mask_key[i % 4] for i, b in enumerate(payload))
        return opcode, payload

    def read_text(self):
        opcode, payload = self.read_frame()
        if opcode == 0x8:  # close
            raise ConnectionError("WS closed by peer")
        if opcode == 0x9:  # ping -- reply pong minimally (not expected from CDP often)
            return None
        return payload.decode(errors="replace")


def main():
    duration = float(sys.argv[1]) if len(sys.argv) > 1 else 60.0
    out_path = sys.argv[2] if len(sys.argv) > 2 else "cdp_capture.jsonl"

    targets = list_targets()
    page_targets = [t for t in targets if t.get("type") in ("page", "webview")]
    print(f"Found {len(targets)} total targets, {len(page_targets)} page/webview targets:")
    for t in page_targets:
        print(f"  id={t['id']} type={t['type']} title={t.get('title','')!r} url={t.get('url','')!r}")

    out = open(out_path, "w", encoding="utf-8")
    conns = []
    next_id = [1]

    for t in page_targets:
        try:
            sock = ws_connect(t["webSocketDebuggerUrl"])
        except Exception as e:
            print(f"  connect FAILED for {t['id']}: {e}")
            continue
        reader = WSReader(sock)
        cmd_id = next_id[0]; next_id[0] += 1
        ws_send_text(sock, json.dumps({"id": cmd_id, "method": "Network.enable"}))
        conns.append((t, sock, reader))
        print(f"  Network.enable sent to {t['id']}")

    print(f"\nCapturing for {duration:.0f}s -> {out_path} ...")
    start = time.time()
    import selectors
    sel = selectors.DefaultSelector()
    for (t, sock, reader) in conns:
        sock.setblocking(False)
        sel.register(sock, selectors.EVENT_READ, (t, reader))

    events_captured = 0
    while time.time() - start < duration:
        for key, _ in sel.select(timeout=1.0):
            t, reader = key.data
            try:
                while True:
                    try:
                        text = reader.read_text()
                    except BlockingIOError:
                        break
                    if text is None:
                        continue
                    try:
                        msg = json.loads(text)
                    except Exception:
                        continue
                    method = msg.get("method", "")
                    if method.startswith("Network.webSocket"):
                        events_captured += 1
                        out.write(json.dumps({"recv_ts": time.time(), "target": t["id"], **msg}) + "\n")
                        out.flush()
            except (ConnectionError, OSError):
                pass

    out.close()
    print(f"Done. Captured {events_captured} WebSocket-related events.")


if __name__ == "__main__":
    main()
