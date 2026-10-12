#!/usr/bin/env python3
"""Fuzzer and abuse checks for the Champions Reborn lobby server.

  python3 tools/lobby_fuzz.py --gateway-port N [--mode fuzz|abuse|limits|all] [--minutes M]
                              [--sessions K] [--seed S] [--only-session I] [--pid PID]
                              [--transcripts DIR]

It talks to 127.0.0.1 only and refuses any other host: point it at an instance of your
own, started from a scratch directory on ports of its own (con_gateway_port,
rta_gateway_port, con_lobby_port, rta_lobby_port, discovery_port in config.ini), never
at a server somebody is playing on. --gateway-port is that instance's rta_gateway_port;
everything else is taken from the server's replies.

fuzz    Sessions, numbered from 0, each driven by a random generator seeded with (seed,
        number), so one session can be run again with --only-session. A session opens a
        few connections (Return to Arms or Champions of Norrath lobby, or a gateway),
        does the key exchange, creates an account or logs in, and then sends requests
        for every message id: well-formed ones in the order the game sends them (host,
        discovery datagram, join, start, chat, friends, character upload in pieces) and
        mutated ones (length fields at their limits, truncated, extended, bytes flipped,
        other people's session ids). It also drops connections in the middle of a frame,
        stops reading replies, comes back under the old session id, and sends frames and
        datagrams of pure noise. After every session the server must still answer a
        gateway request; if it does not, or the process given with --pid is gone, the
        fuzzer stops and names the session. Build the server with -fsanitize=address,
        undefined and -fno-sanitize-recover=all, so that a report ends the process.
abuse   One check per thing a stranger could do to the unfixed server: be sent memory the
        server had not set, overwrite another account's character, act without logging
        in, change another host's game, pile up uploads, games and chat rooms without
        end, flood a chat room or the discovery port, never read its replies. Each
        prints what the server did, and PASS or FAIL.
limits  The limits of SECURITY.md, one check each: a connection that sends nothing or
        a byte at a time, connections held by one address, wrong passwords, accounts
        created, requests beyond an address's budget. They need a server started with
        limits_for_local_addresses=1, because the limits per address leave 127.0.0.1
        alone otherwise; and they leave that address in debt for a while, so run them
        last.

Standard library only.
"""
import argparse
import os
import random
import select
import socket
import struct
import sys
import time

KEY = b"dlfk qs';r+t iqe4t9ueerjKDJ wdaj"
ROOMS = ["Champions Reborn", "Adventurous", "Courageous", "Champion", "Legendary", "Epic"]
ALL_IDS = [0x01, 0x02, 0x05, 0x06, 0x08, 0x09, 0x0A, 0x0C, 0x0D, 0x0E, 0x0F, 0x11, 0x15, 0x16,
           0x17, 0x18, 0x19, 0x1C, 0x1D, 0x21, 0x22, 0x23, 0x24, 0x25, 0x27, 0x42, 0x43, 0x44,
           0x54, 0x55, 0x56, 0x57, 0x58, 0x5A, 0x5B, 0x5C, 0x5D, 0x5E, 0x5F, 0x60, 0x61, 0x62,
           0x63, 0x64, 0x66]


# ---------------------------------------------------------------- AES-256, ECB, no padding
def _tables():
    sbox, p, q = [0] * 256, 1, 1
    while True:
        p = p ^ ((p << 1) & 0xFF) ^ (0x1B if p & 0x80 else 0)
        q ^= q << 1
        q ^= q << 2
        q ^= q << 4
        q &= 0xFF
        if q & 0x80:
            q ^= 0x09
        x = q ^ ((q << 1) | (q >> 7)) ^ ((q << 2) | (q >> 6)) ^ ((q << 3) | (q >> 5)) ^ ((q << 4) | (q >> 4))
        sbox[p] = (x ^ 0x63) & 0xFF
        if p == 1:
            break
    sbox[0] = 0x63
    inv = [0] * 256
    for i, v in enumerate(sbox):
        inv[v] = i
    return sbox, inv


SBOX, INV = _tables()


def _mul(a, b):
    r = 0
    while b:
        if b & 1:
            r ^= a
        a = ((a << 1) ^ 0x1B) & 0xFF if a & 0x80 else a << 1
        b >>= 1
    return r


MUL = {n: [_mul(i, n) for i in range(256)] for n in (2, 3, 9, 11, 13, 14)}


def _round_keys(key):
    w = [list(key[i:i + 4]) for i in range(0, 32, 4)]
    rcon = 1
    for i in range(8, 60):
        t = list(w[i - 1])
        if i % 8 == 0:
            t = [SBOX[t[1]] ^ rcon, SBOX[t[2]], SBOX[t[3]], SBOX[t[0]]]
            rcon = _mul(rcon, 2)
        elif i % 8 == 4:
            t = [SBOX[b] for b in t]
        w.append([a ^ b for a, b in zip(w[i - 8], t)])
    return [sum(w[r * 4:r * 4 + 4], []) for r in range(15)]


RK = _round_keys(KEY)
SHIFT = [0, 5, 10, 15, 4, 9, 14, 3, 8, 13, 2, 7, 12, 1, 6, 11]
UNSHIFT = [0, 13, 10, 7, 4, 1, 14, 11, 8, 5, 2, 15, 12, 9, 6, 3]


def _enc_block(b):
    s = [x ^ k for x, k in zip(b, RK[0])]
    for r in range(1, 15):
        s = [SBOX[s[i]] for i in SHIFT]
        if r < 14:
            m2, m3, o = MUL[2], MUL[3], []
            for c in range(0, 16, 4):
                a0, a1, a2, a3 = s[c:c + 4]
                o += [m2[a0] ^ m3[a1] ^ a2 ^ a3, a0 ^ m2[a1] ^ m3[a2] ^ a3,
                      a0 ^ a1 ^ m2[a2] ^ m3[a3], m3[a0] ^ a1 ^ a2 ^ m2[a3]]
            s = o
        s = [x ^ k for x, k in zip(s, RK[r])]
    return bytes(s)


