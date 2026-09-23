/*
 * Tests for steam_ws.c.
 *
 *  1. Offline: drives the frame parser / handshake validator through the
 *     test hooks in steam_ws.c (byte-by-byte feeding, 7/16/64-bit lengths,
 *     fragmentation with interleaved ping, close codes, protocol errors,
 *     freeing from inside callbacks, masking of sent frames).
 *  2. Online: connection failure to a closed port reports closed(error).
 *  3. Online: public echo server (wss://echo.websocket.org, falling back
 *     to wss://ws.postman-echo.com/raw): small text, ~70 KB text and
 *     ~70 KB binary messages echoed byte-for-byte; local close does not
 *     fire `closed`.
 *  4. Online: Steam CM websocket from ISteamDirectory: handshake completes;
 *     the socket is closed and freed from inside the `connected` callback.
 *
 * Set STEAM_DEBUG=1 for libpurple debug output. Set TEST_WS_OFFLINE=1 to
 * run only part 1. TEST_WS_ECHO_INDEX=1 starts with the second echo server.
 * TEST_WS_BIG=<bytes> additionally echoes a binary message of that size.
 */

#include "purple_harness.h"
#include "steam_ws.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "eventloop.h"
#include "util.h"

/* Test hooks exported by steam_ws.c (not in steam_ws.h). */
SteamWebSocket *steam_ws__test_new(const SteamWebSocketCallbacks *callbacks,
                                   gpointer user_data, const gchar *key);
void steam_ws__test_feed(SteamWebSocket *ws, const guint8 *data, gsize len);
GByteArray *steam_ws__test_sent(SteamWebSocket *ws);

static SteamAccount *sa;
static int failures = 0;

#define CHECK(cond, ...) do { \
	if (!(cond)) { \
		failures++; \
		fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
		fprintf(stderr, __VA_ARGS__); \
		fprintf(stderr, "\n"); \
	} \
} while (0)

/* ------------------------------------------------------------------ */
/* Offline parser tests                                                */

typedef struct {
	int connected;
	int messages;
	int closed;
	gchar *close_error;
	GByteArray *last;
	gboolean last_binary;
	gboolean free_in_message;
	gboolean free_in_closed;
	gboolean close_in_message;
	gboolean freed;
} Rec;

static void
rec_connected(SteamWebSocket *ws, gpointer data)
{
	Rec *r = data;
	r->connected++;
}

static void
rec_message(SteamWebSocket *ws, const guint8 *d, gsize len, gboolean binary, gpointer data)
{
	Rec *r = data;
	r->messages++;
	if (!r->last)
		r->last = g_byte_array_new();
	g_byte_array_set_size(r->last, 0);
	g_byte_array_append(r->last, d, len);
	r->last_binary = binary;
	if (r->close_in_message)
		steam_ws_close(ws);
	if (r->free_in_message) {
		steam_ws_free(ws);
		r->freed = TRUE;
	}
}

static void
rec_closed(SteamWebSocket *ws, const gchar *error, gpointer data)
{
	Rec *r = data;
	r->closed++;
	g_free(r->close_error);
	r->close_error = g_strdup(error);
	if (r->free_in_closed) {
		steam_ws_free(ws);
		r->freed = TRUE;
	}
}

static const SteamWebSocketCallbacks rec_cbs = { rec_connected, rec_message, rec_closed };

static void
rec_clear(Rec *r)
{
	g_free(r->close_error);
	if (r->last)
		g_byte_array_free(r->last, TRUE);
	memset(r, 0, sizeof(*r));
}

/* Builds an unmasked (server) frame. */
static GByteArray *
server_frame(GByteArray *out, gboolean fin, guint8 opcode, const guint8 *data, gsize len)
{
	guint8 h[10];
	gsize hl = 0;
	int i;

	if (!out)
		out = g_byte_array_new();
	h[hl++] = (fin ? 0x80 : 0) | opcode;
	if (len < 126) {
		h[hl++] = len;
	} else if (len <= 0xFFFF) {
		h[hl++] = 126;
		h[hl++] = len >> 8;
		h[hl++] = len & 0xFF;
	} else {
		h[hl++] = 127;
		for (i = 0; i < 8; i++)
			h[hl++] = ((guint64) len >> (56 - 8 * i)) & 0xFF;
	}
	g_byte_array_append(out, h, hl);
	g_byte_array_append(out, data, len);
	return out;
}

