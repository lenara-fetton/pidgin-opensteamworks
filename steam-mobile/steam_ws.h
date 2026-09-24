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
 * Minimal RFC 6455 WebSocket client (wss:// only) built on libpurple's
 * purple_ssl_connect(), so it honours the account's proxy settings and
 * certificate handling.
 *
 * Supports: client handshake, masked binary/text frames, fragmented
 * messages (reassembled before delivery), ping/pong (pong sent
 * automatically), close frames. Does not support compression extensions.
 */

#ifndef STEAM_WS_H
#define STEAM_WS_H

#include "libsteam.h"

typedef struct _SteamWebSocket SteamWebSocket;

typedef struct {
	/* Handshake finished; frames may now be sent. */
	void (*connected)(SteamWebSocket *ws, gpointer user_data);
	/* One complete (reassembled) message. `binary` distinguishes opcode
	 * 0x2 from 0x1. `data` is only valid during the callback. */
	void (*message)(SteamWebSocket *ws, const guint8 *data, gsize len,
	                gboolean binary, gpointer user_data);
	/* The socket is gone: server close frame, TLS/TCP error, handshake
	 * failure. `error` is NULL for a clean close. After this callback
	 * the SteamWebSocket is still valid until steam_ws_free() is called,
	 * but it can't be reused. */
	void (*closed)(SteamWebSocket *ws, const gchar *error, gpointer user_data);
} SteamWebSocketCallbacks;

/* Starts connecting to wss://host:port/path. Returns immediately; the
 * result is reported via callbacks. `sa->account` is used for proxy
 * settings. `origin` may be NULL (no Origin header). */
SteamWebSocket *steam_ws_connect(SteamAccount *sa, const gchar *host, guint16 port,
                                 const gchar *path, const gchar *origin,
                                 const SteamWebSocketCallbacks *callbacks,
                                 gpointer user_data);

/* Queues a masked binary (opcode 0x2) frame. Safe to call from inside the
 * `connected` and `message` callbacks. Returns FALSE if not connected. */
gboolean steam_ws_send_binary(SteamWebSocket *ws, const guint8 *data, gsize len);
gboolean steam_ws_send_text(SteamWebSocket *ws, const gchar *text);

/* Sends a close frame (code 1000) and tears the socket down. The `closed`
 * callback is NOT invoked for a locally initiated close. */
void steam_ws_close(SteamWebSocket *ws);

/* Frees the object. Closes the socket first if still open (no callbacks). */
void steam_ws_free(SteamWebSocket *ws);

gboolean steam_ws_is_connected(const SteamWebSocket *ws);

#endif /* STEAM_WS_H */
