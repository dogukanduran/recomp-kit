#!/usr/bin/env python3
"""Pure-stdlib RFC 6455 WebSocket relay for virtual WSOCK32 / WS2_32 networking.

Routes virtual LAN traffic (10.0.0.x, TCP/UDP + UDP broadcast) between
connected browser sessions, and optionally bridges TCP connections to real
external/LAN IPv4 hosts.
"""

import base64
import hashlib
import socket
import struct
import threading

WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

OP_OPEN_BIND = 0x01
OP_LISTEN = 0x02
OP_CONNECT = 0x03
OP_SEND = 0x04
OP_SENDTO = 0x05
OP_CLOSE = 0x06

OP_ASSIGN_IP = 0x80
OP_CONNECT_RESULT = 0x81
OP_ACCEPT = 0x82
OP_RECV = 0x83
OP_RECVFROM = 0x84
OP_CLOSED = 0x85

WSAECONNREFUSED = 10061


def ip_to_str(ip_int):
    return "%d.%d.%d.%d" % (
        (ip_int >> 24) & 0xFF,
        (ip_int >> 16) & 0xFF,
        (ip_int >> 8) & 0xFF,
        ip_int & 0xFF,
    )


def _recv_exact(sock, n):
    buf = bytearray()
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            return None
        buf.extend(chunk)
    return bytes(buf)


def _read_ws_frame(sock):
    hdr = _recv_exact(sock, 2)
    if not hdr:
        return None, None
    b0, b1 = hdr[0], hdr[1]
    opcode = b0 & 0x0F
    masked = bool(b1 & 0x80)
    length = b1 & 0x7F
    if length == 126:
        ext = _recv_exact(sock, 2)
        if not ext:
            return None, None
        length = struct.unpack("!H", ext)[0]
    elif length == 127:
        ext = _recv_exact(sock, 8)
        if not ext:
            return None, None
        length = struct.unpack("!Q", ext)[0]
    mask_key = _recv_exact(sock, 4) if masked else b""
    if masked and mask_key is None:
        return None, None
    payload = _recv_exact(sock, length) if length else b""
    if payload is None:
        return None, None
    if masked and length:
        arr = bytearray(payload)
        for i in range(length):
            arr[i] ^= mask_key[i & 3]
        payload = bytes(arr)
    return opcode, payload


def _build_ws_frame(opcode, payload):
    n = len(payload)
    out = bytearray([0x80 | (opcode & 0x0F)])
    if n < 126:
        out.append(n)
    elif n < 65536:
        out.append(126)
        out.extend(struct.pack("!H", n))
    else:
        out.append(127)
        out.extend(struct.pack("!Q", n))
    out.extend(payload)
    return bytes(out)


class _RelayHub:
    def __init__(self):
        self.lock = threading.Lock()
        self.clients = {}  # ip_int -> Client
        self.next_host_octet = 2
        self.next_child_sid = 0x40000000

    def alloc_client(self, sock):
        with self.lock:
            for _ in range(250):
                octet = self.next_host_octet
                self.next_host_octet += 1
                if self.next_host_octet > 250:
                    self.next_host_octet = 2
                ip_int = 0x0A000000 | octet
                if ip_int not in self.clients:
                    c = _Client(self, sock, ip_int)
                    self.clients[ip_int] = c
                    return c
            return None

    def remove_client(self, client):
        to_notify = []
        with self.lock:
            self.clients.pop(client.ip, None)
            for sid, info in list(client.sockets.items()):
                peer = info.get("peer")
                if peer:
                    peer_client, peer_sid = peer
                    to_notify.append((peer_client, peer_sid))
                    pinfo = peer_client.sockets.get(peer_sid)
                    if pinfo:
                        pinfo["peer"] = None
                ext_sock = info.get("ext_sock")
                if ext_sock:
                    try:
                        ext_sock.close()
                    except OSError:
                        pass
            client.sockets.clear()
        for peer_client, peer_sid in to_notify:
            peer_client.send_packet(struct.pack("!BII", OP_CLOSED, peer_sid, 0))

    def alloc_child_sid(self):
        with self.lock:
            sid = self.next_child_sid
            self.next_child_sid += 1
            if self.next_child_sid >= 0x7FFFFFFF:
                self.next_child_sid = 0x40000000
            return sid