/* Decodes one masked client frame at the start of `buf`. Returns the
 * payload (unmasked) or NULL; `consumed` receives the frame size. */
static GByteArray *
client_frame(const GByteArray *buf, gsize off, guint8 *opcode, gboolean *fin, gsize *consumed)
{
	const guint8 *p = buf->data + off;
	gsize avail = buf->len - off, hl = 2, i;
	guint64 len;
	const guint8 *mask;
	GByteArray *out;

	if (avail < 2 || !(p[1] & 0x80))
		return NULL;
	*fin = (p[0] & 0x80) != 0;
	*opcode = p[0] & 0x0F;
	len = p[1] & 0x7F;
	if (len == 126) {
		len = (p[2] << 8) | p[3];
		hl = 4;
	} else if (len == 127) {
		len = 0;
		for (i = 0; i < 8; i++)
			len = (len << 8) | p[2 + i];
		hl = 10;
	}
	if (avail < hl + 4 + len)
		return NULL;
	mask = p + hl;
	out = g_byte_array_sized_new(len);
	g_byte_array_set_size(out, len);
	for (i = 0; i < len; i++)
		out->data[i] = p[hl + 4 + i] ^ mask[i & 3];
	*consumed = hl + 4 + len;
	return out;
}

static void
feed_bytewise(SteamWebSocket *ws, GByteArray *b, Rec *r)
{
	gsize i;
	for (i = 0; i < b->len && !r->freed; i++)
		steam_ws__test_feed(ws, b->data + i, 1);
}

