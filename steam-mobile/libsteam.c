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
 * libpurple integration layer. All protocol work goes through steam_auth
 * (IAuthenticationService login) and steam_cm (Connection Manager session);
 * see docs/architecture.md.
 */

#include "libsteam.h"
#include "steam_connection.h"
#include "steam_auth.h"
#include "steam_cm.h"
#include "steam_eresult.h"

static gboolean core_is_haze = FALSE;

// Hack to fix OSX compatibility :)
#ifdef __APPLE__
#undef G_OS_UNIX
#endif

#ifdef G_OS_UNIX
#include <dlfcn.h>

#ifdef USE_GNOME_KEYRING
#include <gnome-keyring.h>

// Copy of GNOME_KEYRING_NETWORK_PASSWORD to use locally
static const GnomeKeyringPasswordSchema network_password_schema = {
	GNOME_KEYRING_ITEM_NETWORK_PASSWORD,
	{
		{ "user", GNOME_KEYRING_ATTRIBUTE_TYPE_STRING },
		{ "domain", GNOME_KEYRING_ATTRIBUTE_TYPE_STRING },
		{ "object", GNOME_KEYRING_ATTRIBUTE_TYPE_STRING },
		{ "protocol", GNOME_KEYRING_ATTRIBUTE_TYPE_STRING },
		{ "port", GNOME_KEYRING_ATTRIBUTE_TYPE_UINT32 },
		{ "server", GNOME_KEYRING_ATTRIBUTE_TYPE_STRING },
		{ "NULL", 0 },
	}
};
static const GnomeKeyringPasswordSchema *my_GKNP = &network_password_schema;

static gpointer gnome_keyring_lib = NULL;

typedef gpointer (*gnome_keyring_store_password_type)(const GnomeKeyringPasswordSchema *schema, const gchar *keyring, const gchar *display_name, const gchar *password, GnomeKeyringOperationDoneCallback callback, gpointer data, GDestroyNotify destroy_data, ...);
static gnome_keyring_store_password_type my_gnome_keyring_store_password = NULL;

typedef gpointer (*gnome_keyring_delete_password_type)(const GnomeKeyringPasswordSchema *schema, GnomeKeyringOperationDoneCallback callback, gpointer data, GDestroyNotify destroy_data, ...);
static gnome_keyring_delete_password_type my_gnome_keyring_delete_password = NULL;

typedef gpointer (*gnome_keyring_find_password_type)(const GnomeKeyringPasswordSchema *schema, GnomeKeyringOperationGetStringCallback callback, gpointer data, GDestroyNotify destroy_data, ...);
static gnome_keyring_find_password_type my_gnome_keyring_find_password = NULL;

#else // USE_GNOME_KEYRING

#include <libsecret/secret.h>
// Copy of SECRET_SCHEMA_COMPAT_NETWORK to use locally
static const SecretSchema network_schema = {
	"org.gnome.keyring.NetworkPassword",
	SECRET_SCHEMA_NONE,
	{
		{  "user", SECRET_SCHEMA_ATTRIBUTE_STRING },
		{  "domain", SECRET_SCHEMA_ATTRIBUTE_STRING },
		{  "object", SECRET_SCHEMA_ATTRIBUTE_STRING },
		{  "protocol", SECRET_SCHEMA_ATTRIBUTE_STRING },
		{  "port", SECRET_SCHEMA_ATTRIBUTE_INTEGER },
		{  "server", SECRET_SCHEMA_ATTRIBUTE_STRING },
		{  "authtype", SECRET_SCHEMA_ATTRIBUTE_STRING },
		{  NULL, 0 },
	}
};
static const SecretSchema *my_SSCN = &network_schema;

static gpointer secret_lib = NULL;

typedef gpointer (*secret_password_store_type)(const SecretSchema *schema, const gchar *collection, const gchar *label, const gchar *password, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data, ...);
static secret_password_store_type my_secret_password_store = NULL;

typedef gpointer (*secret_password_clear_type)(const SecretSchema *schema, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data, ...);
static secret_password_clear_type my_secret_password_clear = NULL;

typedef gpointer (*secret_password_lookup_type)(const SecretSchema *schema, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer user_data, ...);
static secret_password_lookup_type my_secret_password_lookup = NULL;

typedef gpointer (*secret_password_lookup_finish_type)(GAsyncResult *result, GError **error);
static secret_password_lookup_finish_type my_secret_password_lookup_finish = NULL;

typedef void (*secret_password_free_type)(gchar *password);
static secret_password_free_type my_secret_password_free = NULL;


#endif // USE_GNOME_KEYRING
#endif


#if !PURPLE_VERSION_CHECK(3, 0, 0)
	#define purple_connection_error purple_connection_error_reason
	#define purple_notify_user_info_add_pair_html purple_notify_user_info_add_pair
#endif

/* Keyring attributes. The legacy (OAuth, pre-2.0) entry used server
 * "api.steamcommunity.com"; that token is useless now and is deleted. */
#define STEAM_KEYRING_SERVER "api.steampowered.com"
#define STEAM_KEYRING_LEGACY_SERVER "api.steamcommunity.com"
#define STEAM_KEYRING_PROTOCOL "steammobile"
#define STEAM_KEYRING_DOMAIN "libpurple"

/* Account setting holding the refresh token when no keyring is used */
#define STEAM_REFRESH_TOKEN_SETTING "refresh_token"

/* Refresh tokens that expire within this many seconds are not used */
#define STEAM_TOKEN_EXPIRY_MARGIN (24 * 60 * 60)

#define STEAM_TYPING_INTERVAL 10
#define STEAM_MAX_ICON_DOWNLOADS 4
#define STEAM_HISTORY_COUNT 100
/* Seconds before "now" that history starts from on the very first login */
#define STEAM_FIRST_LOGIN_HISTORY_MARGIN (5 * 60)
#define STEAM_FRIEND_REQUEST_DELAY 10
#define STEAM_DEFAULT_AVATAR_HASH "fef49e7fa7e1997310d705b2a6158ff8dc1cdfeb"
#define STEAM_AVATAR_URL "https://avatars.steamstatic.com/%s_full.jpg"
#define STEAM_APPDETAILS_URL "https://store.steampowered.com/api/appdetails?appids=%u&filters=basic"
#define STEAM_PROFILE_URL "https://steamcommunity.com/profiles/%s"
#define STEAM_GROUP_NAME "Steam"

/* Values in sa->friend_requests */
#define STEAM_FRIEND_REQUEST_WAITING_NAME 1
#define STEAM_FRIEND_REQUEST_SHOWN 2

/* Big enough for a 64-bit decimal number */
#define STEAM_ID_STR_LEN 24

static void steam_start_password_login(SteamAccount *sa);
static void steam_start_cm(SteamAccount *sa, const gchar *refresh_token);
static void steam_buddy_update_status(SteamAccount *sa, SteamBuddy *sbuddy);

/******************************************************************************/
/* Keyring / refresh token storage */
/******************************************************************************/

static const gchar *
steam_account_get_refresh_token(SteamAccount *sa)
{
	if (core_is_haze) {
		return sa->cached_refresh_token;
	}
	return purple_account_get_string(sa->account, STEAM_REFRESH_TOKEN_SETTING, NULL);
}

#ifdef G_OS_UNIX

#ifdef USE_GNOME_KEYRING
static void
dummy_gnome_callback(GnomeKeyringResult result, gpointer user_data) {
	// Gnome keyring throws toys out of cots if there's no callback!
	if (result == GNOME_KEYRING_RESULT_OK) {
		purple_debug_info("steam", "Keyring operation OK\n");
	} else if (result == GNOME_KEYRING_RESULT_CANCELLED) {
		purple_debug_error("steam", "Keyring operation cancelled by user\n");
	} else {
		purple_debug_error("steam", "Keyring operation failed (%d)\n", result);
	}
}
#endif //USE_GNOME_KEYRING

static void
steam_keyring_clear(SteamAccount *sa, const gchar *server)
{
#ifdef USE_GNOME_KEYRING
	my_gnome_keyring_delete_password(my_GKNP, //GNOME_KEYRING_NETWORK_PASSWORD,
									 dummy_gnome_callback, NULL, NULL,
									 "user",		sa->account->username,
									 "server",		server,
									 "protocol",	STEAM_KEYRING_PROTOCOL,
									 "domain",		STEAM_KEYRING_DOMAIN,
									 NULL);
#else // !USE_GNOME_KEYRING
	my_secret_password_clear(my_SSCN, //SECRET_SCHEMA_COMPAT_NETWORK
						  NULL, NULL, NULL,
						  "user",     sa->account->username,
						  "server",   server,
						  "protocol", STEAM_KEYRING_PROTOCOL,
						  "domain",   STEAM_KEYRING_DOMAIN,
						  NULL);
#endif // USE_GNOME_KEYRING
}

#endif

static void
steam_account_set_refresh_token(SteamAccount *sa, const gchar *refresh_token)
{
	if (refresh_token != NULL && *refresh_token == '\0')
		refresh_token = NULL;

#ifdef G_OS_UNIX
	if (core_is_haze) {
		g_free(sa->cached_refresh_token);
		sa->cached_refresh_token = g_strdup(refresh_token);

		if (refresh_token != NULL) {
#ifdef USE_GNOME_KEYRING
			my_gnome_keyring_store_password(my_GKNP, //GNOME_KEYRING_NETWORK_PASSWORD,
											NULL,
											_("Steam Refresh Token"),
											refresh_token,
											dummy_gnome_callback, NULL, NULL,
											"user",		sa->account->username,
											"server",	STEAM_KEYRING_SERVER,
											"protocol",	STEAM_KEYRING_PROTOCOL,
											"domain",	STEAM_KEYRING_DOMAIN,
											NULL);
#else // !USE_GNOME_KEYRING
			my_secret_password_store(my_SSCN, //SECRET_SCHEMA_COMPAT_NETWORK
									 NULL,
									 _("Steam Refresh Token"),
									 refresh_token,
									 NULL, NULL, NULL,
									 "user",     sa->account->username,
									 "server",   STEAM_KEYRING_SERVER,
									 "protocol", STEAM_KEYRING_PROTOCOL,
									 "domain",   STEAM_KEYRING_DOMAIN,
									 NULL);
#endif //USE_GNOME_KEYRING
		} else {
			steam_keyring_clear(sa, STEAM_KEYRING_SERVER);
		}
		return;
	}
#endif

	if (refresh_token != NULL) {
		purple_account_set_string(sa->account, STEAM_REFRESH_TOKEN_SETTING, refresh_token);
	} else {
		purple_account_remove_setting(sa->account, STEAM_REFRESH_TOKEN_SETTING);
	}
}

/* Removes settings only the pre-2.0 (mobile web API) plugin used. */
static void
steam_account_remove_legacy_settings(SteamAccount *sa)
{
	PurpleAccount *account = sa->account;

	if (purple_account_get_string(account, "access_token", NULL))
		purple_account_remove_setting(account, "access_token");
	if (purple_account_get_string(account, "steam_guard_code", NULL))
		purple_account_remove_setting(account, "steam_guard_code");
	if (purple_account_get_string(account, "emailsteamid", NULL))
		purple_account_remove_setting(account, "emailsteamid");

#ifdef G_OS_UNIX
	if (core_is_haze) {
		steam_keyring_clear(sa, STEAM_KEYRING_LEGACY_SERVER);
	}
#endif
}

/******************************************************************************/
/* Helpers */
/******************************************************************************/

static const gchar *
steam_personastate_to_statustype(gint64 state)
{
	const char *status_id;
	PurpleStatusPrimitive prim;
	switch(state)
	{
		default:
		case STEAM_PERSONA_OFFLINE:
		case STEAM_PERSONA_INVISIBLE: // Only ever seen for ourselves
			prim = PURPLE_STATUS_OFFLINE;
			break;
		case STEAM_PERSONA_ONLINE:
			prim = PURPLE_STATUS_AVAILABLE;
			break;
		case STEAM_PERSONA_BUSY:
			prim = PURPLE_STATUS_UNAVAILABLE;
			break;
		case STEAM_PERSONA_AWAY:
			prim = PURPLE_STATUS_AWAY;
			break;
		case STEAM_PERSONA_SNOOZE:
			prim = PURPLE_STATUS_EXTENDED_AWAY;
			break;
		case STEAM_PERSONA_LOOKING_TO_TRADE:
			return "trade";
		case STEAM_PERSONA_LOOKING_TO_PLAY:
			return "play";
	}
	status_id = purple_primitive_get_id_from_type(prim);
	return status_id;
}

