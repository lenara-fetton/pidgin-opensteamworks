/*
 *  Steam Mobile Plugin for Pidgin
 *  Copyright (C) 2012-2016 Eion Robb
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



#ifndef STEAM_CONNECTION_H
#define STEAM_CONNECTION_H

#include "libsteam.h"

typedef void (*SteamProxyCallbackFunc)(SteamAccount *sa, JsonObject *obj, gpointer user_data);
typedef void (*SteamProxyCallbackErrorFunc)(SteamAccount *sa, const gchar *data, gssize data_len, gpointer user_data);

/*
 * Raw response callback, for binary (protobuf) bodies. When set on a
 * SteamConnection (sconn->raw_callback = ... right after steam_post_or_get),
 * it is called instead of the JSON callback with:
 *   headers   the status line and response headers, NUL-terminated
 *   body      the (gunzipped) body, body_len bytes; always followed by a NUL
 *             byte for convenience but may itself contain NULs
 * On transport failure (could not connect, timed out, connection closed
 * before any headers arrived) it is called once with headers == NULL,
 * body == NULL, body_len == 0, and -- unlike the JSON path -- the
 * PurpleConnection is NOT put into an error state; the caller decides.
 */
typedef void (*SteamProxyCallbackRawFunc)(SteamAccount *sa, const gchar *headers,
		const guint8 *body, gsize body_len, gpointer user_data);

/*
 * This is a bitmask.
 */
typedef enum
{
	STEAM_METHOD_GET  = 0x0001,
	STEAM_METHOD_POST = 0x0002,
	STEAM_METHOD_SSL  = 0x0004
} SteamMethod;

typedef struct _SteamConnection SteamConnection;
struct _SteamConnection {
	SteamAccount *sa;
	SteamMethod method;
	gchar *hostname;
	gchar *url;
	GString *request;
	SteamProxyCallbackFunc callback;
	gpointer user_data;
	char *rx_buf;
	size_t rx_len;
	PurpleProxyConnectData *connect_data;
	PurpleSslConnection *ssl_conn;
	int fd;
	guint input_watcher;
	gboolean connection_keepalive;
	time_t request_time;
	guint retry_count;
	guint timeout_watcher;
	SteamProxyCallbackErrorFunc error_callback;
	SteamProxyCallbackRawFunc raw_callback;
};

void steam_connection_destroy(SteamConnection *steamcon);
void steam_connection_close(SteamConnection *steamcon);
SteamConnection *steam_post_or_get(SteamAccount *sa, SteamMethod method,
		const gchar *host, const gchar *url, const gchar *postdata,
		SteamProxyCallbackFunc callback_func, gpointer user_data,
		gboolean keepalive);
gchar *steam_cookies_to_string(SteamAccount *sa);

/* Returns a newly allocated, whitespace-trimmed copy of the value of the
 * first header called `name` (case-insensitive) in a raw header block as
 * passed to a SteamProxyCallbackRawFunc, or NULL if absent. */
gchar *steam_connection_get_header(const gchar *headers, const gchar *name);

/* Cancels every queued or in-flight request of `sa` whose user_data is
 * `user_data`: the connections are destroyed without invoking any callback.
 * Returns the number of requests cancelled. (A request that is sitting in the
 * 1-second HTTP 429 back-off cannot be found; callers that need a hard
 * guarantee should also mark their user_data as cancelled.) */
guint steam_connection_cancel_by_user_data(SteamAccount *sa, gpointer user_data);

/* Returns the HTTP status code from the status line, or 0 if unparseable. */
guint steam_connection_get_status(const gchar *headers);

/* Cancels every pending HTTP 429 back-off requeue timer for `sa` (see
 * steam_connection_cancel_all()) and frees the duplicated SteamConnections
 * they would have requeued. Safe to call even if none are pending. */
void steam_connection_cancel_requeues(SteamAccount *sa);

/* Destroys every connection belonging to `sa`: everything in sa->conns and
 * sa->waiting_conns, plus any pending HTTP 429 requeue timer (see
 * steam_connection_cancel_requeues()). No callbacks are invoked. This
 * should be called from steam_close() (in libsteam.c) instead of/alongside
 * its manual sa->conns / sa->waiting_conns teardown loops, so a requeue
 * timer never fires against a freed SteamAccount. */
void steam_connection_cancel_all(SteamAccount *sa);

#endif /* STEAM_CONNECTION_H */