class _Client:
    def __init__(self, hub, sock, ip_int):
        self.hub = hub
        self.sock = sock
        self.ip = ip_int
        self.tx_lock = threading.Lock()
        self.sockets = {}  # sid -> dict(proto, port, listening, peer=(client,sid), ext_sock=...)
        self.closed = False

    def send_ws(self, opcode, payload):
        frame = _build_ws_frame(opcode, payload)
        with self.tx_lock:
            if self.closed:
                return
            try:
                self.sock.sendall(frame)
            except OSError:
                self.closed = True

    def send_packet(self, payload):
        self.send_ws(0x02, payload)

    def handle_packet(self, data):
        if not data:
            return
        op = data[0]
        if op == OP_OPEN_BIND and len(data) >= 10:
            _, sid, proto, port, reuse, bcast = struct.unpack("!BIBHBB", data[:10])
            with self.hub.lock:
                self.sockets[sid] = {
                    "proto": proto,
                    "port": port,
                    "listening": False,
                    "peer": None,
                    "ext_sock": None,
                }
        elif op == OP_LISTEN and len(data) >= 7:
            _, sid, backlog = struct.unpack("!BIH", data[:7])
            with self.hub.lock:
                if sid in self.sockets:
                    self.sockets[sid]["listening"] = True
        elif op == OP_CONNECT and len(data) >= 11:
            _, sid, target_ip, target_port = struct.unpack("!BIIH", data[:11])
            self._handle_connect(sid, target_ip, target_port)
        elif op == OP_SEND and len(data) >= 5:
            _, sid = struct.unpack("!BI", data[:5])
            payload = data[5:]
            peer = None
            ext_sock = None
            with self.hub.lock:
                info = self.sockets.get(sid)
                if info:
                    peer = info.get("peer")
                    ext_sock = info.get("ext_sock")
            if peer:
                peer_client, peer_sid = peer
                peer_client.send_packet(struct.pack("!BI", OP_RECV, peer_sid) + payload)
            elif ext_sock:
                try:
                    ext_sock.sendall(payload)
                except OSError:
                    self.send_packet(struct.pack("!BII", OP_CLOSED, sid, 10054))
        elif op == OP_SENDTO and len(data) >= 11:
            _, sid, target_ip, target_port = struct.unpack("!BIIH", data[:11])
            payload = data[11:]
            self._handle_sendto(sid, target_ip, target_port, payload)
        elif op == OP_CLOSE and len(data) >= 5:
            _, sid = struct.unpack("!BI", data[:5])
            self._handle_close(sid)

    def _handle_connect(self, sid, target_ip, target_port):
        # Virtual gateway 10.0.0.1 routes WormNET HTTP (80/17080) and IRC (6667) to local WormNET server
        if target_ip == 0x0A000001 or (target_ip == 0x7F000001 and target_port in (17080, 6667)):
            mapped_port = 17080 if target_port in (80, 17080) else target_port
            threading.Thread(
                target=self._bridge_outbound_tcp,
                args=(sid, 0x7F000001, mapped_port),
                daemon=True,
            ).start()
            return
        if target_ip == 0x7F000001:  # 127.0.0.1 -> self
            target_ip = self.ip
        host_match = None
        with self.hub.lock:
            target_client = self.hub.clients.get(target_ip)
            if target_client:
                for lsid, linfo in target_client.sockets.items():
                    if linfo["proto"] == 6 and linfo["listening"] and linfo["port"] == target_port:
                        host_match = (target_client, lsid)
                        break
            if host_match:
                host_client, listen_sid = host_match
                child_sid = self.hub.alloc_child_sid()
                src_info = self.sockets.setdefault(
                    sid, {"proto": 6, "port": 0, "listening": False, "peer": None, "ext_sock": None}
                )
                src_port = src_info["port"] or 49152
                src_info["peer"] = (host_client, child_sid)
                host_client.sockets[child_sid] = {
                    "proto": 6,
                    "port": target_port,
                    "listening": False,
                    "peer": (self, sid),
                    "ext_sock": None,
                }
        if host_match:
            host_client, listen_sid = host_match
            host_client.send_packet(
                struct.pack("!BIIIH", OP_ACCEPT, listen_sid, child_sid, self.ip, src_port)
            )
            self.send_packet(
                struct.pack("!BIIIH", OP_CONNECT_RESULT, sid, 0, self.ip, src_port)
            )
            return

        # If not a virtual 10.0.0.x host, attempt a real outbound TCP bridge
        if (target_ip & 0xFF000000) != 0x0A000000:
            threading.Thread(
                target=self._bridge_outbound_tcp,
                args=(sid, target_ip, target_port),
                daemon=True,
            ).start()
            return

        self.send_packet(struct.pack("!BIIIH", OP_CONNECT_RESULT, sid, WSAECONNREFUSED, 0, 0))

    def _bridge_outbound_tcp(self, sid, target_ip, target_port):
        ext = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        ext.settimeout(5.0)
        try:
            ext.connect((ip_to_str(target_ip), target_port))
            ext.settimeout(None)
        except OSError:
            try:
                ext.close()
            except OSError:
                pass
            self.send_packet(struct.pack("!BIIIH", OP_CONNECT_RESULT, sid, WSAECONNREFUSED, 0, 0))
            return

        with self.hub.lock:
            info = self.sockets.setdefault(
                sid, {"proto": 6, "port": 0, "listening": False, "peer": None, "ext_sock": None}
            )
            info["ext_sock"] = ext
        self.send_packet(struct.pack("!BIIIH", OP_CONNECT_RESULT, sid, 0, self.ip, info["port"] or 49152))

        try:
            while not self.closed:
                chunk = ext.recv(16384)
                if not chunk:
                    break
                self.send_packet(struct.pack("!BI", OP_RECV, sid) + chunk)
        except OSError:
            pass
        finally:
            try:
                ext.close()
            except OSError:
                pass
            self.send_packet(struct.pack("!BII", OP_CLOSED, sid, 0))

    def _handle_sendto(self, sid, target_ip, target_port, payload):
        is_bcast = target_ip in (0xFFFFFFFF, 0x0A0000FF)
        if target_ip == 0x7F000001:
            target_ip = self.ip
        targets = []
        src_port = 0
        with self.hub.lock:
            sinfo = self.sockets.get(sid)
            if sinfo:
                src_port = sinfo["port"]
            for cip, c in self.hub.clients.items():
                if is_bcast and c is self:
                    continue
                if not is_bcast and cip != target_ip:
                    continue
                for dsid, dinfo in c.sockets.items():
                    if dinfo["proto"] == 17 and dinfo["port"] == target_port:
                        targets.append((c, dsid))
        pkt_tail = struct.pack("!IH", self.ip, src_port) + payload
        for c, dsid in targets:
            c.send_packet(struct.pack("!BI", OP_RECVFROM, dsid) + pkt_tail)

    def _handle_close(self, sid):
        peer = None
        ext_sock = None
        with self.hub.lock:
            info = self.sockets.pop(sid, None)
            if info:
                peer = info.get("peer")
                ext_sock = info.get("ext_sock")
                if peer:
                    pclient, psid = peer
                    pinfo = pclient.sockets.get(psid)
                    if pinfo:
                        pinfo["peer"] = None
        if ext_sock:
            try:
                ext_sock.close()
            except OSError:
                pass
        if peer:
            pclient, psid = peer
            pclient.send_packet(struct.pack("!BII", OP_CLOSED, psid, 0))