/* The persona state to publish for our current purple status and idleness */
static SteamPersonaState
steam_current_persona_state(SteamAccount *sa)
{
	PurpleStatus *status = purple_account_get_active_status(sa->account);
	const gchar *status_id = NULL;
	PurpleStatusPrimitive prim = PURPLE_STATUS_AVAILABLE;
	SteamPersonaState state;

	if (status != NULL) {
		status_id = purple_status_get_id(status);
		prim = purple_status_type_get_primitive(purple_status_get_type(status));
	}

	if (purple_strequal(status_id, "trade")) {
		state = STEAM_PERSONA_LOOKING_TO_TRADE;
	} else if (purple_strequal(status_id, "play")) {
		state = STEAM_PERSONA_LOOKING_TO_PLAY;
	} else {
		switch(prim)
		{
			case PURPLE_STATUS_INVISIBLE:
				state = STEAM_PERSONA_INVISIBLE;
				break;
			case PURPLE_STATUS_UNAVAILABLE:
				state = STEAM_PERSONA_BUSY;
				break;
			case PURPLE_STATUS_AWAY:
				state = STEAM_PERSONA_AWAY;
				break;
			case PURPLE_STATUS_EXTENDED_AWAY:
				state = STEAM_PERSONA_SNOOZE;
				break;
			default:
				state = STEAM_PERSONA_ONLINE;
				break;
		}
	}

	// Steam used to mark idle web/mobile users as away automatically
	if (sa->idletime > 0 && (state == STEAM_PERSONA_ONLINE ||
			state == STEAM_PERSONA_LOOKING_TO_TRADE || state == STEAM_PERSONA_LOOKING_TO_PLAY)) {
		state = STEAM_PERSONA_AWAY;
	}

	return state;
}

static void
steam_apply_persona_state(SteamAccount *sa)
{
	if (sa->cm == NULL || !steam_cm_is_logged_on(sa->cm))
		return;

	steam_cm_set_persona_state(sa->cm, steam_current_persona_state(sa), NULL);
}

static const gchar *
steam_id_to_str(guint64 steamid, gchar *buf)
{
	g_snprintf(buf, STEAM_ID_STR_LEN, "%" G_GUINT64_FORMAT, steamid);
	return buf;
}

/* Parses a SteamID64 buddy name. Returns 0 unless it is exactly 17 digits
 * naming an individual account. */
static guint64
steam_str_to_id(const gchar *who)
{
	const gchar *p;
	guint64 steamid;

	if (who == NULL || strlen(who) != 17)
		return 0;
	for (p = who; *p; p++) {
		if (!g_ascii_isdigit(*p))
			return 0;
	}

	steamid = g_ascii_strtoull(who, NULL, 10);
	if (!steam_cm_steamid_is_individual(steamid))
		return 0;
	return steamid;
}

static gchar *
steam_text_to_html(const gchar *text)
{
	gchar *salvaged, *escaped, *html;

	// Server-supplied: make sure purple only ever sees valid UTF-8
	salvaged = purple_utf8_salvage(text ? text : "");
	escaped = purple_markup_escape_text(salvaged, -1);
	html = purple_strreplace(escaped, "\n", "<br>");
	g_free(escaped);
	g_free(salvaged);

	return html;
}

/* Remembers the first live message shown for `who` since the last logon,
 * so offline history fetched afterwards does not show it (or anything newer)
 * a second time. */
static void
steam_note_live_message(SteamAccount *sa, const gchar *who, guint32 timestamp)
{
	if (timestamp == 0 || g_hash_table_contains(sa->live_message_since, who))
		return;
	g_hash_table_replace(sa->live_message_since, g_strdup(who), GUINT_TO_POINTER(timestamp));
}

static void
steam_update_last_message_timestamp(SteamAccount *sa, guint32 timestamp)
{
	if (timestamp > sa->last_message_timestamp) {
		sa->last_message_timestamp = timestamp;
		purple_account_set_int(sa->account, "last_message_timestamp", (int) timestamp);
	}
}

/* Shows a message we sent (from this or another client) in the conversation */
static void
steam_write_sent_message(SteamAccount *sa, const gchar *who, const gchar *html,
		PurpleMessageFlags flags, time_t timestamp)
{
	PurpleConversation *conv;

	conv = purple_find_conversation_with_account(PURPLE_CONV_TYPE_IM, who, sa->account);
	if (conv == NULL)
	{
		conv = purple_conversation_new(PURPLE_CONV_TYPE_IM, sa->account, who);
	}
	purple_conversation_write(conv, purple_account_get_username(sa->account), html,
			PURPLE_MESSAGE_SEND | flags, timestamp);
}

static PurpleGroup *
steam_get_buddy_group(void)
{
	PurpleGroup *group = purple_find_group(STEAM_GROUP_NAME);

	if (!group)
	{
		group = purple_group_new(STEAM_GROUP_NAME);
		purple_blist_add_group(group, NULL);
	}
	return group;
}

static SteamBuddy *
steam_buddy_get_or_create(SteamAccount *sa, PurpleBuddy *buddy)
{
	SteamBuddy *sbuddy = buddy->proto_data;

	if (sbuddy == NULL)
	{
		sbuddy = g_new0(SteamBuddy, 1);
		sbuddy->sa = sa;
		sbuddy->buddy = buddy;
		sbuddy->steamid = g_strdup(purple_buddy_get_name(buddy));
		sbuddy->relationship = STEAM_RELATIONSHIP_FRIEND;
		buddy->proto_data = sbuddy;
	}
	return sbuddy;
}

static void
steam_buddy_free(PurpleBuddy *buddy)
{
	SteamBuddy *sbuddy = buddy->proto_data;
	if (sbuddy != NULL)
	{
		buddy->proto_data = NULL;

		g_free(sbuddy->steamid);
		g_free(sbuddy->personaname);
		g_free(sbuddy->nickname);
		g_free(sbuddy->avatar);
		g_free(sbuddy->gameid);
		g_free(sbuddy->gameextrainfo);
		g_free(sbuddy->gameserversteamid);
		g_free(sbuddy->lobbysteamid);
		g_free(sbuddy->gameserverip);

		g_free(sbuddy);
	}
}

/* Adds a buddy for `who` if missing. Returns the buddy. */
static PurpleBuddy *
steam_ensure_buddy(SteamAccount *sa, const gchar *who, gboolean *added)
{
	PurpleBuddy *buddy = purple_find_buddy(sa->account, who);
	const gchar *nickname;

	if (added)
		*added = FALSE;
	if (buddy == NULL)
	{
		buddy = purple_buddy_new(sa->account, who, NULL);
		purple_blist_add_buddy(buddy, NULL, steam_get_buddy_group(), NULL);
		if (added)
			*added = TRUE;

		nickname = g_hash_table_lookup(sa->nicknames, who);
		if (nickname && *nickname)
		{
			SteamBuddy *sbuddy = steam_buddy_get_or_create(sa, buddy);
			g_free(sbuddy->nickname);
			sbuddy->nickname = g_strdup(nickname);
			purple_serv_got_private_alias(sa->pc, who, nickname);
		}
	}
	steam_buddy_get_or_create(sa, buddy);
	return buddy;
}

/* A SendMessage in flight */
typedef struct {
	SteamAccount *sa;
	gchar *who;
} SteamSendContext;

/******************************************************************************/
/* Buddy icons */
/******************************************************************************/

typedef struct {
	SteamAccount *sa;
	gchar *who;
	gchar *hash;
	PurpleUtilFetchUrlData *url_data;
} SteamIconFetch;

static void
steam_icon_fetch_free(SteamIconFetch *fetch)
{
	g_free(fetch->who);
	g_free(fetch->hash);
	g_free(fetch);
}

static void steam_icon_queue_process(SteamAccount *sa);

static void
steam_get_icon_cb(PurpleUtilFetchUrlData *url_data, gpointer user_data, const gchar *url_text, gsize len, const gchar *error_message)
{
	SteamIconFetch *fetch = user_data;
	SteamAccount *sa = fetch->sa;

	sa->icon_fetches = g_slist_remove(sa->icon_fetches, fetch);

	if (error_message != NULL || url_text == NULL || len == 0)
	{
		purple_debug_warning("steam", "could not fetch buddy icon for %s: %s\n",
				fetch->who, error_message ? error_message : "empty response");
	} else if (purple_find_buddy(sa->account, fetch->who) != NULL)
	{
		purple_buddy_icons_set_for_user(sa->account, fetch->who, g_memdup2(url_text, len), len, fetch->hash);
	}

	steam_icon_fetch_free(fetch);
	steam_icon_queue_process(sa);
}

static void
steam_icon_queue_process(SteamAccount *sa)
{
	// Only allow a few simultaneous downloads
	while (g_slist_length(sa->icon_fetches) < STEAM_MAX_ICON_DOWNLOADS &&
			!g_queue_is_empty(sa->icon_queue))
	{
		SteamIconFetch *fetch = g_queue_pop_head(sa->icon_queue);
		PurpleBuddy *buddy = purple_find_buddy(sa->account, fetch->who);
		SteamBuddy *sbuddy = buddy ? buddy->proto_data : NULL;
		PurpleUtilFetchUrlData *url_data;
		const gchar *old_avatar;
		gchar *url;

		// Skip if the buddy went away or its avatar changed again since
		if (sbuddy == NULL || !purple_strequal(sbuddy->avatar, fetch->hash))
		{
			steam_icon_fetch_free(fetch);
			continue;
		}
		old_avatar = purple_buddy_icons_get_checksum_for_user(buddy);
		if (purple_strequal(old_avatar, fetch->hash))
		{
			steam_icon_fetch_free(fetch);
			continue;
		}

		purple_debug_info("steam", "getting new buddy icon for %s\n", fetch->who);

		url = g_strdup_printf(STEAM_AVATAR_URL, fetch->hash);
#if PURPLE_VERSION_CHECK(3, 0, 0)
		url_data = purple_util_fetch_url_request(sa->account, url, TRUE, NULL, FALSE, NULL, FALSE, -1, steam_get_icon_cb, fetch);
#else
		url_data = purple_util_fetch_url_request(url, TRUE, NULL, FALSE, NULL, FALSE, steam_get_icon_cb, fetch);
#endif
		g_free(url);

		if (url_data == NULL)
		{
			// The fetch failed immediately: steam_get_icon_cb already ran
			// and freed `fetch`, so it must not be touched here
			continue;
		}
		fetch->url_data = url_data;
		sa->icon_fetches = g_slist_prepend(sa->icon_fetches, fetch);
	}
}

static void
steam_get_icon(SteamAccount *sa, PurpleBuddy *buddy)
{
	SteamBuddy *sbuddy;
	SteamIconFetch *fetch;
	const gchar *old_avatar;

	if (!buddy || !buddy->proto_data)
		return;
	sbuddy = buddy->proto_data;
	if (!sbuddy->avatar || !*sbuddy->avatar)
		return;

	old_avatar = purple_buddy_icons_get_checksum_for_user(buddy);
	if (purple_strequal(old_avatar, sbuddy->avatar))
		return;

	fetch = g_new0(SteamIconFetch, 1);
	fetch->sa = sa;
	fetch->who = g_strdup(purple_buddy_get_name(buddy));
	fetch->hash = g_strdup(sbuddy->avatar);
	g_queue_push_tail(sa->icon_queue, fetch);

	steam_icon_queue_process(sa);
}

/******************************************************************************/
/* Game names */
/******************************************************************************/

/* The CM only sends game_name for non-Steam games, so names of Steam apps
 * are looked up once per session from the store API. */

typedef struct {
	SteamAccount *sa;
	guint32 appid;
	PurpleUtilFetchUrlData *url_data;
} SteamAppFetch;

static void
steam_got_app_name_cb(PurpleUtilFetchUrlData *url_data, gpointer user_data, const gchar *url_text, gsize len, const gchar *error_message)
{
	SteamAppFetch *fetch = user_data;
	SteamAccount *sa = fetch->sa;
	gchar *name = NULL;
	JsonParser *parser;
	GSList *buddies, *l;

	sa->app_fetches = g_slist_remove(sa->app_fetches, fetch);

	if (error_message == NULL && url_text != NULL && len > 0)
	{
		parser = json_parser_new();
		if (json_parser_load_from_data(parser, url_text, len, NULL))
		{
			JsonNode *root = json_parser_get_root(parser);
			gchar appid_str[STEAM_ID_STR_LEN];

			g_snprintf(appid_str, sizeof(appid_str), "%u", fetch->appid);
			if (root != NULL && JSON_NODE_HOLDS_OBJECT(root))
			{
				JsonObject *app = json_object_get_object_member(json_node_get_object(root), appid_str);
				JsonObject *data = app ? json_object_get_object_member(app, "data") : NULL;
				const gchar *app_name = data ? json_object_get_string_member(data, "name") : NULL;

				if (app_name && *app_name)
					name = purple_utf8_salvage(app_name);
			}
		}
		g_object_unref(parser);
	}

	if (name == NULL)
	{
		purple_debug_info("steam", "no name found for app %u\n", fetch->appid);
		g_free(fetch);
		return;
	}

	g_hash_table_replace(sa->app_names, GUINT_TO_POINTER(fetch->appid), g_strdup(name));

	buddies = purple_find_buddies(sa->account, NULL);
	for (l = buddies; l; l = l->next)
	{
		PurpleBuddy *buddy = l->data;
		SteamBuddy *sbuddy = buddy->proto_data;

		if (sbuddy && sbuddy->game_app_id == fetch->appid && !sbuddy->gameextrainfo)
		{
			sbuddy->gameextrainfo = g_strdup(name);
			steam_buddy_update_status(sa, sbuddy);
		}
	}
	g_slist_free(buddies);

	g_free(name);
	g_free(fetch);
}

