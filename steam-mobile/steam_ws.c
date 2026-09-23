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
 * RFC 6455 WebSocket client over purple_ssl_connect(). See steam_ws.h.
 *
 * Re-entrancy model: every function entered from the event loop (SSL
 * connect/error callbacks, read/write watchers, timeouts) brackets its
 * work with ws_enter()/ws_leave(). User callbacks are only ever invoked
 * between those two calls. steam_ws_free() called while `depth` > 0 only
 * tears the socket down and marks the object; the memory is released by
 * the outermost ws_leave(). After every user callback the code re-checks
 * ws->state and bails out if the socket was closed or freed meanwhile.
 */

#include "steam_ws.h"

#include <errno.h>
#include <string.h>

#include "cipher.h"
#include "debug.h"
#include "eventloop.h"
#include "sslconn.h"

#define WS_CONNECT_TIMEOUT_SECONDS 30
#define WS_MAX_HANDSHAKE_SIZE      (32 * 1024)
#define WS_MAX_MESSAGE_SIZE        (64 * 1024 * 1024)
#define WS_READ_CHUNK              16384
#define WS_GUID                    "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

#define WS_OP_CONT   0x0
#define WS_OP_TEXT   0x1
#define WS_OP_BINARY 0x2
#define WS_OP_CLOSE  0x8
#define WS_OP_PING   0x9
#define WS_OP_PONG   0xA

#define WS_CLOSE_NORMAL         1000
#define WS_CLOSE_PROTOCOL_ERROR 1002
#define WS_CLOSE_NO_STATUS      1005
#define WS_CLOSE_INVALID_DATA   1007
#define WS_CLOSE_TOO_BIG        1009

typedef enum {
	WS_STATE_CONNECTING = 0, /* TCP/proxy/TLS in progress */
	WS_STATE_HANDSHAKE,      /* HTTP upgrade request sent, waiting for 101 */
	WS_STATE_OPEN,
	WS_STATE_CLOSED          /* terminal; object waits for steam_ws_free() */
} SteamWebSocketState;

struct _SteamWebSocket {
	SteamAccount *sa;
	gchar *host;
	guint16 port;
	gchar *path;
	gchar *origin;

	SteamWebSocketCallbacks cb;
	gpointer user_data;

	SteamWebSocketState state;
	PurpleSslConnection *ssl;
	guint write_watcher;
	guint connect_timeout;

	gchar *accept_expected;  /* base64(SHA1(key + GUID)) */

	GByteArray *rbuf;        /* unprocessed received bytes */
	GByteArray *wbuf;        /* bytes waiting to be written, from wpos */
	gsize wpos;

	GByteArray *frag;        /* reassembly buffer for fragmented messages */
	guint8 frag_opcode;      /* 0 = no fragmented message in progress */

	/* Deferred `closed` notification (failures detected outside an
	 * event-loop callback, e.g. purple_ssl_connect() returning NULL or a
	 * write error in steam_ws_send_*). */
	guint pending_close_timeout;
	gchar *pending_close_error;

	gint depth;              /* nesting of event-loop entry points */
	gboolean free_pending;
	gboolean in_connect;     /* inside purple_ssl_connect() call */
	gboolean test_mode;      /* no socket; see steam_ws__test_* */
};

static void ws_really_free(SteamWebSocket *ws);

/* ------------------------------------------------------------------ */
/* Entry-point bracketing                                              */

static void
ws_enter(SteamWebSocket *ws)
{
	ws->depth++;
}

/* Returns FALSE if the object has been freed (caller must not touch it). */
static gboolean
ws_leave(SteamWebSocket *ws)
{
	ws->depth--;
	if (ws->depth == 0 && ws->free_pending) {
		ws_really_free(ws);
		return FALSE;
	}
	return TRUE;
}

/* ------------------------------------------------------------------ */
/* Teardown                                                            */

/* Releases the socket, watchers and timers. No callbacks. */
static void
ws_teardown(SteamWebSocket *ws)
{
	if (ws->write_watcher) {
		purple_input_remove(ws->write_watcher);
		ws->write_watcher = 0;
	}
	if (ws->connect_timeout) {
		purple_timeout_remove(ws->connect_timeout);
		ws->connect_timeout = 0;
	}
	if (ws->ssl) {
		/* Also cancels a pending proxy connect / TLS handshake and
		 * removes the read watcher added with purple_ssl_input_add(). */
		purple_ssl_close(ws->ssl);
		ws->ssl = NULL;
	}
	ws->state = WS_STATE_CLOSED;
}

/* Terminates the connection and reports it through `closed`. Must only be
 * called from inside an entry point (depth > 0). `error` NULL = clean. */
