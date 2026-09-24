/*
 * Tests for steam_cm (CM session layer).
 *
 *   make -C tests test_cm && ./tests/test_cm
 *
 * 1. Offline: synthetic packets built with steam_msgs encoders are fed
 *    through the steam_cm__test_* hooks: ClientHello/ClientLogon framing,
 *    LogOnResponse, a gzipped Multi (FriendsList + PersonaState +
 *    ClientPlayerNicknameList), IncomingMessage, ServiceMethodResponse for
 *    SendMessage / GetRecentMessages / GetActiveMessageSessions /
 *    GetNicknameList, ClientLoggedOff, logon failure, and steam_cm_free()
 *    from inside callbacks (run the ASan build to check the latter).
 * 2. Live: logon with a bogus refresh token must end in `logon_failed`
 *    with a non-OK EResult (directory, WebSocket, framing, header parsing,
 *    LogOnResponse decoding against the real server).
 * 3. Only if STEAM_TEST_REFRESH_TOKEN is set: full logon, persona ONLINE,
 *    wait for friends list, persona states and nicknames, clean logoff.
 *
 * Set STEAM_DEBUG=1 for libpurple debug output. TEST_CM_OFFLINE=1 runs
 * only part 1.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zlib.h>

#include "purple_harness.h"
#include "steam_cm.h"
#include "steam_msgs.h"

/* Test hooks exported by steam_cm.c (not in steam_cm.h). */
SteamCM *steam_cm__test_new(SteamAccount *sa, const SteamCMCallbacks *callbacks,
                            gpointer user_data, guint64 steamid);
void steam_cm__test_dispatch(SteamCM *cm, const guint8 *data, gsize len);
GPtrArray *steam_cm__test_sent(SteamCM *cm);
void steam_cm__test_connection_lost(SteamCM *cm);

static int failures = 0;
static SteamAccount *sa = NULL;

#define CHECK(cond, ...) do { \
	if (!(cond)) { \
		failures++; \
		printf("  FAIL %s:%d: ", __FILE__, __LINE__); \
		printf(__VA_ARGS__); \
		printf("\n"); \
	} \
} while (0)

#define OUR_STEAMID   G_GUINT64_CONSTANT(76561197960287930)
#define FRIEND_A      G_GUINT64_CONSTANT(76561197960265729)
#define FRIEND_B      G_GUINT64_CONSTANT(76561198000000002)
#define CLAN_ID       G_GUINT64_CONSTANT(103582791429521412)
#define SESSION_ID    123456

/* ------------------------------------------------------------------ */
/* Recorder                                                            */

typedef struct {
	SteamCM *cm;
	GString *log;             /* sequence of callback names */

	int logged_on;
	guint64 logged_on_steamid;
	int logon_failed;
	int disconnected;
	SteamEResult last_eresult;
	gchar *persona_name;

	int friends_list;
	gboolean friends_incremental;
	GArray *friends;          /* SteamCMFriend */

	int persona;
	SteamCMPersona personas[4];   /* deep copies of the first 4 */

	int messages;
	SteamCMMessage msg;

	int nicknames;
	gboolean nick_incremental, nick_removal;
	guint n_nicks;
	guint64 nick_steamid;
	gchar *nick;

	int add_friend;

	int sent_cb;
	SteamEResult sent_eresult;
	guint32 sent_ts;

	int history_cb;
	guint64 history_friend;
	guint history_n;
	gboolean history_more;
	guint32 history_acct0, history_ts0;
	gchar *history_msg0;

	int sessions_cb;
	guint sessions_n;
	guint32 sessions_ts;
	SteamCMMessageSession session0;

	/* free-in-callback switches */
	gboolean free_in_logged_on;
	gboolean free_in_friends_list;
	gboolean free_in_persona;
	gboolean free_in_sent;
	gboolean free_in_disconnected;
	gboolean freed;

	/* live mode */
	gboolean live;
} Rec;

static void
rec_log(Rec *r, const gchar *what)
{
	if (r->log->len)
		g_string_append_c(r->log, ',');
	g_string_append(r->log, what);
}

static void
rec_free_cm(Rec *r)
{
	steam_cm_free(r->cm);
	r->cm = NULL;
	r->freed = TRUE;
}

static void
cb_logged_on(SteamCM *cm, guint64 steamid, gpointer ud)
{
	Rec *r = ud;
	CHECK(!r->freed, "logged_on after free");
	r->logged_on++;
	r->logged_on_steamid = steamid;
	rec_log(r, "logged_on");
	if (r->live) {
		printf("  live: logged on as %" G_GUINT64_FORMAT "\n", steamid);
		steam_cm_set_persona_state(cm, STEAM_PERSONA_ONLINE, NULL);
	}
	if (r->free_in_logged_on)
		rec_free_cm(r);
}

static void
cb_logon_failed(SteamCM *cm, SteamEResult eresult, const gchar *message, gpointer ud)
{
	Rec *r = ud;
	CHECK(!r->freed, "logon_failed after free");
	r->logon_failed++;
	r->last_eresult = eresult;
	rec_log(r, "logon_failed");
	if (r->live) {
		printf("  live: logon_failed eresult=%d (%s) message=%s\n", eresult,
		       steam_eresult_to_string(eresult), message ? message : "(null)");
		purple_harness_quit(0);
	}
}

static void
cb_disconnected(SteamCM *cm, SteamEResult eresult, const gchar *message, gpointer ud)
{
	Rec *r = ud;
	CHECK(!r->freed, "disconnected after free");
	r->disconnected++;
	r->last_eresult = eresult;
	rec_log(r, "disconnected");
	if (r->live) {
		printf("  live: disconnected eresult=%d (%s) message=%s\n", eresult,
		       steam_eresult_to_string(eresult), message ? message : "(null)");
		purple_harness_quit(0);
	}
	if (r->free_in_disconnected)
		rec_free_cm(r);
}

static void
cb_account_info(SteamCM *cm, const gchar *persona_name, gpointer ud)
{
	Rec *r = ud;
	g_free(r->persona_name);
	r->persona_name = g_strdup(persona_name);
	rec_log(r, "account_info");
	if (r->live)
		printf("  live: account persona name: %s\n", persona_name ? persona_name : "(null)");
}

static void
cb_friends_list(SteamCM *cm, gboolean incremental, const SteamCMFriend *friends,
                guint n, gpointer ud)
{
	Rec *r = ud;
	CHECK(!r->freed, "friends_list after free");
	r->friends_list++;
	r->friends_incremental = incremental;
	g_array_set_size(r->friends, 0);
	g_array_append_vals(r->friends, friends, n);
	rec_log(r, "friends_list");
	if (r->live)
		printf("  live: friends list (%s) with %u entries\n", incremental ? "incremental" : "full", n);
	if (r->free_in_friends_list)
		rec_free_cm(r);
}