/* Returns the cached name for `appid` (or NULL) and starts a lookup if needed */
static const gchar *
steam_get_app_name(SteamAccount *sa, guint32 appid)
{
	const gchar *name;
	SteamAppFetch *fetch;
	PurpleUtilFetchUrlData *url_data;
	gchar *url;

	if (appid == 0)
		return NULL;

	if (g_hash_table_lookup_extended(sa->app_names, GUINT_TO_POINTER(appid), NULL, (gpointer *) &name))
		return (name && *name) ? name : NULL;

	// Remember that we tried, even if the lookup fails
	g_hash_table_replace(sa->app_names, GUINT_TO_POINTER(appid), g_strdup(""));

	fetch = g_new0(SteamAppFetch, 1);
	fetch->sa = sa;
	fetch->appid = appid;

	url = g_strdup_printf(STEAM_APPDETAILS_URL, appid);
#if PURPLE_VERSION_CHECK(3, 0, 0)
	url_data = purple_util_fetch_url_request(sa->account, url, TRUE, NULL, FALSE, NULL, FALSE, -1, steam_got_app_name_cb, fetch);
#else
	url_data = purple_util_fetch_url_request(url, TRUE, NULL, FALSE, NULL, FALSE, steam_got_app_name_cb, fetch);
#endif
	g_free(url);

	if (url_data == NULL)
	{
		// The fetch failed immediately: steam_got_app_name_cb already ran
		// and freed `fetch`
		return NULL;
	}
	fetch->url_data = url_data;
	sa->app_fetches = g_slist_prepend(sa->app_fetches, fetch);

	return NULL;
}

/******************************************************************************/
/* Friend requests */
/******************************************************************************/

typedef struct {
	PurpleAccount *account;
	gchar *steamid;
	gpointer ui_handle;        /* from purple_account_request_authorization, may be NULL */
} SteamFriendRequest;

static void
steam_friend_request_free(SteamFriendRequest *req)
{
	g_free(req->steamid);
	g_free(req);
}

static SteamAccount *
steam_account_for_request(PurpleAccount *account)
{
	PurpleConnection *pc = purple_account_get_connection(account);

	if (pc == NULL || pc->proto_data == NULL)
		return NULL;
	return pc->proto_data;
}

static void
steam_friend_request_accept_cb(gpointer user_data)
{
	SteamFriendRequest *req = user_data;
	SteamAccount *sa = steam_account_for_request(req->account);
	guint64 steamid = g_ascii_strtoull(req->steamid, NULL, 10);

	if (sa)
		sa->auth_requests = g_slist_remove(sa->auth_requests, req);

	if (sa && sa->cm && steamid)
	{
		steam_cm_add_friend(sa->cm, steamid);
	}

	steam_friend_request_free(req);
}

static void
steam_friend_request_reject_cb(gpointer user_data)
{
	SteamFriendRequest *req = user_data;
	SteamAccount *sa = steam_account_for_request(req->account);
	guint64 steamid = g_ascii_strtoull(req->steamid, NULL, 10);

	if (sa)
		sa->auth_requests = g_slist_remove(sa->auth_requests, req);

	if (sa && sa->cm && steamid)
	{
		steam_cm_remove_friend(sa->cm, steamid);
	}

	steam_friend_request_free(req);
}

static void
steam_friend_request_show(SteamAccount *sa, const gchar *who, const gchar *personaname)
{
	SteamFriendRequest *req;
	gpointer ui_handle;

	g_hash_table_replace(sa->friend_requests, g_strdup(who), GINT_TO_POINTER(STEAM_FRIEND_REQUEST_SHOWN));

	req = g_new0(SteamFriendRequest, 1);
	req->account = sa->account;
	req->steamid = g_strdup(who);
	// Tracked until answered; steam_close frees unanswered ones
	sa->auth_requests = g_slist_prepend(sa->auth_requests, req);

	ui_handle = purple_account_request_authorization(
		sa->account, who, NULL, (personaname && *personaname) ? personaname : NULL,
		NULL, TRUE,
		steam_friend_request_accept_cb, steam_friend_request_reject_cb, req);

	if (!g_slist_find(sa->auth_requests, req))
		return; // Answered synchronously; the callback freed it

	if (ui_handle == NULL)
	{
		// No UI to ask: neither callback will ever run
		sa->auth_requests = g_slist_remove(sa->auth_requests, req);
		steam_friend_request_free(req);
		return;
	}
	req->ui_handle = ui_handle;
}

static gboolean
steam_friend_request_flush(gpointer user_data)
{
	SteamAccount *sa = user_data;
	GHashTableIter iter;
	gpointer key, value;
	GSList *waiting = NULL, *l;

	sa->friend_request_timeout = 0;

	// Show any requests whose name never arrived
	g_hash_table_iter_init(&iter, sa->friend_requests);
	while (g_hash_table_iter_next(&iter, &key, &value))
	{
		if (GPOINTER_TO_INT(value) == STEAM_FRIEND_REQUEST_WAITING_NAME)
			waiting = g_slist_prepend(waiting, g_strdup(key));
	}
	for (l = waiting; l; l = l->next)
	{
		steam_friend_request_show(sa, l->data, NULL);
	}
	g_slist_free_full(waiting, g_free);

	return FALSE;
}

static void
steam_friend_request_received(SteamAccount *sa, guint64 steamid, const gchar *who)
{
	if (g_hash_table_lookup(sa->friend_requests, who))
		return; // Already asked this session

	// Find out the name of the buddy before we display the auth request
	g_hash_table_replace(sa->friend_requests, g_strdup(who), GINT_TO_POINTER(STEAM_FRIEND_REQUEST_WAITING_NAME));
	steam_cm_request_friend_data(sa->cm, &steamid, 1);

	if (!sa->friend_request_timeout)
		sa->friend_request_timeout = purple_timeout_add_seconds(STEAM_FRIEND_REQUEST_DELAY, steam_friend_request_flush, sa);
}

/******************************************************************************/
/* CM callbacks */
/******************************************************************************/

static void
steam_got_history_cb(SteamCM *cm, guint64 friend_steamid,
		const SteamCMHistoryMessage *messages, guint n,
		gboolean more_available, gpointer user_data)
{
	SteamAccount *sa = user_data;
	gchar who[STEAM_ID_STR_LEN];
	guint32 own_accountid = steam_cm_steamid_to_accountid(sa->steamid);
	guint32 newest = 0;
	guint32 live_since;
	guint i;

	steam_id_to_str(friend_steamid, who);
	// Messages from here on were already shown as they arrived live
	live_since = GPOINTER_TO_UINT(g_hash_table_lookup(sa->live_message_since, who));

	// Steam returns newest first
	for (i = n; i > 0; i--)
	{
		const SteamCMHistoryMessage *message = &messages[i - 1];
		gchar *html;

		if (message->timestamp <= sa->history_since)
			continue;
		if (live_since && message->timestamp >= live_since)
			continue;

		html = steam_text_to_html(message->message);
		if (message->accountid == own_accountid) {
			steam_write_sent_message(sa, who, html, PURPLE_MESSAGE_DELAYED, message->timestamp);
		} else {
			serv_got_im(sa->pc, who, html, PURPLE_MESSAGE_RECV | PURPLE_MESSAGE_DELAYED, message->timestamp);
		}
		g_free(html);

		newest = MAX(newest, message->timestamp);
	}

	steam_update_last_message_timestamp(sa, newest);
}

static void
steam_got_message_sessions_cb(SteamCM *cm, const SteamCMMessageSession *sessions,
		guint n, guint32 server_timestamp, gpointer user_data)
{
	SteamAccount *sa = user_data;
	guint i;

	for (i = 0; i < n; i++)
	{
		const SteamCMMessageSession *session = &sessions[i];

		if (session->last_message > sa->history_since)
		{
			steam_cm_get_recent_messages(cm, steam_cm_accountid_to_steamid(session->accountid_friend),
					sa->history_since, STEAM_HISTORY_COUNT, steam_got_history_cb, sa);
		}
	}
}

static void
steam_cm_logged_on_cb(SteamCM *cm, guint64 steamid, gpointer user_data)
{
	SteamAccount *sa = user_data;
	PurpleConnection *pc = sa->pc;
	gchar steamid_str[STEAM_ID_STR_LEN];

	purple_debug_info("steam", "logged on to Steam as %" G_GUINT64_FORMAT "\n", steamid);

	if (steamid != 0 && steamid != sa->steamid)
	{
		sa->steamid = steamid;
		purple_account_set_string(sa->account, "steamid", steam_id_to_str(steamid, steamid_str));
	}

	if (purple_connection_get_state(pc) != PURPLE_CONNECTED)
	{
		purple_connection_set_state(pc, PURPLE_CONNECTED);
	}

	// The token worked: a later token rejection (after an internal
	// reconnect) may fall back to the password once more
	sa->password_login_tried = FALSE;

	// Live messages before this logon are older than history_since below
	g_hash_table_remove_all(sa->live_message_since);

	// Needed for friends to see us and for persona updates to flow; also
	// re-applied after every internal reconnect.
	steam_apply_persona_state(sa);

	if (purple_account_get_bool(sa->account, "download_offline_history", TRUE))
	{
		sa->history_since = sa->last_message_timestamp;
		if (sa->history_since > 0)
		{
			steam_cm_get_active_message_sessions(cm, sa->history_since, steam_got_message_sessions_cb, sa);
		} else {
			// First login: don't dump old history, but pick up from now on.
			// Back off a little in case the local clock runs fast; the
			// live-message record keeps the overlap from showing twice.
			time_t now = time(NULL);

			steam_update_last_message_timestamp(sa,
					(guint32) (now > STEAM_FIRST_LOGIN_HISTORY_MARGIN ? now - STEAM_FIRST_LOGIN_HISTORY_MARGIN : now));
		}
	}
}

static gboolean
steam_free_dead_cm(gpointer user_data)
{
	SteamAccount *sa = user_data;

	sa->dead_cm_timeout = 0;
	if (sa->dead_cm)
	{
		steam_cm_free(sa->dead_cm);
		sa->dead_cm = NULL;
	}
	return FALSE;
}

static void
steam_cm_logon_failed_cb(SteamCM *cm, SteamEResult eresult, const gchar *message, gpointer user_data)
{
	SteamAccount *sa = user_data;
	const gchar *reason = (message && *message) ? message : steam_eresult_to_string(eresult);

	purple_debug_error("steam", "CM logon failed: %d %s\n", eresult, reason ? reason : "");

	// The refresh token is no good any more
	steam_account_set_refresh_token(sa, NULL);

	if (!sa->password_login_tried)
	{
		// Don't free the CM from inside its own callback
		if (cm == sa->cm)
		{
			if (sa->dead_cm)
				steam_cm_free(sa->dead_cm);
			sa->dead_cm = sa->cm;
			sa->cm = NULL;
			if (!sa->dead_cm_timeout)
				sa->dead_cm_timeout = purple_timeout_add(0, steam_free_dead_cm, sa);
		}
		steam_start_password_login(sa);
		return;
	}

	purple_connection_error(sa->pc, PURPLE_CONNECTION_ERROR_AUTHENTICATION_FAILED,
			reason ? reason : _("Steam rejected the login"));
}

static void
steam_cm_disconnected_cb(SteamCM *cm, SteamEResult eresult, const gchar *message, gpointer user_data)
{
	SteamAccount *sa = user_data;
	const gchar *reason = (message && *message) ? message : steam_eresult_to_string(eresult);
	PurpleConnectionError error = PURPLE_CONNECTION_ERROR_NETWORK_ERROR;

	purple_debug_error("steam", "disconnected from Steam: %d %s\n", eresult, reason ? reason : "");

	if (eresult == STEAM_ERESULT_LOGGED_IN_ELSEWHERE || eresult == STEAM_ERESULT_LOGON_SESSION_REPLACED)
	{
		error = PURPLE_CONNECTION_ERROR_NAME_IN_USE;
	}
	else if (eresult == STEAM_ERESULT_RATE_LIMIT_EXCEEDED || eresult == STEAM_ERESULT_ACCOUNT_LOGIN_DENIED_THROTTLE ||
			eresult == STEAM_ERESULT_BANNED || eresult == STEAM_ERESULT_ACCOUNT_DISABLED)
	{
		// Not a network error: don't let purple auto-reconnect into more rate limiting
		error = PURPLE_CONNECTION_ERROR_OTHER_ERROR;
	}

	purple_connection_error(sa->pc, error, reason ? reason : _("Disconnected from Steam"));
}

