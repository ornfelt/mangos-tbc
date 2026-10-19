#!/usr/bin/env python3
"""
Headless TBC 2.4.3 client that checks the USE_CUSTOM_CHANGES battleground bot fill on a local
mangos-tbc: it logs a real (socket) player in, GM-teleports it next to the Orgrimmar battlemasters,
queues it, waits for the invite, enters the match, counts the players in it (MSG_PVP_LOG_DATA),
leaves, teleports the character back to where it was and logs out.

The account needs GM level 3 (.go xyz). The identity file holds WOW_USER=, WOW_PASS=, WOW_CHAR=
lines (the wow client's .probe-identity format); --char overrides WOW_CHAR. The password is never
printed. The character must be horde (the Orgrimmar battlemasters).

Usage examples:
warsong gulch with the identity file's character:
python3 bg_fill_test.py ~/Code2/Wow/tools/my_wow/c/wow/.probe-identity

the same identity as a named option, another character:
python3 bg_fill_test.py --identity ~/probe-identity --char Jonwar

arena skirmish 2v2, up to 300 s in the match (an arena sends its scoreboard only when it ends):
python3 bg_fill_test.py ~/probe-identity --queue 2v2 --stay 300

only check that the queue pops, do not enter the match:
python3 bg_fill_test.py ~/probe-identity --no-enter

back-to-back runs: remove the Deserter debuff an earlier run left (leaving a match early casts it):
python3 bg_fill_test.py ~/probe-identity --clear-deserter

another realm server, longer pop timeout:
python3 bg_fill_test.py ~/probe-identity --realm 192.168.1.20:3724 --pop-timeout 300

(no --show-cmd option: the work is a game protocol session, there is no shell command doing it)
"""

import argparse
import hashlib
import hmac
import os
import secrets
import select
import socket
import struct
import sys
import time
import zlib


# ---- colored prints ---------------------------------------------------------------------------

if os.name == "nt":
    os.system("")  # enables ANSI escape sequences in the Windows console

COLOR_RESET = "\033[0m"
COLOR_INFO = "\033[36m"
COLOR_OK = "\033[32m"
COLOR_WARN = "\033[33m"
COLOR_ERR = "\033[31m"


def print_info(msg):
    print(f"{COLOR_INFO}[info]{COLOR_RESET} {msg}", flush=True)


def print_ok(msg):
    print(f"{COLOR_OK}[ ok ]{COLOR_RESET} {msg}", flush=True)


def print_warn(msg):
    print(f"{COLOR_WARN}[warn]{COLOR_RESET} {msg}", flush=True)


def print_err(msg):
    print(f"{COLOR_ERR}[fail]{COLOR_RESET} {msg}", flush=True)


# ---- protocol constants -----------------------------------------------------------------------

CLIENT_BUILD = 8606
AUTH_LOGON_CHALLENGE = 0x00
AUTH_LOGON_PROOF = 0x01
AUTH_PROTOCOL_VERSION = 8
SRP_K = 3

CMSG_CHAR_ENUM = 0x037
SMSG_CHAR_ENUM = 0x03B
CMSG_PLAYER_LOGIN = 0x03D
SMSG_NEW_WORLD = 0x03E
CMSG_LOGOUT_REQUEST = 0x04B
SMSG_LOGOUT_COMPLETE = 0x04D
CMSG_MESSAGECHAT = 0x095
SMSG_MESSAGECHAT = 0x096
MSG_MOVE_WORLDPORT_ACK = 0x0DC
SMSG_NOTIFICATION = 0x1CB
SMSG_AUTH_CHALLENGE = 0x1EC
CMSG_AUTH_SESSION = 0x1ED
SMSG_AUTH_RESPONSE = 0x1EE
SMSG_LOGIN_VERIFY_WORLD = 0x236
SMSG_BATTLEFIELD_STATUS = 0x2D4
CMSG_BATTLEFIELD_PORT = 0x2D5
MSG_PVP_LOG_DATA = 0x2E0
CMSG_LEAVE_BATTLEFIELD = 0x2E1
SMSG_GROUP_JOINED_BATTLEGROUND = 0x2E8
CMSG_BATTLEMASTER_JOIN = 0x2EE
CMSG_BATTLEMASTER_JOIN_ARENA = 0x358