static void
offline_tests(void)
{
	Rec r;
	SteamWebSocket *ws;
	GByteArray *b, *pl;
	guint8 big[70000];
	gsize i, consumed;
	guint8 op;
	gboolean fin;
	const char *resp =
		"HTTP/1.1 101 Switching Protocols\r\n"
		"Upgrade: websocket\r\n"
		"connection: keep-alive, Upgrade\r\n"
		"Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"
		"\r\n";

	for (i = 0; i < sizeof(big); i++)
		big[i] = (i * 7919) & 0xFF;

	/* Handshake (RFC 6455 sample key) + first frame in the same read,
	 * fed one byte at a time. */
	memset(&r, 0, sizeof(r));
	ws = steam_ws__test_new(&rec_cbs, &r, "dGhlIHNhbXBsZSBub25jZQ==");
	b = g_byte_array_new();
	g_byte_array_append(b, (const guint8 *) resp, strlen(resp));
	server_frame(b, TRUE, 0x1, (const guint8 *) "hi", 2);
	feed_bytewise(ws, b, &r);
	CHECK(r.connected == 1, "handshake: connected=%d", r.connected);
	CHECK(r.messages == 1 && r.last->len == 2 && memcmp(r.last->data, "hi", 2) == 0 && !r.last_binary,
	      "handshake: frame after header not delivered");
	CHECK(steam_ws_is_connected(ws), "handshake: not connected");
	g_byte_array_free(b, TRUE);

	/* 16-bit and 64-bit lengths, byte by byte and all at once. */
	b = server_frame(NULL, TRUE, 0x2, big, 300);
	feed_bytewise(ws, b, &r);
	CHECK(r.messages == 2 && r.last_binary && r.last->len == 300 && memcmp(r.last->data, big, 300) == 0,
	      "16-bit length frame");
	g_byte_array_free(b, TRUE);
	b = server_frame(NULL, TRUE, 0x2, big, sizeof(big));
	steam_ws__test_feed(ws, b->data, 5);
	steam_ws__test_feed(ws, b->data + 5, b->len - 5);
	CHECK(r.messages == 3 && r.last->len == sizeof(big) && memcmp(r.last->data, big, sizeof(big)) == 0,
	      "64-bit length frame");
	g_byte_array_free(b, TRUE);

	/* Fragmented text with an interleaved ping; zero-length final frame. */
	b = server_frame(NULL, FALSE, 0x1, (const guint8 *) "Hel", 3);
	server_frame(b, TRUE, 0x9, (const guint8 *) "pp", 2);
	server_frame(b, FALSE, 0x0, (const guint8 *) "lo", 2);
	server_frame(b, TRUE, 0x0, NULL, 0);
	steam_ws__test_feed(ws, b->data, b->len);
	CHECK(r.messages == 4 && r.last->len == 5 && memcmp(r.last->data, "Hello", 5) == 0 && !r.last_binary,
	      "fragmented message");
	g_byte_array_free(b, TRUE);
	pl = client_frame(steam_ws__test_sent(ws), 0, &op, &fin, &consumed);
	CHECK(pl && op == 0xA && fin && pl->len == 2 && memcmp(pl->data, "pp", 2) == 0, "pong reply");
	if (pl)
		g_byte_array_free(pl, TRUE);
	g_byte_array_set_size(steam_ws__test_sent(ws), 0);

	/* Send path: 64-bit length binary, masked. */
	CHECK(steam_ws_send_binary(ws, big, sizeof(big)), "send_binary");
	CHECK(steam_ws_send_text(ws, "abc"), "send_text");
	pl = client_frame(steam_ws__test_sent(ws), 0, &op, &fin, &consumed);
	CHECK(pl && op == 0x2 && fin && pl->len == sizeof(big) && memcmp(pl->data, big, sizeof(big)) == 0,
	      "sent binary frame");
	if (pl)
		g_byte_array_free(pl, TRUE);
	pl = client_frame(steam_ws__test_sent(ws), consumed, &op, &fin, &consumed);
	CHECK(pl && op == 0x1 && pl->len == 3 && memcmp(pl->data, "abc", 3) == 0, "sent text frame");
	if (pl)
		g_byte_array_free(pl, TRUE);
	g_byte_array_set_size(steam_ws__test_sent(ws), 0);

	/* Clean close (1000) → closed(NULL), close echoed. */
	b = server_frame(NULL, TRUE, 0x8, (const guint8 *) "\x03\xe8", 2);
	server_frame(b, TRUE, 0x1, (const guint8 *) "late", 4);
	steam_ws__test_feed(ws, b->data, b->len);
	CHECK(r.closed == 1 && r.close_error == NULL, "close 1000: closed=%d err=%s", r.closed,
	      r.close_error ? r.close_error : "(null)");
	CHECK(r.messages == 4, "message delivered after close");
	CHECK(!steam_ws_is_connected(ws), "still connected after close");
	CHECK(!steam_ws_send_text(ws, "x"), "send after close succeeded");
	pl = client_frame(steam_ws__test_sent(ws), 0, &op, &fin, &consumed);
	CHECK(pl && op == 0x8 && pl->len == 2 && pl->data[0] == 0x03 && pl->data[1] == 0xe8, "close echo");
	if (pl)
		g_byte_array_free(pl, TRUE);
	g_byte_array_free(b, TRUE);
	steam_ws_free(ws);
	rec_clear(&r);

	/* Close with code 1001 + reason → error mentioning it; free in closed. */
	ws = steam_ws__test_new(&rec_cbs, &r, NULL);
	r.free_in_closed = TRUE;
	b = server_frame(NULL, TRUE, 0x8, (const guint8 *) "\x03\xe9" "going away", 12);
	steam_ws__test_feed(ws, b->data, b->len);
	CHECK(r.closed == 1 && r.close_error && strstr(r.close_error, "1001") && strstr(r.close_error, "going away"),
	      "close 1001: err=%s", r.close_error ? r.close_error : "(null)");
	CHECK(r.freed, "free in closed");
	g_byte_array_free(b, TRUE);
	rec_clear(&r);

	/* Free inside message callback with more frames queued in the same
	 * read: no further callbacks, no use-after-free (run under valgrind). */
	ws = steam_ws__test_new(&rec_cbs, &r, NULL);
	r.free_in_message = TRUE;
	b = server_frame(NULL, TRUE, 0x1, (const guint8 *) "one", 3);
	server_frame(b, TRUE, 0x1, (const guint8 *) "two", 3);
	server_frame(b, TRUE, 0x8, NULL, 0);
	steam_ws__test_feed(ws, b->data, b->len);
	CHECK(r.messages == 1 && r.closed == 0, "free in message: messages=%d closed=%d", r.messages, r.closed);
	g_byte_array_free(b, TRUE);
	rec_clear(&r);

	/* Local close inside message callback: no `closed`, no more messages. */
	ws = steam_ws__test_new(&rec_cbs, &r, NULL);
	r.close_in_message = TRUE;
	b = server_frame(NULL, TRUE, 0x1, (const guint8 *) "one", 3);
	server_frame(b, TRUE, 0x1, (const guint8 *) "two", 3);
	steam_ws__test_feed(ws, b->data, b->len);
	CHECK(r.messages == 1 && r.closed == 0, "close in message: messages=%d closed=%d", r.messages, r.closed);
	steam_ws_free(ws);
	g_byte_array_free(b, TRUE);
	rec_clear(&r);

	/* Protocol errors. */
	{
		struct { const char *name; const guint8 *data; gsize len; } bad[] = {
			{ "masked server frame", (const guint8 *) "\x81\x82\x00\x00\x00\x00hi", 8 },
			{ "reserved bits", (const guint8 *) "\xc1\x02hi", 4 },
			{ "continuation without start", (const guint8 *) "\x80\x02hi", 4 },
			{ "fragmented ping", (const guint8 *) "\x09\x00", 2 },
			{ "unknown opcode", (const guint8 *) "\x83\x00", 2 },
			{ "invalid utf-8 text", (const guint8 *) "\x81\x02\xc3\x28", 4 },
			{ "data frame inside fragmented message", (const guint8 *) "\x01\x01" "a" "\x81\x01" "b", 6 },
			{ "one-byte close payload", (const guint8 *) "\x88\x01\x03", 3 },
			{ "huge length", (const guint8 *) "\x82\x7f\x00\x00\x00\x01\x00\x00\x00\x00", 10 },
		};
		for (i = 0; i < G_N_ELEMENTS(bad); i++) {
			ws = steam_ws__test_new(&rec_cbs, &r, NULL);
			steam_ws__test_feed(ws, bad[i].data, bad[i].len);
			CHECK(r.closed == 1 && r.close_error != NULL && r.messages == 0,
			      "%s: closed=%d messages=%d", bad[i].name, r.closed, r.messages);
			fprintf(stderr, "  %-40s -> %s\n", bad[i].name, r.close_error ? r.close_error : "(none)");
			steam_ws_free(ws);
			rec_clear(&r);
		}
	}

	/* Handshake failures. */
	{
		const char *bad_hs[] = {
			"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
			"Sec-WebSocket-Accept: AAAAAAAAAAAAAAAAAAAAAAAAAAA=\r\n\r\n",
			"HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n",
			"HTTP/1.1 101 Switching Protocols\r\nConnection: Upgrade\r\n"
			"Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n\r\n",
		};
		for (i = 0; i < G_N_ELEMENTS(bad_hs); i++) {
			ws = steam_ws__test_new(&rec_cbs, &r, "dGhlIHNhbXBsZSBub25jZQ==");
			steam_ws__test_feed(ws, (const guint8 *) bad_hs[i], strlen(bad_hs[i]));
			CHECK(r.closed == 1 && r.close_error && r.connected == 0, "bad handshake %d", (int) i);
			fprintf(stderr, "  bad handshake %d -> %s\n", (int) i, r.close_error ? r.close_error : "(none)");
			steam_ws_free(ws);
			rec_clear(&r);
		}
	}

	fprintf(stderr, "offline tests: %s\n", failures ? "FAILED" : "ok");
}

