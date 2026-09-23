/*
 *  Steam Mobile Plugin for Pidgin
 *  Copyright (C) 2012-2016 Eion Robb
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
 * Steam Connection Manager (CM) session over WebSocket.
 *
 * This is the real-time channel: logon with a refresh token, heartbeat,
 * friends list, persona (presence) state, chat messages, typing, nicknames,
 * friend requests. It owns the transport (steam_ws), the framing and the
 * protobuf messages (steam_msgs), and exposes plain C structs and callbacks
 * to libsteam.c so the libpurple layer never touches protobuf.
 *
 * Reconnection: transient failures (socket drop, TryAnotherCM,
 * ServiceUnavailable, heartbeat timeout) are retried internally against
 * other CM servers with backoff. On success `logged_on` fires again and the
 * caller must re-apply its persona state. Only unrecoverable failures reach
 * `disconnected` / `logon_failed`.
 *
 * See docs/architecture.md for message numbers and field references.
 */

#ifndef STEAM_CM_H
#define STEAM_CM_H

#include "libsteam.h"
#include "steam_eresult.h"

typedef struct _SteamCM SteamCM;

/* EPersonaState */
typedef enum {
	STEAM_PERSONA_OFFLINE = 0,
	STEAM_PERSONA_ONLINE = 1,
	STEAM_PERSONA_BUSY = 2,
	STEAM_PERSONA_AWAY = 3,
	STEAM_PERSONA_SNOOZE = 4,
	STEAM_PERSONA_LOOKING_TO_TRADE = 5,
	STEAM_PERSONA_LOOKING_TO_PLAY = 6,
	STEAM_PERSONA_INVISIBLE = 7
} SteamPersonaState;

/* EFriendRelationship */
typedef enum {
	STEAM_RELATIONSHIP_NONE = 0,               /* removed / not a friend */
	STEAM_RELATIONSHIP_BLOCKED = 1,
	STEAM_RELATIONSHIP_REQUEST_RECIPIENT = 2,  /* they sent us a friend request */
	STEAM_RELATIONSHIP_FRIEND = 3,
	STEAM_RELATIONSHIP_REQUEST_INITIATOR = 4,  /* we sent them a request */
	STEAM_RELATIONSHIP_IGNORED = 5,
	STEAM_RELATIONSHIP_IGNORED_FRIEND = 6
} SteamFriendRelationship;

/* EChatEntryType */
typedef enum {
	STEAM_CHAT_ENTRY_INVALID = 0,
	STEAM_CHAT_ENTRY_CHAT_MSG = 1,
	STEAM_CHAT_ENTRY_TYPING = 2,
	STEAM_CHAT_ENTRY_INVITE_GAME = 3,
	STEAM_CHAT_ENTRY_LEFT_CONVERSATION = 6,
	STEAM_CHAT_ENTRY_ENTERED = 7,
	STEAM_CHAT_ENTRY_WAS_KICKED = 8,
	STEAM_CHAT_ENTRY_WAS_BANNED = 9,
	STEAM_CHAT_ENTRY_DISCONNECTED = 10,
	STEAM_CHAT_ENTRY_HISTORICAL_CHAT = 11,
	STEAM_CHAT_ENTRY_LINK_BLOCKED = 14
} SteamChatEntryType;

/* Bits of persona_state_flags (EPersonaStateFlag) */
#define STEAM_PERSONA_FLAG_HAS_RICH_PRESENCE 0x001
#define STEAM_PERSONA_FLAG_IN_JOINABLE_GAME  0x002
#define STEAM_PERSONA_FLAG_GOLDEN            0x004
#define STEAM_PERSONA_FLAG_REMOTE_PLAY       0x008
#define STEAM_PERSONA_FLAG_CLIENT_WEB        0x100
#define STEAM_PERSONA_FLAG_CLIENT_MOBILE     0x200
#define STEAM_PERSONA_FLAG_CLIENT_TENFOOT    0x400
#define STEAM_PERSONA_FLAG_CLIENT_VR         0x800
#define STEAM_PERSONA_FLAG_LAUNCH_TYPE_GAMEPAD 0x1000

typedef struct {
	guint64 steamid;
	SteamFriendRelationship relationship;
} SteamCMFriend;

/* One friend's persona update. Only fields flagged by a has_ member or a
 * non-NULL pointer are present; a persona message carries just the fields
 * that changed or were requested. */
typedef struct {
	guint64 steamid;

	gboolean has_state;
	SteamPersonaState state;
	guint32 state_flags;

	gchar *player_name;        /* NULL if absent */
	gchar *avatar_hash;        /* hex, NULL if absent, "" for the default avatar */

	gboolean has_game;         /* game fields below are valid */
	guint32 game_app_id;       /* 0 when not in a game */
	guint64 gameid;            /* full GameID (0 when not in a game) */
	gchar *game_name;          /* NULL if absent */
	guint64 game_lobby_id;
	guint64 game_server_steamid;
	guint32 game_server_ip;
	guint16 game_server_port;

	guint32 last_logoff;       /* unix time, 0 if absent */
	guint32 last_logon;
} SteamCMPersona;