AUTH_OK = 0x0C
CHAT_MSG_SAY = 0x01
LANG_ORCISH = 1
HIGHGUID_UNIT = 0xF130
SPELL_DESERTER = 26013
# SMSG_GROUP_JOINED_BATTLEGROUND error codes (BattleGroundGroupJoinStatus)
JOIN_ERRORS = {-1: "not eligible", -2: "deserter", -3: "not in an arena team", -4: "too many queues",
               -5: "cannot queue for rated", -6: "queued for rated", -7: "team left the queue"}

STATUS_WAIT_QUEUE = 1
STATUS_WAIT_JOIN = 2
STATUS_IN_PROGRESS = 3

CRYPT_SEED = bytes([0x38, 0xA7, 0x83, 0x15, 0xF8, 0x92, 0x25, 0x30, 0x71, 0x98, 0x67, 0xB1, 0x8C, 0x04, 0xE2, 0xAA])
ADDON_NAME = b"Blizzard_AuctionUI\x00"
ADDON_CRC = 0x4C1C776D

# Orgrimmar, Valley of Honor: in front of the battlemasters
BATTLEMASTER_SPOT = (1985.0, -4796.0, 56.0, 1)
# (creature entry, spawn guid) of Brakgul Deathbringer - both join handlers only check that the
# creature is a battlemaster on the player's map, so it serves the arena queues as well
BATTLEMASTER = (3890, 4765)

# queue name -> (bg type id, arena slot or None, players per team)
QUEUES = {
    "av": (1, None, 40),
    "wsg": (2, None, 10),
    "ab": (3, None, 15),
    "eye": (7, None, 15),
    "2v2": (6, 0, 2),
    "3v3": (6, 1, 3),
    "5v5": (6, 2, 5),
}

GRID_LOAD_WAIT = 3.0      # s after a teleport, until the battlemaster grid is loaded
PVP_LOG_INTERVAL = 5.0    # s between scoreboard requests inside a battleground (arenas send it only at their end)


def sha1(*parts):
    h = hashlib.sha1()
    for part in parts:
        h.update(part)
    return h.digest()


def read_cstring(data, pos):
    end = data.index(b"\x00", pos)
    return data[pos:end].decode("utf-8", "replace"), end + 1


# ---- realm (auth) server ----------------------------------------------------------------------

def recv_exact(sock, size):
    data = b""
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            raise ConnectionError("connection closed")
        data += chunk
    return data