static void
steam_cm_account_info_cb(SteamCM *cm, const gchar *persona_name, gpointer user_data)
{
	SteamAccount *sa = user_data;

	if (persona_name && *persona_name)
	{
		gchar *name = purple_utf8_salvage(persona_name);
		purple_connection_set_display_name(sa->pc, name);
		g_free(name);
	}
}

static void
steam_cm_friends_list_cb(SteamCM *cm, gboolean incremental,
		const SteamCMFriend *friends, guint n_friends, gpointer user_data)
{
	SteamAccount *sa = user_data;
	GHashTable *keep = NULL;
	GArray *new_buddies = g_array_new(FALSE, FALSE, sizeof(guint64));
	guint i;

	if (!incremental)
		keep = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

	for (i = 0; i < n_friends; i++)
	{
		const SteamCMFriend *friend = &friends[i];
		gchar who[STEAM_ID_STR_LEN];
		PurpleBuddy *buddy;
		SteamBuddy *sbuddy;

		if (!steam_cm_steamid_is_individual(friend->steamid))
			continue;
		steam_id_to_str(friend->steamid, who);

		switch (friend->relationship)
		{
			case STEAM_RELATIONSHIP_FRIEND:
			case STEAM_RELATIONSHIP_REQUEST_INITIATOR:
				buddy = steam_ensure_buddy(sa, who, NULL);
				sbuddy = buddy->proto_data;
				sbuddy->relationship = friend->relationship;
				g_hash_table_remove(sa->friend_requests, who);
				if (keep)
					g_hash_table_replace(keep, g_strdup(who), NULL);
				// The CM layer requests data for the full list after logon;
				// later additions (or accepted requests) need it explicitly
				if (incremental)
					g_array_append_val(new_buddies, friend->steamid);
				if (friend->relationship == STEAM_RELATIONSHIP_REQUEST_INITIATOR)
				{
					// Friend request sent, not accepted yet: show offline
					purple_prpl_got_user_status(sa->account, who, steam_personastate_to_statustype(STEAM_PERSONA_OFFLINE), NULL);
				}
				break;

			case STEAM_RELATIONSHIP_REQUEST_RECIPIENT:
				steam_friend_request_received(sa, friend->steamid, who);
				break;

			case STEAM_RELATIONSHIP_NONE:
				g_hash_table_remove(sa->friend_requests, who);
				buddy = purple_find_buddy(sa->account, who);
				if (buddy)
					purple_blist_remove_buddy(buddy);
				break;

			default:
				purple_debug_info("steam", "ignoring relationship %d for %s\n", friend->relationship, who);
				break;
		}
	}

	// A full list replaces what we have: remove buddies that are no longer
	// friends (or pending requests from us). An empty full list is more
	// likely a glitch than a real empty friends list, so leave things be.
	if (keep && n_friends > 0)
	{
		GSList *buddies = purple_find_buddies(sa->account, NULL), *l;

		for (l = buddies; l; l = l->next)
		{
			PurpleBuddy *buddy = l->data;

			if (!g_hash_table_contains(keep, purple_buddy_get_name(buddy)))
			{
				purple_debug_info("steam", "removing %s, no longer a friend\n", purple_buddy_get_name(buddy));
				purple_blist_remove_buddy(buddy);
			}
		}
		g_slist_free(buddies);
	}

	if (new_buddies->len > 0)
		steam_cm_request_friend_data(cm, (const guint64 *) new_buddies->data, new_buddies->len);

	g_array_free(new_buddies, TRUE);
	if (keep)
		g_hash_table_destroy(keep);
}

/* Changes our saved status message while we are in game (account option) */
static void
steam_own_game_changed(SteamAccount *sa, const gchar *gameid, const gchar *game_name)
{
	const gchar *last_gameid = purple_account_get_string(sa->account, "current_gameid", NULL);
	PurpleSavedStatus *current_status;

	if (last_gameid && !*last_gameid)
		last_gameid = NULL;
	if (purple_strequal(last_gameid, gameid))
		return;

	// We changed our in-game status
	current_status = purple_savedstatus_get_current();
	purple_account_set_string(sa->account, "current_gameid", gameid);

	if (!last_gameid) {
		//Starting a game
		purple_account_set_string(sa->account, "last_status_message", purple_savedstatus_get_message(current_status));
	}
	if (!gameid) {
		//Finishing game
		purple_savedstatus_set_message(current_status, purple_account_get_string(sa->account, "last_status_message", NULL));
		purple_account_set_string(sa->account, "last_status_message", NULL);
	} else {
		//Starting or changing a game
		gchar *new_message;
		if (game_name && *game_name)
			new_message = g_markup_printf_escaped("In game %s", game_name);
		else
			new_message = g_strdup("In game");
		purple_savedstatus_set_message(current_status, new_message);
		g_free(new_message);
	}
	purple_savedstatus_activate(current_status);
}

/* Pushes a SteamBuddy's state and game into purple */
static void
steam_buddy_update_status(SteamAccount *sa, SteamBuddy *sbuddy)
{
	const gchar *steamid = sbuddy->steamid;

	if (sbuddy->personastate_known)
	{
		const gchar *status_id = steam_personastate_to_statustype(sbuddy->personastate);

		if (core_is_haze) {
			if (sbuddy->gameextrainfo && *(sbuddy->gameextrainfo)) {
				gchar *message = g_markup_printf_escaped("In game %s", sbuddy->gameextrainfo);
				purple_prpl_got_user_status(sa->account, steamid, status_id, "message", message, NULL);
				g_free(message);
			} else {
				purple_prpl_got_user_status(sa->account, steamid, status_id, "message", NULL, NULL);
			}
		} else {
			purple_prpl_got_user_status(sa->account, steamid, status_id, NULL);
		}
	}

	if (sbuddy->gameextrainfo && *(sbuddy->gameextrainfo)) {
		/* Rich presence for UIs that show it (pidgin4): "game" is the name,
		 * "game_app_id" the Steam app id (unset for non-Steam games) */
		gchar *app_id = sbuddy->game_app_id ? g_strdup_printf("%u", sbuddy->game_app_id) : NULL;

		if (app_id)
			purple_prpl_got_user_status(sa->account, steamid, "ingame", "game", sbuddy->gameextrainfo,
			                            "game_app_id", app_id, NULL);
		else
			purple_prpl_got_user_status(sa->account, steamid, "ingame", "game", sbuddy->gameextrainfo, NULL);
		g_free(app_id);
	} else {
		purple_prpl_got_user_status_deactive(sa->account, steamid, "ingame");
	}
}

static void
steam_buddy_set_game(SteamAccount *sa, SteamBuddy *sbuddy, const SteamCMPersona *persona)
{
	g_free(sbuddy->gameid); sbuddy->gameid = NULL;
	g_free(sbuddy->gameextrainfo); sbuddy->gameextrainfo = NULL;
	g_free(sbuddy->gameserversteamid); sbuddy->gameserversteamid = NULL;
	g_free(sbuddy->lobbysteamid); sbuddy->lobbysteamid = NULL;
	g_free(sbuddy->gameserverip); sbuddy->gameserverip = NULL;
	sbuddy->game_app_id = 0;

	if (persona == NULL || (persona->gameid == 0 && persona->game_app_id == 0))
		return;

	sbuddy->game_app_id = persona->game_app_id;
	sbuddy->gameid = g_strdup_printf("%" G_GUINT64_FORMAT,
			persona->gameid ? persona->gameid : (guint64) persona->game_app_id);

	if (persona->game_name && *persona->game_name) {
		sbuddy->gameextrainfo = purple_utf8_salvage(persona->game_name);
	} else {
		sbuddy->gameextrainfo = g_strdup(steam_get_app_name(sa, persona->game_app_id));
	}

	if (persona->game_server_steamid)
		sbuddy->gameserversteamid = g_strdup_printf("%" G_GUINT64_FORMAT, persona->game_server_steamid);
	if (persona->game_lobby_id)
		sbuddy->lobbysteamid = g_strdup_printf("%" G_GUINT64_FORMAT, persona->game_lobby_id);
	if (persona->game_server_ip)
	{
		guint32 ip = persona->game_server_ip;
		sbuddy->gameserverip = g_strdup_printf("%u.%u.%u.%u:%u",
				(ip >> 24) & 0xff, (ip >> 16) & 0xff, (ip >> 8) & 0xff, ip & 0xff,
				(guint) persona->game_server_port);
	}
}

static void
steam_cm_persona_state_cb(SteamCM *cm, const SteamCMPersona *persona, gpointer user_data)
{
	SteamAccount *sa = user_data;
	gchar who[STEAM_ID_STR_LEN];
	PurpleBuddy *buddy;
	SteamBuddy *sbuddy;
	gchar *player_name = NULL;

	steam_id_to_str(persona->steamid, who);

	// Server-supplied: make sure purple only ever sees valid UTF-8
	if (persona->player_name && *persona->player_name)
		player_name = purple_utf8_salvage(persona->player_name);

	if (persona->steamid == sa->steamid)
	{
		// Ourselves: pick up our name and (optionally) our game
		if (player_name)
			purple_connection_set_display_name(sa->pc, player_name);

		if (persona->has_game && purple_account_get_bool(sa->account, "change_status_to_game", FALSE))
		{
			gchar *gameid = NULL;
			gchar *salvaged_game_name = NULL;
			const gchar *game_name = persona->game_name;

			if (persona->gameid || persona->game_app_id)
				gameid = g_strdup_printf("%" G_GUINT64_FORMAT,
						persona->gameid ? persona->gameid : (guint64) persona->game_app_id);
			if (game_name && *game_name)
				game_name = salvaged_game_name = purple_utf8_salvage(game_name);
			else
				game_name = steam_get_app_name(sa, persona->game_app_id);

			steam_own_game_changed(sa, gameid, game_name);
			g_free(salvaged_game_name);
			g_free(gameid);
		}
		g_free(player_name);
		return;
	}

	if (GPOINTER_TO_INT(g_hash_table_lookup(sa->friend_requests, who)) == STEAM_FRIEND_REQUEST_WAITING_NAME)
	{
		steam_friend_request_show(sa, who, player_name);
	}

	buddy = purple_find_buddy(sa->account, who);
	if (!buddy)
	{
		g_free(player_name);
		return;
	}
	sbuddy = steam_buddy_get_or_create(sa, buddy);

	if (player_name)
	{
		g_free(sbuddy->personaname);
		sbuddy->personaname = player_name;
		player_name = NULL;
		serv_got_alias(sa->pc, who, sbuddy->personaname);
	}

	if (persona->avatar_hash != NULL)
	{
		const gchar *hash = persona->avatar_hash;
		const gchar *p;

		// Empty or all-zero hash means the default avatar
		for (p = hash; *p == '0'; p++);
		if (*p == '\0')
			hash = STEAM_DEFAULT_AVATAR_HASH;

		if (!purple_strequal(sbuddy->avatar, hash))
		{
			g_free(sbuddy->avatar);
			sbuddy->avatar = g_strdup(hash);
		}
		steam_get_icon(sa, buddy);
	}

	// Not (yet) a friend, e.g. a friend request we sent: keep it offline
	// until accepted, but still take the name and avatar (above)
	if (sbuddy->relationship != STEAM_RELATIONSHIP_FRIEND)
		return;

	if (persona->has_state)
	{
		sbuddy->personastate = persona->state;
		sbuddy->personastateflags = persona->state_flags;
		sbuddy->personastate_known = TRUE;

		if (persona->state == STEAM_PERSONA_OFFLINE)
			steam_buddy_set_game(sa, sbuddy, NULL);
	}

	if (persona->has_game)
		steam_buddy_set_game(sa, sbuddy, persona);

	if (persona->last_logoff)
		sbuddy->lastlogoff = persona->last_logoff;

	if (persona->has_state || persona->has_game)
		steam_buddy_update_status(sa, sbuddy);
}