/* ------------------------------------------------------------------ */
/* Online tests (sequenced through `step`)                             */

static void next_step(void);
static int step = 0;
static SteamWebSocket *cur;
static guint watchdog;

static gboolean
watchdog_cb(gpointer data)
{
	fprintf(stderr, "FAIL: step %d timed out\n", step);
	failures++;
	watchdog = 0;
	if (cur) {
		steam_ws_free(cur);
		cur = NULL;
	}
	next_step();
	return FALSE;
}

static void
arm_watchdog(guint seconds)
{
	if (watchdog)
		purple_timeout_remove(watchdog);
	watchdog = purple_timeout_add_seconds(seconds, watchdog_cb, NULL);
}

static gboolean
next_step_idle(gpointer data)
{
	next_step();
	return FALSE;
}

static void
schedule_next(guint ms)
{
	if (watchdog) {
		purple_timeout_remove(watchdog);
		watchdog = 0;
	}
	purple_timeout_add(ms, next_step_idle, NULL);
}

/* --- step: connection failure --- */

static void
fail_connected(SteamWebSocket *ws, gpointer d)
{
	fprintf(stderr, "FAIL: connected to a closed port?\n");
	failures++;
}

static void
fail_closed(SteamWebSocket *ws, const gchar *error, gpointer d)
{
	fprintf(stderr, "connection-failure test: closed(\"%s\")\n", error ? error : "(null)");
	CHECK(error != NULL, "closed port: error is NULL");
	steam_ws_free(ws); /* free inside `closed` */
	cur = NULL;
	schedule_next(0);
}