def realm_login(host, port, user, password):
    """SRP6 login at the realm server, returns the 40 byte session key K."""
    user_up = user.upper().encode()
    pass_up = password.upper().encode()

    sock = socket.create_connection((host, port), timeout=10)
    body = (b"WoW\x00" + bytes([2, 4, 3]) + struct.pack("<H", CLIENT_BUILD) + b"68x\x00" + b"niW\x00" + b"SUne"
            + struct.pack("<II", 0, 0x0100007F) + bytes([len(user_up)]) + user_up)
    sock.sendall(struct.pack("<BBH", AUTH_LOGON_CHALLENGE, AUTH_PROTOCOL_VERSION, len(body)) + body)

    cmd, _, error = recv_exact(sock, 3)
    if cmd != AUTH_LOGON_CHALLENGE or error != 0:
        raise RuntimeError(f"logon challenge refused (error {error})")
    B_bytes = recv_exact(sock, 32)
    g_len = recv_exact(sock, 1)[0]
    g_bytes = recv_exact(sock, g_len)
    N_len = recv_exact(sock, 1)[0]
    N_bytes = recv_exact(sock, N_len)
    salt = recv_exact(sock, 32)
    recv_exact(sock, 16)
    security_flags = recv_exact(sock, 1)[0]
    if security_flags:
        raise RuntimeError(f"account has security flags {security_flags} (pin / authenticator), not supported")

    N = int.from_bytes(N_bytes, "little")
    g = int.from_bytes(g_bytes, "little")
    B = int.from_bytes(B_bytes, "little")
    x = int.from_bytes(sha1(salt, sha1(user_up + b":" + pass_up)), "little")
    a = int.from_bytes(secrets.token_bytes(19), "little")
    A_bytes = pow(g, a, N).to_bytes(32, "little")
    u = int.from_bytes(sha1(A_bytes, B_bytes), "little")
    S = pow((B - SRP_K * pow(g, x, N)) % N, a + u * x, N)
    S_bytes = S.to_bytes(32, "little")
    even, odd = sha1(S_bytes[0::2]), sha1(S_bytes[1::2])
    K = bytes(b for pair in zip(even, odd) for b in pair)
    ng = bytes(p ^ q for p, q in zip(sha1(N_bytes), sha1(g_bytes)))
    M1 = sha1(ng, sha1(user_up), salt, A_bytes, B_bytes, K)

    sock.sendall(bytes([AUTH_LOGON_PROOF]) + A_bytes + M1 + bytes(20) + bytes([0, 0]))
    cmd, error = recv_exact(sock, 2)
    if cmd != AUTH_LOGON_PROOF or error != 0:
        raise RuntimeError(f"logon proof refused (error {error}), wrong password?")
    sock.close()
    return K


# ---- world server -----------------------------------------------------------------------------

class WorldClient:
    def __init__(self, host, port, account, session_key):
        self.sock = socket.create_connection((host, port), timeout=10)
        self.account = account.upper()
        self.K = session_key
        self.crypt_key = None
        self.send_i = self.send_j = self.recv_i = self.recv_j = 0
        self.buffer = b""
        self.pending_header = None

    # K as the server's BigNumber::AsByteArray(): little endian without high zero bytes
    def key_bytes(self):
        return self.K.rstrip(b"\x00")

    def init_crypt(self):
        self.crypt_key = hmac.new(CRYPT_SEED, self.key_bytes(), hashlib.sha1).digest()

    def encrypt(self, header):
        out = bytearray(header)
        for t in range(len(out)):
            self.send_i %= len(self.crypt_key)
            x = ((out[t] ^ self.crypt_key[self.send_i]) + self.send_j) & 0xFF
            self.send_i += 1
            out[t] = self.send_j = x
        return bytes(out)

    def decrypt(self, header):
        out = bytearray(header)
        for t in range(len(out)):
            self.recv_i %= len(self.crypt_key)
            x = ((out[t] - self.recv_j) & 0xFF) ^ self.crypt_key[self.recv_i]
            self.recv_i += 1
            self.recv_j = header[t]
            out[t] = x
        return bytes(out)

    def send(self, opcode, payload=b""):
        header = struct.pack(">H", len(payload) + 4) + struct.pack("<I", opcode)
        if self.crypt_key:
            header = self.encrypt(header)
        self.sock.sendall(header + payload)

    def poll(self, timeout):
        """Returns the packets (opcode, payload) received within timeout seconds."""
        packets = []
        ready, _, _ = select.select([self.sock], [], [], timeout)
        if ready:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("world server closed the connection")
            self.buffer += chunk
        while True:
            if self.pending_header is None:
                if len(self.buffer) < 4:
                    break
                header = self.buffer[:4]
                self.buffer = self.buffer[4:]
                if self.crypt_key:
                    header = self.decrypt(header)
                size = struct.unpack(">H", header[:2])[0]
                opcode = struct.unpack("<H", header[2:4])[0]
                self.pending_header = (size - 2, opcode)
            size, opcode = self.pending_header
            if len(self.buffer) < size:
                break
            packets.append((opcode, self.buffer[:size]))
            self.buffer = self.buffer[size:]
            self.pending_header = None
        return packets

    def wait_for(self, opcode, timeout, handler=None):
        deadline = time.time() + timeout
        while time.time() < deadline:
            for op, payload in self.poll(0.2):
                if handler:
                    handler(op, payload)
                if op == opcode:
                    return payload
        raise TimeoutError(f"no opcode 0x{opcode:03X} within {timeout} s")

    def authenticate(self):
        payload = self.wait_for(SMSG_AUTH_CHALLENGE, 10)
        server_seed = payload[:4]
        client_seed = secrets.token_bytes(4)
        digest = sha1(self.account.encode(), bytes(4), client_seed, server_seed, self.key_bytes())
        addons = ADDON_NAME + struct.pack("<IIB", ADDON_CRC, 0, 0)
        body = (struct.pack("<II", CLIENT_BUILD, 0) + self.account.encode() + b"\x00" + client_seed + digest
                + struct.pack("<I", len(addons)) + zlib.compress(addons))
        self.send(CMSG_AUTH_SESSION, body)
        self.init_crypt()
        response = self.wait_for(SMSG_AUTH_RESPONSE, 10)
        if response[0] != AUTH_OK:
            raise RuntimeError(f"world auth refused (code 0x{response[0]:02X})")