def _dec_block(b):
    s = [x ^ k for x, k in zip(b, RK[14])]
    for r in range(13, -1, -1):
        s = [INV[s[i]] for i in UNSHIFT]
        s = [x ^ k for x, k in zip(s, RK[r])]
        if r > 0:
            m9, m11, m13, m14, o = MUL[9], MUL[11], MUL[13], MUL[14], []
            for c in range(0, 16, 4):
                a0, a1, a2, a3 = s[c:c + 4]
                o += [m14[a0] ^ m11[a1] ^ m13[a2] ^ m9[a3], m9[a0] ^ m14[a1] ^ m11[a2] ^ m13[a3],
                      m13[a0] ^ m9[a1] ^ m14[a2] ^ m11[a3], m11[a0] ^ m13[a1] ^ m9[a2] ^ m14[a3]]
            s = o
    return bytes(s)


_CACHE = {}


def aes_enc(data):
    data += bytes(-len(data) % 16)
    if data not in _CACHE:
        if len(_CACHE) > 4096:
            _CACHE.clear()
        _CACHE[data] = b"".join(_enc_block(data[i:i + 16]) for i in range(0, len(data), 16))
    return _CACHE[data]


def aes_dec(data):
    return b"".join(_dec_block(data[i:i + 16]) for i in range(0, len(data) - 15, 16))


assert aes_dec(aes_enc(b"0123456789abcdef" * 2)) == b"0123456789abcdef" * 2


# ---------------------------------------------------------------- wire types
def u8(v): return struct.pack("<B", v & 0xFF)
def u16(v): return struct.pack("<H", v & 0xFFFF)
def u32(v): return struct.pack("<I", v & 0xFFFFFFFF)
def wstr(s): return u32(len(s)) + s.encode("utf-16-le", "surrogatepass")
def nstr(b): return u32(len(b)) + b
def frame(payload): return struct.pack(">I", len(payload) + 4) + payload


def estr(s):
    """Encrypted wide string: u32 (cipher bytes + 4) / 2, u32 plain bytes, cipher."""
    plain = s.encode("utf-16-le", "surrogatepass")
    cipher = aes_enc(plain)
    return u32((len(cipher) + 4) // 2) + u32(len(plain)) + cipher


def read_estr(body, pos):
    units, plain = struct.unpack_from("<II", body, pos)
    n = units * 2 - 4
    text = aes_dec(body[pos + 8:pos + 8 + n])[:plain].decode("utf-16-le", "replace")
    return text, pos + 8 + n


def meta(level="1", name="Fuzz"):
    keys, values = ["name", "raceandclass", "level", "ability"], [name, "0 0", level, "0 0 0 0 0 0"]
    return u32(4) + b"".join(wstr(k) for k in keys) + u32(4) + b"".join(wstr(v) for v in values)


def character(seed, plain=0):
    body = bytes((seed + i) % 255 + 1 for i in range(plain))
    left = 19504 - plain
    while left > 256:
        body, left = body + b"\x00\xff", left - 256
    return bytes([seed & 0xFF, 1, 2, 3]) + body + bytes([0, left - 1])


class Gone(Exception):
    pass


class Conn:
    """One TCP connection. Sends without waiting; replies are collected when asked for."""

    def __init__(self, port, transcript, version=6):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=5)
        self.buf, self.track, self.version, self.transcript = b"", 0, version, transcript
        self.session, self.user, self.handle, self.password = "", "", "", ""

    def raw(self, data):
        self.transcript.append(data)
        try:
            self.sock.sendall(data)
        except OSError as e:
            raise Gone(str(e))

    def send(self, pid, body=b"", track=None):
        if track is None:
            self.track += 1
            track = self.track
        self.raw(frame(u16(pid) + u32(track) + u32(self.version) + body))
        return track

    def frames(self, wait=0.05):
        """Frames received within `wait` seconds, as (id, track, rest of payload)."""
        out, end = [], time.monotonic() + wait
        while True:
            left = end - time.monotonic()
            r, _, _ = select.select([self.sock], [], [], max(left, 0))
            if r:
                try:
                    data = self.sock.recv(65536)
                except OSError:
                    data = b""
                if not data:
                    break
                self.buf += data
            while len(self.buf) >= 4:
                n = struct.unpack(">I", self.buf[:4])[0]
                if n < 4 or len(self.buf) < n:
                    break
                p, self.buf = self.buf[4:n], self.buf[n:]
                if len(p) >= 6:
                    out.append((struct.unpack_from("<H", p)[0], struct.unpack_from("<I", p, 2)[0], p[6:]))
            if not r or left <= 0:
                break
        return out

    def call(self, pid, body=b"", wait=5.0):
        """Send and wait for the reply with the same tracking number. None if none came."""
        track, end = self.send(pid, body), time.monotonic() + wait
        while time.monotonic() < end:
            for fid, ftrack, rest in self.frames(0.02):
                if ftrack == track and fid == pid and len(rest) >= 4:
                    return struct.unpack_from("<I", rest)[0], rest[4:]
        return None

    def close(self, hard=False):
        try:
            if hard:                                # RST instead of FIN
                self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
            self.sock.close()
        except OSError:
            pass


class Target:
    def __init__(self, gateway_port):
        self.rta_gateway, self.con_gateway = gateway_port, gateway_port - 10
        self.rta_lobby = self.gateway(self.rta_gateway, 6)
        try:
            self.con_lobby = self.gateway(self.con_gateway, 2)
        except (OSError, Gone, TypeError, ValueError, struct.error):
            self.con_lobby = None
        self.discovery = None

    def gateway(self, port, version, wait=5.0):
        c = Conn(port, [], version)
        try:
            got = c.call(0x43, u32(12), wait)
            if got is None or got[0] != 0:
                raise Gone("no gateway reply")
            body = got[1]
            if version == 6:
                n = struct.unpack_from("<I", body)[0]
                return struct.unpack_from("<I", body, 4 + n)[0]
            return struct.unpack_from("<I", body, body.index(b"\0") + 1)[0]
        finally:
            c.close()

    def alive(self, wait=8.0):
        try:
            return self.gateway(self.rta_gateway, 6, wait) == self.rta_lobby
        except (OSError, Gone, TypeError, ValueError, struct.error):
            return False


