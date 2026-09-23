#!/usr/bin/env python3
"""
Generate protobuf cross-check vectors for tests/test_proto.c.

Builds Steam CM messages with the official (protoc-generated) Python
bindings from https://github.com/SteamDatabase/Protobufs and prints them as
hex C string constants. test_proto.c decodes these exact bytes with
steam_msgs.c and asserts every field, and re-encodes the same values and
compares byte-for-byte.

Usage:
    tests/gen_vectors.py [--protobufs DIR] [--update tests/test_proto.c]

  --protobufs DIR  an existing clone of SteamDatabase/Protobufs (default:
                   $PROTOBUFS_DIR, otherwise a shallow clone into a temp dir)
  --update FILE    rewrite the region between the BEGIN/END GENERATED
                   VECTORS markers in FILE instead of printing to stdout

Requires protoc and python3-protobuf. The values used here are mirrored in
test_proto.c; if you change one, change the other.
"""

import argparse
import gzip
import importlib
import os
import struct
import subprocess
import sys
import tempfile

PROTO_FILES = [
    "steammessages_base.proto",
    "steammessages_unified_base.steamclient.proto",
    "enums.proto",
    "enums_clientserver.proto",
    "steammessages_clientserver_login.proto",
    "steammessages_clientserver_friends.proto",
    "steammessages_friendmessages.steamclient.proto",
    "steammessages_player.steamclient.proto",
]

BEGIN = "/* BEGIN GENERATED VECTORS"
END = "/* END GENERATED VECTORS */"

PROTO_MASK = 0x80000000


def setup(protobufs_dir, tmp):
    if protobufs_dir is None:
        protobufs_dir = os.path.join(tmp, "Protobufs")
        subprocess.check_call(["git", "clone", "-q", "--depth", "1",
                               "https://github.com/SteamDatabase/Protobufs",
                               protobufs_dir])
    steam = os.path.join(protobufs_dir, "steam")
    out = os.path.join(tmp, "pygen")
    os.makedirs(out, exist_ok=True)
    subprocess.check_call(["protoc", "-I", steam,
                           "-I", os.path.join(protobufs_dir, "google"),
                           "--python_out=" + out] +
                          [os.path.join(steam, f) for f in PROTO_FILES],
                          stderr=subprocess.DEVNULL)
    sys.path.insert(0, out)
    try:
        rev = subprocess.check_output(["git", "-C", protobufs_dir, "rev-parse",
                                       "--short", "HEAD"], text=True).strip()
    except Exception:
        rev = "unknown"
    return rev


def varint(v):
    out = bytearray()
    while True:
        b = v & 0x7f
        v >>= 7
        if v:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def unknown_fields():
    """A blob of fields with numbers no Steam message here uses, one per
    wire type, to prove decoders skip unknown fields."""
    return (varint((900 << 3) | 0) + varint(123456789) +
            varint((901 << 3) | 1) + struct.pack("<Q", 0x0102030405060708) +
            varint((902 << 3) | 2) + varint(3) + b"xyz" +
            varint((903 << 3) | 5) + struct.pack("<I", 0xdeadbeef))


def packet(emsg, hdr, body):
    h = hdr.SerializeToString()
    return struct.pack("<II", emsg | PROTO_MASK, len(h)) + h + body