static void
ws_fail(SteamWebSocket *ws, const gchar *error)
{
	if (ws->state == WS_STATE_CLOSED)
		return;

	if (error)
		purple_debug_warning("steam", "WebSocket %s: %s\n", ws->host, error);
	else
		purple_debug_info("steam", "WebSocket %s closed by server\n", ws->host);

	ws_teardown(ws);

	if (ws->cb.closed && !ws->free_pending)
		ws->cb.closed(ws, error, ws->user_data);
}

static gboolean
ws_pending_close_cb(gpointer data)
{
	SteamWebSocket *ws = data;
	gchar *error;

	ws->pending_close_timeout = 0;
	error = ws->pending_close_error;
	ws->pending_close_error = NULL;

	ws_enter(ws);
	/* The socket is already torn down; only the notification is left. */
	if (ws->cb.closed && !ws->free_pending)
		ws->cb.closed(ws, error, ws->user_data);
	g_free(error);
	ws_leave(ws);

	return FALSE;
}

/* Like ws_fail() but usable outside entry points: tears down now, reports
 * `closed` from a 0-timeout so the caller never sees re-entrancy. */
static void
ws_fail_async(SteamWebSocket *ws, const gchar *error)
{
	if (ws->state == WS_STATE_CLOSED)
		return;

	purple_debug_warning("steam", "WebSocket %s: %s\n", ws->host,
	                     error ? error : "closed");
	ws_teardown(ws);

	g_free(ws->pending_close_error);
	ws->pending_close_error = g_strdup(error);
	if (!ws->pending_close_timeout && !ws->test_mode)
		ws->pending_close_timeout = purple_timeout_add(0, ws_pending_close_cb, ws);
}

/* ------------------------------------------------------------------ */
/* Write path                                                          */

static void ws_write_cb(gpointer data, gint fd, PurpleInputCondition cond);

/* Writes as much of wbuf as the socket takes. Installs/removes the write
 * watcher as needed. Returns FALSE on a hard write error (errno set). */