static void
cb_persona(SteamCM *cm, const SteamCMPersona *p, gpointer ud)
{
	Rec *r = ud;
	CHECK(!r->freed, "persona_state after free");
	if (r->persona < 4) {
		SteamCMPersona *c = &r->personas[r->persona];
		*c = *p;
		c->player_name = g_strdup(p->player_name);
		c->avatar_hash = g_strdup(p->avatar_hash);
		c->game_name = g_strdup(p->game_name);
	}
	r->persona++;
	rec_log(r, "persona");
	if (r->live && r->persona <= 10)
		printf("  live: persona %" G_GUINT64_FORMAT " state=%d%s name=%s avatar=%s game=%u/%s\n",
		       p->steamid, p->state, p->has_state ? "" : "(absent)",
		       p->player_name ? p->player_name : "(null)",
		       p->avatar_hash ? p->avatar_hash : "(null)", p->game_app_id,
		       p->game_name ? p->game_name : "(null)");
	if (r->free_in_persona)
		rec_free_cm(r);
}

static void
cb_message(SteamCM *cm, const SteamCMMessage *m, gpointer ud)
{
	Rec *r = ud;
	r->messages++;
	g_free(r->msg.message);
	r->msg = *m;
	r->msg.message = g_strdup(m->message);
	rec_log(r, "message");
}

static void
cb_nicknames(SteamCM *cm, const SteamCMNickname *n, guint count, gboolean incremental,
             gboolean removal, gpointer ud)
{
	Rec *r = ud;
	r->nicknames++;
	r->nick_incremental = incremental;
	r->nick_removal = removal;
	r->n_nicks = count;
	g_free(r->nick);
	r->nick = count ? g_strdup(n[0].nickname) : NULL;
	r->nick_steamid = count ? n[0].steamid : 0;
	rec_log(r, "nicknames");
	if (r->live)
		printf("  live: %u nicknames (incremental %d)\n", count, incremental);
}

static void
cb_add_friend(SteamCM *cm, SteamEResult eresult, guint64 steamid, const gchar *name, gpointer ud)
{
	Rec *r = ud;
	r->add_friend++;
	rec_log(r, "add_friend");
}

static const SteamCMCallbacks rec_callbacks = {
	cb_logged_on, cb_logon_failed, cb_disconnected, cb_account_info,
	cb_friends_list, cb_persona, cb_message, cb_nicknames, cb_add_friend,
};

static Rec *
rec_new(void)
{
	Rec *r = g_new0(Rec, 1);
	r->log = g_string_new(NULL);
	r->friends = g_array_new(FALSE, TRUE, sizeof(SteamCMFriend));
	return r;
}

static void
rec_free(Rec *r)
{
	int i;

	if (r->cm)
		steam_cm_free(r->cm);
	for (i = 0; i < 4 && i < r->persona; i++) {
		g_free(r->personas[i].player_name);
		g_free(r->personas[i].avatar_hash);
		g_free(r->personas[i].game_name);
	}
	g_string_free(r->log, TRUE);
	g_array_free(r->friends, TRUE);
	g_free(r->persona_name);
	g_free(r->msg.message);
	g_free(r->nick);
	g_free(r->history_msg0);
	g_free(r);
}

/* ------------------------------------------------------------------ */
/* Packet builders                                                     */

static GByteArray *
packet(guint32 emsg, const SteamMsgProtoBufHeader *hdr, GByteArray *body)
{
	GByteArray *out = g_byte_array_new();
	steam_msg_packet_build(emsg, hdr, body, out);
	if (body)
		g_byte_array_unref(body);
	return out;
}

static void
feed(SteamCM *cm, GByteArray *pkt)
{
	steam_cm__test_dispatch(cm, pkt->data, pkt->len);
	g_byte_array_unref(pkt);
}

static GByteArray *
build_logon_response(gint32 eresult)
{
	SteamMsgProtoBufHeader hdr;
	SteamMsgClientLogonResponse m;
	GByteArray *body = g_byte_array_new(), *pkt;

	steam_msg_protobuf_header_init(&hdr);
	STEAM_MSG_SET(&hdr, steamid, OUR_STEAMID);
	STEAM_MSG_SET(&hdr, client_sessionid, SESSION_ID);
	steam_msg_client_logon_response_init(&m);
	STEAM_MSG_SET(&m, eresult, eresult);
	STEAM_MSG_SET(&m, heartbeat_seconds, 9);
	steam_msg_client_logon_response_encode(&m, body);
	steam_msg_client_logon_response_clear(&m);
	pkt = packet(STEAM_EMSG_CLIENT_LOG_ON_RESPONSE, &hdr, body);
	steam_msg_protobuf_header_clear(&hdr);
	return pkt;
}

static GByteArray *
build_friends_list(void)
{
	SteamMsgClientFriendsList m;
	SteamMsgClientFriendsListFriend f;
	GByteArray *body = g_byte_array_new();

	steam_msg_client_friends_list_init(&m);
	STEAM_MSG_SET(&m, bincremental, FALSE);
	memset(&f, 0, sizeof(f));
	STEAM_MSG_SET(&f, ulfriendid, FRIEND_A);
	STEAM_MSG_SET(&f, efriendrelationship, STEAM_RELATIONSHIP_FRIEND);
	g_array_append_val(m.friends, f);
	memset(&f, 0, sizeof(f));
	STEAM_MSG_SET(&f, ulfriendid, CLAN_ID);
	STEAM_MSG_SET(&f, efriendrelationship, 3);
	g_array_append_val(m.friends, f);
	memset(&f, 0, sizeof(f));
	STEAM_MSG_SET(&f, ulfriendid, FRIEND_B);
	STEAM_MSG_SET(&f, efriendrelationship, STEAM_RELATIONSHIP_REQUEST_RECIPIENT);
	g_array_append_val(m.friends, f);
	steam_msg_client_friends_list_encode(&m, body);
	steam_msg_client_friends_list_clear(&m);
	return packet(STEAM_EMSG_CLIENT_FRIENDS_LIST, NULL, body);
}

static GByteArray *
build_persona_state(void)
{
	SteamMsgClientPersonaState m;
	SteamMsgPersonaFriend *f;
	GByteArray *body = g_byte_array_new();
	guint8 hash[20];
	int i;

	steam_msg_client_persona_state_init(&m);
	STEAM_MSG_SET(&m, status_flags, STEAM_PERSONA_REQ_DEFAULT);

	f = steam_msg_client_persona_state_add_friend(&m);
	STEAM_MSG_SET(f, friendid, FRIEND_A);
	STEAM_MSG_SET(f, persona_state, STEAM_PERSONA_AWAY);
	STEAM_MSG_SET(f, persona_state_flags, STEAM_PERSONA_FLAG_CLIENT_WEB);
	STEAM_MSG_SET(f, game_played_app_id, 440);
	STEAM_MSG_SET(f, game_server_ip, 0x7f000001);
	STEAM_MSG_SET(f, game_server_port, 27015);
	f->player_name = g_strdup("Alice");
	for (i = 0; i < 20; i++)
		hash[i] = (guint8)(0xa0 + i);
	f->avatar_hash = g_byte_array_new();
	g_byte_array_append(f->avatar_hash, hash, 20);
	STEAM_MSG_SET(f, last_logoff, 1000);
	STEAM_MSG_SET(f, last_logon, 2000);
	f->game_name = g_strdup("Team Fortress 2");
	STEAM_MSG_SET(f, gameid, 440);
	STEAM_MSG_SET(f, game_lobby_id, G_GUINT64_CONSTANT(109775240000000000));

	f = steam_msg_client_persona_state_add_friend(&m);
	STEAM_MSG_SET(f, friendid, FRIEND_B);
	memset(hash, 0, sizeof(hash));
	f->avatar_hash = g_byte_array_new();
	g_byte_array_append(f->avatar_hash, hash, 20);

	steam_msg_client_persona_state_encode(&m, body);
	steam_msg_client_persona_state_clear(&m);
	return packet(STEAM_EMSG_CLIENT_PERSONA_STATE, NULL, body);
}