def build():
    base = importlib.import_module("steammessages_base_pb2")
    login = importlib.import_module("steammessages_clientserver_login_pb2")
    friends = importlib.import_module("steammessages_clientserver_friends_pb2")
    fm = importlib.import_module("steammessages_friendmessages.steamclient_pb2")
    player = importlib.import_module("steammessages_player.steamclient_pb2")

    v = []  # (name, comment, bytes)

    def add(name, comment, msg_or_bytes):
        if not isinstance(msg_or_bytes, (bytes, bytearray)):
            msg_or_bytes = msg_or_bytes.SerializeToString()
        v.append((name, comment, bytes(msg_or_bytes)))

    # ---- CMsgProtoBufHeader ----
    h = base.CMsgProtoBufHeader()
    h.steamid = 76561197960287930
    h.client_sessionid = -123456
    h.routing_appid = 730
    h.jobid_source = 0x0102030405060708
    h.jobid_target = 0x1122334455667788
    h.target_job_name = "FriendMessages.SendMessage#1"
    h.eresult = 15
    h.error_message = "nope"
    h.realm = 1
    add("VEC_HEADER", "CMsgProtoBufHeader, all fields we model", h)

    hl = base.CMsgProtoBufHeader()
    hl.steamid = 76561197960287930
    hl.client_sessionid = 0
    add("VEC_HEADER_LOGON", "CMsgProtoBufHeader for ClientLogon: steamid + explicit client_sessionid 0", hl)

    hx = base.CMsgProtoBufHeader()
    hx.CopyFrom(h)
    hx.seq_num = 7
    hx.messageid = 99
    hx.ip = 0x7f000001
    hx.forward_to_sysid.extend([1, 2])
    hx.session_disposition = 1
    hx.trace_tag = 5
    add("VEC_HEADER_EXTRA", "CMsgProtoBufHeader with fields we do not model (decode only)",
        hx.SerializeToString() + unknown_fields())

    # ---- CMsgClientHello ----
    m = login.CMsgClientHello()
    m.protocol_version = 65580
    add("VEC_HELLO", "CMsgClientHello", m)

    # ---- CMsgClientLogon ----
    m = login.CMsgClientLogon()
    m.protocol_version = 65580
    m.cell_id = 7
    m.client_package_version = 1771
    m.client_language = "english"
    m.client_os_type = (-203) & 0xffffffff   # uint32 on the wire
    m.should_remember_password = True
    m.obfuscated_private_ip.v4 = 0xB0ADF203   # 10.0.2.14 ^ 0xBAADF00D
    m.qos_level = 2
    m.client_supplied_steam_id = 76561197960287930
    m.machine_id = b"\x00MessageObject\x00\x01BB3\x00abc\x00\x08\x08"
    m.chat_mode = 2
    m.account_name = "testuser"
    m.machine_name = "pidgin"
    m.supports_rate_limit_response = True
    m.access_token = "eyJhbGciOiJFZERTQSJ9.eyJzdWIiOiI3NjU2In0.c2ln"
    logon = m
    add("VEC_LOGON", "CMsgClientLogon", m)

    # ---- CMsgClientLogonResponse ----
    m = login.CMsgClientLogonResponse()
    m.eresult = 1
    m.legacy_out_of_game_heartbeat_seconds = 9
    m.heartbeat_seconds = 10
    m.rtime32_server_time = 1700000000
    m.account_flags = 0x80000005
    m.cell_id = 4
    m.email_domain = "example.com"
    m.eresult_extended = -1
    m.vanity_url = "gaben"
    m.client_supplied_steamid = 76561197960287930
    m.ip_country_code = "US"
    m.client_instance_id = (1 << 40) + 5
    m.agreement_session_url = "https://store.steampowered.com/x"
    add("VEC_LOGON_RESPONSE", "CMsgClientLogonResponse", m)
    lr = m

    m = login.CMsgClientLogonResponse()
    m.CopyFrom(lr)
    m.public_ip.v4 = 0x01020304
    m.parental_settings = b"\x01\x02"
    m.token_id = 77
    m.family_group_id = 88
    add("VEC_LOGON_RESPONSE_EXTRA", "CMsgClientLogonResponse with unmodelled fields (decode only)",
        m.SerializeToString() + unknown_fields())

    add("VEC_LOGON_RESPONSE_EMPTY", "CMsgClientLogonResponse with no fields: eresult defaults to 2",
        login.CMsgClientLogonResponse())

    # ---- CMsgClientLoggedOff / HeartBeat ----
    m = login.CMsgClientLoggedOff()
    m.eresult = 34
    add("VEC_LOGGED_OFF", "CMsgClientLoggedOff", m)

    m = login.CMsgClientHeartBeat()
    m.send_reply = True
    add("VEC_HEARTBEAT", "CMsgClientHeartBeat", m)

    # ---- CMsgClientAccountInfo ----
    m = login.CMsgClientAccountInfo()
    m.persona_name = "Théodis ☃"
    m.ip_country = "DE"
    m.count_authed_computers = 3
    m.account_flags = 5
    m.is_phone_verified = True
    m.two_factor_state = 1
    add("VEC_ACCOUNT_INFO", "CMsgClientAccountInfo", m)

    # ---- CMsgClientFriendsList ----
    m = friends.CMsgClientFriendsList()
    m.bincremental = False
    for sid, rel in ((76561197960287931, 3), (76561197960287932, 2),
                     (103582791429521412, 3), (76561197960287933, 0)):
        f = m.friends.add()
        f.ulfriendid = sid
        f.efriendrelationship = rel
    m.max_friend_count = 250
    m.active_friend_count = 2
    add("VEC_FRIENDS_LIST", "CMsgClientFriendsList (bincremental explicitly false)", m)

    # ---- CMsgClientRequestFriendData ----
    m = friends.CMsgClientRequestFriendData()
    m.persona_state_requested = 1 | 2 | 16 | 64 | 256
    m.friends.extend([76561197960287931, 76561197960287932, 76561197960287933])
    add("VEC_REQUEST_FRIEND_DATA", "CMsgClientRequestFriendData", m)

    # ---- CMsgClientPersonaState ----
    m = friends.CMsgClientPersonaState()
    m.status_flags = 1 | 2 | 16 | 64 | 256 | 4096
    f = m.friends.add()
    f.friendid = 76561197960287931
    f.persona_state = 1
    f.game_played_app_id = 730
    f.game_server_ip = 0xC0A80001
    f.game_server_port = 27015
    f.persona_state_flags = 0x201
    f.online_session_instances = 1
    f.persona_set_by_user = True
    f.player_name = "Alice"
    f.query_port = 27016
    f.steamid_source = 90071992547409920
    f.avatar_hash = bytes(range(0xa0, 0xb4))
    f.last_logoff = 1699999000
    f.last_logon = 1699999999
    f.last_seen_online = 1700000001
    f.clan_rank = 3
    f.game_name = "Counter-Strike 2"
    f.gameid = 730
    f.game_data_blob = b"\x01\x02\x03"
    f.clan_data.ogg_app_id = 440
    f.clan_data.chat_group_id = 1 << 33
    f.clan_tag = "[TAG]"
    kv = f.rich_presence.add()
    kv.key = "status"
    kv.value = "In menus"
    kv = f.rich_presence.add()
    kv.key = "steam_display"
    kv.value = "#Menu"
    f.broadcast_id = 0xfedcba9876543210
    f.game_lobby_id = 109775240917090001
    f.watching_broadcast_accountid = 5
    f.watching_broadcast_appid = 6
    f.watching_broadcast_viewers = 7
    f.watching_broadcast_title = "cast"
    f.is_community_banned = False
    f.player_name_pending_review = True
    f.avatar_pending_review = False
    f.on_steam_deck = True
    f.gaming_device_type = 528
    f = m.friends.add()
    f.friendid = 76561197960287932
    f.persona_state = 0
    f.player_name = "Bob"
    f.avatar_hash = bytes(20)
    f.last_logoff = 1600000000
    f = m.friends.add()
    f.friendid = 76561197960287933
    add("VEC_PERSONA_STATE", "CMsgClientPersonaState with three friends", m)
    ps = m

    m = friends.CMsgClientPersonaState()
    m.status_flags = 2
    f = m.friends.add()
    f.friendid = 76561197960287931
    f.player_name = "Alice"
    og = f.other_game_data.add()
    og.gameid = 440
    kv = og.rich_presence.add()
    kv.key = "k"
    kv.value = "v"
    add("VEC_PERSONA_STATE_EXTRA", "CMsgClientPersonaState with other_game_data and unknown fields (decode only)",
        m.SerializeToString() + unknown_fields())

    # ---- CMsgClientChangeStatus ----
    m = friends.CMsgClientChangeStatus()
    m.persona_state = 1
    m.player_name = "Me"
    m.is_auto_generated_name = False
    m.high_priority = True
    m.persona_set_by_user = True
    m.persona_state_flags = 0
    m.need_persona_response = True
    m.is_client_idle = False
    add("VEC_CHANGE_STATUS", "CMsgClientChangeStatus", m)

    # ---- Add / remove friend ----
    m = friends.CMsgClientAddFriend()
    m.steamid_to_add = 76561197960287934
    m.accountname_or_email_to_add = "x@y.z"
    add("VEC_ADD_FRIEND", "CMsgClientAddFriend", m)

    m = friends.CMsgClientAddFriendResponse()
    m.eresult = 29
    m.steam_id_added = 76561197960287934
    m.persona_name_added = "Carol"
    add("VEC_ADD_FRIEND_RESPONSE", "CMsgClientAddFriendResponse", m)

    m = friends.CMsgClientRemoveFriend()
    m.friendid = 76561197960287935
    add("VEC_REMOVE_FRIEND", "CMsgClientRemoveFriend", m)

    # ---- CMsgClientPlayerNicknameList ----
    m = friends.CMsgClientPlayerNicknameList()
    m.removal = False
    m.incremental = True
    n = m.nicknames.add()
    n.steamid = 76561197960287931
    n.nickname = "Nick1"
    n = m.nicknames.add()
    n.steamid = 76561197960287932
    n.nickname = ""
    add("VEC_NICKNAME_LIST", "CMsgClientPlayerNicknameList", m)

    # ---- FriendMessages ----
    m = fm.CFriendMessages_SendMessage_Request()
    m.steamid = 76561197960287931
    m.chat_entry_type = 1
    m.message = "hello [b]world[/b]"
    m.contains_bbcode = True
    m.echo_to_sender = True
    m.low_priority = False
    m.client_message_id = "42"
    add("VEC_SEND_MESSAGE_REQUEST", "CFriendMessages_SendMessage_Request", m)

    m = fm.CFriendMessages_SendMessage_Response()
    m.modified_message = "hello [b]world[/b]"
    m.server_timestamp = 1700000100
    m.ordinal = 3
    m.message_without_bb_code = "hello world"
    add("VEC_SEND_MESSAGE_RESPONSE", "CFriendMessages_SendMessage_Response", m)

    m = fm.CFriendMessages_IncomingMessage_Notification()
    m.steamid_friend = 76561197960287931
    m.chat_entry_type = 1
    m.from_limited_account = False
    m.message = "/me waves [url]x[/url]"
    m.rtime32_server_timestamp = 1700000123
    m.ordinal = 2
    m.local_echo = True
    m.message_no_bbcode = "/me waves x"
    m.low_priority = True
    add("VEC_INCOMING_MESSAGE", "CFriendMessages_IncomingMessage_Notification", m)

    m = fm.CFriendMessages_IncomingMessage_Notification()
    m.steamid_friend = 76561197960287932
    m.chat_entry_type = 2
    m.rtime32_server_timestamp = 1700000124
    add("VEC_INCOMING_TYPING", "CFriendMessages_IncomingMessage_Notification (typing)", m)
    typing = m

    m = fm.CFriendMessages_GetRecentMessages_Request()
    m.steamid1 = 76561197960287930
    m.steamid2 = 76561197960287931
    m.count = 50
    m.most_recent_conversation = False
    m.rtime32_start_time = 1690000000
    m.bbcode_format = True
    m.start_ordinal = 4
    m.time_last = 1700000200
    m.ordinal_last = 5
    add("VEC_GET_RECENT_REQUEST", "CFriendMessages_GetRecentMessages_Request", m)

    m = fm.CFriendMessages_GetRecentMessages_Response()
    x = m.messages.add()
    x.accountid = 22202
    x.timestamp = 1700000300
    x.message = "newest"
    x.ordinal = 1
    x = m.messages.add()
    x.accountid = 22203
    x.timestamp = 1700000299
    x.message = "older"
    m.more_available = True
    add("VEC_GET_RECENT_RESPONSE", "CFriendMessages_GetRecentMessages_Response", m)

    r = m.messages[0].reactions.add()
    r.reaction_type = 1
    r.reaction = ":steamhappy:"
    r.reactors.extend([22203, 22204])
    add("VEC_GET_RECENT_RESPONSE_EXTRA", "GetRecentMessages_Response with reactions (decode only)", m)

    m = fm.CFriendsMessages_GetActiveMessageSessions_Request()
    m.lastmessage_since = 1690000000
    m.only_sessions_with_messages = True
    add("VEC_ACTIVE_SESSIONS_REQUEST", "CFriendsMessages_GetActiveMessageSessions_Request", m)

    m = fm.CFriendsMessages_GetActiveMessageSessions_Response()
    s = m.message_sessions.add()
    s.accountid_friend = 22202
    s.last_message = 1700000300
    s.last_view = 1700000000
    s.unread_message_count = 4
    s = m.message_sessions.add()
    s.accountid_friend = 22203
    s.last_message = 1700000299
    m.timestamp = 1700000400
    add("VEC_ACTIVE_SESSIONS_RESPONSE", "CFriendsMessages_GetActiveMessageSessions_Response", m)

    m.message_sessions[1].notices.extend([1, 1])
    add("VEC_ACTIVE_SESSIONS_RESPONSE_EXTRA", "GetActiveMessageSessions_Response with notices (decode only)", m)

    # ---- Player.GetNicknameList ----
    add("VEC_NICKNAME_GET_REQUEST", "CPlayer_GetNicknameList_Request (empty)",
        player.CPlayer_GetNicknameList_Request())

    m = player.CPlayer_GetNicknameList_Response()
    n = m.nicknames.add()
    n.accountid = 22202
    n.nickname = "Pal"
    n = m.nicknames.add()
    n.accountid = 0xffffffff
    n.nickname = "max"
    add("VEC_NICKNAME_GET_RESPONSE", "CPlayer_GetNicknameList_Response (accountid is fixed32)", m)

    # ---- Packets ----
    add("VEC_PACKET_LOGON", "packet: EMsg ClientLogon (5514) + VEC_HEADER_LOGON + VEC_LOGON",
        packet(5514, hl, logon.SerializeToString()))

    sm = base.CMsgProtoBufHeader()
    sm.jobid_target = 0x1122334455667788
    sm.target_job_name = "FriendMessagesClient.IncomingMessage#1"
    p_notify = packet(146, sm, typing.SerializeToString())
    add("VEC_PACKET_SERVICE_METHOD", "packet: EMsg ServiceMethod (146) carrying VEC_INCOMING_TYPING", p_notify)

    lo = login.CMsgClientLoggedOff()
    lo.eresult = 34
    p_loggedoff = packet(757, base.CMsgProtoBufHeader(), lo.SerializeToString())
    add("VEC_PACKET_LOGGED_OFF", "packet: EMsg ClientLoggedOff (757), empty header", p_loggedoff)

    p_persona = packet(766, base.CMsgProtoBufHeader(), ps.SerializeToString())
    add("VEC_PACKET_PERSONA", "packet: EMsg ClientPersonaState (766), empty header, VEC_PERSONA_STATE", p_persona)

    payload = b"".join(struct.pack("<I", len(p)) + p
                       for p in (p_notify, p_loggedoff, p_persona))

    m = base.CMsgMulti()
    m.message_body = payload
    add("VEC_MULTI_PLAIN", "CMsgMulti, uncompressed: SERVICE_METHOD, LOGGED_OFF, PERSONA packets", m)

    m = base.CMsgMulti()
    m.size_unzipped = len(payload)
    m.message_body = gzip.compress(payload, mtime=0)
    add("VEC_MULTI_GZIP", "CMsgMulti, gzip (mtime 0): same three packets", m)

    return v