static void
steam_cm_message_cb(SteamCM *cm, const SteamCMMessage *message, gpointer user_data)
{
	SteamAccount *sa = user_data;
	gchar who[STEAM_ID_STR_LEN];
	time_t timestamp = message->timestamp ? (time_t) message->timestamp : time(NULL);

	steam_id_to_str(message->from_steamid, who);

	switch (message->type)
	{
		case STEAM_CHAT_ENTRY_CHAT_MSG:
		{
			gchar *html;

			// Also covers the echo of a message sent from here, which is shown
			steam_note_live_message(sa, who, (guint32) timestamp);

			if (message->local_echo)
			{
				// Drop the echo of something we sent from here
				gchar *key = g_strconcat(who, "\n", message->message ? message->message : "", NULL);
				gboolean ours = g_hash_table_remove(sa->sent_messages_hash, key);
				g_free(key);
				if (ours)
					break;
			}

			html = steam_text_to_html(message->message);
			if (message->local_echo) {
				steam_write_sent_message(sa, who, html, 0, timestamp);
			} else {
				serv_got_typing_stopped(sa->pc, who);
				serv_got_im(sa->pc, who, html, PURPLE_MESSAGE_RECV, timestamp);
			}
			g_free(html);

			steam_update_last_message_timestamp(sa, message->timestamp);
			break;
		}

		case STEAM_CHAT_ENTRY_TYPING:
			if (!message->local_echo)
				serv_got_typing(sa->pc, who, 20, PURPLE_TYPING);
			break;

		case STEAM_CHAT_ENTRY_LEFT_CONVERSATION:
		{
			PurpleConversation *conv;

			if (message->local_echo)
				break;
			serv_got_typing_stopped(sa->pc, who);

			conv = purple_find_conversation_with_account(PURPLE_CONV_TYPE_IM, who, sa->account);
			if (conv != NULL)
			{
				PurpleBuddy *buddy = purple_find_buddy(sa->account, who);
				const gchar *alias = buddy ? purple_buddy_get_alias(buddy) : NULL;
				gchar *has_left_msg = g_strdup_printf("%s has left the conversation", alias ? alias : "User");
				gchar *has_left_html = purple_markup_escape_text(has_left_msg, -1);

				purple_conversation_write(conv, "", has_left_html, PURPLE_MESSAGE_SYSTEM, time(NULL));
				g_free(has_left_html);
				g_free(has_left_msg);
			}
			break;
		}

		default:
			purple_debug_info("steam", "ignoring chat entry type %d from %s\n", message->type, who);
			break;
	}
}

static void
steam_set_nickname(SteamAccount *sa, const gchar *who, const gchar *nickname)
{
	PurpleBuddy *buddy = purple_find_buddy(sa->account, who);
	gchar *old_nickname = g_strdup(g_hash_table_lookup(sa->nicknames, who));
	gchar *salvaged = NULL;

	if (nickname && !*nickname)
		nickname = NULL;
	// Server-supplied: make sure purple only ever sees valid UTF-8
	if (nickname)
		nickname = salvaged = purple_utf8_salvage(nickname);

	if (nickname)
		g_hash_table_replace(sa->nicknames, g_strdup(who), g_strdup(nickname));
	else
		g_hash_table_remove(sa->nicknames, who);

	if (buddy)
	{
		SteamBuddy *sbuddy = steam_buddy_get_or_create(sa, buddy);

		g_free(sbuddy->nickname);
		sbuddy->nickname = g_strdup(nickname);

		if (nickname) {
			purple_serv_got_private_alias(sa->pc, who, nickname);
		} else if (old_nickname && purple_strequal(purple_buddy_get_local_buddy_alias(buddy), old_nickname)) {
			// Only drop the local alias if it was the Steam nickname
			purple_serv_got_private_alias(sa->pc, who, NULL);
		}
	}

	g_free(old_nickname);
	g_free(salvaged);
}

static void
steam_cm_nicknames_cb(SteamCM *cm, const SteamCMNickname *nicknames, guint n,
		gboolean incremental, gboolean removal, gpointer user_data)
{
	SteamAccount *sa = user_data;
	GHashTable *seen = NULL;
	guint i;

	if (!incremental && !removal)
		seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

	for (i = 0; i < n; i++)
	{
		gchar who[STEAM_ID_STR_LEN];

		steam_id_to_str(nicknames[i].steamid, who);
		steam_set_nickname(sa, who, removal ? NULL : nicknames[i].nickname);
		if (seen)
			g_hash_table_replace(seen, g_strdup(who), NULL);
	}

	if (seen)
	{
		// Full list: forget nicknames that are not in it
		GList *known = g_hash_table_get_keys(sa->nicknames), *l;

		for (l = known; l; l = l->next)
		{
			if (!g_hash_table_contains(seen, l->data))
			{
				gchar *who = g_strdup(l->data);
				steam_set_nickname(sa, who, NULL);
				g_free(who);
			}
		}
		g_list_free(known);
		g_hash_table_destroy(seen);
	}
}

static void
steam_cm_add_friend_response_cb(SteamCM *cm, SteamEResult eresult, guint64 steamid,
		const gchar *persona_name, gpointer user_data)
{
	SteamAccount *sa = user_data;
	gchar who[STEAM_ID_STR_LEN];
	PurpleBuddy *buddy;
	gchar *primary;
	const gchar *reason;

	if (eresult == STEAM_ERESULT_OK)
		return; // The friends list update follows

	steam_id_to_str(steamid, who);
	reason = steam_eresult_to_string(eresult);

	if (persona_name && *persona_name) {
		gchar *name = purple_utf8_salvage(persona_name);
		primary = g_strdup_printf(_("Could not add %s as a friend"), name);
		g_free(name);
	} else {
		primary = g_strdup_printf(_("Could not add %s as a friend"), who);
	}
	purple_notify_error(sa->pc, _("Add friend"), primary, reason);
	g_free(primary);

	// Don't keep a buddy that is not (going to be) a friend
	buddy = steamid ? purple_find_buddy(sa->account, who) : NULL;
	if (buddy)
	{
		SteamBuddy *sbuddy = buddy->proto_data;
		if (!sbuddy || sbuddy->relationship != STEAM_RELATIONSHIP_FRIEND)
			purple_blist_remove_buddy(buddy);
	}
}

static const SteamCMCallbacks steam_cm_callbacks = {
	steam_cm_logged_on_cb,
	steam_cm_logon_failed_cb,
	steam_cm_disconnected_cb,
	steam_cm_account_info_cb,
	steam_cm_friends_list_cb,
	steam_cm_persona_state_cb,
	steam_cm_message_cb,
	steam_cm_nicknames_cb,
	steam_cm_add_friend_response_cb,
};

/******************************************************************************/
/* Auth callbacks */
/******************************************************************************/

static void
steam_close_guard_request(SteamAccount *sa)
{
	if (sa->guard_request)
	{
		gpointer handle = sa->guard_request;
		sa->guard_request = NULL;
		purple_request_close((PurpleRequestType) sa->guard_request_type, handle);
	}
}

static void
steam_guard_code_ok_cb(gpointer user_data, const gchar *code)
{
	SteamAccount *sa = user_data;

	sa->guard_request = NULL;

	if (sa->auth == NULL)
		return;

	if (code && *code) {
		gchar *trimmed = g_strstrip(g_strdup(code));

		purple_connection_update_progress(sa->pc, _("Verifying Steam Guard code"), 2, 4);
		steam_auth_submit_guard_code(sa->auth, (SteamGuardType) sa->guard_type, trimmed);
		g_free(trimmed);
	} else {
		purple_connection_error(sa->pc, PURPLE_CONNECTION_ERROR_AUTHENTICATION_FAILED,
				_("Steam Guard cancelled"));
	}
}

static void
steam_guard_cancel_cb(gpointer user_data)
{
	SteamAccount *sa = user_data;

	sa->guard_request = NULL;
	purple_connection_error(sa->pc, PURPLE_CONNECTION_ERROR_AUTHENTICATION_FAILED,
			_("Steam Guard cancelled"));
}

static void
steam_guard_action_cancel_cb(gpointer user_data, int action)
{
	steam_guard_cancel_cb(user_data);
}

static void
steam_auth_guard_required_cb(SteamAuth *auth, const SteamGuardType *allowed,
		guint n_allowed, const gchar *email_domain, gpointer user_data)
{
	SteamAccount *sa = user_data;
	gboolean device_code = FALSE, email_code = FALSE;
	gboolean device_confirmation = FALSE, email_confirmation = FALSE;
	gchar *primary, *secondary = NULL;
	guint i;

	for (i = 0; i < n_allowed; i++)
	{
		switch (allowed[i])
		{
			case STEAM_GUARD_DEVICE_CODE: device_code = TRUE; break;
			case STEAM_GUARD_EMAIL_CODE: email_code = TRUE; break;
			case STEAM_GUARD_DEVICE_CONFIRMATION: device_confirmation = TRUE; break;
			case STEAM_GUARD_EMAIL_CONFIRMATION: email_confirmation = TRUE; break;
			default: break;
		}
	}

	purple_debug_info("steam", "Steam Guard required (device code %d, email code %d, device confirmation %d, email confirmation %d)\n",
			device_code, email_code, device_confirmation, email_confirmation);

	steam_close_guard_request(sa);
	purple_connection_update_progress(sa->pc, _("Waiting for Steam Guard"), 2, 4);

	if (device_code || email_code)
	{
		if (device_code) {
			sa->guard_type = STEAM_GUARD_DEVICE_CODE;
			primary = g_strdup(_("Enter the Steam Guard code from your mobile app"));
		} else if (email_domain && *email_domain) {
			sa->guard_type = STEAM_GUARD_EMAIL_CODE;
			primary = g_strdup_printf(_("Enter the Steam Guard code sent to your email (%s)"), email_domain);
		} else {
			sa->guard_type = STEAM_GUARD_EMAIL_CODE;
			primary = g_strdup(_("Enter the Steam Guard code sent to your email"));
		}

		if (device_confirmation) {
			secondary = g_strdup(_("You can also approve this login in the Steam mobile app."));
		} else if (email_confirmation) {
			secondary = g_strdup(_("You can also approve this login using the link in the email Steam sent you."));
		}

		sa->guard_request_type = PURPLE_REQUEST_INPUT;
		sa->guard_request = purple_request_input(sa->pc, _("Steam Guard"), primary, secondary,
					NULL, FALSE, FALSE, NULL,
					_("OK"), G_CALLBACK(steam_guard_code_ok_cb),
					_("Cancel"), G_CALLBACK(steam_guard_cancel_cb),
					sa->account, NULL, NULL, sa);

		g_free(primary);
		g_free(secondary);
		return;
	}

	if (device_confirmation || email_confirmation)
	{
		// Nothing to type in; steam_auth keeps polling until approved
		sa->guard_type = STEAM_GUARD_UNKNOWN;
		sa->guard_request_type = PURPLE_REQUEST_ACTION;
		sa->guard_request = purple_request_action(sa->pc, _("Steam Guard"),
					device_confirmation ? _("Approve this login in the Steam mobile app")
					                    : _("Approve this login using the link in the email Steam sent you"),
					_("The login will continue automatically once it is approved."),
					0, sa->account, NULL, NULL, sa, 1,
					_("Cancel"), G_CALLBACK(steam_guard_action_cancel_cb));
		return;
	}

	purple_connection_error(sa->pc, PURPLE_CONNECTION_ERROR_AUTHENTICATION_IMPOSSIBLE,
			_("Steam asked for a kind of Steam Guard confirmation this plugin does not support"));
}

static void
steam_auth_success_cb(SteamAuth *auth, const gchar *refresh_token,
		const gchar *access_token, guint64 steamid,
		const gchar *account_name, gpointer user_data)
{
	SteamAccount *sa = user_data;
	gchar steamid_str[STEAM_ID_STR_LEN];

	purple_debug_info("steam", "password login succeeded for %" G_GUINT64_FORMAT "\n", steamid);

	// The SteamAuth object is freed after this callback returns
	sa->auth = NULL;
	steam_close_guard_request(sa);

	if (!refresh_token || !*refresh_token || !steamid)
	{
		purple_connection_error(sa->pc, PURPLE_CONNECTION_ERROR_NETWORK_ERROR,
				_("Steam did not return a login token"));
		return;
	}

	steam_account_set_refresh_token(sa, refresh_token);
	sa->steamid = steamid;
	purple_account_set_string(sa->account, "steamid", steam_id_to_str(steamid, steamid_str));

	steam_start_cm(sa, refresh_token);
}

static void
steam_auth_error_cb(SteamAuth *auth, SteamEResult eresult, const gchar *message,
		gboolean bad_credentials, gpointer user_data)
{
	SteamAccount *sa = user_data;
	const gchar *reason = (message && *message) ? message : steam_eresult_to_string(eresult);
	PurpleConnectionError error;

	purple_debug_error("steam", "password login failed: %d %s\n", eresult, reason ? reason : "");

	// The SteamAuth object is freed after this callback returns
	sa->auth = NULL;
	steam_close_guard_request(sa);

	if (bad_credentials) {
		error = PURPLE_CONNECTION_ERROR_AUTHENTICATION_FAILED;
	} else if (eresult == STEAM_ERESULT_INVALID_NAME || eresult == STEAM_ERESULT_ACCOUNT_NOT_FOUND) {
		error = PURPLE_CONNECTION_ERROR_INVALID_USERNAME;
	} else if (eresult == STEAM_ERESULT_TIMEOUT) {
		// Nobody approved the Steam Guard prompt. Auto-reconnecting would
		// just send the user another login prompt/email every time.
		error = PURPLE_CONNECTION_ERROR_OTHER_ERROR;
		reason = _("Steam Guard confirmation timed out");
	} else if (eresult == STEAM_ERESULT_RATE_LIMIT_EXCEEDED || eresult == STEAM_ERESULT_ACCOUNT_LOGIN_DENIED_THROTTLE ||
			eresult == STEAM_ERESULT_BANNED || eresult == STEAM_ERESULT_ACCESS_DENIED) {
		// Don't let purple auto-reconnect into more rate limiting
		error = PURPLE_CONNECTION_ERROR_OTHER_ERROR;
	} else {
		error = PURPLE_CONNECTION_ERROR_NETWORK_ERROR;
	}

	purple_connection_error(sa->pc, error, reason ? reason : _("Steam login failed"));
}

