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


#ifndef LIBSTEAM_H
#define LIBSTEAM_H

/* Maximum number of simultaneous connections to a server */
#define STEAM_MAX_CONNECTIONS 16

#include <glib.h>

#include <errno.h>
#include <string.h>
#include <glib/gi18n.h>
#include <sys/types.h>
#ifdef __GNUC__
	#include <unistd.h>
#endif

#ifndef G_GNUC_NULL_TERMINATED
#	if __GNUC__ >= 4
#		define G_GNUC_NULL_TERMINATED __attribute__((__sentinel__))
#	else
#		define G_GNUC_NULL_TERMINATED
#	endif /* __GNUC__ >= 4 */
#endif /* G_GNUC_NULL_TERMINATED */

#ifdef _WIN32
#	include "win32dep.h"
#	define dlopen(a,b) LoadLibrary(a)
#	define RTLD_LAZY
#	define dlsym(a,b) GetProcAddress(a,b)
#	define dlclose(a) FreeLibrary(a)
#else
#	include <arpa/inet.h>
#	include <dlfcn.h>
#	include <netinet/in.h>
#	include <sys/socket.h>
#endif

#include <json-glib/json-glib.h>

#define json_object_get_int_member(JSON_OBJECT, MEMBER) \
	(json_object_has_member(JSON_OBJECT, MEMBER) ? json_object_get_int_member(JSON_OBJECT, MEMBER) : 0)
#define json_object_get_string_member(JSON_OBJECT, MEMBER) \
	(json_object_has_member(JSON_OBJECT, MEMBER) ? json_object_get_string_member(JSON_OBJECT, MEMBER) : NULL)
#define json_object_get_array_member(JSON_OBJECT, MEMBER) \
	(json_object_has_member(JSON_OBJECT, MEMBER) ? json_object_get_array_member(JSON_OBJECT, MEMBER) : NULL)
#define json_object_get_object_member(JSON_OBJECT, MEMBER) \
	(json_object_has_member(JSON_OBJECT, MEMBER) ? json_object_get_object_member(JSON_OBJECT, MEMBER) : NULL)
#define json_object_get_boolean_member(JSON_OBJECT, MEMBER) \
	(json_object_has_member(JSON_OBJECT, MEMBER) ? json_object_get_boolean_member(JSON_OBJECT, MEMBER) : FALSE)

#ifndef PURPLE_PLUGINS
#	define PURPLE_PLUGINS
#endif

#include "accountopt.h"
#include "blist.h"
#include "buddyicon.h"
#include "conversation.h"
#include "core.h"
#include "connection.h"
#include "debug.h"
#include "dnsquery.h"
#include "proxy.h"
#include "prpl.h"
#include "notify.h"
#include "request.h"
#include "savedstatuses.h"
#include "server.h"
#include "sslconn.h"
#include "util.h"
#include "version.h"

#if GLIB_MAJOR_VERSION >= 2 && GLIB_MINOR_VERSION >= 12
#	define atoll(a) g_ascii_strtoll(a, NULL, 0)
#endif

#define STEAM_PLUGIN_ID "prpl-steam-mobile"
#define STEAM_PLUGIN_VERSION "2.0"

typedef struct _SteamAccount SteamAccount;
typedef struct _SteamBuddy SteamBuddy;
struct _SteamAuth;   /* steam_auth.h */
struct _SteamCM;     /* steam_cm.h */

typedef void (*SteamFunc)(SteamAccount *sa);

struct _SteamAccount {
	PurpleAccount *account;
	PurpleConnection *pc;

	/* HTTPS helper state (steam_connection.c) */
	GSList *conns; /**< A list of all active SteamConnections */
	GQueue *waiting_conns; /**< A list of all SteamConnections waiting to process */
	GSList *dns_queries;
	GHashTable *cookie_table;
	GHashTable *hostname_ip_cache;

	/* "<steamid>\n<text>" -> send time; used to drop the local_echo of
	 * messages we sent ourselves */
	GHashTable *sent_messages_hash;

	guint64 steamid;               /* our 64-bit SteamID, 0 until known */
	gint idletime;                 /* seconds idle as reported by set_idle, 0 when active */
	guint32 last_message_timestamp;/* newest message seen, persisted as "last_message_timestamp" */
	guint32 history_since;         /* last_message_timestamp as it was at logon */
	GHashTable *live_message_since;/* steamid string -> timestamp of the first live message shown since logon */
	gchar *cached_refresh_token;   /* keyring (Telepathy-Haze) copy of the refresh token */
	gpointer keyring_lookup;       /* in-flight keyring lookup, or NULL */

	/* New protocol layers (see docs/architecture.md) */
	struct _SteamAuth *auth;   /* in-progress IAuthenticationService login, or NULL */
	struct _SteamCM *cm;       /* CM session, or NULL */
	struct _SteamCM *dead_cm;  /* CM that rejected our token, freed from an idle callback */
	guint dead_cm_timeout;

	gboolean password_login_tried; /* a password login was started since the last successful CM logon */
	gint guard_type;               /* SteamGuardType the open code dialog asks for */
	gpointer guard_request;        /* open Steam Guard request dialog, or NULL */
	gint guard_request_type;       /* its PurpleRequestType */

	GHashTable *typing_sent;       /* steamid string -> time we last sent a typing notification */
	GHashTable *friend_requests;   /* steamid string -> STEAM_FRIEND_REQUEST_* */
	guint friend_request_timeout;
	GSList *auth_requests;         /* SteamFriendRequest with an open authorization dialog */
	GHashTable *nicknames;         /* steamid string -> nickname */

	GQueue *icon_queue;            /* SteamIconFetch waiting to start */
	GSList *icon_fetches;          /* SteamIconFetch in flight */
	GHashTable *app_names;         /* appid -> game name ("" while unknown) */
	GSList *app_fetches;           /* SteamAppFetch in flight */
	GSList *pending_sends;         /* SteamSendContext awaiting a SendMessage reply */
};

struct _SteamBuddy {
	SteamAccount *sa;
	PurpleBuddy *buddy;

	gchar *steamid;
	gchar *personaname;
	gchar *nickname;
	gchar *avatar;                 /* avatar hash (hex) */
	guint lastlogoff;
	guint personastateflags;
	gint personastate;             /* SteamPersonaState */
	gboolean personastate_known;
	gint relationship;             /* SteamFriendRelationship */

	guint32 game_app_id;
	gchar *gameid;                 /* 64-bit GameID as a string, NULL when not in game */
	gchar *gameextrainfo;          /* game name */
	gchar *gameserversteamid;
	gchar *lobbysteamid;
	gchar *gameserverip;           /* "a.b.c.d:port" */
};

#define STEAMID_IS_GROUP(id) G_UNLIKELY(((g_ascii_strtoll((id), NULL, 10) >> 52) & 0x0F) == 7)

#endif /* LIBSTEAM_H */