def new_account(t, c, rnd, tag):
    rnd.getrandbits(32)
    c.user = "fz%s%s" % (tag, os.urandom(4).hex())            # a new name every run, whatever the seed
    c.handle, c.password = c.user[:15], "pw-%08x" % rnd.getrandbits(32)
    c.call(0x27, nstr(b"fuzz") + u32(12))
    got = c.call(0x06, b"".join(estr(s) for s in (c.user, c.password, c.user + "@example.invalid",
                                                  "01/01/1990", c.handle)))
    if got and got[0] == 0:
        c.session = read_estr(got[1], 0)[0]
    return bool(got and got[0] == 0)


def login(c, password=None):
    got = c.call(0x16, estr(c.user) + estr(c.password if password is None else password))
    if got and got[0] == 0:
        c.session = read_estr(got[1], 0)[0]
    return got[0] if got else None


def upload(c, pid, head, data, piece=800):
    track = c.send(pid, head + nstr(data[:piece]) + u32(0))
    sent = min(piece, len(data))
    while True:
        rest = len(data) - sent
        n, last = (rest, 1) if rest <= piece else (piece, 0)
        c.send(0x5D, estr(c.session) + nstr(data[sent:sent + n]) + u32(last), track=track)
        sent += n
        if last:
            return track


def discovery(t, session, udp, mangle=None):
    if t.discovery:
        d = u32(0x20) + aes_enc(session.encode("utf-16-le")[:32].ljust(32, b"\0"))
        udp.sendto(mangle(d) if mangle else d, ("127.0.0.1", t.discovery))


# ---------------------------------------------------------------- mutation
EDGE = [0, 1, 2, 3, 4, 15, 16, 17, 255, 256, 1023, 1024, 2047, 2048, 0x7FFF, 0x8000, 0xFFFF, 0x10000,
        0x7FFFFFF0, 0x7FFFFFFF, 0x80000000, 0x80000001, 0xFFFFFFF0, 0xFFFFFFFE, 0xFFFFFFFF]
NASTY = ["", "\0", "a\0b", "A" * 15, "A" * 16, "A" * 64, "A" * 400, "{}{}{}", "%s%n%x", "'; DROP TABLE RealmUsers;--",
         "x' OR '1'='1", "\r\n[ERROR]: forged", "\x1b[2J", "\ud800", "\udfff\ud800", "[", "]", "a]b[c", "|",
         "1,2,3,4,5|", "999999999999,1,1,1,1|n", "-1,-1,-1,-1,-1|n", "1 / 4 :x", "￿" * 8, "é" * 10, "127.0.0.1",
         "0" * 16, "Champions Reborn"]


def rnd_text(rnd):
    k = rnd.random()
    if k < 0.5:
        return rnd.choice(NASTY)
    if k < 0.8:
        return "".join(chr(rnd.randrange(0x20, 0x7F)) for _ in range(rnd.randrange(0, 24)))
    return "".join(chr(rnd.choice([rnd.randrange(0, 0x20), rnd.randrange(0x20, 0xD800), rnd.randrange(0xE000, 0xFFFF)]))
                   for _ in range(rnd.randrange(0, 80)))


def field(rnd, kind, ctx):
    """One body field of the given kind, mostly well-formed."""
    bad = rnd.random() < 0.25
    if kind == "S":
        if not bad:
            return estr(ctx["session"])
        return rnd.choice([estr(rnd.choice(ctx["others"] + ["0" * 16, "", "F" * 15, "F" * 17])),
                           estr("%016X" % rnd.getrandbits(64)), u32(rnd.choice(EDGE)) + u32(rnd.choice(EDGE)),
                           u32(18) + u32(32) + os.urandom(32), u32(rnd.choice(EDGE)) + u32(32) + bytes(rnd.randrange(0, 40))])
    if kind == "E":
        s = rnd_text(rnd) if bad else rnd.choice(ctx["names"])
        return rnd.choice([estr(s), u32(rnd.choice(EDGE)) + u32(rnd.choice(EDGE)) + bytes(rnd.randrange(0, 40))]) if bad else estr(s)
    if kind == "W":
        s = rnd_text(rnd) if bad or rnd.random() < 0.3 else rnd.choice(ctx["names"])
        return u32(rnd.choice(EDGE)) + s.encode("utf-16-le", "surrogatepass") if bad and rnd.random() < 0.4 else wstr(s)
    if kind == "N":
        b = os.urandom(rnd.randrange(0, 40)) if bad else rnd.choice([b"1 / 4 :fuzz".ljust(300, b" "), b"x" * 256, b"192.0.2.1"])
        return u32(rnd.choice(EDGE)) + b if bad and rnd.random() < 0.5 else nstr(b)
    if kind == "Z":
        return rnd.choice([b"en\0", b"", b"abc", bytes(rnd.randrange(0, 5)), os.urandom(rnd.randrange(0, 30))])
    if kind == "B":
        b = rnd.choice([character(rnd.randrange(256)), os.urandom(rnd.randrange(0, 900)), b"", b"\0" * rnd.randrange(1, 900),
                        character(1, 700)[:800]])
        return u32(rnd.choice(EDGE)) + b[:rnd.randrange(0, 60)] if bad else nstr(b)
    if kind == "M":
        if not bad:
            return meta(str(rnd.randrange(1, 80)))
        n = rnd.choice([0, 1, 3, 4, 5, 0xFFFFFFFF, 0x7FFFFFFF])
        return u32(n) + b"".join(wstr(rnd_text(rnd)) for _ in range(min(n, 6))) + u32(rnd.choice(EDGE)) + \
            b"".join(wstr(rnd_text(rnd)) for _ in range(rnd.randrange(0, 6)))
    if kind == "u8":
        return u8(rnd.getrandbits(8))
    if kind == "u16":
        return u16(rnd.choice([0, 1, 0xFFFF, rnd.getrandbits(16)]))
    if kind == "id":                                # a game or character id
        return u32(rnd.choice(EDGE) if bad else rnd.randrange(0, 40))
    if kind == "F":                                 # the filter list of 0x18
        n = rnd.choice([0, 1, 2, 3]) if not bad else rnd.choice(EDGE)
        return u32(n) + b"".join(u16(1) + u32(2) + wstr("f") + u32(1) * 3 + u16(1) for _ in range(min(n, 4)))
    return u32(rnd.choice(EDGE) if bad else rnd.randrange(0, 5))