typedef struct {
	guint64 from_steamid;
	SteamChatEntryType type;
	gchar *message;            /* plain text (bbcode stripped when Steam provides it), may be NULL for typing */
	guint32 timestamp;         /* unix time from the server */
	gboolean local_echo;       /* sent by us from another client; from_steamid is the peer */
	gchar *message_bbcode;     /* the message with Steam's BBCode ([emoticon], [img], [url], ...), NULL if absent */
	guint32 ordinal;           /* tells apart messages with the same timestamp */
} SteamCMMessage;

typedef struct {
	guint32 accountid;         /* low 32 bits of the sender's SteamID */
	guint32 timestamp;
	gchar *message;
	guint32 ordinal;
} SteamCMHistoryMessage;

typedef struct {
	guint32 accountid_friend;
	guint32 last_message;      /* unix time */
	guint32 last_view;
	guint32 unread_message_count;
} SteamCMMessageSession;

typedef struct {
	guint64 steamid;
	gchar *nickname;           /* NULL/"" means nickname removed */
} SteamCMNickname;

typedef struct {
	/* Logged on to a CM. Fires again after an internal reconnect. */
	void (*logged_on)(SteamCM *cm, guint64 steamid, gpointer user_data);

	/* Logon rejected and not retryable, e.g. the refresh token is expired or
	 * revoked (EXPIRED, ACCESS_DENIED, INVALID_PASSWORD). The caller should
	 * clear the stored token and start a password login. */
	void (*logon_failed)(SteamCM *cm, SteamEResult eresult, const gchar *message,
	                     gpointer user_data);

	/* Unrecoverable loss of connection after retries were exhausted, or the
	 * server logged us off for good (e.g. LOGON_SESSION_REPLACED). */
	void (*disconnected)(SteamCM *cm, SteamEResult eresult, const gchar *message,
	                     gpointer user_data);

	/* Our own account info (ClientAccountInfo). */
	void (*account_info)(SteamCM *cm, const gchar *persona_name, gpointer user_data);

	/* ClientFriendsList. When `incremental` is FALSE this is the full list
	 * (sent right after logon); otherwise only the changed entries. Entries
	 * with relationship NONE were removed. Group/clan SteamIDs are filtered
	 * out by the CM layer. */
	void (*friends_list)(SteamCM *cm, gboolean incremental,
	                     const SteamCMFriend *friends, guint n_friends,
	                     gpointer user_data);

	/* ClientPersonaState: one call per friend in the message. May include
	 * our own SteamID. */
	void (*persona_state)(SteamCM *cm, const SteamCMPersona *persona,
	                      gpointer user_data);

	/* FriendMessagesClient.IncomingMessage: chat message, typing, etc. */
	void (*message)(SteamCM *cm, const SteamCMMessage *message, gpointer user_data);

	/* ClientPlayerNicknameList (sent after logon) and Player.GetNicknameList. */
	void (*nicknames)(SteamCM *cm, const SteamCMNickname *nicknames, guint n,
	                  gboolean incremental, gboolean removal, gpointer user_data);

	/* ClientAddFriendResponse. `persona_name` may be NULL. */
	void (*add_friend_response)(SteamCM *cm, SteamEResult eresult, guint64 steamid,
	                            const gchar *persona_name, gpointer user_data);

	/* FriendMessagesClient.NotifyAckMessageEcho: another session of ours
	 * read the conversation with `steamid_partner` up to `timestamp`. */
	void (*ack_echo)(SteamCM *cm, guint64 steamid_partner, guint32 timestamp,
	                 gpointer user_data);
} SteamCMCallbacks;

SteamCM *steam_cm_new(SteamAccount *sa, const SteamCMCallbacks *callbacks,
                      gpointer user_data);

/* Looks up a CM via ISteamDirectory/GetCMListForConnect, connects over
 * WebSocket and logs on with `refresh_token`. `steamid` is the account's
 * 64-bit SteamID (from the token's "sub" claim). */
void steam_cm_connect(SteamCM *cm, const gchar *refresh_token, guint64 steamid);

/* Sends ClientLogOff and closes. No callbacks fire. */
void steam_cm_disconnect(SteamCM *cm);

/* Disconnects if needed and frees. */
void steam_cm_free(SteamCM *cm);

gboolean steam_cm_is_logged_on(const SteamCM *cm);
guint64 steam_cm_get_steamid(const SteamCM *cm);

/* ClientChangeStatus. Must be called with a non-OFFLINE state after logon
 * for friends to see us and for persona updates to start flowing.
 * `player_name` may be NULL to leave it unchanged. */