/* --- step: echo server --- */

/* host, path, size of the big messages, binary supported. The fallback
 * ws.postman-echo.com rejects frames over 64 KiB (close 1009), drops the
 * TCP connection on any binary frame, and echoes large text messages
 * fragmented into 16 KiB frames (which exercises reassembly). 65536 bytes
 * still needs the 64-bit length encoding. */
static const struct { const char *host; const char *path; gsize big; gboolean binary; } echo_urls[] = {
	{ "echo.websocket.org", "/", 70000, TRUE },
	{ "ws.postman-echo.com", "/raw", 65536, FALSE },
};
static guint echo_idx = 0;
static GPtrArray *echo_expected;   /* GByteArray*, in send order; [0] of each = 'T'/'B' */
static guint echo_got;
static gboolean echo_done;
static gboolean echo_closed_fired;

static GByteArray *
make_expected(gboolean binary, gsize len)
{
	GByteArray *b = g_byte_array_sized_new(len + 1);
	gsize i;
	guint8 c = binary ? 'B' : 'T';

	g_byte_array_append(b, &c, 1);
	for (i = 0; i < len; i++) {
		guint8 v = binary ? (guint8) g_random_int() : (guint8) (' ' + g_random_int_range(0, 95));
		g_byte_array_append(b, &v, 1);
	}
	return b;
}

static void
echo_connected(SteamWebSocket *ws, gpointer d)
{
	guint i;

	fprintf(stderr, "echo: connected to %s\n", echo_urls[echo_idx].host);
	echo_expected = g_ptr_array_new_with_free_func((GDestroyNotify) g_byte_array_unref);
	g_ptr_array_add(echo_expected, make_expected(FALSE, 200));
	g_ptr_array_add(echo_expected, make_expected(FALSE, echo_urls[echo_idx].big));
	if (echo_urls[echo_idx].binary) {
		/* TEST_WS_BIG=<bytes> adds a larger binary message, e.g. to
		 * force the queued-write path. */
		if (getenv("TEST_WS_BIG"))
			g_ptr_array_add(echo_expected, make_expected(TRUE, atoi(getenv("TEST_WS_BIG"))));
		g_ptr_array_add(echo_expected, make_expected(TRUE, echo_urls[echo_idx].big));
		g_ptr_array_add(echo_expected, make_expected(TRUE, 5));
	} else {
		fprintf(stderr, "echo: %s does not support binary frames, text only\n",
		        echo_urls[echo_idx].host);
	}
	echo_got = 0;

	for (i = 0; i < echo_expected->len; i++) {
		GByteArray *e = g_ptr_array_index(echo_expected, i);
		gboolean ok;
		if (e->data[0] == 'T') {
			gchar *s = g_strndup((const gchar *) e->data + 1, e->len - 1);
			ok = steam_ws_send_text(ws, s);
			g_free(s);
		} else {
			ok = steam_ws_send_binary(ws, e->data + 1, e->len - 1);
		}
		CHECK(ok, "echo: send %u failed", i);
	}
}