static const SteamAuthCallbacks steam_auth_callbacks = {
	steam_auth_guard_required_cb,
	steam_auth_success_cb,
	steam_auth_error_cb,
};

/******************************************************************************/
/* Login / close */
/******************************************************************************/

static void
steam_start_cm(SteamAccount *sa, const gchar *refresh_token)
{
	purple_connection_update_progress(sa->pc, _("Connecting to Steam"), 3, 4);

	if (sa->cm == NULL)
		sa->cm = steam_cm_new(sa, &steam_cm_callbacks, sa);
	if (sa->cm == NULL)
	{
		purple_connection_error(sa->pc, PURPLE_CONNECTION_ERROR_OTHER_ERROR,
				_("Unable to start a Steam session"));
		return;
	}

	steam_cm_connect(sa->cm, refresh_token, sa->steamid);
}

static void
steam_login_with_password(SteamAccount *sa, const gchar *password)
{
	const gchar *username = purple_account_get_username(sa->account);

	if (!username || !*username)
	{
		purple_connection_error(sa->pc, PURPLE_CONNECTION_ERROR_INVALID_USERNAME,
				_("No Steam account name set"));
		return;
	}

	purple_connection_update_progress(sa->pc, _("Authenticating"), 1, 4);

	sa->auth = steam_auth_login_password(sa, username, password, &steam_auth_callbacks, sa);
	if (sa->auth == NULL)
	{
		purple_connection_error(sa->pc, PURPLE_CONNECTION_ERROR_NETWORK_ERROR,
				_("Unable to start the Steam login"));
	}
}

static void
steam_request_password_ok_cb(PurpleAccount *account, PurpleRequestFields *fields)
{
	SteamAccount *sa = steam_account_for_request(account);
	const gchar *entry = purple_request_fields_get_string(fields, "password");
	gboolean remember = purple_request_fields_get_bool(fields, "remember");

	if (sa == NULL)
		return;

	if (!entry || !*entry)
	{
		purple_connection_error(sa->pc, PURPLE_CONNECTION_ERROR_AUTHENTICATION_FAILED,
				_("Password is required to sign on."));
		return;
	}

	if (remember)
		purple_account_set_remember_password(account, TRUE);
	purple_account_set_password(account, entry);

	steam_login_with_password(sa, entry);
}

static void
steam_request_password_cancel_cb(PurpleAccount *account, PurpleRequestFields *fields)
{
	SteamAccount *sa = steam_account_for_request(account);

	if (sa == NULL)
		return;

	purple_connection_error(sa->pc, PURPLE_CONNECTION_ERROR_AUTHENTICATION_FAILED,
			_("Password is required to sign on."));
}

static void
steam_start_password_login(SteamAccount *sa)
{
	const gchar *password = purple_connection_get_password(sa->pc);

	sa->password_login_tried = TRUE;

	if (!password || !*password)
		password = purple_account_get_password(sa->account);

	if (!password || !*password)
	{
		purple_account_request_password(sa->account,
				G_CALLBACK(steam_request_password_ok_cb),
				G_CALLBACK(steam_request_password_cancel_cb),
				sa->account);
		return;
	}

	steam_login_with_password(sa, password);
}

/* Uses the stored refresh token if it is still good, else logs in with the
 * password. */
static void
steam_login_with_refresh_token(SteamAccount *sa, const gchar *refresh_token)
{
	guint64 steamid = 0;
	gint64 expiry = 0;
	gboolean is_client_token = FALSE;
	gchar steamid_str[STEAM_ID_STR_LEN];

	if (refresh_token && *refresh_token)
	{
		if (steam_auth_jwt_decode(refresh_token, &steamid, &expiry, &is_client_token) &&
				steamid != 0 && is_client_token &&
				expiry > (gint64) time(NULL) + STEAM_TOKEN_EXPIRY_MARGIN)
		{
			gchar *token = g_strdup(refresh_token);

			purple_debug_info("steam", "using stored refresh token\n");
			sa->steamid = steamid;
			purple_account_set_string(sa->account, "steamid", steam_id_to_str(steamid, steamid_str));
			steam_start_cm(sa, token);
			g_free(token);
			return;
		}

		purple_debug_info("steam", "stored refresh token is expired or unusable\n");
		steam_account_set_refresh_token(sa, NULL);
	}

	steam_start_password_login(sa);
}

#ifdef G_OS_UNIX

typedef struct {
	SteamAccount *sa;      /* NULL once the connection is closed */
} SteamKeyringLookup;

static void

#ifdef USE_GNOME_KEYRING