static GByteArray *
build_nickname_list(void)
{
	SteamMsgClientPlayerNicknameList m;
	SteamMsgClientPlayerNickname n;
	GByteArray *body = g_byte_array_new();

	steam_msg_client_player_nickname_list_init(&m);
	memset(&n, 0, sizeof(n));
	STEAM_MSG_SET(&n, steamid, FRIEND_A);
	n.nickname = g_strdup("Ally");
	g_array_append_val(m.nicknames, n);
	steam_msg_client_player_nickname_list_encode(&m, body);
	steam_msg_client_player_nickname_list_clear(&m);
	return packet(STEAM_EMSG_CLIENT_PLAYER_NICKNAME_LIST, NULL, body);
}

/* Wraps packets into a (gzipped when `gzip`) Multi packet. Consumes them. */
static GByteArray *
build_multi(GByteArray **pkts, int n, gboolean gzip)
{
	GByteArray *payload = g_byte_array_new(), *body = g_byte_array_new();
	SteamMsgMulti m;
	int i;

	for (i = 0; i < n; i++) {
		guint8 le[4];
		guint32 len = pkts[i]->len;
		le[0] = len & 0xff; le[1] = (len >> 8) & 0xff;
		le[2] = (len >> 16) & 0xff; le[3] = (len >> 24) & 0xff;
		g_byte_array_append(payload, le, 4);
		g_byte_array_append(payload, pkts[i]->data, pkts[i]->len);
		g_byte_array_unref(pkts[i]);
	}

	steam_msg_multi_init(&m);
	if (gzip) {
		z_stream zs;
		GByteArray *z = g_byte_array_new();
		g_byte_array_set_size(z, payload->len + 256);
		memset(&zs, 0, sizeof(zs));
		deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, MAX_WBITS + 16, 8, Z_DEFAULT_STRATEGY);
		zs.next_in = payload->data;
		zs.avail_in = payload->len;
		zs.next_out = z->data;
		zs.avail_out = z->len;
		deflate(&zs, Z_FINISH);
		g_byte_array_set_size(z, zs.total_out);
		deflateEnd(&zs);
		STEAM_MSG_SET(&m, size_unzipped, payload->len);
		m.message_body = z;
	} else {
		m.message_body = g_byte_array_ref(payload);
	}
	steam_msg_multi_encode(&m, body);
	steam_msg_multi_clear(&m);
	g_byte_array_unref(payload);
	return packet(STEAM_EMSG_MULTI, NULL, body);
}

static GByteArray *
build_incoming_message(void)
{
	SteamMsgProtoBufHeader hdr;
	SteamMsgFriendMessagesIncomingMessage m;
	GByteArray *body = g_byte_array_new(), *pkt;

	steam_msg_protobuf_header_init(&hdr);
	hdr.target_job_name = g_strdup(STEAM_NOTIFY_FRIEND_MESSAGES_INCOMING_MESSAGE);
	steam_msg_friend_messages_incoming_message_init(&m);
	STEAM_MSG_SET(&m, steamid_friend, FRIEND_A);
	STEAM_MSG_SET(&m, chat_entry_type, STEAM_CHAT_ENTRY_CHAT_MSG);
	m.message = g_strdup("[b]hello[/b]");
	m.message_no_bbcode = g_strdup("hello");
	STEAM_MSG_SET(&m, rtime32_server_timestamp, 1700000123);
	STEAM_MSG_SET(&m, local_echo, TRUE);
	steam_msg_friend_messages_incoming_message_encode(&m, body);
	steam_msg_friend_messages_incoming_message_clear(&m);
	pkt = packet(STEAM_EMSG_SERVICE_METHOD, &hdr, body);
	steam_msg_protobuf_header_clear(&hdr);
	return pkt;
}

static GByteArray *
build_service_response(guint64 jobid, gint32 eresult, GByteArray *body)
{
	SteamMsgProtoBufHeader hdr;
	GByteArray *pkt;

	steam_msg_protobuf_header_init(&hdr);
	STEAM_MSG_SET(&hdr, jobid_target, jobid);
	STEAM_MSG_SET(&hdr, eresult, eresult);
	pkt = packet(STEAM_EMSG_SERVICE_METHOD_RESPONSE, &hdr, body);
	steam_msg_protobuf_header_clear(&hdr);
	return pkt;
}

static GByteArray *
build_logged_off(gint32 eresult)
{
	SteamMsgClientLoggedOff m;
	GByteArray *body = g_byte_array_new();

	steam_msg_client_logged_off_init(&m);
	STEAM_MSG_SET(&m, eresult, eresult);
	steam_msg_client_logged_off_encode(&m, body);
	steam_msg_client_logged_off_clear(&m);
	return packet(STEAM_EMSG_CLIENT_LOGGED_OFF, NULL, body);
}

/* Parses sent packet `idx`; returns its EMsg, fills hdr/body. */
static guint32
sent_packet(SteamCM *cm, guint idx, SteamMsgProtoBufHeader *hdr,
            const guint8 **body, gsize *body_len)
{
	GPtrArray *sent = steam_cm__test_sent(cm);
	GByteArray *pkt;
	guint32 emsg = 0;

	if (idx >= sent->len) {
		steam_msg_protobuf_header_init(hdr);
		return 0;
	}
	pkt = g_ptr_array_index(sent, idx);
	if (!steam_msg_packet_parse(pkt->data, pkt->len, &emsg, hdr, body, body_len))
		return 0;
	return emsg;
}

/* Returns the jobid_source of the last sent packet (a service call). */
static guint64
last_job(SteamCM *cm, const gchar *method)
{
	SteamMsgProtoBufHeader hdr;
	const guint8 *body;
	gsize len;
	guint32 emsg;
	guint64 jobid;

	emsg = sent_packet(cm, steam_cm__test_sent(cm)->len - 1, &hdr, &body, &len);
	CHECK(emsg == STEAM_EMSG_SERVICE_METHOD_CALL_FROM_CLIENT, "service call emsg %u", emsg);
	CHECK(hdr.target_job_name && strcmp(hdr.target_job_name, method) == 0,
	      "target_job_name %s", hdr.target_job_name ? hdr.target_job_name : "(null)");
	CHECK(hdr.has_jobid_source && hdr.jobid_source != STEAM_JOBID_NONE && hdr.jobid_source != 0,
	      "jobid_source set");
	CHECK(hdr.steamid == OUR_STEAMID && hdr.client_sessionid == SESSION_ID,
	      "service call header carries session");
	jobid = hdr.jobid_source;
	steam_msg_protobuf_header_clear(&hdr);
	return jobid;
}

/* ------------------------------------------------------------------ */
/* Offline tests                                                       */