static void
echo_message(SteamWebSocket *ws, const guint8 *data, gsize len, gboolean binary, gpointer d)
{
	GByteArray *e;

	if (echo_got >= echo_expected->len) {
		fprintf(stderr, "echo: extra message (%" G_GSIZE_FORMAT " bytes) ignored\n", len);
		return;
	}
	e = g_ptr_array_index(echo_expected, echo_got);
	if (binary != (e->data[0] == 'B') || len != e->len - 1 || memcmp(data, e->data + 1, len) != 0) {
		/* echo.websocket.org greets with "Request served by ..." */
		if (!binary && len >= 17 && len < 200 && memcmp(data, "Request served by", 17) == 0) {
			fprintf(stderr, "echo: ignoring greeting: %.*s\n", (int) len, data);
			return;
		}
		CHECK(FALSE, "echo: message %u mismatch (got %s %" G_GSIZE_FORMAT " bytes, expected %s %u)",
		      echo_got, binary ? "binary" : "text", len, e->data[0] == 'B' ? "binary" : "text", e->len - 1);
	} else {
		fprintf(stderr, "echo: message %u ok (%s, %" G_GSIZE_FORMAT " bytes)\n",
		        echo_got, binary ? "binary" : "text", len);
	}
	echo_got++;
	if (echo_got == echo_expected->len) {
		/* Local close: `closed` must not fire. Wait a bit to be sure. */
		steam_ws_close(ws);
		CHECK(!steam_ws_is_connected(ws), "echo: connected after close");
		g_ptr_array_free(echo_expected, TRUE);
		echo_expected = NULL;
		echo_done = TRUE;
		schedule_next(1500);
	}
}

static void
echo_closed(SteamWebSocket *ws, const gchar *error, gpointer d)
{
	if (echo_done) {
		echo_closed_fired = TRUE; /* must not happen after a local close */
		return;
	}
	fprintf(stderr, "echo: closed(\"%s\") from %s\n", error ? error : "(null)", echo_urls[echo_idx].host);
	if (echo_expected) {
		g_ptr_array_free(echo_expected, TRUE);
		echo_expected = NULL;
	}
	steam_ws_free(ws);
	cur = NULL;
	if (echo_idx + 1 < G_N_ELEMENTS(echo_urls)) {
		echo_idx++;
		step--; /* retry this step with the next server */
		schedule_next(0);
	} else {
		CHECK(FALSE, "echo: all echo servers failed");
		schedule_next(0);
	}
}

/* --- step: Steam CM --- */

static void
cm_connected(SteamWebSocket *ws, gpointer d)
{
	fprintf(stderr, "cm: connected (handshake complete), closing from inside the callback\n");
	steam_ws_close(ws);
	steam_ws_free(ws);
	cur = NULL;
	schedule_next(0);
}

static void
cm_message(SteamWebSocket *ws, const guint8 *data, gsize len, gboolean binary, gpointer d)
{
	fprintf(stderr, "cm: unexpected message\n");
}

static void
cm_closed(SteamWebSocket *ws, const gchar *error, gpointer d)
{
	CHECK(FALSE, "cm: closed before connecting: %s", error ? error : "(null)");
	steam_ws_free(ws);
	cur = NULL;
	schedule_next(0);
}