steam_keyring_got_token(GnomeKeyringResult res, const gchar *refresh_token, gpointer user_data) {
	SteamKeyringLookup *lookup = user_data;

#else // !USE_GNOME_KEYRING

steam_keyring_got_token(GObject *source_object, GAsyncResult *res, gpointer user_data) {
	SteamKeyringLookup *lookup = user_data;
	gchar *refresh_token = my_secret_password_lookup_finish(res, NULL);

#endif

	SteamAccount *sa = lookup->sa;

	if (sa != NULL)
	{
		sa->keyring_lookup = NULL;

		g_free(sa->cached_refresh_token);
		sa->cached_refresh_token = (refresh_token && *refresh_token) ? g_strdup(refresh_token) : NULL;

		steam_login_with_refresh_token(sa, sa->cached_refresh_token);
	}

#ifndef USE_GNOME_KEYRING
	// Wipes the secret; optional, older libsecret builds may not export it
	if (my_secret_password_free)
		my_secret_password_free(refresh_token);
	else
		g_free(refresh_token);
	g_free(lookup);
#endif
}

#endif

static void
steam_login(PurpleAccount *account)
{
	PurpleConnection *pc = purple_account_get_connection(account);
	SteamAccount *sa = g_new0(SteamAccount, 1);

	pc->proto_data = sa;

	sa->account = account;
	sa->pc = pc;

	sa->cookie_table = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	sa->hostname_ip_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	sa->sent_messages_hash = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	sa->waiting_conns = g_queue_new();

	sa->typing_sent = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	sa->friend_requests = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	sa->nicknames = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	sa->icon_queue = g_queue_new();
	sa->app_names = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
	sa->live_message_since = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

	sa->last_message_timestamp = (guint32) purple_account_get_int(account, "last_message_timestamp", 0);
	sa->steamid = g_ascii_strtoull(purple_account_get_string(account, "steamid", "0"), NULL, 10);

	if (!purple_ssl_is_supported()) {
		purple_connection_error (pc,
								PURPLE_CONNECTION_ERROR_NO_SSL_SUPPORT,
								_("Server requires TLS/SSL for login.  No TLS/SSL support found."));
		return;
	}

	purple_connection_set_state(pc, PURPLE_CONNECTING);
	purple_connection_update_progress(pc, _("Connecting"), 0, 4);

	steam_account_remove_legacy_settings(sa);

#ifdef G_OS_UNIX
	if(core_is_haze) {
		SteamKeyringLookup *lookup = g_new0(SteamKeyringLookup, 1);
		lookup->sa = sa;
		sa->keyring_lookup = lookup;
#ifdef USE_GNOME_KEYRING
		my_gnome_keyring_find_password(my_GKNP, //GNOME_KEYRING_NETWORK_PASSWORD,
										steam_keyring_got_token, lookup, g_free,
										"user",		account->username,
										"server",	STEAM_KEYRING_SERVER,
										"protocol",	STEAM_KEYRING_PROTOCOL,
										"domain",	STEAM_KEYRING_DOMAIN,
										NULL);
#else // !USE_GNOME_KEYRING
		my_secret_password_lookup(my_SSCN, //SECRET_SCHEMA_COMPAT_NETWORK
								  NULL, steam_keyring_got_token, lookup,
								  "user",     account->username,
								  "server",   STEAM_KEYRING_SERVER,
								  "protocol", STEAM_KEYRING_PROTOCOL,
								  "domain",   STEAM_KEYRING_DOMAIN,
								  NULL);
#endif
		return;
	}
#endif

	steam_login_with_refresh_token(sa, steam_account_get_refresh_token(sa));
}

static void steam_close(PurpleConnection *pc)
{
	SteamAccount *sa;
	GSList *buddies, *l;

	g_return_if_fail(pc != NULL);
	g_return_if_fail(pc->proto_data != NULL);

	sa = pc->proto_data;

#ifdef G_OS_UNIX
	if (sa->keyring_lookup) {
		// The lookup callback still fires; make it a no-op
		((SteamKeyringLookup *) sa->keyring_lookup)->sa = NULL;
		sa->keyring_lookup = NULL;
	}
#endif

	steam_close_guard_request(sa);

	if (sa->friend_request_timeout)
		purple_timeout_remove(sa->friend_request_timeout);
	if (sa->dead_cm_timeout)
		purple_timeout_remove(sa->dead_cm_timeout);

	if (sa->auth) {
		steam_auth_cancel(sa->auth);
		sa->auth = NULL;
	}
	if (sa->cm) {
		// Sends ClientLogOff if still connected
		steam_cm_free(sa->cm);
		sa->cm = NULL;
	}
	if (sa->dead_cm) {
		steam_cm_free(sa->dead_cm);
		sa->dead_cm = NULL;
	}

	while (sa->icon_fetches != NULL) {
		SteamIconFetch *fetch = sa->icon_fetches->data;
		sa->icon_fetches = g_slist_remove(sa->icon_fetches, fetch);
		purple_util_fetch_url_cancel(fetch->url_data);
		steam_icon_fetch_free(fetch);
	}
	while (!g_queue_is_empty(sa->icon_queue))
		steam_icon_fetch_free(g_queue_pop_head(sa->icon_queue));
	g_queue_free(sa->icon_queue);

	while (sa->app_fetches != NULL) {
		SteamAppFetch *fetch = sa->app_fetches->data;
		sa->app_fetches = g_slist_remove(sa->app_fetches, fetch);
		purple_util_fetch_url_cancel(fetch->url_data);
		g_free(fetch);
	}

	while (sa->auth_requests != NULL) {
		// Unanswered friend request dialogs; closing them runs neither callback
		SteamFriendRequest *req = sa->auth_requests->data;
		sa->auth_requests = g_slist_remove(sa->auth_requests, req);
		if (req->ui_handle)
			purple_account_request_close(req->ui_handle);
		steam_friend_request_free(req);
	}

	while (sa->pending_sends != NULL) {
		// steam_cm_free() dropped their callbacks
		SteamSendContext *ctx = sa->pending_sends->data;
		sa->pending_sends = g_slist_remove(sa->pending_sends, ctx);
		g_free(ctx->who);
		g_free(ctx);
	}

	if (sa->last_message_timestamp > 0)
		purple_account_set_int(sa->account, "last_message_timestamp", (int) sa->last_message_timestamp);

	// Anything the HTTPS helper still has in flight (auth, CM directory)
	purple_debug_info("steam", "destroying %d waiting connections\n",
					  g_queue_get_length(sa->waiting_conns));

	steam_connection_cancel_requeues(sa); /* pending 429 retry timers */
	while (!g_queue_is_empty(sa->waiting_conns))
		steam_connection_destroy(g_queue_pop_tail(sa->waiting_conns));
	g_queue_free(sa->waiting_conns);

	purple_debug_info("steam", "destroying %d incomplete connections\n",
			g_slist_length(sa->conns));

	while (sa->conns != NULL)
		steam_connection_destroy(sa->conns->data);

	while (sa->dns_queries != NULL) {
		PurpleDnsQueryData *dns_query = sa->dns_queries->data;
		purple_debug_info("steam", "canceling dns query for %s\n",
					purple_dnsquery_get_host(dns_query));
		sa->dns_queries = g_slist_remove(sa->dns_queries, dns_query);
		purple_dnsquery_destroy(dns_query);
	}

	// Buddies outlive the connection; their SteamBuddy must not
	buddies = purple_find_buddies(sa->account, NULL);
	for (l = buddies; l; l = l->next)
		steam_buddy_free(l->data);
	g_slist_free(buddies);

	g_hash_table_destroy(sa->sent_messages_hash);
	g_hash_table_destroy(sa->cookie_table);
	g_hash_table_destroy(sa->hostname_ip_cache);
	g_hash_table_destroy(sa->typing_sent);
	g_hash_table_destroy(sa->friend_requests);
	g_hash_table_destroy(sa->nicknames);
	g_hash_table_destroy(sa->app_names);
	g_hash_table_destroy(sa->live_message_since);

	g_free(sa->cached_refresh_token);
	g_free(sa);
	pc->proto_data = NULL;
}

/******************************************************************************/
/* PRPL functions */
/******************************************************************************/

static const char *steam_list_icon(PurpleAccount *account, PurpleBuddy *buddy)
{
	return "steam";
}

static gchar *steam_status_text(PurpleBuddy *buddy)
{
	SteamBuddy *sbuddy = buddy->proto_data;

	if (sbuddy && sbuddy->relationship == STEAM_RELATIONSHIP_REQUEST_INITIATOR)
	{
		return g_strdup("Friend request sent");
	}

	if (sbuddy && sbuddy->gameextrainfo)
	{
		if (sbuddy->game_app_id)
		{
			return g_markup_printf_escaped("In game %s", sbuddy->gameextrainfo);
		} else {
			return g_markup_printf_escaped("In non-Steam game %s", sbuddy->gameextrainfo);
		}
	}

	return NULL;
}

static void
steam_tooltip_text(PurpleBuddy *buddy, PurpleNotifyUserInfo *user_info, gboolean full)
{
	SteamBuddy *sbuddy = buddy->proto_data;

	if (sbuddy)
	{
		if (sbuddy->personaname)
			purple_notify_user_info_add_pair_plaintext(user_info, "Name", sbuddy->personaname);
		if (sbuddy->nickname)
			purple_notify_user_info_add_pair_plaintext(user_info, "Nickname", sbuddy->nickname);
		if (sbuddy->relationship == STEAM_RELATIONSHIP_REQUEST_INITIATOR)
			purple_notify_user_info_add_pair_plaintext(user_info, "Friend request", "Sent, not accepted yet");
		if (sbuddy->gameextrainfo)
		{
			if (sbuddy->game_app_id)
			{
				purple_notify_user_info_add_pair_plaintext(user_info, "In game", sbuddy->gameextrainfo);
			} else {
				purple_notify_user_info_add_pair_plaintext(user_info, "In non-Steam game", sbuddy->gameextrainfo);
			}
		}
	}
}

static const gchar *
steam_list_emblem(PurpleBuddy *buddy)
{
	SteamBuddy *sbuddy = buddy->proto_data;

	if (sbuddy)
	{
		if (sbuddy->gameid || sbuddy->personastateflags & STEAM_PERSONA_FLAG_IN_JOINABLE_GAME)
		{
			return "game";
		}
		if (sbuddy->personastateflags & STEAM_PERSONA_FLAG_CLIENT_WEB)
		{
			//Web
			return "external";
		}
		if (sbuddy->personastateflags & STEAM_PERSONA_FLAG_CLIENT_MOBILE)
		{
			//Steam mobile, also Pidgin
			return "mobile";
		}
		if (sbuddy->personastateflags & STEAM_PERSONA_FLAG_CLIENT_TENFOOT)
		{
			//Big Picture mode
			return "hiptop";
		}
	}

	return NULL;
}

static GList *
steam_status_types(PurpleAccount *account)
{
	GList *types = NULL;
	PurpleStatusType *status;

	purple_debug_info("steam", "status_types\n");

	status = purple_status_type_new_full(PURPLE_STATUS_AVAILABLE, NULL, "Online", TRUE, TRUE, FALSE);
	types = g_list_append(types, status);
	status = purple_status_type_new_full(PURPLE_STATUS_OFFLINE, NULL, "Offline", TRUE, TRUE, FALSE);
	types = g_list_append(types, status);
	status = purple_status_type_new_full(PURPLE_STATUS_UNAVAILABLE, NULL, "Busy", TRUE, TRUE, FALSE);
	types = g_list_append(types, status);
	status = purple_status_type_new_full(PURPLE_STATUS_AWAY, NULL, "Away", TRUE, TRUE, FALSE);
	types = g_list_append(types, status);
	status = purple_status_type_new_full(PURPLE_STATUS_EXTENDED_AWAY, NULL, "Snoozing", TRUE, TRUE, FALSE);
	types = g_list_append(types, status);
	status = purple_status_type_new_full(PURPLE_STATUS_INVISIBLE, NULL, "Invisible", TRUE, TRUE, FALSE);
	types = g_list_append(types, status);

	status = purple_status_type_new_full(PURPLE_STATUS_AVAILABLE, "trade", "Looking to Trade", TRUE, TRUE, FALSE);
	types = g_list_append(types, status);
	status = purple_status_type_new_full(PURPLE_STATUS_AVAILABLE, "play", "Looking to Play", TRUE, TRUE, FALSE);
	types = g_list_append(types, status);

	if (core_is_haze) {
		// Telepathy-Haze only displays status_text if the status has a "message" attr
		GList *iter;
		for(iter = types; iter; iter = iter->next) {
			purple_status_type_add_attr(iter->data, "message", "Game Title", purple_value_new(PURPLE_TYPE_STRING));
		}
	}

	// Independent, unsettable status for being in-game.
	// "game" is the game's name, "game_app_id" its Steam app id (a decimal
	// string; unset for non-Steam games). UIs that don't know the attributes
	// ignore them.
	status = purple_status_type_new_with_attrs(PURPLE_STATUS_TUNE,
			"ingame", NULL, FALSE, FALSE, TRUE,
			"game", "Game Title", purple_value_new(PURPLE_TYPE_STRING),
			"game_app_id", "Game App ID", purple_value_new(PURPLE_TYPE_STRING),
			NULL);
	types = g_list_append(types, status);

	return types;
}

static unsigned int
steam_send_typing(PurpleConnection *pc, const gchar *name, PurpleTypingState state)
{
	SteamAccount *sa = pc->proto_data;
	guint64 steamid;
	time_t now, last;

	if (state != PURPLE_TYPING || sa == NULL || sa->cm == NULL || !steam_cm_is_logged_on(sa->cm))
		return 0;

	steamid = steam_str_to_id(name);
	if (!steamid)
		return 0;

	// Steam shows typing for a while; don't repeat it more than every few seconds
	now = time(NULL);
	last = (time_t) GPOINTER_TO_SIZE(g_hash_table_lookup(sa->typing_sent, name));
	if (last && now - last < STEAM_TYPING_INTERVAL)
		return STEAM_TYPING_INTERVAL;

	g_hash_table_replace(sa->typing_sent, g_strdup(name), GSIZE_TO_POINTER((gsize) now));
	steam_cm_send_message(sa->cm, steamid, STEAM_CHAT_ENTRY_TYPING, "", NULL, NULL);

	return STEAM_TYPING_INTERVAL;
}

static void
steam_set_status(PurpleAccount *account, PurpleStatus *status)
{
	PurpleConnection *pc = purple_account_get_connection(account);
	SteamAccount *sa = pc ? pc->proto_data : NULL;

	if (sa == NULL || !purple_status_is_active(status))
		return;

	// Offline is handled by purple disconnecting us
	if (purple_status_type_get_primitive(purple_status_get_type(status)) == PURPLE_STATUS_OFFLINE)
		return;

	steam_apply_persona_state(sa);
}

static void
steam_set_idle(PurpleConnection *pc, int time)
{
	SteamAccount *sa = pc->proto_data;
	gboolean was_idle, is_idle;

	if (sa == NULL)
		return;

	was_idle = sa->idletime > 0;
	sa->idletime = time;
	is_idle = sa->idletime > 0;

	if (was_idle != is_idle)
		steam_apply_persona_state(sa);
}

static gboolean
steam_sent_message_expired(gpointer key, gpointer value, gpointer user_data)
{
	time_t now = *(time_t *) user_data;

	return now - (time_t) GPOINTER_TO_SIZE(value) > 60;
}

static void
steam_send_im_cb(SteamCM *cm, SteamEResult eresult, guint32 server_timestamp, gpointer user_data)
{
	SteamSendContext *ctx = user_data;
	SteamAccount *sa = ctx->sa;
	gchar *who = ctx->who;

	sa->pending_sends = g_slist_remove(sa->pending_sends, ctx);

	if (eresult == STEAM_ERESULT_OK) {
		// Shown in the conversation when it was sent
		steam_note_live_message(sa, who, server_timestamp);
		steam_update_last_message_timestamp(sa, server_timestamp);
	} else {
		const gchar *reason = steam_eresult_to_string(eresult);
		gchar *error = g_strdup_printf(_("Message could not be sent: %s"), reason ? reason : _("unknown error"));

		purple_debug_error("steam", "sending message to %s failed: %d\n", who, eresult);
		if (!purple_conv_present_error(who, sa->account, error))
		{
			purple_notify_error(sa->pc, _("Steam"), _("Message could not be sent"), reason);
		}
		g_free(error);
	}

	g_free(who);
	g_free(ctx);
}

static gint
steam_send_im(PurpleConnection *pc, const gchar *who, const gchar *msg,
		PurpleMessageFlags flags)
{
	SteamAccount *sa = pc->proto_data;
	guint64 steamid = steam_str_to_id(who);
	gchar *text;
	SteamSendContext *ctx;
	time_t now = time(NULL);

	if (sa == NULL || sa->cm == NULL || !steam_cm_is_logged_on(sa->cm))
		return -ENOTCONN;
	if (!steamid)
		return -EINVAL;

	// Also decodes entities, so no extra purple_unescape_html() is needed
	text = purple_markup_strip_html(msg);

	g_hash_table_foreach_remove(sa->sent_messages_hash, steam_sent_message_expired, &now);
	g_hash_table_replace(sa->sent_messages_hash, g_strconcat(who, "\n", text, NULL), GSIZE_TO_POINTER((gsize) now));

	ctx = g_new0(SteamSendContext, 1);
	ctx->sa = sa;
	ctx->who = g_strdup(who);
	sa->pending_sends = g_slist_prepend(sa->pending_sends, ctx);

	steam_cm_send_message(sa->cm, steamid, STEAM_CHAT_ENTRY_CHAT_MSG, text, steam_send_im_cb, ctx);

	// Sending a message ends the typing notification
	g_hash_table_remove(sa->typing_sent, who);
	g_free(text);

	return 1;
}

static void
steam_fake_group_buddy(PurpleConnection *pc, const char *who, const char *old_group, const char *new_group)
{
	// Do nothing to stop the remove+add behaviour
}

static void
steam_fake_group_rename(PurpleConnection *pc, const char *old_name, PurpleGroup *group, GList *moved_buddies)
{
	// Do nothing to stop the remove+add behaviour
}

static void
#if PURPLE_VERSION_CHECK(3, 0, 0)
steam_add_buddy(PurpleConnection *pc, PurpleBuddy *buddy, PurpleGroup *group, const char* message)
#else
steam_add_buddy(PurpleConnection *pc, PurpleBuddy *buddy, PurpleGroup *group)
#endif
{
	SteamAccount *sa = pc->proto_data;
	guint64 steamid = steam_str_to_id(purple_buddy_get_name(buddy));
	SteamBuddy *sbuddy;

	if (!steamid)
	{
		purple_blist_remove_buddy(buddy);
		purple_notify_error(pc, "Invalid friend id", "Invalid friend id",
				"Friends must be added by their 17-digit SteamID64 (for example 76561197960287930).\n"
				"It is shown in the address of their profile page: steamcommunity.com/profiles/<SteamID64>");
		return;
	}

	if (steamid == sa->steamid)
	{
		purple_blist_remove_buddy(buddy);
		return;
	}

	sbuddy = steam_buddy_get_or_create(sa, buddy);
	if (sbuddy->relationship != STEAM_RELATIONSHIP_FRIEND || !sbuddy->personastate_known)
		sbuddy->relationship = STEAM_RELATIONSHIP_REQUEST_INITIATOR;

	if (sa->cm)
	{
		steam_cm_add_friend(sa->cm, steamid);
		steam_cm_request_friend_data(sa->cm, &steamid, 1);
	}
}

static void
steam_buddy_remove(PurpleConnection *pc, PurpleBuddy *buddy, PurpleGroup *group)
{
	SteamAccount *sa = pc->proto_data;
	guint64 steamid = steam_str_to_id(purple_buddy_get_name(buddy));

	if (sa && sa->cm && steamid)
	{
		steam_cm_remove_friend(sa->cm, steamid);
	}
}

/* SteamIDs are plain numbers; other names (the account name) are
 * case-insensitive. */
static const char *
steam_normalize(const PurpleAccount *account, const char *str)
{
	static gchar buf[256];
	const gchar *p;
	gsize len;

	if (str == NULL)
		return NULL;

	while (g_ascii_isspace(*str))
		str++;
	len = strlen(str);
	while (len > 0 && g_ascii_isspace(str[len - 1]))
		len--;

	for (p = str; p < str + len; p++)
	{
		if (!g_ascii_isdigit(*p))
			break;
	}
	if (len > 0 && p == str + len && len < sizeof(buf))
	{
		memcpy(buf, str, len);
		buf[len] = '\0';
		return buf;
	}

	return purple_normalize_nocase(account, str);
}

/******************************************************************************/
/* Menus */
/******************************************************************************/

static void
steam_blist_launch_game(PurpleBlistNode *node, gpointer data)
{
	PurpleBuddy *buddy;
	SteamBuddy *sbuddy;
	PurplePlugin *handle = purple_find_prpl(STEAM_PLUGIN_ID);

	if(!PURPLE_BLIST_NODE_IS_BUDDY(node))
		return;
	buddy = (PurpleBuddy *) node;
	if (!buddy)
		return;
	sbuddy = buddy->proto_data;
	if (sbuddy && sbuddy->gameid)
	{
		gchar *runurl = g_strdup_printf("steam://rungameid/%s", sbuddy->gameid);
		purple_notify_uri(handle, runurl);
		g_free(runurl);
	}
}

static gboolean
steam_buddy_has_joinable_server(SteamBuddy *sbuddy)
{
	return sbuddy->gameserverip && (!sbuddy->gameserversteamid || !g_str_equal(sbuddy->gameserversteamid, "1"));
}

static void
steam_blist_join_game(PurpleBlistNode *node, gpointer data)
{
	PurpleBuddy *buddy;
	SteamBuddy *sbuddy;
	PurplePlugin *handle = purple_find_prpl(STEAM_PLUGIN_ID);

	if(!PURPLE_BLIST_NODE_IS_BUDDY(node))
		return;
	buddy = (PurpleBuddy *) node;
	if (!buddy)
		return;
	sbuddy = buddy->proto_data;
	if (sbuddy) {
		if (steam_buddy_has_joinable_server(sbuddy))
		{
			gchar *joinurl = g_strdup_printf("steam://connect/%s", sbuddy->gameserverip);
			purple_notify_uri(handle, joinurl);
			g_free(joinurl);
		} else if (sbuddy->lobbysteamid && sbuddy->game_app_id) {
			gchar *joinurl = g_strdup_printf("steam://joinlobby/%u/%s/%s", sbuddy->game_app_id, sbuddy->lobbysteamid, sbuddy->steamid);
			purple_notify_uri(handle, joinurl);
			g_free(joinurl);
		}
	}
}

static void
steam_blist_view_profile(PurpleBlistNode *node, gpointer data)
{
	PurpleBuddy *buddy;
	PurplePlugin *handle = purple_find_prpl(STEAM_PLUGIN_ID);
	gchar *profileurl;

	if(!PURPLE_BLIST_NODE_IS_BUDDY(node))
		return;
	buddy = (PurpleBuddy *) node;
	if (!buddy)
		return;

	profileurl = g_strdup_printf(STEAM_PROFILE_URL, purple_url_encode(purple_buddy_get_name(buddy)));
	purple_notify_uri(handle, profileurl);
	g_free(profileurl);
}

static GList *
steam_node_menu(PurpleBlistNode *node)
{
	GList *m = NULL;
	PurpleMenuAction *act;
	PurpleBuddy *buddy;
	SteamBuddy *sbuddy;

	if(PURPLE_BLIST_NODE_IS_BUDDY(node))
	{
		buddy = (PurpleBuddy *)node;

		act = purple_menu_action_new("View online Profile",
				PURPLE_CALLBACK(steam_blist_view_profile),
				NULL, NULL);
		m = g_list_append(m, act);

		sbuddy = buddy->proto_data;
		if (sbuddy && sbuddy->gameid)
		{
			act = purple_menu_action_new("Launch Game",
					PURPLE_CALLBACK(steam_blist_launch_game),
					NULL, NULL);
			m = g_list_append(m, act);

			if ((sbuddy->lobbysteamid && sbuddy->game_app_id) || steam_buddy_has_joinable_server(sbuddy))
			{
				act = purple_menu_action_new("Join Game",
						PURPLE_CALLBACK(steam_blist_join_game),
						NULL, NULL);
				m = g_list_append(m, act);
			}
		}
	}
	return m;
}

/******************************************************************************/
/* Plugin functions */
/******************************************************************************/

static gboolean plugin_load(PurplePlugin *plugin)
{
	purple_debug_info("steam", "Purple core UI name: %s\n", purple_core_get_ui());

#ifdef G_OS_UNIX
	core_is_haze = g_str_equal(purple_core_get_ui(), "haze");

#ifdef USE_GNOME_KEYRING
	if (core_is_haze && gnome_keyring_lib == NULL) {
		purple_debug_info("steam", "UI Core is Telepathy-Haze, attempting to load Gnome-Keyring\n");

		gnome_keyring_lib = dlopen("libgnome-keyring.so", RTLD_NOW | RTLD_GLOBAL);
		if (!gnome_keyring_lib) {
			purple_debug_error("steam", "Could not load Gnome-Keyring library.  This plugin requires Gnome-Keyring when used with Telepathy-Haze\n");
			return FALSE;
		}

		my_gnome_keyring_store_password = (gnome_keyring_store_password_type) dlsym(gnome_keyring_lib, "gnome_keyring_store_password");
		my_gnome_keyring_delete_password = (gnome_keyring_delete_password_type) dlsym(gnome_keyring_lib, "gnome_keyring_delete_password");
		my_gnome_keyring_find_password = (gnome_keyring_find_password_type) dlsym(gnome_keyring_lib, "gnome_keyring_find_password");

		if (!my_gnome_keyring_store_password || !my_gnome_keyring_delete_password || !my_gnome_keyring_find_password) {
			dlclose(gnome_keyring_lib);
			gnome_keyring_lib = NULL;
			purple_debug_error("steam", "Could not load Gnome-Keyring functions.  This plugin requires Gnome-Keyring when used with Telepathy-Haze\n");
			return FALSE;
		}
	}

#else // !USE_GNOME_KEYRING
	if (core_is_haze && secret_lib == NULL) {
		purple_debug_info("steam", "UI Core is Telepathy-Haze, attempting to load libsecret\n");

		secret_lib = dlopen("libsecret-1.so", RTLD_NOW | RTLD_GLOBAL);
		if (!secret_lib) {
			purple_debug_error("steam", "Could not load libsecret library.  This plugin requires libsecret when used with Telepathy-Haze\n");
			return FALSE;
		}

		my_secret_password_store = (secret_password_store_type) dlsym(secret_lib, "secret_password_store");
		my_secret_password_clear = (secret_password_clear_type) dlsym(secret_lib, "secret_password_clear");
		my_secret_password_lookup = (secret_password_lookup_type) dlsym(secret_lib, "secret_password_lookup");
		my_secret_password_lookup_finish = (secret_password_lookup_finish_type) dlsym(secret_lib, "secret_password_lookup_finish");
		my_secret_password_free = (secret_password_free_type) dlsym(secret_lib, "secret_password_free");

		if (!my_secret_password_store || !my_secret_password_clear || !my_secret_password_lookup || !my_secret_password_lookup_finish) {
			dlclose(secret_lib);
			secret_lib = NULL;
			purple_debug_error("steam", "Could not load libsecret functions.  This plugin requires libsecret when used with Telepathy-Haze\n");
			return FALSE;
		}
	}

#endif // USE_GNOME_KEYRING

#endif

	return TRUE;
}

static gboolean plugin_unload(PurplePlugin *plugin)
{
#ifdef G_OS_UNIX

#ifdef USE_GNOME_KEYRING
	if (gnome_keyring_lib) {
		dlclose(gnome_keyring_lib);
		gnome_keyring_lib = NULL;
	}

#else // !USE_GNOME_KEYRING
	if (secret_lib) {
		dlclose(secret_lib);
		secret_lib = NULL;
		my_secret_password_free = NULL;
	}

#endif // USE_GNOME_KEYRING

#endif
	return TRUE;
}

static void plugin_init(PurplePlugin *plugin)
{
	PurpleAccountOption *option;
	PurplePluginInfo *info = plugin->info;
	PurplePluginProtocolInfo *prpl_info = info->extra_info;

	option = purple_account_option_bool_new(
		_("Change status when in-game"),
		"change_status_to_game", FALSE);
	prpl_info->protocol_options = g_list_append(
		prpl_info->protocol_options, option);

	option = purple_account_option_bool_new(
		_("Download offline history"),
		"download_offline_history", TRUE);
	prpl_info->protocol_options = g_list_append(
		prpl_info->protocol_options, option);
}

static PurplePluginProtocolInfo prpl_info = {
#if PURPLE_VERSION_CHECK(3, 0, 0)
	sizeof(PurplePluginProtocolInfo),	/* struct_size */
#endif

	/* options */
	0,

	NULL,                   /* user_splits */
	NULL,                   /* protocol_options */
	/* NO_BUDDY_ICONS */    /* icon_spec */
	{"png,jpeg", 0, 0, 64, 64, 0, PURPLE_ICON_SCALE_DISPLAY}, /* icon_spec */
	steam_list_icon,           /* list_icon */
	steam_list_emblem,         /* list_emblems */
	steam_status_text,         /* status_text */
	steam_tooltip_text,        /* tooltip_text */
	steam_status_types,        /* status_types */
	steam_node_menu,           /* blist_node_menu */
	NULL,                   /* chat_info */
	NULL,                   /* chat_info_defaults */
	steam_login,               /* login */
	steam_close,               /* close */
	steam_send_im,             /* send_im */
	NULL,                   /* set_info */
	steam_send_typing,         /* send_typing */
	NULL,                   /* get_info */
	steam_set_status,          /* set_status */
	steam_set_idle,            /* set_idle */
	NULL,                   /* change_passwd */
	steam_add_buddy,           /* add_buddy */
	NULL,                   /* add_buddies */
	steam_buddy_remove,        /* remove_buddy */
	NULL,                   /* remove_buddies */
	NULL,                   /* add_permit */
	NULL,                   /* add_deny */
	NULL,                   /* rem_permit */
	NULL,                   /* rem_deny */
	NULL,                   /* set_permit_deny */
	NULL,                   /* join_chat */
	NULL,                   /* reject chat invite */
	NULL,                   /* get_chat_name */
	NULL,                   /* chat_invite */
	NULL,                   /* chat_leave */
	NULL,                   /* chat_whisper */
	NULL,                   /* chat_send */
	NULL,                   /* keepalive */
	NULL,                   /* register_user */
	NULL,                   /* get_cb_info */
#if !PURPLE_VERSION_CHECK(3, 0, 0)
	NULL,                   /* get_cb_away */
#endif
	NULL,                   /* alias_buddy */
	steam_fake_group_buddy,    /* group_buddy */
	steam_fake_group_rename,   /* rename_group */
	steam_buddy_free,          /* buddy_free */
	NULL,                   /* convo_closed */
	steam_normalize,           /* normalize */
	NULL,                   /* set_buddy_icon */
	NULL,                   /* remove_group */
	NULL,                   /* get_cb_real_name */
	NULL,                   /* set_chat_topic */
	NULL,                   /* find_blist_chat */
	NULL,                   /* roomlist_get_list */
	NULL,                   /* roomlist_cancel */
	NULL,                   /* roomlist_expand_category */
	NULL,                   /* can_receive_file */
	NULL,                   /* send_file */
	NULL,                   /* new_xfer */
	NULL,                   /* offline_message */
	NULL,                   /* whiteboard_prpl_ops */
	NULL,                   /* send_raw */
	NULL,                   /* roomlist_room_serialize */
	NULL,                   /* unregister_user */
	NULL,                   /* send_attention */
	NULL,                   /* attention_types */
#if (PURPLE_MAJOR_VERSION == 2 && PURPLE_MINOR_VERSION >= 5) || PURPLE_MAJOR_VERSION > 2
#if PURPLE_MAJOR_VERSION == 2 && PURPLE_MINOR_VERSION >= 5
	sizeof(PurplePluginProtocolInfo), /* struct_size */
#endif
	NULL,                   /* get_account_text_table */
	NULL,
	NULL,
	NULL,
	NULL,
	NULL
#else
	(gpointer) sizeof(PurplePluginProtocolInfo)
#endif
};

static PurplePluginInfo info = {
	PURPLE_PLUGIN_MAGIC,
	PURPLE_MAJOR_VERSION,				/* major_version */
	PURPLE_MINOR_VERSION, 				/* minor version */
	PURPLE_PLUGIN_PROTOCOL, 			/* type */
	NULL, 						/* ui_requirement */
	0, 						/* flags */
	NULL, 						/* dependencies */
	PURPLE_PRIORITY_DEFAULT, 			/* priority */
	STEAM_PLUGIN_ID,				/* id */
	"Steam", 					/* name */
	STEAM_PLUGIN_VERSION, 			/* version */
	N_("Steam Protocol Plugin"), 		/* summary */
	N_("Steam Protocol Plugin"), 		/* description */
	"Eion Robb <eionrobb@gmail.com>", 		/* author */
	"https://github.com/EionRobb/pidgin-opensteamworks",	/* homepage */
	plugin_load, 					/* load */
	plugin_unload, 					/* unload */
	NULL, 						/* destroy */
	NULL, 						/* ui_info */
	&prpl_info, 					/* extra_info */
	NULL, 						/* prefs_info */
	NULL, 					/* actions */

							/* padding */
	NULL,
	NULL,
	NULL,
	NULL
};

PURPLE_INIT_PLUGIN(steam, plugin_init, info);