LAYOUT = {
    0x01: "S W", 0x02: "S W", 0x05: "S", 0x06: "E E E E E", 0x08: "S W", 0x09: "S W", 0x0A: "S u16 u32 u32 u32 W",
    0x0C: "S W", 0x0D: "S W", 0x0E: "S", 0x0F: "", 0x11: "S W", 0x15: "S W", 0x16: "E E", 0x17: "S",
    0x18: "S u16 u32 u32 u32 F u8 u32 u32", 0x19: "S W W", 0x1C: "S W", 0x1D: "S W", 0x21: "S W W", 0x22: "S W W",
    0x23: "S", 0x24: "S", 0x25: "S id", 0x27: "N u32", 0x42: "Z", 0x43: "u32", 0x44: "S N",
    0x54: "S u16 u32 u32 u32 W u32 u32 W u32", 0x55: "S", 0x56: "S W W u32", 0x57: "S W", 0x58: "S u32 u32 M B u32",
    0x5A: "S id", 0x5B: "S", 0x5C: "S id", 0x5D: "S B u32", 0x5E: "S S id u32 u32 M B u32", 0x5F: "S S", 0x60: "S",
    0x61: "S", 0x62: "S", 0x63: "S", 0x64: "E E", 0x66: "S id W u32",
}


def mutated(rnd, pid, ctx):
    body = b"".join(field(rnd, k, ctx) for k in LAYOUT.get(pid, "S W u32").split())
    k = rnd.random()
    if k < 0.12 and body:
        body = body[:rnd.randrange(0, len(body))]
    elif k < 0.2:
        body += os.urandom(rnd.randrange(1, 64))
    elif k < 0.3 and body:
        b = bytearray(body)
        for _ in range(rnd.randrange(1, 6)):
            b[rnd.randrange(len(b))] ^= 1 << rnd.randrange(8)
        body = bytes(b)
    elif k < 0.33:
        body = os.urandom(rnd.randrange(0, 200))
    return body[:2030]


