/*
 *  Steam Mobile Plugin for Pidgin
 *  Copyright (C) 2026 pidgin-opensteamworks contributors
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * Steam CM messages: plain C structs with protobuf encode/decode.
 *
 * Field numbers and wire types follow SteamDatabase/Protobufs (the steam/ dir)
 * and are cross-checked against protoc-generated Python in tests/test_proto.c.
 * This module depends only on glib, zlib and steam_proto; it must not
 * include libpurple headers.
 *
 * Conventions for every message struct SteamMsgXxx:
 *
 *   steam_msg_xxx_init(m)     zero the struct, apply proto defaults, allocate
 *                             repeated-field arrays. Call before anything else.
 *   steam_msg_xxx_clear(m)    free everything the struct owns. The struct can
 *                             be re-initialised afterwards.
 *   steam_msg_xxx_encode(m, out)
 *                             append the serialised message to `out`. Fields
 *                             are written in ascending field-number order and
 *                             only when present, so the output matches the
 *                             official protobuf serialiser byte-for-byte.
 *   steam_msg_xxx_decode(m, data, len)
 *                             clear + re-init `m`, then parse. Unknown fields
 *                             (and known fields with an unexpected wire type)
 *                             are skipped. Returns FALSE on malformed input;
 *                             `m` may then be partially filled and must still
 *                             be cleared.
 *
 * Field representation:
 *   - optional scalar:  `gboolean has_<field>` + value. Decoding sets has_
 *                       when the field was on the wire; encoding writes the
 *                       field iff has_ is TRUE. Use STEAM_MSG_SET() to set
 *                       both. When absent the value holds the proto default
 *                       (0, or e.g. 2 = FAIL for eresult fields).
 *   - string:           `gchar *` owned by the struct, NULL = absent.
 *                       Assign g_strdup()ed memory before encoding.
 *   - bytes:            `GByteArray *` owned by the struct, NULL = absent.
 *   - repeated:         `GArray *` of the element type, never NULL after
 *                       init (use g_array_index / ->len). Element structs
 *                       that own strings are freed by the parent's _clear.
 *                       For encoding, append zero-initialised elements
 *                       (e.g. via steam_msg_..._add_friend()) and fill them.
 *
 * Integer types: proto `uint32`/`fixed32` -> guint32, `int32` -> gint32,
 * `uint64`/`fixed64` -> guint64. The wire type (varint vs fixed) is handled
 * internally; the comments name the proto type for reference.
 */

#ifndef STEAM_MSGS_H
#define STEAM_MSGS_H

#include <glib.h>

#include "steam_eresult.h"