def format_c(vectors, rev):
    lines = [BEGIN + " -- generated by tests/gen_vectors.py from",
             " * SteamDatabase/Protobufs @ %s. Do not edit by hand; rerun" % rev,
             " * `tests/gen_vectors.py --update tests/test_proto.c`. */"]
    for name, comment, data in vectors:
        hx = data.hex()
        lines.append("/* %s (%d bytes) */" % (comment, len(data)))
        if not hx:
            lines.append('static const char %s[] = "";' % name)
            continue
        lines.append("static const char %s[] =" % name)
        chunks = [hx[i:i + 72] for i in range(0, len(hx), 72)]
        for i, c in enumerate(chunks):
            lines.append('\t"%s"%s' % (c, ";" if i == len(chunks) - 1 else ""))
    lines.append(END)
    return "\n".join(lines) + "\n"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--protobufs", default=os.environ.get("PROTOBUFS_DIR"))
    ap.add_argument("--update")
    args = ap.parse_args()

    with tempfile.TemporaryDirectory() as tmp:
        rev = setup(args.protobufs, tmp)
        text = format_c(build(), rev)

    if not args.update:
        sys.stdout.write(text)
        return
    with open(args.update) as fp:
        src = fp.read()
    a = src.index(BEGIN)
    b = src.index(END) + len(END) + 1
    with open(args.update, "w") as fp:
        fp.write(src[:a] + text + src[b:])


if __name__ == "__main__":
    main()