# ---------------------------------------------------------------- one fuzz session
def session(t, seed, number, udp):
    rnd = random.Random("%d/%d" % (seed, number))
    transcript, conns = [], []
    ctx = {"session": "0" * 16, "others": [], "names": ROOMS + ["fuzzgame%d" % number, "1,0,0,0,0|fuzzgame%d" % number,
                                                              "fuzzgame%d [stage]" % number, "192.0.2.7"]}

    def open_conn(kind):
        port = {"rta": t.rta_lobby, "con": t.con_lobby or t.rta_lobby, "gw": t.rta_gateway + rnd.randrange(8),
                "cgw": t.con_gateway + rnd.randrange(8)}[kind]
        c = Conn(port, transcript, 2 if kind in ("con", "cgw") else 6)
        c.kind = kind
        conns.append(c)
        return c

    try:
        for _ in range(rnd.randrange(1, 4)):
            c = open_conn(rnd.choice(["rta"] * 6 + ["con"] * 2 + ["gw", "cgw"]))
            if c.kind == "rta" and rnd.random() < 0.9:
                if new_account(t, c, rnd, "%x" % (number & 0xFFF)) and rnd.random() < 0.5:
                    c.close(hard=rnd.random() < 0.3)            # come back by logging in
                    conns.remove(c)
                    d = open_conn("rta")
                    d.user, d.handle, d.password = c.user, c.handle, c.password
                    d.call(0x27, nstr(b"fuzz") + u32(12))
                    if rnd.random() < 0.3:
                        login(d, "wrong-password")
                    login(d)
                    c = d
            elif c.kind == "con" and rnd.random() < 0.9:
                c.user, c.password = "foo", "bar"
                c.call(0x27, nstr(b"fuzz") + u32(12))
                login(c)
            if c.session:
                ctx["others"].append(c.session)
                ctx["names"].append(c.handle or "x")
        logged = [c for c in conns if c.session]
        for step in range(rnd.randrange(20, 120)):
            c = rnd.choice(conns)
            ctx["session"] = c.session or "0" * 16
            k = rnd.random()
            if k < 0.45:                                            # any id, mutated body
                c.send(rnd.choice(ALL_IDS) if rnd.random() < 0.97 else rnd.randrange(0, 0x70), mutated(rnd, 0, ctx)
                       if rnd.random() < 0.03 else mutated(rnd, rnd.choice(ALL_IDS), ctx))
            elif k < 0.75:                                          # the right body for the id
                pid = rnd.choice(ALL_IDS if rnd.random() < 0.8 else [0x54, 0x57, 0x5F, 0x23, 0x60, 0x09, 0x0C, 0x22, 0x58, 0x5E, 0x5D])
                if pid == 0x17 and rnd.random() < 0.8:
                    continue
                c.send(pid, mutated(rnd, pid, ctx))
            elif k < 0.85 and logged:                               # what the game does, in order
                a, b = rnd.choice(logged), rnd.choice(logged)
                name, sa, sb = "fuzzgame%d" % number, estr(a.session), estr(b.session)
                flow = rnd.randrange(6)
                if flow == 0:
                    got = a.call(0x54, sa + u16(1) + u32(0) * 3 + wstr("1,0,0,0,0|" + name) + u32(0) * 2 + wstr("192.0.2.7") + u32(4001), 2)
                    if got and got[0] == 0 and len(got[1]) >= 8:
                        n = struct.unpack_from("<I", got[1])[0]
                        t.discovery = struct.unpack_from("<I", got[1], 4 + n)[0] if len(got[1]) >= 8 + n else t.discovery
                    discovery(t, a.session, udp)
                    a.send(0x09, sa + wstr(name))
                elif flow == 1:
                    b.send(0x57, sb + wstr(name))
                    discovery(t, b.session, udp)
                    b.send(0x5F, sb + sa)
                    b.send(0x0C, sb + wstr(name))
                elif flow == 2:
                    a.send(rnd.choice([0x23, 0x60, 0x60]), sa)
                elif flow == 3:
                    data = character(rnd.randrange(256), rnd.choice([0, 0, 900, 1454, 5000]))
                    upload(a, 0x58, sa + u32(0) * 2 + meta(), data, rnd.choice([800, 800, 1, 1000, 1900]))
                    a.send(0x5B, sa)
                    a.send(0x5C, sa + u32(rnd.randrange(1, 30)))
                elif flow == 4:
                    data = character(rnd.randrange(256), rnd.choice([0, 1700]))
                    upload(a, 0x5E, sa + sb + u32(rnd.randrange(0, 30)) + u32(0) * 2 + meta("9"), data)
                else:
                    a.send(0x0C, sa + wstr(rnd.choice(ROOMS)))
                    a.send(0x22, sa + wstr(rnd.choice(ROOMS)) + wstr(rnd_text(rnd)))
                    a.send(0x01, sa + wstr(b.handle))
                    a.send(0x21, sa + wstr(b.handle) + wstr(rnd_text(rnd)))
                    a.send(rnd.choice([0x02, 0x1C, 0x1D, 0x61, 0x63]), sa + wstr(b.handle))
            elif k < 0.88:                                          # frames no parser should see
                c.raw(rnd.choice([frame(b""), frame(os.urandom(rnd.randrange(0, 300))), frame(b"")[:rnd.randrange(1, 4)],
                                  struct.pack(">i", rnd.choice([0, 1, 3, -1, 2049, 0x7FFFFFFF])) + os.urandom(8),
                                  os.urandom(rnd.randrange(1, 400))]))
            elif k < 0.92:                                          # datagrams
                for _ in range(rnd.randrange(1, 8)):
                    if rnd.random() < 0.5 and c.session:
                        discovery(t, c.session, udp, rnd.choice([None, lambda d: d[:rnd.randrange(0, len(d))],
                                                                 lambda d: d + os.urandom(16), lambda d: d[:4] + os.urandom(32)]))
                    elif t.discovery:
                        udp.sendto(rnd.choice([b"", u32(0x20) + os.urandom(rnd.choice([0, 16, 31, 32, 48, 900])),
                                               os.urandom(rnd.randrange(0, 1200))]), ("127.0.0.1", t.discovery))
            elif k < 0.96:                                          # drop it, maybe mid-frame, and come back
                if rnd.random() < 0.5:
                    c.raw(frame(u16(0x5E) + u32(1) + u32(6) + os.urandom(200))[:rnd.randrange(1, 150)])
                old, kind = c, c.kind
                c.close(hard=rnd.random() < 0.5)
                conns.remove(c)
                c = open_conn(kind)
                c.user, c.handle, c.password, c.session = old.user, old.handle, old.password, old.session
                if old in logged:
                    logged.remove(old)
                    logged.append(c)
                    if rnd.random() < 0.5:
                        c.send(0x55, estr(old.session))             # carry on under the old session id
                    else:
                        login(c)
                        ctx["others"].append(c.session)
            else:                                                   # read what has piled up, or do not
                if rnd.random() < 0.7:
                    c.frames(0.0)
    except (Gone, OSError, struct.error, ValueError, TypeError, IndexError):
        pass                                                        # the server may hang up on any of this
    finally:
        for c in conns:
            try:
                c.frames(0.0)
            except (OSError, ValueError):
                pass
            c.close(hard=rnd.random() < 0.2)
    return transcript


def pid_alive(pid):
    try:
        os.kill(pid, 0)
        return True
    except OSError:
        return False


def fuzz(args):
    t = Target(args.gateway_port)
    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    end = time.monotonic() + args.minutes * 60 if args.minutes else None
    numbers = [args.only_session] if args.only_session is not None else range(args.start, args.start + (args.sessions or 10 ** 9))
    done, frames_sent, started = 0, 0, time.monotonic()
    for number in numbers:
        if end and time.monotonic() > end:
            break
        try:
            transcript = session(t, args.seed, number, udp)
        except (Gone, OSError) as e:
            transcript = [("could not connect: %s" % e).encode()]
        done, frames_sent = done + 1, frames_sent + len(transcript)
        dead = args.pid and not pid_alive(args.pid)
        if dead or not t.alive():
            time.sleep(0.5)
            dead = args.pid and not pid_alive(args.pid)
            if dead or not t.alive(20):
                if args.transcripts:
                    os.makedirs(args.transcripts, exist_ok=True)
                    with open(os.path.join(args.transcripts, "session-%d-%d.hex" % (args.seed, number)), "w") as f:
                        f.writelines(d.hex() + "\n" for d in transcript)
                print("FAIL: the server %s after session %d (seed %d); run again with --seed %d --only-session %d"
                      % ("process is gone" if dead else "stopped answering", number, args.seed, args.seed, number))
                return 1
        if done % 200 == 0:
            print("  %d sessions, %d messages, %.0f s" % (done, frames_sent, time.monotonic() - started), flush=True)
    print("PASS: %d fuzz sessions (%d messages, seed %d, from session %d), the server still answers"
          % (done, frames_sent, args.seed, numbers[0]))
    return 0


# ---------------------------------------------------------------- abuse checks
def logged_in(t, rnd, tag="l"):
    c = Conn(t.rta_lobby, [])
    if not new_account(t, c, rnd, tag):
        c.close()
        return None
    return c


def answer_time(t):
    start = time.monotonic()
    return time.monotonic() - start if t.alive(10) else None


def game_count(c):
    got = c.call(0x55, estr(c.session))
    return struct.unpack_from("<I", got[1])[0] if got and got[0] == 0 and len(got[1]) >= 4 else None