/* Set an optional scalar field and mark it present. */
#define STEAM_MSG_SET(m, field, value) \
	G_STMT_START { (m)->has_##field = TRUE; (m)->field = (value); } G_STMT_END

/* ======================================================================
 * EMsg numbers (enums_clientserver.proto, enum EMsg)
 * ====================================================================== */

/* High bit of the first packet word: set when the header is a
 * CMsgProtoBufHeader. All packets used over WebSocket CMs are protobuf. */
#define STEAM_EMSG_PROTO_MASK 0x80000000u

typedef enum {
	STEAM_EMSG_MULTI                           = 1,
	STEAM_EMSG_DEST_JOB_FAILED                 = 113,
	STEAM_EMSG_SERVICE_METHOD                  = 146,  /* server -> client notification */
	STEAM_EMSG_SERVICE_METHOD_RESPONSE         = 147,  /* reply to our ServiceMethodCallFromClient */
	STEAM_EMSG_SERVICE_METHOD_CALL_FROM_CLIENT = 151,
	STEAM_EMSG_CLIENT_HEART_BEAT               = 703,
	STEAM_EMSG_CLIENT_LOG_OFF                  = 706,
	STEAM_EMSG_CLIENT_REMOVE_FRIEND            = 714,
	STEAM_EMSG_CLIENT_CHANGE_STATUS            = 716,
	STEAM_EMSG_CLIENT_GAMES_PLAYED             = 742,
	STEAM_EMSG_CLIENT_LOG_ON_RESPONSE          = 751,
	STEAM_EMSG_CLIENT_LOGGED_OFF               = 757,
	STEAM_EMSG_CLIENT_PERSONA_STATE            = 766,
	STEAM_EMSG_CLIENT_FRIENDS_LIST             = 767,
	STEAM_EMSG_CLIENT_ACCOUNT_INFO             = 768,
	STEAM_EMSG_CLIENT_GAME_CONNECT_TOKENS      = 779,
	STEAM_EMSG_CLIENT_LICENSE_LIST             = 780,
	STEAM_EMSG_CLIENT_VAC_BAN_STATUS           = 782,
	STEAM_EMSG_CLIENT_CM_LIST                  = 783,  /* legacy; no longer in the current EMsg enum */
	STEAM_EMSG_CLIENT_ADD_FRIEND               = 791,
	STEAM_EMSG_CLIENT_ADD_FRIEND_RESPONSE      = 792,
	STEAM_EMSG_CLIENT_UPDATE_GUEST_PASSES_LIST = 798,
	STEAM_EMSG_CLIENT_REQUEST_FRIEND_DATA      = 815,
	STEAM_EMSG_CLIENT_CLAN_STATE               = 822,
	STEAM_EMSG_CLIENT_SESSION_TOKEN            = 850,
	STEAM_EMSG_CLIENT_SERVER_LIST              = 880,
	STEAM_EMSG_CLIENT_IS_LIMITED_ACCOUNT       = 5430,
	STEAM_EMSG_CLIENT_EMAIL_ADDR_INFO          = 5456,
	STEAM_EMSG_CLIENT_SERVER_UNAVAILABLE       = 5500,
	STEAM_EMSG_CLIENT_SERVERS_AVAILABLE        = 5501,
	STEAM_EMSG_CLIENT_LOGON                    = 5514,
	STEAM_EMSG_CLIENT_WALLET_INFO_UPDATE       = 5528,
	STEAM_EMSG_CLIENT_FRIENDS_GROUPS_LIST      = 5553,
	STEAM_EMSG_CLIENT_PLAYER_NICKNAME_LIST     = 5587,
	STEAM_EMSG_CLIENT_USER_NOTIFICATIONS       = 5599,
	STEAM_EMSG_CLIENT_RICH_PRESENCE_INFO       = 7503,
	STEAM_EMSG_CLIENT_CHAT_OFFLINE_MESSAGE_NOTIFICATION = 7523,
	STEAM_EMSG_CLIENT_PICS_CHANGES_SINCE_RESPONSE = 8902,
	STEAM_EMSG_CLIENT_PICS_PRODUCT_INFO_RESPONSE  = 8904,
	STEAM_EMSG_CLIENT_PICS_ACCESS_TOKEN_RESPONSE  = 8906,
	STEAM_EMSG_CLIENT_PLAYING_SESSION_STATE    = 9600,
	STEAM_EMSG_SERVICE_METHOD_CALL_FROM_CLIENT_NON_AUTHED = 9804,
	STEAM_EMSG_CLIENT_HELLO                    = 9805
} SteamEMsg;

/* Returns a static name for the EMsgs above (for debug logs), or NULL. */
const char *steam_emsg_to_string(guint32 emsg);

/* Absent job ids in CMsgProtoBufHeader mean "no job". */
#define STEAM_JOBID_NONE G_GUINT64_CONSTANT(0xFFFFFFFFFFFFFFFF)

/* Values for CMsgClientHello / CMsgClientLogon protocol_version */
#define STEAM_PROTOCOL_VERSION 65580

/* EOSType values for CMsgClientLogon.client_os_type (uint32 on the wire,
 * negative values are stored as their 32-bit two's complement). */
#define STEAM_OS_TYPE_LINUX_UNKNOWN ((guint32)-203)
#define STEAM_OS_TYPE_WINDOWS_10    16u

/* EClientPersonaStateFlag: bits for CMsgClientRequestFriendData
 * persona_state_requested and CMsgClientPersonaState status_flags. */
#define STEAM_PERSONA_REQ_STATUS          0x0001
#define STEAM_PERSONA_REQ_PLAYER_NAME     0x0002
#define STEAM_PERSONA_REQ_QUERY_PORT      0x0004
#define STEAM_PERSONA_REQ_SOURCE_ID       0x0008
#define STEAM_PERSONA_REQ_PRESENCE        0x0010  /* state, game, avatar, last logon/logoff */
#define STEAM_PERSONA_REQ_LAST_SEEN       0x0040
#define STEAM_PERSONA_REQ_USER_CLAN_RANK  0x0080
#define STEAM_PERSONA_REQ_GAME_EXTRA_INFO 0x0100  /* game_name, gameid, lobby */
#define STEAM_PERSONA_REQ_GAME_DATA_BLOB  0x0200
#define STEAM_PERSONA_REQ_CLAN_DATA       0x0400
#define STEAM_PERSONA_REQ_RICH_PRESENCE   0x1000
#define STEAM_PERSONA_REQ_BROADCAST       0x2000
#define STEAM_PERSONA_REQ_WATCHING        0x4000
/* What the plugin asks for by default. */
#define STEAM_PERSONA_REQ_DEFAULT \
	(STEAM_PERSONA_REQ_STATUS | STEAM_PERSONA_REQ_PLAYER_NAME | \
	 STEAM_PERSONA_REQ_PRESENCE | STEAM_PERSONA_REQ_LAST_SEEN | \
	 STEAM_PERSONA_REQ_GAME_EXTRA_INFO | STEAM_PERSONA_REQ_RICH_PRESENCE)

/* target_job_name values for unified service methods */
#define STEAM_METHOD_FRIEND_MESSAGES_SEND_MESSAGE        "FriendMessages.SendMessage#1"
#define STEAM_METHOD_FRIEND_MESSAGES_GET_RECENT_MESSAGES "FriendMessages.GetRecentMessages#1"
#define STEAM_METHOD_FRIEND_MESSAGES_GET_ACTIVE_SESSIONS "FriendMessages.GetActiveMessageSessions#1"
#define STEAM_METHOD_PLAYER_GET_NICKNAME_LIST            "Player.GetNicknameList#1"
#define STEAM_NOTIFY_FRIEND_MESSAGES_INCOMING_MESSAGE    "FriendMessagesClient.IncomingMessage#1"
#define STEAM_METHOD_FRIEND_MESSAGES_ACK_MESSAGE         "FriendMessages.AckMessage#1"
#define STEAM_NOTIFY_FRIEND_MESSAGES_ACK_ECHO            "FriendMessagesClient.NotifyAckMessageEcho#1"
#define STEAM_METHOD_FRIEND_MESSAGES_UPDATE_REACTION     "FriendMessages.UpdateMessageReaction#1"
#define STEAM_NOTIFY_FRIEND_MESSAGES_MESSAGE_REACTION    "FriendMessagesClient.MessageReaction#1"

/* ======================================================================
 * Base (steammessages_base.proto)
 * ====================================================================== */

/* CMsgProtoBufHeader (only the fields the plugin needs; others are skipped) */
typedef struct {
	gboolean has_steamid;           /* 1  fixed64: our SteamID (set on every packet after logon, and on ClientLogon) */
	guint64 steamid;
	gboolean has_client_sessionid;  /* 2  int32: from the logon response header; 0 on ClientLogon */
	gint32 client_sessionid;
	gboolean has_routing_appid;     /* 3  uint32 */
	guint32 routing_appid;
	gboolean has_jobid_source;      /* 10 fixed64: our job id on ServiceMethodCallFromClient */
	guint64 jobid_source;           /*    STEAM_JOBID_NONE when absent */
	gboolean has_jobid_target;      /* 11 fixed64: equals our jobid_source on ServiceMethodResponse */
	guint64 jobid_target;           /*    STEAM_JOBID_NONE when absent */
	gchar *target_job_name;         /* 12 string: e.g. "FriendMessages.SendMessage#1" */
	gboolean has_eresult;           /* 13 int32: result of a service call; default 2 (FAIL) */
	gint32 eresult;
	gchar *error_message;           /* 14 string */
	gboolean has_realm;             /* 32 uint32 */
	guint32 realm;
} SteamMsgProtoBufHeader;

void steam_msg_protobuf_header_init(SteamMsgProtoBufHeader *m);
void steam_msg_protobuf_header_clear(SteamMsgProtoBufHeader *m);
void steam_msg_protobuf_header_encode(const SteamMsgProtoBufHeader *m, GByteArray *out);
gboolean steam_msg_protobuf_header_decode(SteamMsgProtoBufHeader *m, const guint8 *data, gsize len);

/* CMsgMulti (EMsg 1): several packets batched together. */
typedef struct {
	gboolean has_size_unzipped;     /* 1 uint32: if > 0, message_body is gzip and inflates to this size */
	guint32 size_unzipped;
	GByteArray *message_body;       /* 2 bytes: sequence of (uint32 LE length, packet) */
} SteamMsgMulti;

void steam_msg_multi_init(SteamMsgMulti *m);
void steam_msg_multi_clear(SteamMsgMulti *m);
void steam_msg_multi_encode(const SteamMsgMulti *m, GByteArray *out);
gboolean steam_msg_multi_decode(SteamMsgMulti *m, const guint8 *data, gsize len);

/* Inflates the body if needed and splits it into packets. Appends one
 * newly allocated GByteArray per packet to `out_packets` (the caller frees
 * them, e.g. with g_ptr_array_new_with_free_func(g_byte_array_unref)).
 * Each packet can be fed to steam_msg_packet_parse(). Returns FALSE (and
 * appends nothing) on corrupt gzip data, size mismatch or bad framing. */
gboolean steam_msg_multi_unpack(const SteamMsgMulti *m, GPtrArray *out_packets);

/* ======================================================================
 * Packet framing
 *
 *   uint32 LE  emsg | STEAM_EMSG_PROTO_MASK
 *   uint32 LE  header_len
 *   header_len bytes CMsgProtoBufHeader
 *   body
 * ====================================================================== */

/* Parses one packet. On success `*emsg` is the EMsg without the proto bit,
 * `hdr` is decoded (it is initialised here; the caller must clear it even
 * on failure) and `*body`/`*body_len` point into `data`.
 * Returns FALSE if the packet is too short, not a protobuf packet (the
 * EMsg is still stored in *emsg when at least 4 bytes were available) or
 * the header is malformed. */
gboolean steam_msg_packet_parse(const guint8 *data, gsize len, guint32 *emsg,
                                SteamMsgProtoBufHeader *hdr,
                                const guint8 **body, gsize *body_len);

/* Appends a complete packet to `out`. `hdr` may be NULL (empty header),
 * `body` may be NULL (empty body). */
void steam_msg_packet_build(guint32 emsg, const SteamMsgProtoBufHeader *hdr,
                            const GByteArray *body, GByteArray *out);

/* ======================================================================
 * Login (steammessages_clientserver_login.proto)
 * ====================================================================== */

/* CMsgClientHello (EMsg 9805): send first after the socket connects. */
typedef struct {
	gboolean has_protocol_version;  /* 1 uint32: STEAM_PROTOCOL_VERSION */
	guint32 protocol_version;
} SteamMsgClientHello;

void steam_msg_client_hello_init(SteamMsgClientHello *m);
void steam_msg_client_hello_clear(SteamMsgClientHello *m);
void steam_msg_client_hello_encode(const SteamMsgClientHello *m, GByteArray *out);
gboolean steam_msg_client_hello_decode(SteamMsgClientHello *m, const guint8 *data, gsize len);

/* CMsgClientLogon (EMsg 5514). For refresh-token logon set
 * protocol_version, client_os_type, client_language, account_name,
 * access_token (= the refresh token), should_remember_password, chat_mode = 2,
 * supports_rate_limit_response, machine_name, obfuscated_private_ip and
 * optionally machine_id. The packet header must carry
 * our steamid and client_sessionid = 0. Other proto fields are not modelled. */
typedef struct {
	gboolean has_protocol_version;          /* 1   uint32 */
	guint32 protocol_version;
	gboolean has_cell_id;                   /* 3   uint32 */
	guint32 cell_id;
	gboolean has_client_package_version;    /* 5   uint32 */
	guint32 client_package_version;
	gchar *client_language;                 /* 6   string: "english" */
	gboolean has_client_os_type;            /* 7   uint32 (EOSType, see STEAM_OS_TYPE_*) */
	guint32 client_os_type;
	gboolean has_should_remember_password;  /* 8   bool */
	gboolean should_remember_password;
	gboolean has_obfuscated_private_ip;     /* 11  CMsgIPAddress: only the v4 (fixed32 = 1) arm */
	guint32 obfuscated_private_ip;          /*     is modelled; a v6-only value decodes as absent */
	gboolean has_qos_level;                 /* 21  uint32 */
	guint32 qos_level;
	gboolean has_client_supplied_steam_id;  /* 22  fixed64 */
	guint64 client_supplied_steam_id;
	GByteArray *machine_id;                 /* 30  bytes */
	gboolean has_chat_mode;                 /* 33  uint32: 2 = new Steam chat */
	guint32 chat_mode;
	gchar *account_name;                    /* 50  string */
	gchar *password;                        /* 51  string (leave NULL for token logon) */
	gchar *machine_name;                    /* 96  string */
	gboolean has_supports_rate_limit_response; /* 102 bool */
	gboolean supports_rate_limit_response;
	gchar *access_token;                    /* 108 string: the refresh token (JWT) */
} SteamMsgClientLogon;

void steam_msg_client_logon_init(SteamMsgClientLogon *m);
void steam_msg_client_logon_clear(SteamMsgClientLogon *m);
void steam_msg_client_logon_encode(const SteamMsgClientLogon *m, GByteArray *out);
gboolean steam_msg_client_logon_decode(SteamMsgClientLogon *m, const guint8 *data, gsize len);

/* CMsgClientLogonResponse (EMsg 751). NB: our client_sessionid and SteamID
 * arrive in the packet *header*, not in this body. */
typedef struct {
	gboolean has_eresult;                   /* 1  int32 EResult; default 2 (FAIL) */
	gint32 eresult;
	gboolean has_legacy_out_of_game_heartbeat_seconds; /* 2 int32 */
	gint32 legacy_out_of_game_heartbeat_seconds;
	gboolean has_heartbeat_seconds;         /* 3  int32: heartbeat interval */
	gint32 heartbeat_seconds;
	gboolean has_rtime32_server_time;       /* 5  fixed32: unix time */
	guint32 rtime32_server_time;
	gboolean has_account_flags;             /* 6  uint32 */
	guint32 account_flags;
	gboolean has_cell_id;                   /* 7  uint32 */
	guint32 cell_id;
	gchar *email_domain;                    /* 8  string */
	gboolean has_eresult_extended;          /* 10 int32 */
	gint32 eresult_extended;
	gchar *vanity_url;                      /* 14 string */
	gboolean has_client_supplied_steamid;   /* 20 fixed64 */
	guint64 client_supplied_steamid;
	gchar *ip_country_code;                 /* 21 string */
	gboolean has_client_instance_id;        /* 27 uint64 */
	guint64 client_instance_id;
	gchar *agreement_session_url;           /* 29 string */
} SteamMsgClientLogonResponse;

void steam_msg_client_logon_response_init(SteamMsgClientLogonResponse *m);
void steam_msg_client_logon_response_clear(SteamMsgClientLogonResponse *m);
void steam_msg_client_logon_response_encode(const SteamMsgClientLogonResponse *m, GByteArray *out);
gboolean steam_msg_client_logon_response_decode(SteamMsgClientLogonResponse *m, const guint8 *data, gsize len);

/* CMsgClientLogOff (EMsg 706): empty message. */
typedef struct {
	gint unused;
} SteamMsgClientLogOff;

void steam_msg_client_logoff_init(SteamMsgClientLogOff *m);
void steam_msg_client_logoff_clear(SteamMsgClientLogOff *m);
void steam_msg_client_logoff_encode(const SteamMsgClientLogOff *m, GByteArray *out);
gboolean steam_msg_client_logoff_decode(SteamMsgClientLogOff *m, const guint8 *data, gsize len);

/* CMsgClientLoggedOff (EMsg 757): the server ended our session. */
typedef struct {
	gboolean has_eresult;   /* 1 int32; default 2 (FAIL) */
	gint32 eresult;
} SteamMsgClientLoggedOff;

void steam_msg_client_logged_off_init(SteamMsgClientLoggedOff *m);
void steam_msg_client_logged_off_clear(SteamMsgClientLoggedOff *m);
void steam_msg_client_logged_off_encode(const SteamMsgClientLoggedOff *m, GByteArray *out);
gboolean steam_msg_client_logged_off_decode(SteamMsgClientLoggedOff *m, const guint8 *data, gsize len);

/* CMsgClientHeartBeat (EMsg 703) */
typedef struct {
	gboolean has_send_reply;  /* 1 bool */
	gboolean send_reply;
} SteamMsgClientHeartBeat;

void steam_msg_client_heartbeat_init(SteamMsgClientHeartBeat *m);
void steam_msg_client_heartbeat_clear(SteamMsgClientHeartBeat *m);
void steam_msg_client_heartbeat_encode(const SteamMsgClientHeartBeat *m, GByteArray *out);
gboolean steam_msg_client_heartbeat_decode(SteamMsgClientHeartBeat *m, const guint8 *data, gsize len);

/* CMsgClientAccountInfo (EMsg 768): our own account. */
typedef struct {
	gchar *persona_name;                  /* 1  string: our display name */
	gchar *ip_country;                    /* 2  string */
	gboolean has_count_authed_computers;  /* 5  int32 */
	gint32 count_authed_computers;
	gboolean has_account_flags;           /* 7  uint32 */
	guint32 account_flags;
	gboolean has_is_phone_verified;       /* 16 bool */
	gboolean is_phone_verified;
	gboolean has_two_factor_state;        /* 17 uint32 */
	guint32 two_factor_state;
} SteamMsgClientAccountInfo;

void steam_msg_client_account_info_init(SteamMsgClientAccountInfo *m);
void steam_msg_client_account_info_clear(SteamMsgClientAccountInfo *m);
void steam_msg_client_account_info_encode(const SteamMsgClientAccountInfo *m, GByteArray *out);
gboolean steam_msg_client_account_info_decode(SteamMsgClientAccountInfo *m, const guint8 *data, gsize len);

/* ======================================================================
 * Friends (steammessages_clientserver_friends.proto)
 * ====================================================================== */

/* CMsgClientFriendsList.Friend */
typedef struct {
	gboolean has_ulfriendid;          /* 1 fixed64: SteamID (individual or clan) */
	guint64 ulfriendid;
	gboolean has_efriendrelationship; /* 2 uint32: EFriendRelationship (0 = removed) */
	guint32 efriendrelationship;
} SteamMsgClientFriendsListFriend;

/* CMsgClientFriendsList (EMsg 767) */
typedef struct {
	gboolean has_bincremental;        /* 1 bool: FALSE = full list */
	gboolean bincremental;
	GArray *friends;                  /* 2 repeated SteamMsgClientFriendsListFriend */
	gboolean has_max_friend_count;    /* 3 uint32 */
	guint32 max_friend_count;
	gboolean has_active_friend_count; /* 4 uint32 */
	guint32 active_friend_count;
	gboolean has_friends_limit_hit;   /* 5 bool */
	gboolean friends_limit_hit;
} SteamMsgClientFriendsList;

void steam_msg_client_friends_list_init(SteamMsgClientFriendsList *m);
void steam_msg_client_friends_list_clear(SteamMsgClientFriendsList *m);
void steam_msg_client_friends_list_encode(const SteamMsgClientFriendsList *m, GByteArray *out);
gboolean steam_msg_client_friends_list_decode(SteamMsgClientFriendsList *m, const guint8 *data, gsize len);

/* CMsgClientRequestFriendData (EMsg 815) */
typedef struct {
	gboolean has_persona_state_requested; /* 1 uint32: STEAM_PERSONA_REQ_* bits */
	guint32 persona_state_requested;
	GArray *friends;                      /* 2 repeated fixed64 (guint64 SteamIDs) */
} SteamMsgClientRequestFriendData;

void steam_msg_client_request_friend_data_init(SteamMsgClientRequestFriendData *m);
void steam_msg_client_request_friend_data_clear(SteamMsgClientRequestFriendData *m);
void steam_msg_client_request_friend_data_encode(const SteamMsgClientRequestFriendData *m, GByteArray *out);
gboolean steam_msg_client_request_friend_data_decode(SteamMsgClientRequestFriendData *m, const guint8 *data, gsize len);

/* CMsgClientPersonaState.Friend.KV (rich presence entry) */
typedef struct {
	gchar *key;     /* 1 string, e.g. "status", "steam_display", "connect" */
	gchar *value;   /* 2 string */
} SteamMsgPersonaKV;

/* CMsgClientPersonaState.Friend: one user's persona. A message carries only
 * the fields selected by the request flags / that changed. */
typedef struct {
	gboolean has_friendid;                 /* 1  fixed64 SteamID */
	guint64 friendid;
	gboolean has_persona_state;            /* 2  uint32 EPersonaState */
	guint32 persona_state;
	gboolean has_game_played_app_id;       /* 3  uint32: 0 = not in game */
	guint32 game_played_app_id;
	gboolean has_game_server_ip;           /* 4  uint32 IPv4, host order */
	guint32 game_server_ip;
	gboolean has_game_server_port;         /* 5  uint32 */
	guint32 game_server_port;
	gboolean has_persona_state_flags;      /* 6  uint32 EPersonaStateFlag bits */
	guint32 persona_state_flags;
	gboolean has_online_session_instances; /* 7  uint32 */
	guint32 online_session_instances;
	gboolean has_persona_set_by_user;      /* 10 bool */
	gboolean persona_set_by_user;
	gchar *player_name;                    /* 15 string */
	gboolean has_query_port;               /* 20 uint32 */
	guint32 query_port;
	gboolean has_steamid_source;           /* 25 fixed64 */
	guint64 steamid_source;
	GByteArray *avatar_hash;               /* 31 bytes: 20-byte SHA1; all zero = default avatar */
	gboolean has_last_logoff;              /* 45 uint32 unix time */
	guint32 last_logoff;
	gboolean has_last_logon;               /* 46 uint32 unix time */
	guint32 last_logon;
	gboolean has_last_seen_online;         /* 47 uint32 unix time */
	guint32 last_seen_online;
	gboolean has_clan_rank;                /* 50 uint32 */
	guint32 clan_rank;
	gchar *game_name;                      /* 55 string: non-Steam game / mod name */
	gboolean has_gameid;                   /* 56 fixed64 GameID */
	guint64 gameid;
	GByteArray *game_data_blob;            /* 60 bytes */
	gboolean has_clan_data;                /* 64 message ClanData { */
	gboolean has_clan_ogg_app_id;          /*      1 uint32 ogg_app_id */
	guint32 clan_ogg_app_id;
	gboolean has_clan_chat_group_id;       /*      2 uint64 chat_group_id } */
	guint64 clan_chat_group_id;
	gchar *clan_tag;                       /* 65 string */
	GArray *rich_presence;                 /* 71 repeated SteamMsgPersonaKV */
	gboolean has_broadcast_id;             /* 72 fixed64 */
	guint64 broadcast_id;
	gboolean has_game_lobby_id;            /* 73 fixed64 */
	guint64 game_lobby_id;
	gboolean has_watching_broadcast_accountid; /* 74 uint32 */
	guint32 watching_broadcast_accountid;
	gboolean has_watching_broadcast_appid; /* 75 uint32 */
	guint32 watching_broadcast_appid;
	gboolean has_watching_broadcast_viewers; /* 76 uint32 */
	guint32 watching_broadcast_viewers;
	gchar *watching_broadcast_title;       /* 77 string */
	gboolean has_is_community_banned;      /* 78 bool */
	gboolean is_community_banned;
	gboolean has_player_name_pending_review; /* 79 bool */
	gboolean player_name_pending_review;
	gboolean has_avatar_pending_review;    /* 80 bool */
	gboolean avatar_pending_review;
	gboolean has_on_steam_deck;            /* 81 bool */
	gboolean on_steam_deck;
	/* 82 repeated OtherGameData other_game_data: not modelled (skipped) */
	gboolean has_gaming_device_type;       /* 83 uint32 */
	guint32 gaming_device_type;
} SteamMsgPersonaFriend;

/* CMsgClientPersonaState (EMsg 766) */
typedef struct {
	gboolean has_status_flags;  /* 1 uint32: STEAM_PERSONA_REQ_* bits describing which fields are included */
	guint32 status_flags;
	GArray *friends;            /* 2 repeated SteamMsgPersonaFriend */
} SteamMsgClientPersonaState;

void steam_msg_client_persona_state_init(SteamMsgClientPersonaState *m);
void steam_msg_client_persona_state_clear(SteamMsgClientPersonaState *m);
void steam_msg_client_persona_state_encode(const SteamMsgClientPersonaState *m, GByteArray *out);
gboolean steam_msg_client_persona_state_decode(SteamMsgClientPersonaState *m, const guint8 *data, gsize len);
/* Appends a zero-initialised friend (with its rich_presence array) and returns it. */
SteamMsgPersonaFriend *steam_msg_client_persona_state_add_friend(SteamMsgClientPersonaState *m);
/* Appends a rich presence entry (strings are copied). */
void steam_msg_persona_friend_add_rich_presence(SteamMsgPersonaFriend *f, const gchar *key, const gchar *value);
/* Looks up a rich presence value by key, NULL if absent. */
const gchar *steam_msg_persona_friend_get_rich_presence(const SteamMsgPersonaFriend *f, const gchar *key);

/* CMsgClientChangeStatus (EMsg 716): set our own persona state/name. */
typedef struct {
	gboolean has_persona_state;          /* 1 uint32 EPersonaState */
	guint32 persona_state;
	gchar *player_name;                  /* 2 string: NULL = unchanged */
	gboolean has_is_auto_generated_name; /* 3 bool */
	gboolean is_auto_generated_name;
	gboolean has_high_priority;          /* 4 bool */
	gboolean high_priority;
	gboolean has_persona_set_by_user;    /* 5 bool */
	gboolean persona_set_by_user;
	gboolean has_persona_state_flags;    /* 6 uint32 */
	guint32 persona_state_flags;
	gboolean has_need_persona_response;  /* 7 bool */
	gboolean need_persona_response;
	gboolean has_is_client_idle;         /* 8 bool */
	gboolean is_client_idle;
} SteamMsgClientChangeStatus;

void steam_msg_client_change_status_init(SteamMsgClientChangeStatus *m);
void steam_msg_client_change_status_clear(SteamMsgClientChangeStatus *m);
void steam_msg_client_change_status_encode(const SteamMsgClientChangeStatus *m, GByteArray *out);
gboolean steam_msg_client_change_status_decode(SteamMsgClientChangeStatus *m, const guint8 *data, gsize len);

/* CMsgClientAddFriend (EMsg 791): also accepts an incoming request. */
typedef struct {
	gboolean has_steamid_to_add;       /* 1 fixed64 */
	guint64 steamid_to_add;
	gchar *accountname_or_email_to_add; /* 2 string */
} SteamMsgClientAddFriend;

void steam_msg_client_add_friend_init(SteamMsgClientAddFriend *m);
void steam_msg_client_add_friend_clear(SteamMsgClientAddFriend *m);
void steam_msg_client_add_friend_encode(const SteamMsgClientAddFriend *m, GByteArray *out);
gboolean steam_msg_client_add_friend_decode(SteamMsgClientAddFriend *m, const guint8 *data, gsize len);

/* CMsgClientAddFriendResponse (EMsg 792) */
typedef struct {
	gboolean has_eresult;         /* 1 int32; default 2 (FAIL) */
	gint32 eresult;
	gboolean has_steam_id_added;  /* 2 fixed64 */
	guint64 steam_id_added;
	gchar *persona_name_added;    /* 3 string */
} SteamMsgClientAddFriendResponse;

void steam_msg_client_add_friend_response_init(SteamMsgClientAddFriendResponse *m);
void steam_msg_client_add_friend_response_clear(SteamMsgClientAddFriendResponse *m);
void steam_msg_client_add_friend_response_encode(const SteamMsgClientAddFriendResponse *m, GByteArray *out);
gboolean steam_msg_client_add_friend_response_decode(SteamMsgClientAddFriendResponse *m, const guint8 *data, gsize len);

/* CMsgClientRemoveFriend (EMsg 714): also declines an incoming request. */
typedef struct {
	gboolean has_friendid;  /* 1 fixed64 */
	guint64 friendid;
} SteamMsgClientRemoveFriend;

void steam_msg_client_remove_friend_init(SteamMsgClientRemoveFriend *m);
void steam_msg_client_remove_friend_clear(SteamMsgClientRemoveFriend *m);
void steam_msg_client_remove_friend_encode(const SteamMsgClientRemoveFriend *m, GByteArray *out);
gboolean steam_msg_client_remove_friend_decode(SteamMsgClientRemoveFriend *m, const guint8 *data, gsize len);

/* CMsgClientPlayerNicknameList.PlayerNickname */
typedef struct {
	gboolean has_steamid;   /* 1 fixed64 */
	guint64 steamid;
	gchar *nickname;        /* 3 string (NB: field 3, not 2); "" or NULL = removed */
} SteamMsgClientPlayerNickname;

/* CMsgClientPlayerNicknameList (EMsg 5587) */
typedef struct {
	gboolean has_removal;      /* 1 bool */
	gboolean removal;
	gboolean has_incremental;  /* 2 bool */
	gboolean incremental;
	GArray *nicknames;         /* 3 repeated SteamMsgClientPlayerNickname */
} SteamMsgClientPlayerNicknameList;

void steam_msg_client_player_nickname_list_init(SteamMsgClientPlayerNicknameList *m);
void steam_msg_client_player_nickname_list_clear(SteamMsgClientPlayerNicknameList *m);
void steam_msg_client_player_nickname_list_encode(const SteamMsgClientPlayerNicknameList *m, GByteArray *out);
gboolean steam_msg_client_player_nickname_list_decode(SteamMsgClientPlayerNicknameList *m, const guint8 *data, gsize len);

/* ======================================================================
 * FriendMessages service (steammessages_friendmessages.steamclient.proto)
 * ====================================================================== */

/* CFriendMessages_SendMessage_Request (FriendMessages.SendMessage#1) */
typedef struct {
	gboolean has_steamid;          /* 1 fixed64: recipient */
	guint64 steamid;
	gboolean has_chat_entry_type;  /* 2 int32 EChatEntryType: 1 = message, 2 = typing */
	gint32 chat_entry_type;
	gchar *message;                /* 3 string */
	gboolean has_contains_bbcode;  /* 4 bool */
	gboolean contains_bbcode;
	gboolean has_echo_to_sender;   /* 5 bool: echo to our other sessions */
	gboolean echo_to_sender;
	gboolean has_low_priority;     /* 6 bool */
	gboolean low_priority;
	gchar *client_message_id;      /* 8 string */
} SteamMsgFriendMessagesSendMessageRequest;

void steam_msg_friend_messages_send_message_request_init(SteamMsgFriendMessagesSendMessageRequest *m);
void steam_msg_friend_messages_send_message_request_clear(SteamMsgFriendMessagesSendMessageRequest *m);
void steam_msg_friend_messages_send_message_request_encode(const SteamMsgFriendMessagesSendMessageRequest *m, GByteArray *out);
gboolean steam_msg_friend_messages_send_message_request_decode(SteamMsgFriendMessagesSendMessageRequest *m, const guint8 *data, gsize len);

/* CFriendMessages_SendMessage_Response */
typedef struct {
	gchar *modified_message;        /* 1 string */
	gboolean has_server_timestamp;  /* 2 uint32 unix time */
	guint32 server_timestamp;
	gboolean has_ordinal;           /* 3 uint32 */
	guint32 ordinal;
	gchar *message_without_bb_code; /* 4 string */
} SteamMsgFriendMessagesSendMessageResponse;

void steam_msg_friend_messages_send_message_response_init(SteamMsgFriendMessagesSendMessageResponse *m);
void steam_msg_friend_messages_send_message_response_clear(SteamMsgFriendMessagesSendMessageResponse *m);
void steam_msg_friend_messages_send_message_response_encode(const SteamMsgFriendMessagesSendMessageResponse *m, GByteArray *out);
gboolean steam_msg_friend_messages_send_message_response_decode(SteamMsgFriendMessagesSendMessageResponse *m, const guint8 *data, gsize len);

/* CFriendMessages_IncomingMessage_Notification
 * (FriendMessagesClient.IncomingMessage#1, arrives as EMsg ServiceMethod) */
typedef struct {
	gboolean has_steamid_friend;           /* 1 fixed64: the peer (also for local_echo) */
	guint64 steamid_friend;
	gboolean has_chat_entry_type;          /* 2 int32 EChatEntryType */
	gint32 chat_entry_type;
	gboolean has_from_limited_account;     /* 3 bool */
	gboolean from_limited_account;
	gchar *message;                        /* 4 string (bbcode) */
	gboolean has_rtime32_server_timestamp; /* 5 fixed32 unix time */
	guint32 rtime32_server_timestamp;
	gboolean has_ordinal;                  /* 6 uint32 */
	guint32 ordinal;
	gboolean has_local_echo;               /* 7 bool: we sent it from another session */
	gboolean local_echo;
	gchar *message_no_bbcode;              /* 8 string */
	gboolean has_low_priority;             /* 9 bool */
	gboolean low_priority;
} SteamMsgFriendMessagesIncomingMessage;

void steam_msg_friend_messages_incoming_message_init(SteamMsgFriendMessagesIncomingMessage *m);
void steam_msg_friend_messages_incoming_message_clear(SteamMsgFriendMessagesIncomingMessage *m);
void steam_msg_friend_messages_incoming_message_encode(const SteamMsgFriendMessagesIncomingMessage *m, GByteArray *out);
gboolean steam_msg_friend_messages_incoming_message_decode(SteamMsgFriendMessagesIncomingMessage *m, const guint8 *data, gsize len);

/* CFriendMessages_GetRecentMessages_Request (FriendMessages.GetRecentMessages#1) */
typedef struct {
	gboolean has_steamid1;                 /* 1 fixed64: us */
	guint64 steamid1;
	gboolean has_steamid2;                 /* 2 fixed64: the friend */
	guint64 steamid2;
	gboolean has_count;                    /* 3 uint32 */
	guint32 count;
	gboolean has_most_recent_conversation; /* 4 bool */
	gboolean most_recent_conversation;
	gboolean has_rtime32_start_time;       /* 5 fixed32 unix time */
	guint32 rtime32_start_time;
	gboolean has_bbcode_format;            /* 6 bool */
	gboolean bbcode_format;
	gboolean has_start_ordinal;            /* 7 uint32 */
	guint32 start_ordinal;
	gboolean has_time_last;                /* 8 uint32 */
	guint32 time_last;
	gboolean has_ordinal_last;             /* 9 uint32 */
	guint32 ordinal_last;
} SteamMsgFriendMessagesGetRecentMessagesRequest;

void steam_msg_friend_messages_get_recent_messages_request_init(SteamMsgFriendMessagesGetRecentMessagesRequest *m);
void steam_msg_friend_messages_get_recent_messages_request_clear(SteamMsgFriendMessagesGetRecentMessagesRequest *m);
void steam_msg_friend_messages_get_recent_messages_request_encode(const SteamMsgFriendMessagesGetRecentMessagesRequest *m, GByteArray *out);
gboolean steam_msg_friend_messages_get_recent_messages_request_decode(SteamMsgFriendMessagesGetRecentMessagesRequest *m, const guint8 *data, gsize len);

/* CFriendMessages_GetRecentMessages_Response.FriendMessage */
typedef struct {
	gboolean has_accountid;  /* 1 uint32: sender's account id (low 32 bits of SteamID) */
	guint32 accountid;
	gboolean has_timestamp;  /* 2 uint32 unix time */
	guint32 timestamp;
	gchar *message;          /* 3 string */
	gboolean has_ordinal;    /* 4 uint32 */
	guint32 ordinal;
	GArray *reactions;       /* 5 repeated SteamMsgFriendMessageReaction; NULL when none */
} SteamMsgFriendMessage;

/* EMessageReactionType */
#define STEAM_REACTION_TYPE_INVALID  0
#define STEAM_REACTION_TYPE_EMOTICON 1
#define STEAM_REACTION_TYPE_STICKER  2

/* CFriendMessages_GetRecentMessages_Response.FriendMessage.MessageReaction */
typedef struct {
	gboolean has_reaction_type;  /* 1 enum EMessageReactionType */
	gint32 reaction_type;
	gchar *reaction;             /* 2 string: the emoticon or sticker name */
	GArray *reactors;            /* 3 repeated uint32 account ids (never NULL in a decoded reaction) */
} SteamMsgFriendMessageReaction;

/* Appends an empty reaction to `fm` (for encoding) and returns it. */
SteamMsgFriendMessageReaction *steam_msg_friend_message_add_reaction(SteamMsgFriendMessage *fm);

/* CFriendMessages_GetRecentMessages_Response */
typedef struct {
	GArray *messages;           /* 1 repeated SteamMsgFriendMessage, newest first */
	gboolean has_more_available; /* 4 bool */
	gboolean more_available;
} SteamMsgFriendMessagesGetRecentMessagesResponse;

void steam_msg_friend_messages_get_recent_messages_response_init(SteamMsgFriendMessagesGetRecentMessagesResponse *m);
void steam_msg_friend_messages_get_recent_messages_response_clear(SteamMsgFriendMessagesGetRecentMessagesResponse *m);
void steam_msg_friend_messages_get_recent_messages_response_encode(const SteamMsgFriendMessagesGetRecentMessagesResponse *m, GByteArray *out);
gboolean steam_msg_friend_messages_get_recent_messages_response_decode(SteamMsgFriendMessagesGetRecentMessagesResponse *m, const guint8 *data, gsize len);

/* CFriendsMessages_GetActiveMessageSessions_Request (note the "Friends"
 * spelling in the proto name; FriendMessages.GetActiveMessageSessions#1) */
typedef struct {
	gboolean has_lastmessage_since;           /* 1 uint32 unix time */
	guint32 lastmessage_since;
	gboolean has_only_sessions_with_messages; /* 2 bool */
	gboolean only_sessions_with_messages;
} SteamMsgFriendMessagesGetActiveMessageSessionsRequest;

void steam_msg_friend_messages_get_active_message_sessions_request_init(SteamMsgFriendMessagesGetActiveMessageSessionsRequest *m);
void steam_msg_friend_messages_get_active_message_sessions_request_clear(SteamMsgFriendMessagesGetActiveMessageSessionsRequest *m);
void steam_msg_friend_messages_get_active_message_sessions_request_encode(const SteamMsgFriendMessagesGetActiveMessageSessionsRequest *m, GByteArray *out);
gboolean steam_msg_friend_messages_get_active_message_sessions_request_decode(SteamMsgFriendMessagesGetActiveMessageSessionsRequest *m, const guint8 *data, gsize len);

/* CFriendsMessages_GetActiveMessageSessions_Response.FriendMessageSession */
typedef struct {
	gboolean has_accountid_friend;     /* 1 uint32 */
	guint32 accountid_friend;
	gboolean has_last_message;         /* 2 uint32 unix time */
	guint32 last_message;
	gboolean has_last_view;            /* 3 uint32 unix time */
	guint32 last_view;
	gboolean has_unread_message_count; /* 4 uint32 */
	guint32 unread_message_count;
	/* 5 repeated EChatSessionNotice notices: not modelled (skipped) */
} SteamMsgFriendMessageSession;

/* CFriendsMessages_GetActiveMessageSessions_Response */
typedef struct {
	GArray *message_sessions;  /* 1 repeated SteamMsgFriendMessageSession */
	gboolean has_timestamp;    /* 2 uint32 server time */
	guint32 timestamp;
} SteamMsgFriendMessagesGetActiveMessageSessionsResponse;

void steam_msg_friend_messages_get_active_message_sessions_response_init(SteamMsgFriendMessagesGetActiveMessageSessionsResponse *m);
void steam_msg_friend_messages_get_active_message_sessions_response_clear(SteamMsgFriendMessagesGetActiveMessageSessionsResponse *m);
void steam_msg_friend_messages_get_active_message_sessions_response_encode(const SteamMsgFriendMessagesGetActiveMessageSessionsResponse *m, GByteArray *out);
gboolean steam_msg_friend_messages_get_active_message_sessions_response_decode(SteamMsgFriendMessagesGetActiveMessageSessionsResponse *m, const guint8 *data, gsize len);

/* CFriendMessages_AckMessage_Notification: sent as FriendMessages.AckMessage#1
 * (we have read the conversation up to `timestamp`; no response), received
 * as FriendMessagesClient.NotifyAckMessageEcho#1 (another session of ours
 * did). */
typedef struct {
	gboolean has_steamid_partner;  /* 1 fixed64 */
	guint64 steamid_partner;
	gboolean has_timestamp;        /* 2 uint32 unix time of the newest message read */
	guint32 timestamp;
} SteamMsgFriendMessagesAckMessage;

void steam_msg_friend_messages_ack_message_init(SteamMsgFriendMessagesAckMessage *m);
void steam_msg_friend_messages_ack_message_clear(SteamMsgFriendMessagesAckMessage *m);
void steam_msg_friend_messages_ack_message_encode(const SteamMsgFriendMessagesAckMessage *m, GByteArray *out);
gboolean steam_msg_friend_messages_ack_message_decode(SteamMsgFriendMessagesAckMessage *m, const guint8 *data, gsize len);

/* CFriendMessages_UpdateMessageReaction_Request (FriendMessages.UpdateMessageReaction#1) */
typedef struct {
	gboolean has_steamid;          /* 1 fixed64: the friend */
	guint64 steamid;
	gboolean has_server_timestamp; /* 2 uint32: the message's timestamp */
	guint32 server_timestamp;
	gboolean has_ordinal;          /* 3 uint32: the message's ordinal */
	guint32 ordinal;
	gboolean has_reaction_type;    /* 4 enum EMessageReactionType */
	gint32 reaction_type;
	gchar *reaction;               /* 5 string: emoticon or sticker name */
	gboolean has_is_add;           /* 6 bool */
	gboolean is_add;
} SteamMsgFriendMessagesUpdateMessageReactionRequest;

void steam_msg_friend_messages_update_message_reaction_request_init(SteamMsgFriendMessagesUpdateMessageReactionRequest *m);
void steam_msg_friend_messages_update_message_reaction_request_clear(SteamMsgFriendMessagesUpdateMessageReactionRequest *m);
void steam_msg_friend_messages_update_message_reaction_request_encode(const SteamMsgFriendMessagesUpdateMessageReactionRequest *m, GByteArray *out);
gboolean steam_msg_friend_messages_update_message_reaction_request_decode(SteamMsgFriendMessagesUpdateMessageReactionRequest *m, const guint8 *data, gsize len);

/* CFriendMessages_UpdateMessageReaction_Response */
typedef struct {
	GArray *reactors;              /* 1 repeated uint32 account ids */
} SteamMsgFriendMessagesUpdateMessageReactionResponse;

void steam_msg_friend_messages_update_message_reaction_response_init(SteamMsgFriendMessagesUpdateMessageReactionResponse *m);
void steam_msg_friend_messages_update_message_reaction_response_clear(SteamMsgFriendMessagesUpdateMessageReactionResponse *m);
void steam_msg_friend_messages_update_message_reaction_response_encode(const SteamMsgFriendMessagesUpdateMessageReactionResponse *m, GByteArray *out);
gboolean steam_msg_friend_messages_update_message_reaction_response_decode(SteamMsgFriendMessagesUpdateMessageReactionResponse *m, const guint8 *data, gsize len);

/* CFriendMessages_MessageReaction_Notification
 * (FriendMessagesClient.MessageReaction#1) */
typedef struct {
	gboolean has_steamid_friend;   /* 1 fixed64: the conversation's friend */
	guint64 steamid_friend;
	gboolean has_server_timestamp; /* 2 uint32: the reacted-to message */
	guint32 server_timestamp;
	gboolean has_ordinal;          /* 3 uint32 */
	guint32 ordinal;
	gboolean has_reactor;          /* 4 fixed64: who reacted */
	guint64 reactor;
	gboolean has_reaction_type;    /* 5 enum EMessageReactionType */
	gint32 reaction_type;
	gchar *reaction;               /* 6 string */
	gboolean has_is_add;           /* 7 bool */
	gboolean is_add;
} SteamMsgFriendMessagesMessageReaction;

void steam_msg_friend_messages_message_reaction_init(SteamMsgFriendMessagesMessageReaction *m);
void steam_msg_friend_messages_message_reaction_clear(SteamMsgFriendMessagesMessageReaction *m);
void steam_msg_friend_messages_message_reaction_encode(const SteamMsgFriendMessagesMessageReaction *m, GByteArray *out);
gboolean steam_msg_friend_messages_message_reaction_decode(SteamMsgFriendMessagesMessageReaction *m, const guint8 *data, gsize len);

/* ======================================================================
 * Player service (steammessages_player.steamclient.proto)
 * ====================================================================== */

/* CPlayer_GetNicknameList_Request (Player.GetNicknameList#1): empty. */
typedef struct {
	gint unused;
} SteamMsgPlayerGetNicknameListRequest;

void steam_msg_player_get_nickname_list_request_init(SteamMsgPlayerGetNicknameListRequest *m);
void steam_msg_player_get_nickname_list_request_clear(SteamMsgPlayerGetNicknameListRequest *m);
void steam_msg_player_get_nickname_list_request_encode(const SteamMsgPlayerGetNicknameListRequest *m, GByteArray *out);
gboolean steam_msg_player_get_nickname_list_request_decode(SteamMsgPlayerGetNicknameListRequest *m, const guint8 *data, gsize len);

/* CPlayer_GetNicknameList_Response.PlayerNickname */
typedef struct {
	gboolean has_accountid;  /* 1 fixed32 (NB: fixed32, not uint32) */
	guint32 accountid;
	gchar *nickname;         /* 2 string */
} SteamMsgPlayerNickname;

/* CPlayer_GetNicknameList_Response */
typedef struct {
	GArray *nicknames;  /* 1 repeated SteamMsgPlayerNickname */
} SteamMsgPlayerGetNicknameListResponse;

void steam_msg_player_get_nickname_list_response_init(SteamMsgPlayerGetNicknameListResponse *m);
void steam_msg_player_get_nickname_list_response_clear(SteamMsgPlayerGetNicknameListResponse *m);
void steam_msg_player_get_nickname_list_response_encode(const SteamMsgPlayerGetNicknameListResponse *m, GByteArray *out);
gboolean steam_msg_player_get_nickname_list_response_decode(SteamMsgPlayerGetNicknameListResponse *m, const guint8 *data, gsize len);

#endif /* STEAM_MSGS_H */