void steam_cm_set_persona_state(SteamCM *cm, SteamPersonaState state,
                                const gchar *player_name);

/* ClientRequestFriendData for name/avatar/status/game info. Results arrive
 * via `persona_state`. The CM layer requests data for the whole friends list
 * automatically after logon. */
void steam_cm_request_friend_data(SteamCM *cm, const guint64 *steamids, guint n);

/* ClientAddFriend. Also accepts a pending incoming friend request. */
void steam_cm_add_friend(SteamCM *cm, guint64 steamid);
/* ClientRemoveFriend. Also declines a pending incoming friend request. */
void steam_cm_remove_friend(SteamCM *cm, guint64 steamid);

/* FriendMessages.SendMessage. `callback` may be NULL. `type` is
 * STEAM_CHAT_ENTRY_CHAT_MSG for text or STEAM_CHAT_ENTRY_TYPING for typing
 * notifications (with an empty message). */
typedef void (*SteamCMSendMessageFunc)(SteamCM *cm, SteamEResult eresult,
                                       guint32 server_timestamp, gpointer user_data);
void steam_cm_send_message(SteamCM *cm, guint64 steamid, SteamChatEntryType type,
                           const gchar *message, SteamCMSendMessageFunc callback,
                           gpointer user_data);

/* The same, with the message's ordinal from the reply (0 when absent or on
 * failure); the server timestamp and ordinal identify the message. */
typedef void (*SteamCMSendMessageFullFunc)(SteamCM *cm, SteamEResult eresult,
                                           guint32 server_timestamp, guint32 ordinal,
                                           gpointer user_data);
void steam_cm_send_message_full(SteamCM *cm, guint64 steamid, SteamChatEntryType type,
                                const gchar *message, SteamCMSendMessageFullFunc callback,
                                gpointer user_data);

/* FriendMessages.AckMessage: we have read the conversation with
 * `steamid_partner` up to the message at `timestamp`. No reply. */
void steam_cm_ack_message(SteamCM *cm, guint64 steamid_partner, guint32 timestamp);

/* FriendMessages.GetRecentMessages for one conversation, newest first as
 * returned by Steam. `since` is a unix time (0 for none). */
typedef void (*SteamCMHistoryFunc)(SteamCM *cm, guint64 friend_steamid,
                                   const SteamCMHistoryMessage *messages, guint n,
                                   gboolean more_available, gpointer user_data);
void steam_cm_get_recent_messages(SteamCM *cm, guint64 friend_steamid,
                                  guint32 since, guint count,
                                  SteamCMHistoryFunc callback, gpointer user_data);

/* The same with every request field. Messages come newest first. */
typedef struct {
	guint count;                     /* count: at most this many */
	gboolean most_recent_conversation;
	guint32 start_time;              /* rtime32_start_time: nothing older (0 = no bound) */
	guint32 start_ordinal;           /* start_ordinal, with start_time */
	guint32 time_last;               /* time_last: nothing newer (0 = not sent) */
	guint32 ordinal_last;            /* ordinal_last, with time_last (0 = not sent) */
	gboolean bbcode;                 /* bbcode_format: messages keep Steam's BBCode */
} SteamCMHistoryQuery;
void steam_cm_get_recent_messages_query(SteamCM *cm, guint64 friend_steamid,
                                        const SteamCMHistoryQuery *query,
                                        SteamCMHistoryFunc callback, gpointer user_data);

/* FriendMessages.GetActiveMessageSessions: which friends have unread or
 * recent messages, used to fetch offline history after logon. */
typedef void (*SteamCMSessionsFunc)(SteamCM *cm, const SteamCMMessageSession *sessions,
                                    guint n, guint32 server_timestamp, gpointer user_data);
void steam_cm_get_active_message_sessions(SteamCM *cm, guint32 since,
                                          SteamCMSessionsFunc callback,
                                          gpointer user_data);

/* Player.GetNicknameList; results arrive via the `nicknames` callback. */
void steam_cm_get_nickname_list(SteamCM *cm);

/* SteamID helpers */
#define STEAM_ID_ACCOUNT_BASE G_GUINT64_CONSTANT(76561197960265728) /* universe public, type individual, instance 1 */
static inline guint64 steam_cm_accountid_to_steamid(guint32 accountid) { return STEAM_ID_ACCOUNT_BASE + accountid; }
static inline guint32 steam_cm_steamid_to_accountid(guint64 steamid) { return (guint32)(steamid & 0xffffffff); }
static inline gboolean steam_cm_steamid_is_individual(guint64 steamid) { return ((steamid >> 52) & 0xf) == 1; }
static inline gboolean steam_cm_steamid_is_clan(guint64 steamid) { return ((steamid >> 52) & 0xf) == 7; }

#endif /* STEAM_CM_H */