def probe_from(t, other):
    """Seconds a gateway request from another address of this machine takes, or None."""
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(5)
    try:
        s.bind((other, 0))
        s.connect(("127.0.0.1", t.rta_gateway))
        began = time.monotonic()
        s.sendall(frame(u16(0x43) + u32(1) + u32(6) + u32(12)))
        return time.monotonic() - began if s.recv(64) else None
    except OSError:
        return None
    finally:
        s.close()


def check_connections(t, rnd, other=""):
    """Connections that say nothing: how many one address may hold, and for how long."""
    held, refused = [], 0
    for _ in range(1500):
        try:
            s = socket.create_connection(("127.0.0.1", t.rta_lobby), timeout=2)
            held.append(s)
        except OSError:
            refused += 1
    time.sleep(1.0)
    open_now = 0
    for s in held:
        s.setblocking(False)
        try:
            if s.recv(1) == b"":
                continue
        except BlockingIOError:
            open_now += 1
        except OSError:
            continue
    took = probe_from(t, other) if other else None
    start = time.monotonic()
    while time.monotonic() - start < 45:
        r, _, _ = select.select(held[:1000], [], [], 1.0)
        alive = 0
        for s in held:
            try:
                if s.recv(1) != b"":
                    alive += 1
            except BlockingIOError:
                alive += 1
            except OSError:
                pass
        if alive == 0:
            break
    lasted = time.monotonic() - start
    for s in held:
        s.close()
    ok = open_now <= 64 and alive == 0 and (not other or (took is not None and took < 2))
    return ok, ("of 1500 silent connections from one address %d were kept (%d refused at once)%s; the silent ones %s"
                % (open_now, refused, "" if not other else "; a console at %s was answered in %s meanwhile"
                   % (other, "%.3f s" % took if took is not None else "NOT AT ALL"),
                   "were all closed after %.0f s" % lasted if alive == 0 else "were still open after %.0f s (%d of them)" % (lasted, alive)))


def check_slow_frame(t, rnd):
    """A frame that arrives a byte at a time must not keep a connection open for ever."""
    s = socket.create_connection(("127.0.0.1", t.rta_lobby), timeout=2)
    data, start, sent = frame(u16(0x42) + u32(1) + u32(6) + b"x" * 1500), time.monotonic(), 0
    try:
        while time.monotonic() - start < 150:
            s.sendall(data[sent:sent + 1])
            sent += 1
            r, _, _ = select.select([s], [], [], 5.0)
            if r and s.recv(16) == b"":
                break
    except OSError:
        pass
    lasted = time.monotonic() - start
    s.close()
    return lasted < 140, "a connection sending one byte every 5 s was closed after %.0f s" % lasted


def check_no_reader(t, rnd):
    """A client that asks and never reads: the others must not wait, and it must be dropped."""
    c = logged_in(t, rnd)
    if c is None:
        return False, "could not create an account"
    worst, dropped, start = 0.0, False, time.monotonic()
    try:
        c.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 2048)
        for i in range(400):
            c.raw(b"".join(frame(u16(0x0E) + u32(i) + u32(6) + estr(c.session)) for _ in range(50)))
            if i % 40 == 0:
                took = answer_time(t)
                worst = max(worst, took if took is not None else 99)
    except Gone:
        dropped = True
    c.close()
    return worst < 2, ("20,000 unread requests: the slowest answer to somebody else took %.3f s; the sender was %s"
                       % (worst, "dropped" if dropped else "slowed down, not dropped, in %.0f s" % (time.monotonic() - start)))


def check_games(t, rnd):
    c, watcher = logged_in(t, rnd, "g"), logged_in(t, rnd, "w")
    if c is None or watcher is None:
        return False, "could not create an account"
    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    made = 0
    for i in range(300):
        got = c.call(0x54, estr(c.session) + u16(1) + u32(0) * 3 + wstr("1,0,0,0,0|flood%d_%d" % (i, rnd.getrandbits(20)))
                     + u32(0) * 2 + wstr("192.0.2.9") + u32(4001), 10)
        if got and got[0] == 0:
            made += 1
            n = struct.unpack_from("<I", got[1])[0]
            t.discovery = struct.unpack_from("<I", got[1], 4 + n)[0]
            discovery(t, c.session, udp)
            c.frames(0.02)
    time.sleep(0.3)
    listed = game_count(watcher)
    c.close()
    time.sleep(0.5)
    after = game_count(watcher)
    watcher.close()
    return listed is not None and listed <= 1 and after == 0, \
        "one session opened a game 300 times (%d accepted): %s listed, %s after it left" % (made, listed, after)


def check_rooms(t, rnd):
    """A session has one game and so one game chat room: making another must not leave the first behind."""
    c, d = logged_in(t, rnd, "r"), logged_in(t, rnd, "s")
    if c is None or d is None:
        return False, "could not create an account"
    made, first = 0, "room0_%d" % rnd.getrandbits(20)
    c.send(0x09, estr(c.session) + wstr(first))
    for i in range(1, 2000):
        c.send(0x09, estr(c.session) + wstr("room%d_%d" % (i, rnd.getrandbits(20))))
        if i % 200 == 0:
            made += sum(fid == 0x09 and rest[:4] == u32(0) for fid, _, rest in c.frames(0.0))
    made += sum(fid == 0x09 and rest[:4] == u32(0) for fid, _, rest in c.frames(8.0))
    again = d.call(0x09, estr(d.session) + wstr(first))
    took = answer_time(t)
    c.close()
    d.close()
    gone = bool(again and again[0] == 0)
    return gone and took is not None and took < 2, (
        "2000 game chat rooms asked for by one session, %d granted; the first of them %s; a newcomer was answered in %s"
        % (made, "was gone by then (each replaced the one before)" if gone else "STILL EXISTED, with the 1999 after it",
           "%.3f s" % took if took is not None else "never"))


def check_accounts(t, rnd):
    made = 0
    for i in range(60):
        c = Conn(t.rta_lobby, [])
        made += new_account(t, c, rnd, "a")
        c.close()
    return made <= 30, "60 accounts asked for from one address in a row: %d created" % made