def parse_char_enum(payload):
    chars = []
    count = payload[0]
    pos = 1
    for _ in range(count):
        guid = struct.unpack_from("<Q", payload, pos)[0]
        pos += 8
        name, pos = read_cstring(payload, pos)
        race, cls, _, _, _, _, _, _, level = payload[pos:pos + 9]
        pos += 9
        zone, map_id, x, y, z = struct.unpack_from("<IIfff", payload, pos)
        pos += 20
        pos += 4 + 4 + 1 + 12 + 20 * 9   # guild, flags, first login, pet, equipment
        chars.append(dict(guid=guid, name=name, race=race, cls=cls, level=level, map=map_id, x=x, y=y, z=z))
    return chars


def parse_battlefield_status(payload):
    queue_slot = struct.unpack_from("<I", payload, 0)[0]
    packed = struct.unpack_from("<Q", payload, 4)[0]
    if packed == 0:
        return dict(slot=queue_slot, status=0)
    status = struct.unpack_from("<I", payload, 17)[0]
    return dict(slot=queue_slot, status=status, arena_type=packed & 0xFF, unk=(packed >> 8) & 0xFF,
                bg_type=(packed >> 16) & 0xFFFFFFFF)


def parse_pvp_log(payload):
    """Returns (players, alliance, horde) - the team split only for arenas."""
    is_arena = payload[0]
    pos = 1
    if is_arena:
        pos += 16
        _, pos = read_cstring(payload, pos)
        _, pos = read_cstring(payload, pos)
    finished = payload[pos]
    pos += 1 + (1 if finished else 0)
    count = struct.unpack_from("<i", payload, pos)[0]
    pos += 4
    alliance = horde = 0
    for _ in range(count):
        pos += 8 + 4
        if is_arena:
            if payload[pos]:
                alliance += 1
            else:
                horde += 1
            pos += 1
        else:
            pos += 12
        pos += 8
        stats = struct.unpack_from("<I", payload, pos)[0]
        pos += 4 + 4 * stats
    return count, alliance, horde


def say(client, text):
    client.send(CMSG_MESSAGECHAT, struct.pack("<II", CHAT_MSG_SAY, LANG_ORCISH) + text.encode() + b"\x00")


def teleport(client, x, y, z, map_id, handler):
    """GM teleport; a far one needs the worldport ack."""
    say(client, f".go xyz {x:.2f} {y:.2f} {z:.2f} {map_id}")
    try:
        client.wait_for(SMSG_NEW_WORLD, 10, handler)
        client.send(MSG_MOVE_WORLDPORT_ACK)
    except TimeoutError:
        pass   # same map: a near teleport, nothing to acknowledge