static void
directory_cb(PurpleUtilFetchUrlData *url_data, gpointer d, const gchar *body, gsize len,
             const gchar *error)
{
	static const SteamWebSocketCallbacks cm_cbs = { cm_connected, cm_message, cm_closed };
	JsonParser *parser;
	JsonArray *list;
	gchar *endpoint = NULL;
	gint64 best_load = G_MAXINT64;
	guint i;

	if (!body) {
		CHECK(FALSE, "directory fetch failed: %s", error ? error : "?");
		schedule_next(0);
		return;
	}
	parser = json_parser_new();
	if (!json_parser_load_from_data(parser, body, len, NULL)) {
		CHECK(FALSE, "directory: bad JSON");
		g_object_unref(parser);
		schedule_next(0);
		return;
	}
	list = json_object_get_array_member(json_object_get_object_member(
	               json_node_get_object(json_parser_get_root(parser)), "response"), "serverlist");
	for (i = 0; list && i < json_array_get_length(list); i++) {
		JsonObject *o = json_array_get_object_element(list, i);
		if (g_strcmp0(json_object_get_string_member(o, "type"), "websockets") == 0 &&
		    json_object_get_int_member(o, "load") < best_load) {
			best_load = json_object_get_int_member(o, "load");
			g_free(endpoint);
			endpoint = g_strdup(json_object_get_string_member(o, "endpoint"));
		}
	}
	g_object_unref(parser);

	if (!endpoint) {
		CHECK(FALSE, "directory: no websockets endpoint");
		schedule_next(0);
		return;
	}
	{
		gchar *colon = strrchr(endpoint, ':');
		guint16 port = 443;
		if (colon) {
			*colon = '\0';
			port = atoi(colon + 1);
		}
		fprintf(stderr, "cm: connecting to wss://%s:%u/cmsocket/ (load %" G_GINT64_FORMAT ")\n",
		        endpoint, port, best_load);
		cur = steam_ws_connect(sa, endpoint, port, "/cmsocket/", NULL, &cm_cbs, NULL);
	}
	g_free(endpoint);
}

static void
next_step(void)
{
	step++;
	switch (step) {
	case 1: {
		static const SteamWebSocketCallbacks cbs = { fail_connected, NULL, fail_closed };
		fprintf(stderr, "--- connection failure test (127.0.0.1:1)\n");
		arm_watchdog(35);
		cur = steam_ws_connect(sa, "127.0.0.1", 1, "/", NULL, &cbs, NULL);
		break;
	}
	case 2: {
		static const SteamWebSocketCallbacks cbs = { echo_connected, echo_message, echo_closed };
		fprintf(stderr, "--- echo test: wss://%s%s\n", echo_urls[echo_idx].host, echo_urls[echo_idx].path);
		arm_watchdog(40);
		echo_closed_fired = FALSE;
		cur = steam_ws_connect(sa, echo_urls[echo_idx].host, 443, echo_urls[echo_idx].path,
		                       NULL, &cbs, NULL);
		break;
	}
	case 3:
		/* Entered 1.5 s after the local close of the echo socket. */
		CHECK(!echo_closed_fired, "echo: closed fired after local close");
		if (cur) {
			steam_ws_free(cur);
			cur = NULL;
		}
		fprintf(stderr, "--- Steam CM test\n");
		arm_watchdog(40);
		purple_util_fetch_url("https://api.steampowered.com/ISteamDirectory/GetCMListForConnect/v1/?cellid=0&format=json",
		                      TRUE, NULL, TRUE, directory_cb, NULL);
		break;
	default:
		purple_harness_quit(failures ? 1 : 0);
		break;
	}
}

int
main(int argc, char **argv)
{
	int rc;

	sa = purple_harness_init("steam-ws-test");
	if (getenv("TEST_WS_ECHO_INDEX"))
		echo_idx = MIN((guint) atoi(getenv("TEST_WS_ECHO_INDEX")), G_N_ELEMENTS(echo_urls) - 1);

	offline_tests();

	if (getenv("TEST_WS_OFFLINE") == NULL) {
		next_step();
		purple_harness_run();
	} else {
		purple_harness_quit(failures ? 1 : 0);
	}

	rc = purple_harness_shutdown();
	if (failures)
		rc = 1;
	fprintf(stderr, "%s (%d failure%s)\n", rc == 0 ? "PASS" : "FAIL", failures, failures == 1 ? "" : "s");
	return rc;
}