def check_passwords(t, rnd):
    c = logged_in(t, rnd, "p")
    if c is None:
        return False, "could not create an account"
    d = Conn(t.rta_lobby, [])
    d.user, d.password = c.user, c.password
    c.close()
    start, answered = time.monotonic(), 0
    for i in range(120):
        d.send(0x16, estr(d.user) + estr("guess%d" % i))
    while time.monotonic() - start < 20:
        answered += len(d.frames(0.5))
        if answered >= 120:
            break
    took, other = time.monotonic() - start, answer_time(t)
    d.close()
    return answered < 120 and other is not None and other < 2, \
        ("120 wrong passwords in a row: %d answered in the first %.0f s; somebody else was answered in %s"
         % (answered, took, "%.3f s" % other if other is not None else "never"))


def check_chat(t, rnd):
    c = logged_in(t, rnd, "c")
    if c is None:
        return False, "could not create an account"
    c.call(0x0C, estr(c.session) + wstr(ROOMS[0]))
    start, line = time.monotonic(), "x" * 900
    for i in range(3000):
        c.send(0x22, estr(c.session) + wstr(ROOMS[0]) + wstr(line))
        if i % 10 == 0:
            c.frames(0.0)
    c.frames(0.2)
    took = answer_time(t)
    c.close()
    return took is not None and took < 2, ("3000 chat lines of 900 characters from one session in %.1f s; a newcomer was answered in %s"
                                           % (time.monotonic() - start, "%.3f s" % took if took is not None else "never"))


def check_datagrams(t, rnd):
    if not t.discovery:
        return False, "the discovery port is not known (the game flood check finds it)"
    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    for i in range(20000):
        udp.sendto(u32(0x20) + os.urandom(32), ("127.0.0.1", t.discovery))
    took = answer_time(t)
    return took is not None and took < 2, ("20,000 well-shaped datagrams with unknown session ids (the server never answers a datagram, "
                                           "so there is nothing to amplify); a newcomer was answered in %s" % ("%.3f s" % took if took is not None else "never"))


def host_game(t, c, name, udp):
    got = c.call(0x54, estr(c.session) + u16(1) + u32(0) * 3 + wstr("1,0,0,0,0|" + name) + u32(0) * 2 + wstr("192.0.2.9") + u32(4001))
    if not got or got[0] != 0:
        return False
    n = struct.unpack_from("<I", got[1])[0]
    t.discovery = struct.unpack_from("<I", got[1], 4 + n)[0]
    discovery(t, c.session, udp)
    return any(fid == 0x3E for fid, _, _ in c.frames(1.0))


def check_port(t, rnd):
    """A joiner that never named a local port: the host must not be sent whatever was in memory."""
    a, b = logged_in(t, rnd, "h"), logged_in(t, rnd, "j")
    udp_a, udp_b = (socket.socket(socket.AF_INET, socket.SOCK_DGRAM) for _ in range(2))
    name = "port%d" % rnd.getrandbits(24)
    if a is None or b is None or not host_game(t, a, name, udp_a):
        return False, "could not open a game"
    got = b.call(0x57, estr(b.session) + wstr(name))
    discovery(t, b.session, udp_b)
    port = None
    for fid, _, rest in a.frames(1.5):
        if fid == 0x65:
            n = struct.unpack_from("<I", rest)[0]
            port = struct.unpack_from("<I", rest, 4 + n)[0]
    a.close()
    b.close()
    return port == 0, ("the host was told the joiner's local port is %s" % ("%#x" % port if port is not None else "nothing (no 0x65)")
                       + ("" if port in (0, None) else ", which the joiner never sent: memory the server had not set"))


def check_save(t, rnd):
    """A stranger who knows a session id must not be able to overwrite that player's character."""
    v, x = logged_in(t, rnd, "v"), logged_in(t, rnd, "x")
    if v is None or x is None:
        return False, "could not create an account"
    mine = character(7)
    v.frames(0.0)
    track = upload(v, 0x58, estr(v.session) + u32(0) * 2 + meta("5", "Victim"), mine)
    cid = None
    for fid, ftrack, rest in v.frames(2.0):
        if ftrack == track and len(rest) >= 8 and rest[:4] == u32(0):
            cid = struct.unpack_from("<I", rest, 4)[0]
    if not cid:
        return False, "could not create a character"
    track = upload(x, 0x5E, estr(x.session) + estr(v.session) + u32(cid) + u32(0) * 2 + meta("1", "Overwritten"), character(66))
    code = [rest[:4] for fid, ftrack, rest in x.frames(2.0) if ftrack == track]
    v.send(0x5C, estr(v.session) + u32(cid))
    data = b"".join(rest[8:8 + struct.unpack_from("<I", rest, 4)[0]] for fid, _, rest in v.frames(2.0) if fid == 0x5C and len(rest) > 8)
    kept = data[:len(mine)] == mine
    v.close()
    x.close()
    return kept, ("a save naming another account's session, from a connection that shares no game with it, was answered %s; "
                  "the character was %s" % (code[0].hex() if code else "not at all", "kept" if kept else "OVERWRITTEN"))


def check_no_login(t, rnd):
    """A connection that never logged in must not be able to act in the lobby."""
    c, w = Conn(t.rta_lobby, []), logged_in(t, rnd, "w")
    if w is None:
        return False, "could not create an account"
    w.call(0x0C, estr(w.session) + wstr(ROOMS[1]))
    none = estr("0" * 16)
    entered = c.call(0x0C, none + wstr(ROOMS[1]))
    c.send(0x22, none + wstr(ROOMS[1]) + wstr("unsigned text"))
    heard = any(fid == 0x3D for fid, _, _ in w.frames(1.0))
    game = c.call(0x54, none + u16(1) + u32(0) * 3 + wstr("1,0,0,0,0|nologin%d" % rnd.getrandbits(24)) + u32(0) * 2 + wstr("192.0.2.9") + u32(1))
    room = c.call(0x09, none + wstr("nologin%d" % rnd.getrandbits(24)))
    c.close()
    w.close()
    done = [n for n, got in (("entered a chat room", entered), ("opened a game", game), ("made a private room", room)) if got and got[0] == 0]
    if heard:
        done.append("spoke in a chat room under no name")
    return not done, "without logging in, a connection " + (", ".join(done) if done else "was refused a room, a game and a chat line")