HUB = _RelayHub()


def handle_websocket(handler):
    key = handler.headers.get("Sec-WebSocket-Key")
    if not key:
        handler.send_error(400, "Missing Sec-WebSocket-Key")
        return
    accept = base64.b64encode(
        hashlib.sha1((key.strip() + WS_GUID).encode("ascii")).digest()
    ).decode("ascii")

    handler.send_response(101, "Switching Protocols")
    handler.send_header("Upgrade", "websocket")
    handler.send_header("Connection", "Upgrade")
    handler.send_header("Sec-WebSocket-Accept", accept)
    handler.end_headers()

    sock = handler.connection
    sock.settimeout(None)
    client = HUB.alloc_client(sock)
    if not client:
        return
    print("[net-relay] client connected: %s" % ip_to_str(client.ip), flush=True)
    client.send_packet(struct.pack("!BI", OP_ASSIGN_IP, client.ip))
    try:
        while True:
            opcode, payload = _read_ws_frame(sock)
            if opcode is None or opcode == 0x08:
                break
            if opcode == 0x09:  # ping -> pong
                client.send_ws(0x0A, payload)
            elif opcode == 0x02:  # binary frame
                client.handle_packet(payload)
    except OSError:
        pass
    finally:
        client.closed = True
        HUB.remove_client(client)
        print("[net-relay] client disconnected: %s" % ip_to_str(client.ip), flush=True)