static void
sent_cb(SteamCM *cm, SteamEResult eresult, guint32 ts, gpointer ud)
{
	Rec *r = ud;
	CHECK(!r->freed, "send callback after free");
	r->sent_cb++;
	r->sent_eresult = eresult;
	r->sent_ts = ts;
	rec_log(r, "sent");
	if (r->free_in_sent)
		rec_free_cm(r);
}

static void
history_cb(SteamCM *cm, guint64 friend_steamid, const SteamCMHistoryMessage *msgs, guint n,
           gboolean more, gpointer ud)
{
	Rec *r = ud;
	r->history_cb++;
	r->history_friend = friend_steamid;
	r->history_n = n;
	r->history_more = more;
	if (n > 0) {
		r->history_acct0 = msgs[0].accountid;
		r->history_ts0 = msgs[0].timestamp;
		g_free(r->history_msg0);
		r->history_msg0 = g_strdup(msgs[0].message);
	}
	rec_log(r, "history");
}

static void
sessions_cb(SteamCM *cm, const SteamCMMessageSession *s, guint n, guint32 ts, gpointer ud)
{
	Rec *r = ud;
	r->sessions_cb++;
	r->sessions_n = n;
	r->sessions_ts = ts;
	if (n > 0)
		r->session0 = s[0];
	rec_log(r, "sessions");
}

static void
test_logon_packets(Rec *r)
{
	SteamMsgProtoBufHeader hdr;
	SteamMsgClientHello hello;
	SteamMsgClientLogon logon;
	const guint8 *body;
	gsize len;
	guint32 emsg;

	printf("offline: ClientHello / ClientLogon\n");
	CHECK(steam_cm__test_sent(r->cm)->len == 2, "2 packets sent at start, got %u",
	      steam_cm__test_sent(r->cm)->len);

	emsg = sent_packet(r->cm, 0, &hdr, &body, &len);
	CHECK(emsg == STEAM_EMSG_CLIENT_HELLO, "first packet is ClientHello (%u)", emsg);
	steam_msg_client_hello_init(&hello);
	CHECK(steam_msg_client_hello_decode(&hello, body, len), "hello decodes");
	CHECK(hello.protocol_version == STEAM_PROTOCOL_VERSION, "hello protocol_version");
	steam_msg_client_hello_clear(&hello);
	steam_msg_protobuf_header_clear(&hdr);

	emsg = sent_packet(r->cm, 1, &hdr, &body, &len);
	CHECK(emsg == STEAM_EMSG_CLIENT_LOGON, "second packet is ClientLogon (%u)", emsg);
	CHECK(hdr.has_steamid && hdr.steamid == OUR_STEAMID, "logon header steamid");
	CHECK(hdr.has_client_sessionid && hdr.client_sessionid == 0, "logon header sessionid 0");
	steam_msg_client_logon_init(&logon);
	CHECK(steam_msg_client_logon_decode(&logon, body, len), "logon decodes");
	CHECK(logon.protocol_version == 65580, "protocol_version");
	CHECK(logon.client_package_version == 1771, "client_package_version");
	CHECK(logon.client_language && strcmp(logon.client_language, "english") == 0, "client_language");
	CHECK(logon.client_os_type == STEAM_OS_TYPE_LINUX_UNKNOWN, "client_os_type");
	CHECK(logon.has_should_remember_password && logon.should_remember_password, "remember password");
	CHECK(logon.has_supports_rate_limit_response && logon.supports_rate_limit_response, "rate limit response");
	CHECK(logon.access_token && strcmp(logon.access_token, "test.token.value") == 0, "access_token");
	CHECK(logon.chat_mode == 2, "chat_mode");
	CHECK(logon.has_cell_id && logon.cell_id == 0, "cell_id");
	CHECK(logon.machine_name && strcmp(logon.machine_name, "Pidgin") == 0, "machine_name");
	CHECK(logon.account_name && strcmp(logon.account_name, purple_account_get_username(sa->account)) == 0,
	      "account_name");
	CHECK(logon.has_obfuscated_private_ip &&
	      (logon.obfuscated_private_ip ^ 0xBAADF00Du) ==
	      (guint32) purple_account_get_int(sa->account, "logon_id", 0) &&
	      purple_account_get_int(sa->account, "logon_id", 0) != 0, "obfuscated_private_ip");
	CHECK(logon.machine_id && logon.machine_id->len == 155 &&
	      memcmp(logon.machine_id->data, "\0MessageObject\0\1BB3\0", 20) == 0 &&
	      logon.machine_id->data[153] == 8 && logon.machine_id->data[154] == 8, "machine_id MessageObject");
	steam_msg_client_logon_clear(&logon);
	steam_msg_protobuf_header_clear(&hdr);
}