def check_game_data(t, rnd):
    """Champions of Norrath: only the host may change what the list says about its game."""
    if not t.con_lobby:
        return True, "no Champions of Norrath lobby port"
    a, b = Conn(t.con_lobby, [], 2), Conn(t.con_lobby, [], 2)
    for c in (a, b):
        c.user, c.password = "foo", "bar"
        c.call(0x27, nstr(b"fuzz") + u32(12))
        login(c)
    name = "data%d" % rnd.getrandbits(24)
    made = a.call(0x0A, estr(a.session) + u16(1) + u32(0) * 3 + wstr(name + " [stage]"))
    b.call(0x0D, estr(b.session) + wstr(name + " [stage]"))                 # only asks about the game
    b.call(0x44, estr(b.session) + nstr(b"4 / 4 :defaced".ljust(300, b" ")))
    took = b.call(0x0D, estr(b.session) + wstr(name + " [stage]"))          # "full" is answered with 1
    a.close()
    b.close()
    if not made or made[0] != 0 or not took:
        return False, "could not open a game"
    return took[0] == 0, "a console that had only asked about a game sent new data for it; the game then %s" % (
        "was unchanged" if took[0] == 0 else "counted as full (result %d): its data had been replaced" % took[0])


def check_relogin(t, rnd, pid=0):
    """Logging in again and again with an upload begun each time must not pile uploads up."""
    c = logged_in(t, rnd, "m")
    if c is None:
        return False, "could not create an account"
    rss = lambda: int(os.popen("ps -o rss= -p %d" % pid).read() or 0) if pid else 0
    before, piece = rss(), nstr(b"\x01" * 1500)
    for i in range(3000):
        if login(c) != 0:
            break
        c.send(0x58, estr(c.session) + u32(0) * 2 + meta() + piece + u32(0))
        for _ in range(24):
            c.send(0x5D, estr(c.session) + piece + u32(0))
    took, after = answer_time(t), rss()
    c.close()
    grown = (after - before) / 1024.0
    return took is not None and grown < 40, ("%d logins on one connection, each followed by an unfinished upload of 37 KB: the server's "
                                              "memory grew by %.0f MB%s" % (i + 1, grown, "" if pid else " (not measured: no --pid)"))


def check_budget(t, rnd, other=""):
    """Requests beyond an address's budget wait; nothing is dropped, and other addresses do not wait."""
    c = Conn(t.rta_lobby, [])
    start, answered = time.monotonic(), 0
    for i in range(3000):
        c.send(0x0F)
    probe = probe_from(t, other) if other else None
    while time.monotonic() - start < 10:
        answered += len(c.frames(0.5))
    rate = answered / (time.monotonic() - start)
    still = True
    try:
        c.send(0x0F)
    except Gone:
        still = False
    c.close(hard=True)
    ok = answered < 2000 and still and (probe is None and not other or (probe is not None and probe < 1))
    return ok, ("3000 requests at once from one address: %d answered in the first 10 s (%.0f a second), the rest left waiting, the "
                "connection kept%s" % (answered, rate, "" if not other else "; a console at %s was answered in %s meanwhile"
                                       % (other, "%.3f s" % probe if probe is not None else "NOT AT ALL")))


def run_checks(args, checks):
    t, rnd, failed = Target(args.gateway_port), random.Random(args.seed), 0
    for name, fn in checks:
        if args.only and name not in args.only.split(","):
            continue
        try:
            ok, text = fn(t, rnd)
        except (Gone, OSError, struct.error, TypeError) as e:
            ok, text = False, "the check could not finish: %r" % (e,)
        if not t.alive(30) or (args.pid and not pid_alive(args.pid)):
            ok, text = False, text + "; THE SERVER NO LONGER ANSWERS"
        failed += not ok
        print("%s  %-11s %s" % ("PASS" if ok else "FAIL", name, text), flush=True)
        time.sleep(1.0)
    return 1 if failed else 0


def abuse(args):
    return run_checks(args, [("port", check_port), ("save", check_save), ("no-login", check_no_login), ("game-data", check_game_data),
                             ("relogin", lambda t, rnd: check_relogin(t, rnd, args.pid)), ("games", check_games), ("rooms", check_rooms),
                             ("chat", check_chat), ("datagrams", check_datagrams), ("no-reader", check_no_reader)])


def limits(args):
    return run_checks(args, [("slow-frame", check_slow_frame), ("connections", lambda t, rnd: check_connections(t, rnd, args.other_address)), ("passwords", check_passwords),
                             ("accounts", check_accounts), ("budget", lambda t, rnd: check_budget(t, rnd, args.other_address))])


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--gateway-port", type=int, required=True, help="rta_gateway_port of the instance under test")
    ap.add_argument("--mode", choices=["fuzz", "abuse", "limits", "all"], default="fuzz", help="all: fuzz, then abuse")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--start", type=int, default=0, help="first session number")
    ap.add_argument("--sessions", type=int, default=0, help="number of sessions (default: until --minutes is over, or 500)")
    ap.add_argument("--minutes", type=float, default=0)
    ap.add_argument("--only-session", type=int, default=None)
    ap.add_argument("--only", default="", help="abuse, limits: comma-separated names of the checks to run")
    ap.add_argument("--other-address", default="", help="limits: a second address of this machine (its LAN address), to "
                    "show that one address running out of budget does not hold up another")
    ap.add_argument("--pid", type=int, default=0, help="server process to watch")
    ap.add_argument("--transcripts", default="", help="directory for what was sent in a session that ended the server")
    args = ap.parse_args()
    if not args.sessions and not args.minutes:
        args.sessions = 500
    status = 0
    if args.mode in ("fuzz", "all"):
        status |= fuzz(args)
    if args.mode in ("abuse", "all"):
        status |= abuse(args)
    if args.mode == "limits":
        status |= limits(args)
    return status


if __name__ == "__main__":
    sys.exit(main())