static gboolean
ws_flush(SteamWebSocket *ws)
{
	while (ws->ssl && ws->wpos < ws->wbuf->len) {
		gssize n = (gssize) purple_ssl_write(ws->ssl, ws->wbuf->data + ws->wpos,
		                            ws->wbuf->len - ws->wpos);

		if (n > 0) {
			ws->wpos += n;
			continue;
		}
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
			/* Drop the written prefix now and then, not on every
			 * partial write (large messages would be memmoved over
			 * and over). */
			if (ws->wpos > 65536 && ws->wpos > ws->wbuf->len / 2) {
				g_byte_array_remove_range(ws->wbuf, 0, ws->wpos);
				ws->wpos = 0;
			}
			if (!ws->write_watcher) {
				purple_debug_misc("steam", "WebSocket %s: socket full, %" G_GSIZE_FORMAT " bytes queued\n",
				                  ws->host, ws->wbuf->len - ws->wpos);
				ws->write_watcher = purple_input_add(ws->ssl->fd,
				                                     PURPLE_INPUT_WRITE,
				                                     ws_write_cb, ws);
			}
			return TRUE;
		}
		if (n == 0)
			errno = EPIPE;
		return FALSE;
	}

	if (ws->ssl) {
		/* Everything written. */
		g_byte_array_set_size(ws->wbuf, 0);
		ws->wpos = 0;

		/* If the socket was congested, NSS may have reported the last
		 * TLS record as written while keeping part of it in its own
		 * buffer; it only retries on the next read or write. A
		 * zero-length write flushes that buffer (-1/EAGAIN while it
		 * still can't), so keep the watcher until it succeeds. */
		if (ws->write_watcher) {
			static const guint8 empty[1] = { 0 };
			if ((gssize) purple_ssl_write(ws->ssl, empty, 0) < 0 &&
			    (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
				return TRUE;
		}
	}
	if (ws->write_watcher) {
		purple_input_remove(ws->write_watcher);
		ws->write_watcher = 0;
	}
	return TRUE;
}

static void
ws_write_cb(gpointer data, gint fd, PurpleInputCondition cond)
{
	SteamWebSocket *ws = data;

	ws_enter(ws);
	if (!ws_flush(ws)) {
		gchar *err = g_strdup_printf("Write error: %s", g_strerror(errno));
		ws_fail(ws, err);
		g_free(err);
	}
	ws_leave(ws);
}

/* Appends one masked frame to wbuf (does not flush). */
static void
ws_queue_frame(SteamWebSocket *ws, guint8 opcode, const guint8 *data, gsize len)
{
	guint8 hdr[14];
	gsize hlen = 0;
	guint32 mask_int = g_random_int();
	guint8 mask[4];
	guint old_len;
	guint8 *out;
	gsize i;

	mask[0] = mask_int >> 24;
	mask[1] = mask_int >> 16;
	mask[2] = mask_int >> 8;
	mask[3] = mask_int;

	hdr[hlen++] = 0x80 | (opcode & 0x0F); /* FIN, no fragmentation */
	if (len < 126) {
		hdr[hlen++] = 0x80 | (guint8) len;
	} else if (len <= 0xFFFF) {
		hdr[hlen++] = 0x80 | 126;
		hdr[hlen++] = (len >> 8) & 0xFF;
		hdr[hlen++] = len & 0xFF;
	} else {
		guint64 l = len;
		hdr[hlen++] = 0x80 | 127;
		for (i = 0; i < 8; i++)
			hdr[hlen++] = (l >> (56 - 8 * i)) & 0xFF;
	}
	memcpy(hdr + hlen, mask, 4);
	hlen += 4;

	g_byte_array_append(ws->wbuf, hdr, hlen);
	old_len = ws->wbuf->len;
	g_byte_array_set_size(ws->wbuf, old_len + len);
	out = ws->wbuf->data + old_len;
	for (i = 0; i < len; i++)
		out[i] = data[i] ^ mask[i & 3];

	purple_debug_misc("steam", "WebSocket %s: sent frame opcode %u, %" G_GSIZE_FORMAT " bytes\n",
	                  ws->host, opcode, len);
}

/* Queues a close frame and tries to push out everything queued. Used for
 * locally initiated closes and protocol errors; best effort only. */
static void
ws_send_close_frame(SteamWebSocket *ws, guint16 code, const gchar *reason)
{
	guint8 payload[125];
	gsize len = 0;

	if (!ws->ssl && !ws->test_mode)
		return;

	payload[len++] = code >> 8;
	payload[len++] = code & 0xFF;
	if (reason) {
		gsize rlen = MIN(strlen(reason), sizeof(payload) - 2);
		memcpy(payload + 2, reason, rlen);
		len += rlen;
	}
	ws_queue_frame(ws, WS_OP_CLOSE, payload, len);
	ws_flush(ws);
}

/* Sends a close frame with `code` and fails the connection with `error`. */
static void
ws_fail_protocol(SteamWebSocket *ws, guint16 code, const gchar *error)
{
	if (ws->state == WS_STATE_OPEN)
		ws_send_close_frame(ws, code, NULL);
	ws_fail(ws, error);
}

static gboolean
ws_send(SteamWebSocket *ws, guint8 opcode, const guint8 *data, gsize len)
{
	g_return_val_if_fail(ws != NULL, FALSE);

	if (ws->state != WS_STATE_OPEN)
		return FALSE;

	ws_queue_frame(ws, opcode, data, len);
	if (!ws->write_watcher && !ws_flush(ws)) {
		gchar *err = g_strdup_printf("Write error: %s", g_strerror(errno));
		ws_fail_async(ws, err);
		g_free(err);
		return FALSE;
	}
	return TRUE;
}

/* ------------------------------------------------------------------ */
/* Handshake                                                           */

static gchar *
ws_compute_accept(const gchar *key)
{
	PurpleCipherContext *ctx;
	guchar digest[20];
	gchar *s;

	ctx = purple_cipher_context_new_by_name("sha1", NULL);
	if (ctx == NULL)
		return NULL;
	purple_cipher_context_append(ctx, (const guchar *) key, strlen(key));
	purple_cipher_context_append(ctx, (const guchar *) WS_GUID, strlen(WS_GUID));
	if (!purple_cipher_context_digest(ctx, sizeof(digest), digest, NULL)) {
		purple_cipher_context_destroy(ctx);
		return NULL;
	}
	purple_cipher_context_destroy(ctx);

	s = g_base64_encode(digest, sizeof(digest));
	return s;
}

static gchar *
ws_new_key(void)
{
	guint8 raw[16];
	gint i;

	for (i = 0; i < 16; i += 4) {
		guint32 r = g_random_int();
		memcpy(raw + i, &r, 4);
	}
	return g_base64_encode(raw, sizeof(raw));
}

/* Is `token` one of the comma-separated tokens in `value`? */
static gboolean
ws_header_has_token(const gchar *value, const gchar *token)
{
	gchar **parts = g_strsplit(value, ",", -1);
	gboolean found = FALSE;
	gint i;

	for (i = 0; parts[i] && !found; i++) {
		g_strstrip(parts[i]);
		if (g_ascii_strcasecmp(parts[i], token) == 0)
			found = TRUE;
	}
	g_strfreev(parts);
	return found;
}

/* Searches for "\r\n\r\n". Returns the offset just past it, or 0. */
static gsize
ws_find_header_end(const guint8 *data, gsize len)
{
	gsize i;

	for (i = 0; i + 3 < len; i++) {
		if (data[i] == '\r' && data[i + 1] == '\n' &&
		    data[i + 2] == '\r' && data[i + 3] == '\n')
			return i + 4;
	}
	return 0;
}

/* Parses and validates the HTTP response in `hdr`. Returns NULL on
 * success, otherwise a newly allocated error string. */
static gchar *
ws_check_handshake(SteamWebSocket *ws, const gchar *hdr)
{
	gchar **lines = g_strsplit(hdr, "\r\n", -1);
	gchar *error = NULL;
	gboolean upgrade_ok = FALSE, connection_ok = FALSE, accept_ok = FALSE;
	gint i;

	if (!lines[0] || !g_str_has_prefix(lines[0], "HTTP/1.1 ")) {
		error = g_strdup_printf("Invalid handshake response: %.64s",
		                        lines[0] ? lines[0] : "");
		goto out;
	}
	if (strncmp(lines[0] + 9, "101", 3) != 0 ||
	    (lines[0][12] != ' ' && lines[0][12] != '\0')) {
		error = g_strdup_printf("WebSocket upgrade refused: %.128s", lines[0]);
		goto out;
	}

	for (i = 1; lines[i]; i++) {
		gchar *colon = strchr(lines[i], ':');
		gchar *name, *value;

		if (!colon)
			continue;
		*colon = '\0';
		name = g_strstrip(lines[i]);
		value = g_strstrip(colon + 1);

		if (g_ascii_strcasecmp(name, "Upgrade") == 0) {
			if (g_ascii_strcasecmp(value, "websocket") == 0)
				upgrade_ok = TRUE;
		} else if (g_ascii_strcasecmp(name, "Connection") == 0) {
			if (ws_header_has_token(value, "upgrade"))
				connection_ok = TRUE;
		} else if (g_ascii_strcasecmp(name, "Sec-WebSocket-Accept") == 0) {
			if (ws->accept_expected && strcmp(value, ws->accept_expected) == 0)
				accept_ok = TRUE;
		} else if (g_ascii_strcasecmp(name, "Sec-WebSocket-Extensions") == 0 && *value) {
			error = g_strdup_printf("Server selected unrequested extension '%s'", value);
			goto out;
		} else if (g_ascii_strcasecmp(name, "Sec-WebSocket-Protocol") == 0 && *value) {
			error = g_strdup_printf("Server selected unrequested subprotocol '%s'", value);
			goto out;
		}
	}

	if (!upgrade_ok)
		error = g_strdup("Handshake response lacks 'Upgrade: websocket'");
	else if (!connection_ok)
		error = g_strdup("Handshake response lacks 'Connection: Upgrade'");
	else if (!accept_ok)
		error = g_strdup("Handshake response has a wrong or missing Sec-WebSocket-Accept");

out:
	g_strfreev(lines);
	return error;
}

/* Called while in HANDSHAKE state with new data in rbuf. Returns the
 * number of bytes consumed (0 if the header isn't complete yet). On
 * error or if the user closed the socket in `connected`, state becomes
 * CLOSED. */
static gsize
ws_process_handshake(SteamWebSocket *ws)
{
	gsize end = ws_find_header_end(ws->rbuf->data, ws->rbuf->len);
	gchar *hdr, *error;

	if (end == 0) {
		if (ws->rbuf->len > WS_MAX_HANDSHAKE_SIZE)
			ws_fail(ws, "Handshake response too large");
		return 0;
	}

	hdr = g_strndup((const gchar *) ws->rbuf->data, end - 4);
	purple_debug_misc("steam", "WebSocket %s: handshake response:\n%s\n", ws->host, hdr);
	error = ws_check_handshake(ws, hdr);
	g_free(hdr);

	if (error) {
		ws_fail(ws, error);
		g_free(error);
		return end;
	}

	if (ws->connect_timeout) {
		purple_timeout_remove(ws->connect_timeout);
		ws->connect_timeout = 0;
	}
	ws->state = WS_STATE_OPEN;
	purple_debug_info("steam", "WebSocket connected to %s:%u%s\n",
	                  ws->host, ws->port, ws->path);

	if (ws->cb.connected && !ws->free_pending)
		ws->cb.connected(ws, ws->user_data);

	return end;
}

static void
ws_send_handshake(SteamWebSocket *ws)
{
	GString *req = g_string_new(NULL);
	gchar *key = ws_new_key();

	g_free(ws->accept_expected);
	ws->accept_expected = ws_compute_accept(key);

	g_string_append_printf(req, "GET %s HTTP/1.1\r\n", ws->path);
	if (ws->port == 443)
		g_string_append_printf(req, "Host: %s\r\n", ws->host);
	else
		g_string_append_printf(req, "Host: %s:%u\r\n", ws->host, ws->port);
	g_string_append(req, "Upgrade: websocket\r\n");
	g_string_append(req, "Connection: Upgrade\r\n");
	g_string_append_printf(req, "Sec-WebSocket-Key: %s\r\n", key);
	g_string_append(req, "Sec-WebSocket-Version: 13\r\n");
	if (ws->origin)
		g_string_append_printf(req, "Origin: %s\r\n", ws->origin);
	g_string_append(req, "User-Agent: pidgin-opensteamworks/" STEAM_PLUGIN_VERSION " libpurple\r\n");
	g_string_append(req, "Cache-Control: no-cache\r\n");
	g_string_append(req, "Pragma: no-cache\r\n");
	g_string_append(req, "\r\n");

	g_byte_array_append(ws->wbuf, (const guint8 *) req->str, req->len);
	g_string_free(req, TRUE);
	g_free(key);

	ws->state = WS_STATE_HANDSHAKE;
}

/* ------------------------------------------------------------------ */
/* Frame parser                                                        */

static void
ws_deliver(SteamWebSocket *ws, guint8 opcode, const guint8 *data, gsize len)
{
	if (opcode == WS_OP_TEXT && !g_utf8_validate((const gchar *) data, len, NULL)) {
		ws_fail_protocol(ws, WS_CLOSE_INVALID_DATA, "Received invalid UTF-8 in a text message");
		return;
	}
	purple_debug_misc("steam", "WebSocket %s: received %s message, %" G_GSIZE_FORMAT " bytes\n",
	                  ws->host, opcode == WS_OP_TEXT ? "text" : "binary", len);
	if (ws->cb.message && !ws->free_pending)
		ws->cb.message(ws, data, len, opcode == WS_OP_BINARY, ws->user_data);
}

static void
ws_handle_close_frame(SteamWebSocket *ws, const guint8 *payload, gsize len)
{
	guint16 code = WS_CLOSE_NO_STATUS;
	gchar *reason = NULL;
	gchar *error = NULL;

	if (len == 1) {
		ws_fail_protocol(ws, WS_CLOSE_PROTOCOL_ERROR, "Received malformed close frame");
		return;
	}
	if (len >= 2) {
		code = (payload[0] << 8) | payload[1];
		reason = g_strndup((const gchar *) payload + 2, len - 2);
		if (!g_utf8_validate(reason, -1, NULL)) {
			g_free(reason);
			reason = g_strdup("(invalid UTF-8)");
		}
	}

	purple_debug_info("steam", "WebSocket %s: server sent close (code %u%s%s)\n",
	                  ws->host, code, reason && *reason ? ", " : "",
	                  reason ? reason : "");

	/* Echo the close code back, as RFC 6455 section 5.5.1 asks. */
	if (code == WS_CLOSE_NO_STATUS)
		ws_send_close_frame(ws, WS_CLOSE_NORMAL, NULL);
	else
		ws_send_close_frame(ws, code, NULL);

	if (code != WS_CLOSE_NORMAL && code != WS_CLOSE_NO_STATUS) {
		if (reason && *reason)
			error = g_strdup_printf("Server closed the connection (code %u: %s)", code, reason);
		else
			error = g_strdup_printf("Server closed the connection (code %u)", code);
	}

	ws_fail(ws, error);
	g_free(error);
	g_free(reason);
}

/* Parses as many complete frames from rbuf[*pos..] as available, handling
 * each. Stops early if the connection gets closed (by the peer, an error
 * or the user from a callback). */
static void
ws_process_frames(SteamWebSocket *ws, gsize *pos)
{
	while (ws->state == WS_STATE_OPEN) {
		const guint8 *p = ws->rbuf->data + *pos;
		gsize avail = ws->rbuf->len - *pos;
		gsize hlen = 2;
		guint64 plen;
		gboolean fin;
		guint8 opcode;
		const guint8 *payload;

		if (avail < 2)
			return;

		fin = (p[0] & 0x80) != 0;
		opcode = p[0] & 0x0F;

		if (p[0] & 0x70) {
			ws_fail_protocol(ws, WS_CLOSE_PROTOCOL_ERROR,
			                 "Received frame with reserved bits set");
			return;
		}
		if (p[1] & 0x80) {
			ws_fail_protocol(ws, WS_CLOSE_PROTOCOL_ERROR,
			                 "Received masked frame from server");
			return;
		}

		plen = p[1] & 0x7F;
		if (plen == 126) {
			if (avail < 4)
				return;
			plen = ((guint64) p[2] << 8) | p[3];
			hlen = 4;
		} else if (plen == 127) {
			gint i;
			if (avail < 10)
				return;
			plen = 0;
			for (i = 0; i < 8; i++)
				plen = (plen << 8) | p[2 + i];
			hlen = 10;
			if (plen >> 63) {
				ws_fail_protocol(ws, WS_CLOSE_PROTOCOL_ERROR,
				                 "Received frame with invalid length");
				return;
			}
		}

		if (opcode & 0x08) {
			/* Control frame */
			if (!fin || plen > 125) {
				ws_fail_protocol(ws, WS_CLOSE_PROTOCOL_ERROR,
				                 "Received fragmented or oversized control frame");
				return;
			}
		} else if (plen > WS_MAX_MESSAGE_SIZE ||
		           (opcode == WS_OP_CONT && ws->frag && plen + ws->frag->len > WS_MAX_MESSAGE_SIZE)) {
			ws_fail_protocol(ws, WS_CLOSE_TOO_BIG, "Received message is too large");
			return;
		}

		if (avail - hlen < plen)
			return; /* wait for the rest of the frame */

		payload = p + hlen;
		*pos += hlen + plen;

		purple_debug_misc("steam", "WebSocket %s: frame opcode %u fin %d len %" G_GUINT64_FORMAT "\n",
		                  ws->host, opcode, fin, plen);

		switch (opcode) {
		case WS_OP_TEXT:
		case WS_OP_BINARY:
			if (ws->frag_opcode) {
				ws_fail_protocol(ws, WS_CLOSE_PROTOCOL_ERROR,
				                 "Received new data frame inside a fragmented message");
				return;
			}
			if (fin) {
				ws_deliver(ws, opcode, payload, plen);
			} else {
				ws->frag_opcode = opcode;
				if (!ws->frag)
					ws->frag = g_byte_array_new();
				g_byte_array_set_size(ws->frag, 0);
				g_byte_array_append(ws->frag, payload, plen);
			}
			break;

		case WS_OP_CONT:
			if (!ws->frag_opcode) {
				ws_fail_protocol(ws, WS_CLOSE_PROTOCOL_ERROR,
				                 "Received continuation frame without a message");
				return;
			}
			g_byte_array_append(ws->frag, payload, plen);
			if (fin) {
				GByteArray *msg = ws->frag;
				guint8 op = ws->frag_opcode;

				/* Detach before the callback so a new fragmented
				 * message can't clobber the buffer we hand out. */
				ws->frag = NULL;
				ws->frag_opcode = 0;
				ws_deliver(ws, op, msg->data, msg->len);
				if (ws->frag == NULL) {
					g_byte_array_set_size(msg, 0);
					ws->frag = msg;
				} else {
					g_byte_array_free(msg, TRUE);
				}
			}
			break;

		case WS_OP_PING:
			purple_debug_misc("steam", "WebSocket %s: ping, sending pong\n", ws->host);
			ws_queue_frame(ws, WS_OP_PONG, payload, plen);
			if (!ws->write_watcher && !ws_flush(ws)) {
				gchar *err = g_strdup_printf("Write error: %s", g_strerror(errno));
				ws_fail(ws, err);
				g_free(err);
				return;
			}
			break;

		case WS_OP_PONG:
			break;

		case WS_OP_CLOSE:
			ws_handle_close_frame(ws, payload, plen);
			return;

		default:
			ws_fail_protocol(ws, WS_CLOSE_PROTOCOL_ERROR, "Received frame with unknown opcode");
			return;
		}
	}
}

/* Processes everything in rbuf according to the current state and drops
 * the consumed bytes. */
static void
ws_process_input(SteamWebSocket *ws)
{
	gsize pos = 0;

	if (ws->state == WS_STATE_HANDSHAKE)
		pos = ws_process_handshake(ws);

	if (ws->state == WS_STATE_OPEN)
		ws_process_frames(ws, &pos);

	if (ws->state == WS_STATE_CLOSED)
		g_byte_array_set_size(ws->rbuf, 0);
	else if (pos > 0)
		g_byte_array_remove_range(ws->rbuf, 0, pos);
}

/* ------------------------------------------------------------------ */
/* libpurple callbacks                                                 */

static void
ws_read_cb(gpointer data, PurpleSslConnection *ssl, PurpleInputCondition cond)
{
	SteamWebSocket *ws = data;
	guint8 buf[WS_READ_CHUNK];

	ws_enter(ws);

	/* NSS may hold decrypted data beyond what the fd signals, so read
	 * until the SSL layer says it would block. */
	while (ws->state == WS_STATE_HANDSHAKE || ws->state == WS_STATE_OPEN) {
		gssize n = (gssize) purple_ssl_read(ws->ssl, buf, sizeof(buf));

		if (n > 0) {
			g_byte_array_append(ws->rbuf, buf, n);
			ws_process_input(ws);
			continue;
		}
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
			break;

		if (n == 0) {
			ws_fail(ws, ws->state == WS_STATE_HANDSHAKE
			            ? "Server closed the connection during the handshake"
			            : "Server closed the connection without a close frame");
		} else {
			gchar *err = g_strdup_printf("Read error: %s", g_strerror(errno));
			ws_fail(ws, err);
			g_free(err);
		}
		break;
	}

	ws_leave(ws);
}

static void
ws_ssl_connected_cb(gpointer data, PurpleSslConnection *ssl, PurpleInputCondition cond)
{
	SteamWebSocket *ws = data;

	ws_enter(ws);

	/* Should not happen: ws_teardown() closes `ssl` which cancels this. */
	if (ws->state != WS_STATE_CONNECTING) {
		ws_leave(ws);
		return;
	}
	ws->ssl = ssl;

	purple_debug_info("steam", "WebSocket %s: TLS established, sending upgrade request\n", ws->host);

	ws_send_handshake(ws);
	purple_ssl_input_add(ssl, ws_read_cb, ws);
	if (!ws_flush(ws)) {
		gchar *err = g_strdup_printf("Write error: %s", g_strerror(errno));
		ws_fail(ws, err);
		g_free(err);
	}

	ws_leave(ws);
}

static void
ws_ssl_error_cb(PurpleSslConnection *ssl, PurpleSslErrorType errortype, gpointer data)
{
	SteamWebSocket *ws = data;
	gchar *err;

	/* libpurple closes `ssl` itself right after this callback. */
	ws->ssl = NULL;
	err = g_strdup_printf("SSL connection to %s failed: %s", ws->host,
	                      purple_ssl_strerror(errortype));

	if (ws->in_connect) {
		/* Reported synchronously from within purple_ssl_connect();
		 * the caller hasn't got the pointer yet. */
		ws_fail_async(ws, err);
	} else {
		ws_enter(ws);
		ws_fail(ws, err);
		ws_leave(ws);
	}
	g_free(err);
}

static gboolean
ws_connect_timeout_cb(gpointer data)
{
	SteamWebSocket *ws = data;

	ws->connect_timeout = 0;
	ws_enter(ws);
	ws_fail(ws, "Connection timed out");
	ws_leave(ws);
	return FALSE;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */

static SteamWebSocket *
ws_new(SteamAccount *sa, const gchar *host, guint16 port, const gchar *path,
       const gchar *origin, const SteamWebSocketCallbacks *callbacks,
       gpointer user_data)
{
	SteamWebSocket *ws = g_new0(SteamWebSocket, 1);

	ws->sa = sa;
	ws->host = g_strdup(host);
	ws->port = port ? port : 443;
	ws->path = g_strdup(path && *path ? path : "/");
	ws->origin = g_strdup(origin);
	if (callbacks)
		ws->cb = *callbacks;
	ws->user_data = user_data;
	ws->state = WS_STATE_CONNECTING;
	ws->rbuf = g_byte_array_new();
	ws->wbuf = g_byte_array_new();

	return ws;
}

SteamWebSocket *
steam_ws_connect(SteamAccount *sa, const gchar *host, guint16 port,
                 const gchar *path, const gchar *origin,
                 const SteamWebSocketCallbacks *callbacks,
                 gpointer user_data)
{
	SteamWebSocket *ws;
	PurpleSslConnection *ssl;

	g_return_val_if_fail(host != NULL, NULL);

	ws = ws_new(sa, host, port, path, origin, callbacks, user_data);

	purple_debug_info("steam", "WebSocket connecting to wss://%s:%u%s\n",
	                  ws->host, ws->port, ws->path);

	if (!purple_ssl_is_supported()) {
		ws_fail_async(ws, "SSL support unavailable");
		return ws;
	}

	ws->connect_timeout = purple_timeout_add_seconds(WS_CONNECT_TIMEOUT_SECONDS,
	                                                 ws_connect_timeout_cb, ws);

	ws->in_connect = TRUE;
	ssl = purple_ssl_connect(sa ? sa->account : NULL, ws->host, ws->port,
	                         ws_ssl_connected_cb, ws_ssl_error_cb, ws);
	ws->in_connect = FALSE;

	if (ws->state == WS_STATE_CLOSED) {
		/* error_cb already ran synchronously and scheduled `closed`;
		 * libpurple owns and frees `ssl`. */
		return ws;
	}
	if (ssl == NULL) {
		ws_fail_async(ws, "Unable to start the connection");
		return ws;
	}
	ws->ssl = ssl;

	return ws;
}

gboolean
steam_ws_send_binary(SteamWebSocket *ws, const guint8 *data, gsize len)
{
	g_return_val_if_fail(data != NULL || len == 0, FALSE);
	return ws_send(ws, WS_OP_BINARY, data, len);
}

gboolean
steam_ws_send_text(SteamWebSocket *ws, const gchar *text)
{
	g_return_val_if_fail(text != NULL, FALSE);
	return ws_send(ws, WS_OP_TEXT, (const guint8 *) text, strlen(text));
}

void
steam_ws_close(SteamWebSocket *ws)
{
	if (ws == NULL)
		return;

	if (ws->state == WS_STATE_OPEN) {
		purple_debug_info("steam", "WebSocket %s: closing\n", ws->host);
		/* Best effort: whatever the socket accepts right now. */
		ws_send_close_frame(ws, WS_CLOSE_NORMAL, NULL);
		if (ws->ssl && ws->wbuf->len > ws->wpos)
			purple_debug_warning("steam", "WebSocket %s: dropping %" G_GSIZE_FORMAT " unsent bytes on close\n",
			                     ws->host, ws->wbuf->len - ws->wpos);
	}
	ws_teardown(ws);

	/* A locally initiated close never reports `closed`. */
	if (ws->pending_close_timeout) {
		purple_timeout_remove(ws->pending_close_timeout);
		ws->pending_close_timeout = 0;
	}
	g_free(ws->pending_close_error);
	ws->pending_close_error = NULL;
}

static void
ws_really_free(SteamWebSocket *ws)
{
	if (ws->frag)
		g_byte_array_free(ws->frag, TRUE);
	g_byte_array_free(ws->rbuf, TRUE);
	g_byte_array_free(ws->wbuf, TRUE);
	g_free(ws->accept_expected);
	g_free(ws->pending_close_error);
	g_free(ws->host);
	g_free(ws->path);
	g_free(ws->origin);
	g_free(ws);
}

void
steam_ws_free(SteamWebSocket *ws)
{
	if (ws == NULL || ws->free_pending)
		return;

	steam_ws_close(ws);
	memset(&ws->cb, 0, sizeof(ws->cb));

	if (ws->depth > 0)
		ws->free_pending = TRUE; /* released by the outermost ws_leave() */
	else
		ws_really_free(ws);
}

gboolean
steam_ws_is_connected(const SteamWebSocket *ws)
{
	return ws != NULL && ws->state == WS_STATE_OPEN;
}

/* ------------------------------------------------------------------ */
/* Test hooks (not part of the public API; used by tests/test_ws.c to
 * drive the parser without a socket). Frames the object "sends" stay in
 * wbuf and can be inspected with steam_ws__test_sent(). */

SteamWebSocket *steam_ws__test_new(const SteamWebSocketCallbacks *callbacks,
                                   gpointer user_data, const gchar *key);
void steam_ws__test_feed(SteamWebSocket *ws, const guint8 *data, gsize len);
GByteArray *steam_ws__test_sent(SteamWebSocket *ws);

/* With `key` != NULL the object starts in HANDSHAKE state expecting the
 * accept value for that key; otherwise it starts OPEN. */
SteamWebSocket *
steam_ws__test_new(const SteamWebSocketCallbacks *callbacks, gpointer user_data,
                   const gchar *key)
{
	SteamWebSocket *ws = ws_new(NULL, "test", 443, "/", NULL, callbacks, user_data);

	ws->test_mode = TRUE;
	if (key) {
		ws->accept_expected = ws_compute_accept(key);
		ws->state = WS_STATE_HANDSHAKE;
	} else {
		ws->state = WS_STATE_OPEN;
	}
	return ws;
}

void
steam_ws__test_feed(SteamWebSocket *ws, const guint8 *data, gsize len)
{
	ws_enter(ws);
	if (ws->state == WS_STATE_HANDSHAKE || ws->state == WS_STATE_OPEN) {
		g_byte_array_append(ws->rbuf, data, len);
		ws_process_input(ws);
	}
	ws_leave(ws);
}

GByteArray *
steam_ws__test_sent(SteamWebSocket *ws)
{
	return ws->wbuf;
}