static void
test_session(void)
{
	Rec *r = rec_new();
	GByteArray *pkts[3];
	SteamMsgProtoBufHeader hdr;
	const guint8 *body;
	gsize len;
	guint32 emsg;
	guint64 jobid;
	guint nsent;

	printf("offline: full session\n");
	r->cm = steam_cm__test_new(sa, &rec_callbacks, r, OUR_STEAMID);
	test_logon_packets(r);

	/* Calls before logon are no-ops */
	nsent = steam_cm__test_sent(r->cm)->len;
	steam_cm_send_message(r->cm, FRIEND_A, STEAM_CHAT_ENTRY_CHAT_MSG, "x", sent_cb, r);
	steam_cm_set_persona_state(r->cm, STEAM_PERSONA_ONLINE, NULL);
	steam_cm_get_nickname_list(r->cm);
	CHECK(steam_cm__test_sent(r->cm)->len == nsent, "no packets sent before logon");
	CHECK(!steam_cm_is_logged_on(r->cm), "not logged on yet");
	CHECK(r->sent_cb == 0, "no send callback before logon");

	/* Logon OK */
	feed(r->cm, build_logon_response(STEAM_ERESULT_OK));
	CHECK(r->logged_on == 1, "logged_on fired");
	CHECK(r->logged_on_steamid == OUR_STEAMID, "logged_on steamid");
	CHECK(steam_cm_is_logged_on(r->cm), "is_logged_on");
	CHECK(steam_cm_get_steamid(r->cm) == OUR_STEAMID, "get_steamid");

	/* Persona state after logon: carries the session */
	nsent = steam_cm__test_sent(r->cm)->len;
	steam_cm_set_persona_state(r->cm, STEAM_PERSONA_ONLINE, "Me");
	emsg = sent_packet(r->cm, nsent, &hdr, &body, &len);
	CHECK(emsg == STEAM_EMSG_CLIENT_CHANGE_STATUS, "ChangeStatus sent (%u)", emsg);
	CHECK(hdr.steamid == OUR_STEAMID && hdr.client_sessionid == SESSION_ID, "ChangeStatus header session");
	{
		SteamMsgClientChangeStatus cs;
		steam_msg_client_change_status_init(&cs);
		CHECK(steam_msg_client_change_status_decode(&cs, body, len), "ChangeStatus decodes");
		CHECK(cs.has_persona_state && cs.persona_state == STEAM_PERSONA_ONLINE, "persona_state");
		CHECK(cs.player_name && strcmp(cs.player_name, "Me") == 0, "player_name");
		CHECK(cs.has_persona_set_by_user && cs.persona_set_by_user, "persona_set_by_user");
		steam_msg_client_change_status_clear(&cs);
	}
	steam_msg_protobuf_header_clear(&hdr);

	/* Gzipped Multi: FriendsList + PersonaState + NicknameList */
	nsent = steam_cm__test_sent(r->cm)->len;
	pkts[0] = build_friends_list();
	pkts[1] = build_persona_state();
	pkts[2] = build_nickname_list();
	feed(r->cm, build_multi(pkts, 3, TRUE));

	CHECK(strcmp(r->log->str, "logged_on,friends_list,persona,persona,nicknames") == 0,
	      "callback order: %s", r->log->str);
	CHECK(r->friends_list == 1 && !r->friends_incremental, "full friends list");
	CHECK(r->friends->len == 2, "clan filtered: %u friends", r->friends->len);
	if (r->friends->len == 2) {
		SteamCMFriend *f0 = &g_array_index(r->friends, SteamCMFriend, 0);
		SteamCMFriend *f1 = &g_array_index(r->friends, SteamCMFriend, 1);
		CHECK(f0->steamid == FRIEND_A && f0->relationship == STEAM_RELATIONSHIP_FRIEND, "friend A");
		CHECK(f1->steamid == FRIEND_B && f1->relationship == STEAM_RELATIONSHIP_REQUEST_RECIPIENT, "friend B");
	}

	/* The CM asked for friend data for both */
	emsg = sent_packet(r->cm, nsent, &hdr, &body, &len);
	CHECK(emsg == STEAM_EMSG_CLIENT_REQUEST_FRIEND_DATA, "RequestFriendData sent (%u)", emsg);
	{
		SteamMsgClientRequestFriendData rq;
		steam_msg_client_request_friend_data_init(&rq);
		CHECK(steam_msg_client_request_friend_data_decode(&rq, body, len), "RequestFriendData decodes");
		CHECK(rq.persona_state_requested == STEAM_PERSONA_REQ_DEFAULT, "request flags %u", rq.persona_state_requested);
		CHECK(rq.friends->len == 2 && g_array_index(rq.friends, guint64, 0) == FRIEND_A &&
		      g_array_index(rq.friends, guint64, 1) == FRIEND_B, "requested steamids");
		steam_msg_client_request_friend_data_clear(&rq);
	}
	steam_msg_protobuf_header_clear(&hdr);

	CHECK(r->persona == 2, "two persona callbacks (%d)", r->persona);
	if (r->persona >= 2) {
		SteamCMPersona *a = &r->personas[0], *b = &r->personas[1];
		CHECK(a->steamid == FRIEND_A, "persona A steamid");
		CHECK(a->has_state && a->state == STEAM_PERSONA_AWAY, "persona A state");
		CHECK(a->state_flags == STEAM_PERSONA_FLAG_CLIENT_WEB, "persona A flags");
		CHECK(a->player_name && strcmp(a->player_name, "Alice") == 0, "persona A name");
		CHECK(a->avatar_hash && strcmp(a->avatar_hash, "a0a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b3") == 0,
		      "persona A avatar %s", a->avatar_hash ? a->avatar_hash : "(null)");
		CHECK(a->has_game && a->game_app_id == 440 && a->gameid == 440, "persona A game");
		CHECK(a->game_name && strcmp(a->game_name, "Team Fortress 2") == 0, "persona A game name");
		CHECK(a->game_lobby_id == G_GUINT64_CONSTANT(109775240000000000), "persona A lobby");
		CHECK(a->game_server_ip == 0x7f000001 && a->game_server_port == 27015, "persona A server");
		CHECK(a->game_server_steamid == 0, "persona A server steamid 0");
		CHECK(a->last_logoff == 1000 && a->last_logon == 2000, "persona A last logoff/logon");

		CHECK(b->steamid == FRIEND_B, "persona B steamid");
		CHECK(!b->has_state, "persona B has no state");
		CHECK(b->player_name == NULL, "persona B no name");
		CHECK(b->avatar_hash && strcmp(b->avatar_hash, "") == 0, "persona B default avatar");
		CHECK(!b->has_game && b->game_name == NULL, "persona B not in game");
		CHECK(b->last_logoff == 0 && b->last_logon == 0, "persona B no times");
	}

	CHECK(r->nicknames == 1 && r->n_nicks == 1, "nicknames callback");
	CHECK(r->nick_steamid == FRIEND_A && r->nick && strcmp(r->nick, "Ally") == 0, "nickname value");
	CHECK(!r->nick_incremental && !r->nick_removal, "nickname flags");

	/* A Presence-only update (state, no game fields) means "not in game" */
	{
		SteamMsgClientPersonaState m;
		SteamMsgPersonaFriend *f;
		GByteArray *pbody = g_byte_array_new();

		steam_msg_client_persona_state_init(&m);
		STEAM_MSG_SET(&m, status_flags, STEAM_PERSONA_REQ_PRESENCE);
		f = steam_msg_client_persona_state_add_friend(&m);
		STEAM_MSG_SET(f, friendid, FRIEND_A);
		STEAM_MSG_SET(f, persona_state, STEAM_PERSONA_ONLINE);
		steam_msg_client_persona_state_encode(&m, pbody);
		steam_msg_client_persona_state_clear(&m);
		feed(r->cm, packet(STEAM_EMSG_CLIENT_PERSONA_STATE, NULL, pbody));
	}
	CHECK(r->persona == 3, "presence-only persona callback (%d)", r->persona);
	if (r->persona >= 3) {
		SteamCMPersona *c = &r->personas[2];
		CHECK(c->steamid == FRIEND_A && c->has_state && c->state == STEAM_PERSONA_ONLINE,
		      "presence-only state");
		CHECK(c->has_game && c->game_app_id == 0 && c->gameid == 0 && c->game_name == NULL,
		      "presence-only update clears the game");
	}

	/* IncomingMessage */
	feed(r->cm, build_incoming_message());
	CHECK(r->messages == 1, "message callback");
	CHECK(r->msg.from_steamid == FRIEND_A, "message from");
	CHECK(r->msg.type == STEAM_CHAT_ENTRY_CHAT_MSG, "message type");
	CHECK(r->msg.message && strcmp(r->msg.message, "hello") == 0, "message text prefers no_bbcode");
	CHECK(r->msg.timestamp == 1700000123, "message timestamp");
	CHECK(r->msg.local_echo, "message local_echo");

	/* SendMessage + ServiceMethodResponse */
	steam_cm_send_message(r->cm, FRIEND_B, STEAM_CHAT_ENTRY_CHAT_MSG, "hi there", sent_cb, r);
	jobid = last_job(r->cm, STEAM_METHOD_FRIEND_MESSAGES_SEND_MESSAGE);
	emsg = sent_packet(r->cm, steam_cm__test_sent(r->cm)->len - 1, &hdr, &body, &len);
	{
		SteamMsgFriendMessagesSendMessageRequest rq;
		steam_msg_friend_messages_send_message_request_init(&rq);
		CHECK(steam_msg_friend_messages_send_message_request_decode(&rq, body, len), "SendMessage decodes");
		CHECK(rq.steamid == FRIEND_B && rq.chat_entry_type == STEAM_CHAT_ENTRY_CHAT_MSG, "SendMessage target/type");
		CHECK(rq.message && strcmp(rq.message, "hi there") == 0, "SendMessage text");
		CHECK(rq.has_contains_bbcode && !rq.contains_bbcode, "contains_bbcode false");
		CHECK(rq.has_echo_to_sender && !rq.echo_to_sender, "echo_to_sender false");
		CHECK(rq.client_message_id && *rq.client_message_id, "client_message_id");
		steam_msg_friend_messages_send_message_request_clear(&rq);
	}
	steam_msg_protobuf_header_clear(&hdr);
	{
		SteamMsgFriendMessagesSendMessageResponse resp;
		GByteArray *b = g_byte_array_new();
		steam_msg_friend_messages_send_message_response_init(&resp);
		STEAM_MSG_SET(&resp, server_timestamp, 1700000200);
		steam_msg_friend_messages_send_message_response_encode(&resp, b);
		steam_msg_friend_messages_send_message_response_clear(&resp);
		/* a response for an unknown job is ignored */
		feed(r->cm, build_service_response(jobid + 1000, STEAM_ERESULT_OK, NULL));
		CHECK(r->sent_cb == 0, "unknown job ignored");
		feed(r->cm, build_service_response(jobid, STEAM_ERESULT_OK, b));
	}
	CHECK(r->sent_cb == 1 && r->sent_eresult == STEAM_ERESULT_OK && r->sent_ts == 1700000200,
	      "send callback (%d, eresult %d, ts %u)", r->sent_cb, r->sent_eresult, r->sent_ts);
	/* duplicate response: job already completed */
	feed(r->cm, build_service_response(jobid, STEAM_ERESULT_OK, NULL));
	CHECK(r->sent_cb == 1, "job completes once");

	/* GetRecentMessages */
	steam_cm_get_recent_messages(r->cm, FRIEND_A, 0, 20, history_cb, r);
	jobid = last_job(r->cm, STEAM_METHOD_FRIEND_MESSAGES_GET_RECENT_MESSAGES);
	emsg = sent_packet(r->cm, steam_cm__test_sent(r->cm)->len - 1, &hdr, &body, &len);
	{
		SteamMsgFriendMessagesGetRecentMessagesRequest rq;
		steam_msg_friend_messages_get_recent_messages_request_init(&rq);
		steam_msg_friend_messages_get_recent_messages_request_decode(&rq, body, len);
		CHECK(rq.steamid1 == OUR_STEAMID && rq.steamid2 == FRIEND_A, "history steamids");
		CHECK(rq.count == 20 && rq.has_bbcode_format && !rq.bbcode_format, "history count/bbcode");
		CHECK(rq.has_most_recent_conversation && rq.most_recent_conversation, "most_recent_conversation when since==0");
		CHECK(rq.has_rtime32_start_time && rq.rtime32_start_time == 0, "rtime32_start_time");
		steam_msg_friend_messages_get_recent_messages_request_clear(&rq);
	}
	steam_msg_protobuf_header_clear(&hdr);
	{
		SteamMsgFriendMessagesGetRecentMessagesResponse resp;
		SteamMsgFriendMessage fm;
		GByteArray *b = g_byte_array_new();
		steam_msg_friend_messages_get_recent_messages_response_init(&resp);
		memset(&fm, 0, sizeof(fm));
		STEAM_MSG_SET(&fm, accountid, steam_cm_steamid_to_accountid(FRIEND_A));
		STEAM_MSG_SET(&fm, timestamp, 1700000300);
		fm.message = g_strdup("newest");
		g_array_append_val(resp.messages, fm);
		memset(&fm, 0, sizeof(fm));
		STEAM_MSG_SET(&fm, accountid, steam_cm_steamid_to_accountid(OUR_STEAMID));
		STEAM_MSG_SET(&fm, timestamp, 1700000250);
		fm.message = g_strdup("older");
		g_array_append_val(resp.messages, fm);
		STEAM_MSG_SET(&resp, more_available, TRUE);
		steam_msg_friend_messages_get_recent_messages_response_encode(&resp, b);
		steam_msg_friend_messages_get_recent_messages_response_clear(&resp);
		feed(r->cm, build_service_response(jobid, STEAM_ERESULT_OK, b));
	}
	CHECK(r->history_cb == 1 && r->history_friend == FRIEND_A && r->history_n == 2 && r->history_more,
	      "history callback");
	CHECK(r->history_acct0 == steam_cm_steamid_to_accountid(FRIEND_A) && r->history_ts0 == 1700000300 &&
	      r->history_msg0 && strcmp(r->history_msg0, "newest") == 0, "history first entry (newest)");

	/* GetActiveMessageSessions */
	steam_cm_get_active_message_sessions(r->cm, 1600000000, sessions_cb, r);
	jobid = last_job(r->cm, STEAM_METHOD_FRIEND_MESSAGES_GET_ACTIVE_SESSIONS);
	emsg = sent_packet(r->cm, steam_cm__test_sent(r->cm)->len - 1, &hdr, &body, &len);
	{
		SteamMsgFriendMessagesGetActiveMessageSessionsRequest rq;
		steam_msg_friend_messages_get_active_message_sessions_request_init(&rq);
		steam_msg_friend_messages_get_active_message_sessions_request_decode(&rq, body, len);
		CHECK(rq.lastmessage_since == 1600000000 && rq.only_sessions_with_messages, "sessions request");
		steam_msg_friend_messages_get_active_message_sessions_request_clear(&rq);
	}
	steam_msg_protobuf_header_clear(&hdr);
	{
		SteamMsgFriendMessagesGetActiveMessageSessionsResponse resp;
		SteamMsgFriendMessageSession s;
		GByteArray *b = g_byte_array_new();
		steam_msg_friend_messages_get_active_message_sessions_response_init(&resp);
		memset(&s, 0, sizeof(s));
		STEAM_MSG_SET(&s, accountid_friend, 42);
		STEAM_MSG_SET(&s, last_message, 1700000400);
		STEAM_MSG_SET(&s, last_view, 1700000100);
		STEAM_MSG_SET(&s, unread_message_count, 3);
		g_array_append_val(resp.message_sessions, s);
		STEAM_MSG_SET(&resp, timestamp, 1700000500);
		steam_msg_friend_messages_get_active_message_sessions_response_encode(&resp, b);
		steam_msg_friend_messages_get_active_message_sessions_response_clear(&resp);
		feed(r->cm, build_service_response(jobid, STEAM_ERESULT_OK, b));
	}
	CHECK(r->sessions_cb == 1 && r->sessions_n == 1 && r->sessions_ts == 1700000500, "sessions callback");
	CHECK(r->session0.accountid_friend == 42 && r->session0.last_message == 1700000400 &&
	      r->session0.last_view == 1700000100 && r->session0.unread_message_count == 3, "session values");

	/* Player.GetNicknameList */
	steam_cm_get_nickname_list(r->cm);
	jobid = last_job(r->cm, STEAM_METHOD_PLAYER_GET_NICKNAME_LIST);
	{
		SteamMsgPlayerGetNicknameListResponse resp;
		SteamMsgPlayerNickname n;
		GByteArray *b = g_byte_array_new();
		steam_msg_player_get_nickname_list_response_init(&resp);
		memset(&n, 0, sizeof(n));
		STEAM_MSG_SET(&n, accountid, steam_cm_steamid_to_accountid(FRIEND_B));
		n.nickname = g_strdup("Bobby");
		g_array_append_val(resp.nicknames, n);
		steam_msg_player_get_nickname_list_response_encode(&resp, b);
		steam_msg_player_get_nickname_list_response_clear(&resp);
		feed(r->cm, build_service_response(jobid, STEAM_ERESULT_OK, b));
	}
	CHECK(r->nicknames == 2 && r->n_nicks == 1 && r->nick_steamid == FRIEND_B &&
	      r->nick && strcmp(r->nick, "Bobby") == 0 && !r->nick_incremental && !r->nick_removal,
	      "GetNicknameList -> nicknames callback with steamid64");

	/* Failed service call: eresult from the header */
	r->sent_cb = 0;
	steam_cm_send_message(r->cm, FRIEND_B, STEAM_CHAT_ENTRY_CHAT_MSG, "x", sent_cb, r);
	jobid = last_job(r->cm, STEAM_METHOD_FRIEND_MESSAGES_SEND_MESSAGE);
	feed(r->cm, build_service_response(jobid, STEAM_ERESULT_LIMIT_EXCEEDED, NULL));
	CHECK(r->sent_cb == 1 && r->sent_eresult == STEAM_ERESULT_LIMIT_EXCEEDED && r->sent_ts == 0,
	      "failed send reports header eresult");

	/* Unknown EMsg is ignored */
	feed(r->cm, packet(4242, NULL, NULL));
	feed(r->cm, packet(STEAM_EMSG_CLIENT_LICENSE_LIST, NULL, NULL));

	/* LoggedOff LoggedInElsewhere: pending job fails first, then
	 * disconnected; nothing after that */
	r->sent_cb = 0;
	g_string_truncate(r->log, 0);
	steam_cm_send_message(r->cm, FRIEND_B, STEAM_CHAT_ENTRY_CHAT_MSG, "pending", sent_cb, r);
	feed(r->cm, build_logged_off(STEAM_ERESULT_LOGGED_IN_ELSEWHERE));
	CHECK(strcmp(r->log->str, "sent,disconnected") == 0, "logoff order: %s", r->log->str);
	CHECK(r->sent_eresult == STEAM_ERESULT_NO_CONNECTION, "pending job failed with NO_CONNECTION");
	CHECK(r->disconnected == 1 && r->last_eresult == STEAM_ERESULT_LOGGED_IN_ELSEWHERE, "disconnected eresult");
	CHECK(!steam_cm_is_logged_on(r->cm), "not logged on after logoff");
	feed(r->cm, build_incoming_message());
	feed(r->cm, build_logged_off(STEAM_ERESULT_LOGGED_IN_ELSEWHERE));
	CHECK(r->messages == 1 && r->disconnected == 1, "nothing fires after disconnected");

	rec_free(r); /* steam_cm_free on a finished object */
}