def read_identity(path):
    values = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if "=" in line and not line.startswith("#"):
                key, value = line.split("=", 1)
                values[key.strip()] = value.strip()
    return values


def main():
    parser = argparse.ArgumentParser(description="Check the playerbot battleground queue fill with a headless client.")
    parser.add_argument("identity_pos", nargs="?", metavar="IDENTITY", help="identity file (WOW_USER= / WOW_PASS= / WOW_CHAR=)")
    parser.add_argument("--identity", help="identity file, same as the positional argument")
    parser.add_argument("--char", help="character to log in (default: WOW_CHAR of the identity file)")
    parser.add_argument("--realm", default="127.0.0.1:3724", help="realm server host:port (default 127.0.0.1:3724)")
    parser.add_argument("--queue", default="wsg", type=str.lower, choices=sorted(QUEUES), help="queue to join (default wsg)")
    parser.add_argument("--pop-timeout", type=float, default=180, help="seconds to wait for the invite (default 180)")
    parser.add_argument("--stay", type=float, default=60, help="seconds inside the match before leaving (default 60)")
    parser.add_argument("--no-enter", action="store_true", help="leave the queue when invited instead of entering")
    parser.add_argument("--clear-deserter", action="store_true", help="remove the Deserter debuff before queueing (GM .unaura)")
    args = parser.parse_args()

    identity_path = args.identity or args.identity_pos
    if not identity_path:
        parser.error("an identity file is needed (positional or --identity)")
    identity = read_identity(os.path.expanduser(identity_path))
    user, password = identity.get("WOW_USER"), identity.get("WOW_PASS")
    char_name = (args.char or identity.get("WOW_CHAR") or "").lower()
    if not user or not password or not char_name:
        print_err("identity file needs WOW_USER, WOW_PASS and a character (WOW_CHAR or --char)")
        return 2

    host, realm_port = args.realm.rsplit(":", 1)
    bg_type, arena_slot, team_size = QUEUES[args.queue]

    print_info(f"realm login as {user.upper()} at {args.realm}")
    K = realm_login(host, int(realm_port), user, password)
    client = WorldClient(host, 8085, user, K)
    client.authenticate()
    print_ok("world server authenticated")

    client.send(CMSG_CHAR_ENUM)
    chars = parse_char_enum(client.wait_for(SMSG_CHAR_ENUM, 10))
    char = next((c for c in chars if c["name"].lower() == char_name), None)
    if not char:
        print_err(f"no character {char_name} on the account ({', '.join(c['name'] for c in chars)})")
        return 2

    state = dict(status=None, invite_time=None, in_match=False, players=None)

    def handler(opcode, payload):
        if opcode == SMSG_BATTLEFIELD_STATUS:
            info = parse_battlefield_status(payload)
            if info["status"] == STATUS_WAIT_JOIN and state["status"] != STATUS_WAIT_JOIN:
                state["invite_time"] = time.time()
                state["invite"] = info
            state["status"] = info["status"]
        elif opcode == MSG_PVP_LOG_DATA:
            state["players"] = parse_pvp_log(payload)
        elif opcode == SMSG_GROUP_JOINED_BATTLEGROUND:
            code = struct.unpack_from("<i", payload, 0)[0]
            if code < 0:
                print_err(f"queue join refused: {JOIN_ERRORS.get(code, code)}")
        elif opcode == SMSG_NOTIFICATION:
            print_warn("server notification: " + read_cstring(payload, 0)[0])

    client.send(CMSG_PLAYER_LOGIN, struct.pack("<Q", char["guid"]))
    client.wait_for(SMSG_LOGIN_VERIFY_WORLD, 30, handler)
    print_ok(f"{char['name']} (level {char['level']}) in the world at map {char['map']} ({char['x']:.1f}, {char['y']:.1f}, {char['z']:.1f})")

    result = 1
    try:
        x, y, z, map_id = BATTLEMASTER_SPOT
        teleport(client, x, y, z, map_id, handler)
        end = time.time() + GRID_LOAD_WAIT
        while time.time() < end:
            for op, payload in client.poll(0.2):
                handler(op, payload)

        if args.clear_deserter:
            say(client, f".unaura {SPELL_DESERTER}")

        entry, spawn = BATTLEMASTER
        master_guid = (HIGHGUID_UNIT << 48) | (entry << 24) | spawn
        if arena_slot is None:
            client.send(CMSG_BATTLEMASTER_JOIN, struct.pack("<QIIB", master_guid, bg_type, 0, 0))
        else:
            client.send(CMSG_BATTLEMASTER_JOIN_ARENA, struct.pack("<QBBB", master_guid, arena_slot, 0, 0))
        join_time = time.time()
        print_info(f"queued for {args.queue} ({team_size} per team), waiting up to {args.pop_timeout:.0f} s for the invite")

        while state["invite_time"] is None and time.time() - join_time < args.pop_timeout:
            for op, payload in client.poll(0.5):
                handler(op, payload)
        if state["invite_time"] is None:
            print_err(f"no invite within {args.pop_timeout:.0f} s (queue status {state['status']})")
            return 1
        print_ok(f"queue popped after {state['invite_time'] - join_time:.1f} s")

        invite = state["invite"]
        action = 0 if args.no_enter else 1
        client.send(CMSG_BATTLEFIELD_PORT, struct.pack("<BBIHB", invite["arena_type"], invite["unk"], invite["bg_type"], 0x1F90, action))
        if args.no_enter:
            print_ok("left the queue at the invite (--no-enter)")
            result = 0
        else:
            client.wait_for(SMSG_NEW_WORLD, 20, handler)
            client.send(MSG_MOVE_WORLDPORT_ACK)
            print_ok("entered the match")

            best = (0, 0, 0)
            end = time.time() + args.stay
            next_log = 0
            if arena_slot is not None:
                print_info(f"waiting up to {args.stay:.0f} s for the arena's end (its scoreboard comes only then)")
            while time.time() < end:
                if arena_slot is None and time.time() >= next_log:
                    client.send(MSG_PVP_LOG_DATA)
                    next_log = time.time() + PVP_LOG_INTERVAL
                for op, payload in client.poll(0.5):
                    handler(op, payload)
                if state["players"] and state["players"][0] > best[0]:
                    best = state["players"]
                    split = f" (alliance {best[1]}, horde {best[2]})" if arena_slot is not None else ""
                    print_info(f"{best[0]} of {2 * team_size} players in the match{split}")
                if arena_slot is not None and state["players"]:
                    break

            if best[0] == 2 * team_size:
                print_ok(f"the match is full: {best[0]} players")
                result = 0
            else:
                print_err(f"the match has {best[0]} of {2 * team_size} players")

            client.send(CMSG_LEAVE_BATTLEFIELD, struct.pack("<BBIH", 0, 0, 0, 0))
            try:
                client.wait_for(SMSG_NEW_WORLD, 20, handler)
                client.send(MSG_MOVE_WORLDPORT_ACK)
                print_ok("left the match")
            except TimeoutError:
                print_warn("no teleport out of the match")
    finally:
        teleport(client, char["x"], char["y"], char["z"], char["map"], handler)
        client.send(CMSG_LOGOUT_REQUEST)
        try:
            client.wait_for(SMSG_LOGOUT_COMPLETE, 30, handler)
            print_ok(f"{char['name']} back at map {char['map']} and logged out")
        except (TimeoutError, ConnectionError):
            print_warn("no logout complete, closing the connection")
        client.sock.close()
    return result


if __name__ == "__main__":
    sys.exit(main())