static void
test_logon_failed(void)
{
	Rec *r = rec_new();

	printf("offline: logon failure / retryable logon\n");
	r->cm = steam_cm__test_new(sa, &rec_callbacks, r, OUR_STEAMID);
	feed(r->cm, build_logon_response(STEAM_ERESULT_EXPIRED));
	CHECK(r->logon_failed == 1 && r->last_eresult == STEAM_ERESULT_EXPIRED, "logon_failed EXPIRED");
	CHECK(r->logged_on == 0 && r->disconnected == 0, "no other callbacks");
	feed(r->cm, build_logon_response(STEAM_ERESULT_OK));
	CHECK(r->logged_on == 0 && r->logon_failed == 1, "nothing after logon_failed");
	rec_free(r);

	/* TryAnotherCM: retried internally, no callback; freeing cancels the
	 * reconnect timer */
	r = rec_new();
	r->cm = steam_cm__test_new(sa, &rec_callbacks, r, OUR_STEAMID);
	feed(r->cm, build_logon_response(STEAM_ERESULT_TRY_ANOTHER_CM));
	CHECK(r->log->len == 0, "TryAnotherCM fires no callback (%s)", r->log->str);
	rec_free(r);
}

static void
test_free_in_callbacks(void)
{
	Rec *r;
	GByteArray *pkts[3];

	printf("offline: steam_cm_free inside callbacks\n");

	/* in logged_on, with the logon response inside a Multi followed by more */
	r = rec_new();
	r->free_in_logged_on = TRUE;
	r->cm = steam_cm__test_new(sa, &rec_callbacks, r, OUR_STEAMID);
	{
		SteamCM *cm = r->cm;
		pkts[0] = build_logon_response(STEAM_ERESULT_OK);
		pkts[1] = build_friends_list();
		pkts[2] = build_persona_state();
		feed(cm, build_multi(pkts, 3, FALSE));
	}
	CHECK(strcmp(r->log->str, "logged_on") == 0, "free in logged_on stops dispatch (%s)", r->log->str);
	rec_free(r);

	/* in friends_list inside a gzipped Multi */
	r = rec_new();
	r->free_in_friends_list = TRUE;
	r->cm = steam_cm__test_new(sa, &rec_callbacks, r, OUR_STEAMID);
	feed(r->cm, build_logon_response(STEAM_ERESULT_OK));
	{
		SteamCM *cm = r->cm;
		pkts[0] = build_friends_list();
		pkts[1] = build_persona_state();
		pkts[2] = build_nickname_list();
		feed(cm, build_multi(pkts, 3, TRUE));
	}
	CHECK(strcmp(r->log->str, "logged_on,friends_list") == 0, "free in friends_list (%s)", r->log->str);
	rec_free(r);

	/* in the first of two persona callbacks */
	r = rec_new();
	r->free_in_persona = TRUE;
	r->cm = steam_cm__test_new(sa, &rec_callbacks, r, OUR_STEAMID);
	feed(r->cm, build_logon_response(STEAM_ERESULT_OK));
	{
		SteamCM *cm = r->cm;
		feed(cm, build_persona_state());
	}
	CHECK(r->persona == 1, "free in persona stops the loop (%d)", r->persona);
	rec_free(r);

	/* in a job callback while pending jobs are failed on logoff: the
	 * second job and `disconnected` must not fire */
	r = rec_new();
	r->free_in_sent = TRUE;
	r->cm = steam_cm__test_new(sa, &rec_callbacks, r, OUR_STEAMID);
	feed(r->cm, build_logon_response(STEAM_ERESULT_OK));
	steam_cm_send_message(r->cm, FRIEND_A, STEAM_CHAT_ENTRY_CHAT_MSG, "one", sent_cb, r);
	steam_cm_send_message(r->cm, FRIEND_A, STEAM_CHAT_ENTRY_CHAT_MSG, "two", sent_cb, r);
	{
		SteamCM *cm = r->cm;
		feed(cm, build_logged_off(STEAM_ERESULT_LOGON_SESSION_REPLACED));
	}
	CHECK(r->sent_cb == 1 && r->disconnected == 0, "free in job callback (%s)", r->log->str);
	rec_free(r);

	/* in disconnected */
	r = rec_new();
	r->free_in_disconnected = TRUE;
	r->cm = steam_cm__test_new(sa, &rec_callbacks, r, OUR_STEAMID);
	feed(r->cm, build_logon_response(STEAM_ERESULT_OK));
	{
		SteamCM *cm = r->cm;
		pkts[0] = build_logged_off(STEAM_ERESULT_LOGON_SESSION_REPLACED);
		pkts[1] = build_incoming_message();
		feed(cm, build_multi(pkts, 2, TRUE));
	}
	CHECK(strcmp(r->log->str, "logged_on,disconnected") == 0, "free in disconnected (%s)", r->log->str);
	rec_free(r);

	/* pending jobs dropped silently by steam_cm_free */
	r = rec_new();
	r->cm = steam_cm__test_new(sa, &rec_callbacks, r, OUR_STEAMID);
	feed(r->cm, build_logon_response(STEAM_ERESULT_OK));
	steam_cm_send_message(r->cm, FRIEND_A, STEAM_CHAT_ENTRY_CHAT_MSG, "one", sent_cb, r);
	steam_cm_free(r->cm);
	r->cm = NULL;
	CHECK(r->sent_cb == 0, "free fires no job callbacks");
	rec_free(r);

	/* disconnect sends ClientLogOff, no callbacks */
	r = rec_new();
	r->cm = steam_cm__test_new(sa, &rec_callbacks, r, OUR_STEAMID);
	feed(r->cm, build_logon_response(STEAM_ERESULT_OK));
	steam_cm_send_message(r->cm, FRIEND_A, STEAM_CHAT_ENTRY_CHAT_MSG, "one", sent_cb, r);
	steam_cm_disconnect(r->cm);
	{
		GPtrArray *sent = steam_cm__test_sent(r->cm);
		SteamMsgProtoBufHeader hdr;
		const guint8 *body;
		gsize len;
		guint32 emsg = sent_packet(r->cm, sent->len - 1, &hdr, &body, &len);
		CHECK(emsg == STEAM_EMSG_CLIENT_LOG_OFF, "disconnect sends ClientLogOff (%u)", emsg);
		steam_msg_protobuf_header_clear(&hdr);
	}
	CHECK(strcmp(r->log->str, "logged_on") == 0 && r->sent_cb == 0, "disconnect fires nothing (%s)", r->log->str);
	CHECK(!steam_cm_is_logged_on(r->cm), "not logged on after disconnect");
	rec_free(r);
}

/* ------------------------------------------------------------------ */
/* Live tests                                                          */

static gboolean
live_watchdog(gpointer data)
{
	printf("  FAIL: live test timed out\n");
	failures++;
	purple_harness_quit(1);
	return FALSE;
}

static gboolean
live_real_done(gpointer data)
{
	Rec *r = data;

	printf("  live: %d friends_list, %d persona, %d nicknames callbacks; disconnecting\n",
	       r->friends_list, r->persona, r->nicknames);
	CHECK(r->logged_on >= 1, "real logon succeeded");
	CHECK(r->friends_list >= 1, "friends list received");
	steam_cm_disconnect(r->cm);
	purple_harness_quit(0);
	return FALSE;
}

static void
test_live_bogus(void)
{
	Rec *r = rec_new();
	guint wd;

	printf("live: bogus refresh token\n");
	r->live = TRUE;
	r->cm = steam_cm_new(sa, &rec_callbacks, r);
	steam_cm_connect(r->cm, "bogus.bogus.bogus", OUR_STEAMID);
	wd = g_timeout_add_seconds(120, live_watchdog, NULL);
	purple_harness_run();
	g_source_remove(wd);

	CHECK(r->logon_failed == 1, "logon_failed fired (log: %s)", r->log->str);
	CHECK(r->last_eresult != STEAM_ERESULT_OK, "non-OK eresult %d", r->last_eresult);
	CHECK(r->logged_on == 0, "did not log on");

	/* Reconnect path: reuse the cached server list, simulate a transport
	 * loss while the WebSocket is connecting; after the 1 s back-off the
	 * next server is tried and the logon fails the same way. */
	printf("live: reconnect after a transport loss\n");
	r->logon_failed = 0;
	r->last_eresult = STEAM_ERESULT_OK;
	g_string_truncate(r->log, 0);
	steam_cm_connect(r->cm, "bogus.bogus.bogus", OUR_STEAMID);
	steam_cm__test_connection_lost(r->cm);
	CHECK(r->log->len == 0, "transient loss fires no callback");
	wd = g_timeout_add_seconds(120, live_watchdog, NULL);
	purple_harness_run();
	g_source_remove(wd);
	CHECK(r->logon_failed == 1 && r->last_eresult != STEAM_ERESULT_OK,
	      "logon_failed after reconnect (log: %s)", r->log->str);
	rec_free(r);
}

static guint64
steamid_from_jwt(const gchar *token)
{
	gchar **parts = g_strsplit(token, ".", 3);
	guint64 steamid = 0;

	if (parts[0] && parts[1]) {
		GString *b64 = g_string_new(parts[1]);
		gchar *p;
		guchar *json;
		gsize len;

		for (p = b64->str; *p; p++) {
			if (*p == '-') *p = '+';
			else if (*p == '_') *p = '/';
		}
		while (b64->len % 4)
			g_string_append_c(b64, '=');
		json = g_base64_decode(b64->str, &len);
		if (json) {
			gchar *s = g_strndup((const gchar *) json, len);
			gchar *sub = strstr(s, "\"sub\"");
			if (sub) {
				sub = strchr(sub + 5, '"');
				if (sub)
					steamid = g_ascii_strtoull(sub + 1, NULL, 10);
			}
			g_free(s);
			g_free(json);
		}
		g_string_free(b64, TRUE);
	}
	g_strfreev(parts);
	return steamid;
}

static void
test_live_real(const gchar *token)
{
	Rec *r = rec_new();
	guint wd;
	guint64 steamid = steamid_from_jwt(token);

	printf("live: real refresh token for %" G_GUINT64_FORMAT "\n", steamid);
	r->live = TRUE;
	r->cm = steam_cm_new(sa, &rec_callbacks, r);
	steam_cm_connect(r->cm, token, steamid);
	/* collect for 20 s after start, then log off */
	g_timeout_add_seconds(20, live_real_done, r);
	wd = g_timeout_add_seconds(120, live_watchdog, NULL);
	purple_harness_run();
	g_source_remove(wd);
	rec_free(r);
}

int
main(int argc, char **argv)
{
	int rc;

	sa = purple_harness_init("steam-cm-test");

	test_session();
	test_logon_failed();
	test_free_in_callbacks();

	if (!getenv("TEST_CM_OFFLINE")) {
		test_live_bogus();
		if (getenv("STEAM_TEST_REFRESH_TOKEN") && *getenv("STEAM_TEST_REFRESH_TOKEN"))
			test_live_real(getenv("STEAM_TEST_REFRESH_TOKEN"));
		else
			printf("live: STEAM_TEST_REFRESH_TOKEN not set, skipping real logon\n");
	}

	rc = purple_harness_shutdown();
	printf("%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
	return (failures || rc) ? 1 : 0;
}
