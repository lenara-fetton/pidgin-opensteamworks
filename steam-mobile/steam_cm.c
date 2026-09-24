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
 * Steam CM session layer: directory lookup, WebSocket transport, packet
 * framing, Multi, logon, heartbeat, unified-message jobs and reconnects.
 * See steam_cm.h for the contract and docs/architecture.md for the wire
 * reference.
 *
 * Re-entrancy: every event-loop entry point (WebSocket callbacks, timers,
 * the directory HTTP callback, the test hook) is bracketed by
 * cm_enter()/cm_leave(). The consumer may call steam_cm_free() from inside
 * any callback; the object is then torn down immediately (no further
 * callbacks) and released by the outermost cm_leave().
 */

#include "steam_cm.h"
#include "steam_msgs.h"
#include "steam_ws.h"
#include "steam_connection.h"

#include <string.h>
#include <time.h>

#define CM_DIRECTORY_HOST "api.steampowered.com"
#define CM_DIRECTORY_PATH "/ISteamDirectory/GetCMListForConnect/v1/?cellid=0&format=json"
#define CM_WS_PATH "/cmsocket/"

#define CM_CLIENT_PACKAGE_VERSION 1771
#define CM_CHAT_MODE_NEW 2
#define CM_DEFAULT_HEARTBEAT_SECONDS 9
#define CM_HEARTBEAT_DEAD_INTERVALS 3
#define CM_LOGON_TIMEOUT_SECONDS 30
#define CM_JOB_TIMEOUT_SECONDS 30
#define CM_MAX_FAILURES 8
#define CM_FRIEND_DATA_CHUNK 100
#define CM_MAX_MULTI_NESTING 4

static const guint cm_backoff_seconds[] = { 1, 2, 4, 8, 16, 30 };

typedef enum {
	CM_STATE_IDLE = 0,      /* not connected, nothing scheduled */
	CM_STATE_DIRECTORY,     /* fetching the CM list */
	CM_STATE_CONNECTING,    /* WebSocket handshake in progress */
	CM_STATE_LOGGING_ON,    /* ClientLogon sent, waiting for the response */
	CM_STATE_LOGGED_ON,
	CM_STATE_BACKOFF        /* waiting to reconnect */
} CMState;

typedef struct {
	gchar *host;
	guint16 port;
	gdouble load;
} CMServer;

/* Outstanding directory request. Freed by its HTTP callback, or by
 * steam_cm_free() when the request could be cancelled. */
typedef struct {
	SteamCM *cm;   /* NULL once the CM has gone away */
} CMDirRequest;

typedef struct _CMJob CMJob;

/* body == NULL when the job failed (timeout, disconnect, DestJobFailed). */
typedef void (*CMJobHandler)(SteamCM *cm, CMJob *job, SteamEResult eresult,
                             const guint8 *body, gsize body_len);

struct _CMJob {
	guint64 jobid;          /* hash key points here */
	SteamCM *cm;
	gchar *method;
	CMJobHandler handler;
	gpointer callback;
	gpointer user_data;
	guint64 friend_steamid;
	guint timeout;
};

struct _SteamCM {
	SteamAccount *sa;
	SteamCMCallbacks cb;
	gpointer user_data;

	gchar *refresh_token;
	guint64 steamid;            /* as supplied to steam_cm_connect */

	CMState state;
	guint64 session_steamid;    /* from the logon response header */
	gint32 session_id;          /* client_sessionid from the logon response header */
	gint heartbeat_seconds;
	gint64 last_rx;             /* monotonic time of the last packet received */
	gboolean heartbeat_replies; /* the server answers our heartbeats */
	guint generation;           /* bumped whenever the transport is dropped */

	GPtrArray *servers;         /* CMServer*, sorted by load */
	guint server_index;
	CMDirRequest *dir_req;

	SteamWebSocket *ws;
	guint heartbeat_timer;
	guint logon_timer;
	guint reconnect_timer;
	guint failures;             /* consecutive failed connection attempts */

	GHashTable *jobs;           /* guint64 jobid -> CMJob* */
	guint64 next_jobid;
	guint message_counter;

	gint depth;
	gboolean free_pending;
	gboolean finished;          /* logon_failed/disconnected fired, or disconnected locally */

	gboolean test_mode;
	GPtrArray *test_sent;       /* GByteArray* packets "sent" in test mode */
};

#define CM_ALIVE(cm) (!(cm)->free_pending && !(cm)->finished)

static void cm_really_free(SteamCM *cm);
static void cm_connect_next(SteamCM *cm);

/* ------------------------------------------------------------------ */
/* Entry-point bracketing                                              */

static void
cm_enter(SteamCM *cm)
{
	cm->depth++;
}

/* Returns FALSE if the object has been freed. */
static gboolean
cm_leave(SteamCM *cm)
{
	cm->depth--;
	if (cm->depth == 0 && cm->free_pending) {
		cm_really_free(cm);
		return FALSE;
	}
	return TRUE;
}

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */

static void
cm_server_free(gpointer data)
{
	CMServer *srv = data;

	g_free(srv->host);
	g_free(srv);
}

static const gchar *
cm_emsg_name(guint32 emsg)
{
	const gchar *name = steam_emsg_to_string(emsg);
	return name ? name : "unknown";
}

static const gchar *
cm_eresult_name(gint eresult)
{
	const gchar *name = steam_eresult_to_string(eresult);
	return name ? name : "unknown";
}

static gboolean
cm_is_logged_on(const SteamCM *cm)
{
	return cm->state == CM_STATE_LOGGED_ON;
}

static gchar *
cm_avatar_hex(const GByteArray *hash)
{
	GString *str;
	gboolean all_zero = TRUE;
	guint i;

	for (i = 0; i < hash->len; i++) {
		if (hash->data[i] != 0) {
			all_zero = FALSE;
			break;
		}
	}
	if (all_zero)
		return g_strdup("");

	str = g_string_sized_new(hash->len * 2);
	for (i = 0; i < hash->len; i++)
		g_string_append_printf(str, "%02x", hash->data[i]);
	return g_string_free(str, FALSE);
}

/* ------------------------------------------------------------------ */
/* Sending                                                             */

/* Frames and sends one packet. `body` may be NULL. After logon every
 * header carries our SteamID and session id; ClientLogon carries our
 * SteamID and session id 0; ClientHello goes with an empty header. */
static gboolean
cm_send(SteamCM *cm, guint32 emsg, const gchar *job_name, guint64 jobid,
        const GByteArray *body)
{
	SteamMsgProtoBufHeader hdr;
	GByteArray *pkt;
	gboolean ok;

	steam_msg_protobuf_header_init(&hdr);
	if (emsg != STEAM_EMSG_CLIENT_HELLO) {
		if (cm_is_logged_on(cm)) {
			STEAM_MSG_SET(&hdr, steamid, cm->session_steamid);
			STEAM_MSG_SET(&hdr, client_sessionid, cm->session_id);
		} else {
			STEAM_MSG_SET(&hdr, steamid, cm->steamid);
			STEAM_MSG_SET(&hdr, client_sessionid, 0);
		}
	}
	if (job_name != NULL)
		hdr.target_job_name = g_strdup(job_name);
	if (jobid != STEAM_JOBID_NONE)
		STEAM_MSG_SET(&hdr, jobid_source, jobid);

	pkt = g_byte_array_new();
	steam_msg_packet_build(emsg, &hdr, body, pkt);
	steam_msg_protobuf_header_clear(&hdr);

	purple_debug_misc("steam", "CM send %s (%u)%s%s, %u bytes\n", cm_emsg_name(emsg), emsg,
	                  job_name ? " " : "", job_name ? job_name : "", pkt->len);

	if (cm->test_mode) {
		g_ptr_array_add(cm->test_sent, pkt);
		return TRUE;
	}

	ok = cm->ws != NULL && steam_ws_send_binary(cm->ws, pkt->data, pkt->len);
	if (!ok)
		purple_debug_warning("steam", "CM: could not send %s\n", cm_emsg_name(emsg));
	g_byte_array_unref(pkt);
	return ok;
}

/* ------------------------------------------------------------------ */
/* Jobs (unified service method calls)                                 */

static void
cm_job_free(CMJob *job)
{
	if (job->timeout)
		purple_timeout_remove(job->timeout);
	g_free(job->method);
	g_free(job);
}

/* Removes the job from the table and runs its handler once. */
static void
cm_job_complete(SteamCM *cm, CMJob *job, SteamEResult eresult,
                const guint8 *body, gsize body_len)
{
	g_hash_table_steal(cm->jobs, &job->jobid);
	if (job->timeout) {
		purple_timeout_remove(job->timeout);
		job->timeout = 0;
	}
	if (CM_ALIVE(cm) && job->handler)
		job->handler(cm, job, eresult, body, body_len);
	cm_job_free(job);
}

static gboolean
cm_job_timeout_cb(gpointer data)
{
	CMJob *job = data;
	SteamCM *cm = job->cm;

	job->timeout = 0;
	purple_debug_warning("steam", "CM job %" G_GUINT64_FORMAT " (%s) timed out\n",
	                     job->jobid, job->method);
	cm_enter(cm);
	cm_job_complete(cm, job, STEAM_ERESULT_TIMEOUT, NULL, 0);
	cm_leave(cm);
	return FALSE;
}

/* Fails every pending job with `eresult` (callbacks fire) or, when
 * `notify` is FALSE, drops them silently. */
static void
cm_fail_all_jobs(SteamCM *cm, SteamEResult eresult, gboolean notify)
{
	GList *jobs, *l;

	if (cm->jobs == NULL || g_hash_table_size(cm->jobs) == 0)
		return;

	jobs = g_hash_table_get_values(cm->jobs);
	g_hash_table_steal_all(cm->jobs);

	for (l = jobs; l != NULL; l = l->next) {
		CMJob *job = l->data;

		if (job->timeout) {
			purple_timeout_remove(job->timeout);
			job->timeout = 0;
		}
		if (notify && CM_ALIVE(cm) && job->handler)
			job->handler(cm, job, eresult, NULL, 0);
		cm_job_free(job);
	}
	g_list_free(jobs);
}

/* Sends a ServiceMethodCallFromClient. `handler` NULL = fire and forget. */
static void
cm_call_service(SteamCM *cm, const gchar *method, const GByteArray *body,
                CMJobHandler handler, gpointer callback, gpointer user_data,
                guint64 friend_steamid)
{
	guint64 jobid = cm->next_jobid++;

	if (cm->next_jobid == STEAM_JOBID_NONE || cm->next_jobid == 0)
		cm->next_jobid = 1;

	if (handler != NULL) {
		CMJob *job = g_new0(CMJob, 1);

		job->jobid = jobid;
		job->cm = cm;
		job->method = g_strdup(method);
		job->handler = handler;
		job->callback = callback;
		job->user_data = user_data;
		job->friend_steamid = friend_steamid;
		job->timeout = purple_timeout_add_seconds(CM_JOB_TIMEOUT_SECONDS, cm_job_timeout_cb, job);
		g_hash_table_insert(cm->jobs, &job->jobid, job);
	}

	cm_send(cm, STEAM_EMSG_SERVICE_METHOD_CALL_FROM_CLIENT, method, jobid, body);
}

/* ------------------------------------------------------------------ */
/* Connection teardown, reconnect, terminal states                     */

/* Drops the WebSocket and the per-connection timers. No callbacks. */
static void
cm_drop_transport(SteamCM *cm)
{
	if (cm->heartbeat_timer) {
		purple_timeout_remove(cm->heartbeat_timer);
		cm->heartbeat_timer = 0;
	}
	if (cm->logon_timer) {
		purple_timeout_remove(cm->logon_timer);
		cm->logon_timer = 0;
	}
	if (cm->ws) {
		steam_ws_free(cm->ws);
		cm->ws = NULL;
	}
	cm->generation++;
	cm->session_id = 0;
	cm->heartbeat_replies = FALSE;
	cm->state = CM_STATE_IDLE;
}

static void
cm_cancel_directory(SteamCM *cm)
{
	CMDirRequest *req = cm->dir_req;

	if (req == NULL)
		return;
	cm->dir_req = NULL;

	if (cm->sa != NULL && steam_connection_cancel_by_user_data(cm->sa, req) > 0) {
		g_free(req);
	} else {
		/* Can't find it (e.g. sitting in the HTTP 429 back-off): let the
		 * callback free it when it eventually runs. */
		req->cm = NULL;
	}
}

/* Everything off, no callbacks. Pending jobs are dropped silently. */
static void
cm_shutdown_quiet(SteamCM *cm)
{
	cm_cancel_directory(cm);
	cm_drop_transport(cm);
	if (cm->reconnect_timer) {
		purple_timeout_remove(cm->reconnect_timer);
		cm->reconnect_timer = 0;
	}
	cm_fail_all_jobs(cm, STEAM_ERESULT_NO_CONNECTION, FALSE);
}

typedef enum {
	CM_FINISH_LOGON_FAILED,
	CM_FINISH_DISCONNECTED
} CMFinishKind;

/* Unrecoverable: tear down, fail jobs, then fire exactly one terminal
 * callback. Nothing fires afterwards. */
static void
cm_finish(SteamCM *cm, CMFinishKind kind, SteamEResult eresult, const gchar *message)
{
	gchar *msg;

	if (!CM_ALIVE(cm))
		return;

	purple_debug_info("steam", "CM session %s: %d (%s)%s%s\n",
	                  kind == CM_FINISH_LOGON_FAILED ? "logon failed" : "ended",
	                  eresult, cm_eresult_name(eresult),
	                  message ? ": " : "", message ? message : "");

	msg = g_strdup(message);
	cm_cancel_directory(cm);
	cm_drop_transport(cm);
	if (cm->reconnect_timer) {
		purple_timeout_remove(cm->reconnect_timer);
		cm->reconnect_timer = 0;
	}
	cm_fail_all_jobs(cm, STEAM_ERESULT_NO_CONNECTION, TRUE);

	if (CM_ALIVE(cm)) {
		cm->finished = TRUE;
		if (kind == CM_FINISH_LOGON_FAILED) {
			if (cm->cb.logon_failed)
				cm->cb.logon_failed(cm, eresult, msg, cm->user_data);
		} else {
			if (cm->cb.disconnected)
				cm->cb.disconnected(cm, eresult, msg, cm->user_data);
		}
	}
	g_free(msg);
}

static gboolean
cm_reconnect_cb(gpointer data)
{
	SteamCM *cm = data;

	cm->reconnect_timer = 0;
	cm_enter(cm);
	if (CM_ALIVE(cm))
		cm_connect_next(cm);
	cm_leave(cm);
	return FALSE;
}

/* Transient failure: drop the connection, fail pending jobs, and retry
 * against the next CM after a back-off, or give up after too many. */
static void
cm_connection_lost(SteamCM *cm, SteamEResult eresult, const gchar *message)
{
	guint delay;

	if (!CM_ALIVE(cm))
		return;

	purple_debug_info("steam", "CM connection lost: %d (%s)%s%s\n", eresult,
	                  cm_eresult_name(eresult), message ? ": " : "", message ? message : "");

	cm_cancel_directory(cm);
	cm_drop_transport(cm);
	cm->state = CM_STATE_BACKOFF;
	cm_fail_all_jobs(cm, STEAM_ERESULT_NO_CONNECTION, TRUE);
	if (!CM_ALIVE(cm))
		return;

	cm->failures++;
	if (cm->failures >= CM_MAX_FAILURES) {
		gchar *msg = g_strdup_printf("Unable to reach Steam after %u attempts%s%s",
		                             cm->failures, message ? ": " : "", message ? message : "");
		cm_finish(cm, CM_FINISH_DISCONNECTED, eresult, msg);
		g_free(msg);
		return;
	}

	delay = cm_backoff_seconds[MIN(cm->failures - 1, G_N_ELEMENTS(cm_backoff_seconds) - 1)];
	cm->server_index++;
	purple_debug_info("steam", "CM reconnecting in %u s (attempt %u of %u)\n",
	                  delay, cm->failures + 1, CM_MAX_FAILURES);
	if (cm->reconnect_timer)
		purple_timeout_remove(cm->reconnect_timer);
	cm->reconnect_timer = purple_timeout_add_seconds(delay, cm_reconnect_cb, cm);
}

/* ------------------------------------------------------------------ */
/* Heartbeat / logon timers                                            */

static gboolean
cm_heartbeat_cb(gpointer data)
{
	SteamCM *cm = data;
	SteamMsgClientHeartBeat hb;
	GByteArray *body;
	gint64 silent;

	cm_enter(cm);

	silent = (g_get_monotonic_time() - cm->last_rx) / G_USEC_PER_SEC;
	if (cm->heartbeat_replies &&
	    silent >= (gint64) cm->heartbeat_seconds * CM_HEARTBEAT_DEAD_INTERVALS) {
		gchar *msg = g_strdup_printf("No data from the server for %" G_GINT64_FORMAT " seconds", silent);
		cm->heartbeat_timer = 0; /* this source ends when we return FALSE */
		cm_connection_lost(cm, STEAM_ERESULT_NO_CONNECTION, msg);
		g_free(msg);
		cm_leave(cm);
		return FALSE;
	}

	steam_msg_client_heartbeat_init(&hb);
	STEAM_MSG_SET(&hb, send_reply, TRUE);
	body = g_byte_array_new();
	steam_msg_client_heartbeat_encode(&hb, body);
	cm_send(cm, STEAM_EMSG_CLIENT_HEART_BEAT, NULL, STEAM_JOBID_NONE, body);
	g_byte_array_unref(body);
	steam_msg_client_heartbeat_clear(&hb);

	cm_leave(cm);
	return TRUE;
}

static gboolean
cm_logon_timeout_cb(gpointer data)
{
	SteamCM *cm = data;

	cm->logon_timer = 0;
	cm_enter(cm);
	cm_connection_lost(cm, STEAM_ERESULT_TIMEOUT, "Timed out waiting for the logon response");
	cm_leave(cm);
	return FALSE;
}

/* ------------------------------------------------------------------ */
/* Logon                                                               */

/* Stand-in for the private IP / logon id that node-steam-user and SteamKit
 * send XORed with this mask. A random value, stable per account. */
#define CM_PRIVATE_IP_OBFUSCATION_MASK 0xBAADF00Du
#define CM_MACHINE_NAME "Pidgin"

static guint32
cm_logon_id(SteamCM *cm)
{
	guint32 id;

	if (cm->sa == NULL || cm->sa->account == NULL)
		return g_random_int();
	id = (guint32) purple_account_get_int(cm->sa->account, "logon_id", 0);
	if (id == 0) {
		do
			id = g_random_int();
		while (id == 0);
		purple_account_set_int(cm->sa->account, "logon_id", (int) id);
	}
	return id;
}

/* machine_id as SteamKit / node-steam-user build it: a binary KeyValues
 * "MessageObject" with three string keys (BB3, FF2, 3B3), each the
 * lowercase hex SHA-1 of a machine-specific value, then two end markers
 * (155 bytes total). The values are derived from a random seed stored in
 * the account so the id is stable per account but not the real machine's. */
static GByteArray *
cm_machine_id(SteamCM *cm)
{
	static const gchar *const keys[] = { "BB3", "FF2", "3B3" };
	const gchar *seed = purple_account_get_string(cm->sa->account, "machine_id_seed", NULL);
	gchar *new_seed = NULL;
	GByteArray *b;
	guint i;
	guint8 byte;

	if (seed == NULL || strlen(seed) < 16) {
		new_seed = g_strdup_printf("%08x%08x%08x%08x", g_random_int(), g_random_int(),
		                           g_random_int(), g_random_int());
		purple_account_set_string(cm->sa->account, "machine_id_seed", new_seed);
		seed = new_seed;
	}

	b = g_byte_array_sized_new(155);
	byte = 0x00;
	g_byte_array_append(b, &byte, 1);
	g_byte_array_append(b, (const guint8 *) "MessageObject", sizeof("MessageObject"));
	for (i = 0; i < G_N_ELEMENTS(keys); i++) {
		gchar *value = g_strdup_printf("SteamUser Hash %s %s", keys[i], seed);
		gchar *hash = g_compute_checksum_for_string(G_CHECKSUM_SHA1, value, -1);

		byte = 0x01;
		g_byte_array_append(b, &byte, 1);
		g_byte_array_append(b, (const guint8 *) keys[i], strlen(keys[i]) + 1);
		g_byte_array_append(b, (const guint8 *) hash, strlen(hash) + 1);
		g_free(hash);
		g_free(value);
	}
	byte = 0x08;
	g_byte_array_append(b, &byte, 1);
	g_byte_array_append(b, &byte, 1);

	g_free(new_seed);
	return b;
}

static void
cm_send_hello_and_logon(SteamCM *cm)
{
	SteamMsgClientHello hello;
	SteamMsgClientLogon logon;
	GByteArray *body;

	steam_msg_client_hello_init(&hello);
	STEAM_MSG_SET(&hello, protocol_version, STEAM_PROTOCOL_VERSION);
	body = g_byte_array_new();
	steam_msg_client_hello_encode(&hello, body);
	cm_send(cm, STEAM_EMSG_CLIENT_HELLO, NULL, STEAM_JOBID_NONE, body);
	g_byte_array_unref(body);
	steam_msg_client_hello_clear(&hello);

	steam_msg_client_logon_init(&logon);
	STEAM_MSG_SET(&logon, protocol_version, STEAM_PROTOCOL_VERSION);
	STEAM_MSG_SET(&logon, cell_id, 0);
	STEAM_MSG_SET(&logon, client_package_version, CM_CLIENT_PACKAGE_VERSION);
	logon.client_language = g_strdup("english");
	STEAM_MSG_SET(&logon, client_os_type, STEAM_OS_TYPE_LINUX_UNKNOWN);
	STEAM_MSG_SET(&logon, should_remember_password, TRUE);
	STEAM_MSG_SET(&logon, obfuscated_private_ip, cm_logon_id(cm) ^ CM_PRIVATE_IP_OBFUSCATION_MASK);
	STEAM_MSG_SET(&logon, chat_mode, CM_CHAT_MODE_NEW);
	if (cm->sa != NULL && cm->sa->account != NULL) {
		logon.account_name = g_strdup(purple_account_get_username(cm->sa->account));
		logon.machine_id = cm_machine_id(cm);
	}
	logon.machine_name = g_strdup(CM_MACHINE_NAME);
	STEAM_MSG_SET(&logon, supports_rate_limit_response, TRUE);
	logon.access_token = g_strdup(cm->refresh_token ? cm->refresh_token : "");

	body = g_byte_array_new();
	steam_msg_client_logon_encode(&logon, body);
	cm->state = CM_STATE_LOGGING_ON;
	purple_debug_info("steam", "CM: sending ClientLogon for %" G_GUINT64_FORMAT "\n", cm->steamid);
	cm_send(cm, STEAM_EMSG_CLIENT_LOGON, NULL, STEAM_JOBID_NONE, body);
	/* the body holds the refresh token */
	memset(body->data, 0, body->len);
	g_byte_array_unref(body);
	if (logon.access_token)
		memset(logon.access_token, 0, strlen(logon.access_token));
	steam_msg_client_logon_clear(&logon);

	if (cm->logon_timer)
		purple_timeout_remove(cm->logon_timer);
	if (!cm->test_mode)
		cm->logon_timer = purple_timeout_add_seconds(CM_LOGON_TIMEOUT_SECONDS, cm_logon_timeout_cb, cm);
}

static gboolean
cm_eresult_is_retryable(gint eresult)
{
	switch (eresult) {
	case STEAM_ERESULT_TRY_ANOTHER_CM:
	case STEAM_ERESULT_SERVICE_UNAVAILABLE:
	case STEAM_ERESULT_NO_CONNECTION:
	case STEAM_ERESULT_REMOTE_DISCONNECT:
		return TRUE;
	default:
		return FALSE;
	}
}

/* The token is dead: the consumer should re-authenticate. Throttling,
 * Steam Guard and other logon denials are reported as `disconnected`. */
static gboolean
cm_eresult_is_auth_failure(gint eresult)
{
	switch (eresult) {
	case STEAM_ERESULT_INVALID_PASSWORD:
	case STEAM_ERESULT_ACCESS_DENIED:
	case STEAM_ERESULT_EXPIRED:
	case STEAM_ERESULT_REVOKED:
	/* The token was issued for another account than the account_name we
	 * send (e.g. the Pidgin username was changed): only a new login helps. */
	case STEAM_ERESULT_INVALID_NAME:
		return TRUE;
	default:
		return FALSE;
	}
}

static void
cm_handle_logon_response(SteamCM *cm, const SteamMsgProtoBufHeader *hdr,
                         const guint8 *body, gsize len)
{
	SteamMsgClientLogonResponse resp;
	gint eresult, extended;
	gchar *reason;

	steam_msg_client_logon_response_init(&resp);
	if (!steam_msg_client_logon_response_decode(&resp, body, len))
		purple_debug_warning("steam", "CM: malformed ClientLogOnResponse\n");
	eresult = resp.eresult;

	if (cm->logon_timer) {
		purple_timeout_remove(cm->logon_timer);
		cm->logon_timer = 0;
	}

	if (eresult == STEAM_ERESULT_OK) {
		cm->session_id = hdr->has_client_sessionid ? hdr->client_sessionid : 0;
		cm->session_steamid = (hdr->has_steamid && hdr->steamid != 0) ? hdr->steamid : cm->steamid;
		cm->heartbeat_seconds = (resp.has_heartbeat_seconds && resp.heartbeat_seconds > 0)
		                        ? resp.heartbeat_seconds : CM_DEFAULT_HEARTBEAT_SECONDS;
		cm->state = CM_STATE_LOGGED_ON;
		cm->failures = 0;

		purple_debug_info("steam", "CM logged on as %" G_GUINT64_FORMAT ", session %d, heartbeat %d s\n",
		                  cm->session_steamid, cm->session_id, cm->heartbeat_seconds);

		if (cm->heartbeat_timer)
			purple_timeout_remove(cm->heartbeat_timer);
		cm->heartbeat_timer = purple_timeout_add_seconds(cm->heartbeat_seconds, cm_heartbeat_cb, cm);

		steam_msg_client_logon_response_clear(&resp);
		if (CM_ALIVE(cm) && cm->cb.logged_on)
			cm->cb.logged_on(cm, cm->session_steamid, cm->user_data);
		return;
	}

	extended = resp.has_eresult_extended ? resp.eresult_extended : 0;
	purple_debug_info("steam", "CM logon rejected: %d (%s), extended %d\n", eresult,
	                  cm_eresult_name(eresult), extended);
	steam_msg_client_logon_response_clear(&resp);

	reason = g_strdup_printf("%s (extended %d)", cm_eresult_name(eresult), extended);
	if (cm_eresult_is_retryable(eresult))
		cm_connection_lost(cm, eresult, "Logon: try another server");
	else if (cm_eresult_is_auth_failure(eresult))
		cm_finish(cm, CM_FINISH_LOGON_FAILED, eresult, reason);
	else
		cm_finish(cm, CM_FINISH_DISCONNECTED, eresult, reason);
	g_free(reason);
}

static void
cm_handle_logged_off(SteamCM *cm, const guint8 *body, gsize len)
{
	SteamMsgClientLoggedOff m;
	gint eresult;

	steam_msg_client_logged_off_init(&m);
	steam_msg_client_logged_off_decode(&m, body, len);
	eresult = m.eresult;
	steam_msg_client_logged_off_clear(&m);

	purple_debug_info("steam", "CM: logged off by the server: %d (%s)\n", eresult, cm_eresult_name(eresult));

	if (eresult == STEAM_ERESULT_LOGON_SESSION_REPLACED || eresult == STEAM_ERESULT_LOGGED_IN_ELSEWHERE)
		cm_finish(cm, CM_FINISH_DISCONNECTED, eresult, NULL);
	else
		cm_connection_lost(cm, eresult, "Logged off by the server");
}

/* ------------------------------------------------------------------ */
/* Friends / persona / nicknames                                       */

static void
cm_request_friend_data_internal(SteamCM *cm, const guint64 *steamids, guint n)
{
	guint start;

	for (start = 0; start < n; start += CM_FRIEND_DATA_CHUNK) {
		SteamMsgClientRequestFriendData m;
		GByteArray *body;
		guint i, end = MIN(n, start + CM_FRIEND_DATA_CHUNK);

		steam_msg_client_request_friend_data_init(&m);
		STEAM_MSG_SET(&m, persona_state_requested, STEAM_PERSONA_REQ_DEFAULT);
		for (i = start; i < end; i++)
			g_array_append_val(m.friends, steamids[i]);
		body = g_byte_array_new();
		steam_msg_client_request_friend_data_encode(&m, body);
		cm_send(cm, STEAM_EMSG_CLIENT_REQUEST_FRIEND_DATA, NULL, STEAM_JOBID_NONE, body);
		g_byte_array_unref(body);
		steam_msg_client_request_friend_data_clear(&m);
	}
}

static void
cm_handle_friends_list(SteamCM *cm, const guint8 *body, gsize len)
{
	SteamMsgClientFriendsList m;
	GArray *friends, *request;
	gboolean incremental;
	guint i, gen = cm->generation;

	steam_msg_client_friends_list_init(&m);
	if (!steam_msg_client_friends_list_decode(&m, body, len)) {
		purple_debug_warning("steam", "CM: malformed ClientFriendsList\n");
		steam_msg_client_friends_list_clear(&m);
		return;
	}
	incremental = m.bincremental;

	friends = g_array_sized_new(FALSE, TRUE, sizeof(SteamCMFriend), m.friends->len);
	request = g_array_new(FALSE, FALSE, sizeof(guint64));
	for (i = 0; i < m.friends->len; i++) {
		const SteamMsgClientFriendsListFriend *src =
			&g_array_index(m.friends, SteamMsgClientFriendsListFriend, i);
		SteamCMFriend f;

		if (steam_cm_steamid_is_clan(src->ulfriendid))
			continue;
		f.steamid = src->ulfriendid;
		f.relationship = (SteamFriendRelationship) src->efriendrelationship;
		g_array_append_val(friends, f);

		if (f.relationship == STEAM_RELATIONSHIP_FRIEND ||
		    f.relationship == STEAM_RELATIONSHIP_REQUEST_RECIPIENT ||
		    f.relationship == STEAM_RELATIONSHIP_REQUEST_INITIATOR)
			g_array_append_val(request, f.steamid);
	}
	steam_msg_client_friends_list_clear(&m);

	purple_debug_info("steam", "CM: friends list (%s): %u entries\n",
	                  incremental ? "incremental" : "full", friends->len);

	if (CM_ALIVE(cm) && cm->cb.friends_list)
		cm->cb.friends_list(cm, incremental, (const SteamCMFriend *) friends->data,
		                    friends->len, cm->user_data);

	if (!incremental && request->len > 0 && CM_ALIVE(cm) &&
	    gen == cm->generation && cm_is_logged_on(cm))
		cm_request_friend_data_internal(cm, (const guint64 *) request->data, request->len);

	g_array_free(friends, TRUE);
	g_array_free(request, TRUE);
}

static void
cm_handle_persona_state(SteamCM *cm, const guint8 *body, gsize len)
{
	SteamMsgClientPersonaState m;
	guint i, gen = cm->generation;

	steam_msg_client_persona_state_init(&m);
	if (!steam_msg_client_persona_state_decode(&m, body, len)) {
		purple_debug_warning("steam", "CM: malformed ClientPersonaState\n");
		steam_msg_client_persona_state_clear(&m);
		return;
	}

	for (i = 0; i < m.friends->len; i++) {
		const SteamMsgPersonaFriend *f = &g_array_index(m.friends, SteamMsgPersonaFriend, i);
		SteamCMPersona p;

		if (!CM_ALIVE(cm) || gen != cm->generation)
			break;
		if (!f->has_friendid)
			continue;

		memset(&p, 0, sizeof(p));
		p.steamid = f->friendid;
		p.has_state = f->has_persona_state;
		p.state = (SteamPersonaState) f->persona_state;
		p.state_flags = f->persona_state_flags;
		p.player_name = f->player_name;
		p.avatar_hash = f->avatar_hash ? cm_avatar_hex(f->avatar_hash) : NULL;
		/* A Presence update (anything carrying persona_state) always
		 * includes the game group, so absent fields mean "not in game". */
		p.has_game = f->has_persona_state ||
		             f->has_game_played_app_id || f->has_gameid || f->game_name != NULL ||
		             f->has_game_server_ip || f->has_game_server_port || f->has_game_lobby_id;
		p.game_app_id = f->game_played_app_id;
		p.gameid = f->gameid;
		p.game_name = f->game_name;
		p.game_lobby_id = f->game_lobby_id;
		p.game_server_steamid = 0; /* no source field in CMsgClientPersonaState */
		p.game_server_ip = f->game_server_ip;
		p.game_server_port = (guint16) f->game_server_port;
		p.last_logoff = f->last_logoff;
		p.last_logon = f->last_logon;

		if (cm->cb.persona_state)
			cm->cb.persona_state(cm, &p, cm->user_data);
		g_free(p.avatar_hash);
	}
	steam_msg_client_persona_state_clear(&m);
}

static void
cm_handle_nickname_list(SteamCM *cm, const guint8 *body, gsize len)
{
	SteamMsgClientPlayerNicknameList m;
	SteamCMNickname *nicks;
	guint i;

	steam_msg_client_player_nickname_list_init(&m);
	if (!steam_msg_client_player_nickname_list_decode(&m, body, len)) {
		purple_debug_warning("steam", "CM: malformed ClientPlayerNicknameList\n");
		steam_msg_client_player_nickname_list_clear(&m);
		return;
	}

	nicks = g_new0(SteamCMNickname, m.nicknames->len + 1);
	for (i = 0; i < m.nicknames->len; i++) {
		const SteamMsgClientPlayerNickname *n =
			&g_array_index(m.nicknames, SteamMsgClientPlayerNickname, i);
		nicks[i].steamid = n->steamid;
		nicks[i].nickname = n->nickname;
	}
	purple_debug_misc("steam", "CM: %u nicknames (incremental %d, removal %d)\n",
	                  m.nicknames->len, m.incremental, m.removal);

	if (CM_ALIVE(cm) && cm->cb.nicknames)
		cm->cb.nicknames(cm, nicks, m.nicknames->len, m.incremental, m.removal, cm->user_data);

	g_free(nicks);
	steam_msg_client_player_nickname_list_clear(&m);
}

static void
cm_handle_account_info(SteamCM *cm, const guint8 *body, gsize len)
{
	SteamMsgClientAccountInfo m;

	steam_msg_client_account_info_init(&m);
	steam_msg_client_account_info_decode(&m, body, len);
	purple_debug_misc("steam", "CM: account info, persona name %s\n",
	                  m.persona_name ? m.persona_name : "(none)");
	if (CM_ALIVE(cm) && cm->cb.account_info)
		cm->cb.account_info(cm, m.persona_name, cm->user_data);
	steam_msg_client_account_info_clear(&m);
}

static void
cm_handle_add_friend_response(SteamCM *cm, const guint8 *body, gsize len)
{
	SteamMsgClientAddFriendResponse m;

	steam_msg_client_add_friend_response_init(&m);
	steam_msg_client_add_friend_response_decode(&m, body, len);
	purple_debug_info("steam", "CM: add friend %" G_GUINT64_FORMAT ": %d (%s)\n",
	                  m.steam_id_added, m.eresult, cm_eresult_name(m.eresult));
	if (CM_ALIVE(cm) && cm->cb.add_friend_response)
		cm->cb.add_friend_response(cm, (SteamEResult) m.eresult, m.steam_id_added,
		                           m.persona_name_added, cm->user_data);
	steam_msg_client_add_friend_response_clear(&m);
}

/* ------------------------------------------------------------------ */
/* Service method notifications and responses                          */

static void
cm_handle_incoming_message(SteamCM *cm, const guint8 *body, gsize len)
{
	SteamMsgFriendMessagesIncomingMessage m;
	SteamCMMessage msg;

	steam_msg_friend_messages_incoming_message_init(&m);
	if (!steam_msg_friend_messages_incoming_message_decode(&m, body, len)) {
		purple_debug_warning("steam", "CM: malformed IncomingMessage\n");
		steam_msg_friend_messages_incoming_message_clear(&m);
		return;
	}

	memset(&msg, 0, sizeof(msg));
	msg.from_steamid = m.steamid_friend;
	msg.type = (SteamChatEntryType) m.chat_entry_type;
	msg.message = (m.message_no_bbcode && *m.message_no_bbcode) ? m.message_no_bbcode : m.message;
	msg.timestamp = m.rtime32_server_timestamp;
	msg.local_echo = m.local_echo;
	msg.message_bbcode = m.message;
	msg.ordinal = m.ordinal;

	purple_debug_misc("steam", "CM: message type %d from %" G_GUINT64_FORMAT "%s\n",
	                  msg.type, msg.from_steamid, msg.local_echo ? " (local echo)" : "");

	if (CM_ALIVE(cm) && cm->cb.message)
		cm->cb.message(cm, &msg, cm->user_data);
	steam_msg_friend_messages_incoming_message_clear(&m);
}

static void
cm_handle_ack_echo(SteamCM *cm, const guint8 *body, gsize len)
{
	SteamMsgFriendMessagesAckMessage m;

	steam_msg_friend_messages_ack_message_init(&m);
	if (!steam_msg_friend_messages_ack_message_decode(&m, body, len) ||
	    !m.has_steamid_partner || !m.has_timestamp) {
		purple_debug_warning("steam", "CM: malformed NotifyAckMessageEcho\n");
		steam_msg_friend_messages_ack_message_clear(&m);
		return;
	}
	purple_debug_misc("steam", "CM: read up to %u with %" G_GUINT64_FORMAT " on another session\n",
	                  m.timestamp, m.steamid_partner);
	if (CM_ALIVE(cm) && cm->cb.ack_echo)
		cm->cb.ack_echo(cm, m.steamid_partner, m.timestamp, cm->user_data);
	steam_msg_friend_messages_ack_message_clear(&m);
}

static void
cm_handle_message_reaction(SteamCM *cm, const guint8 *body, gsize len)
{
	SteamMsgFriendMessagesMessageReaction m;
	SteamCMReaction reaction;

	steam_msg_friend_messages_message_reaction_init(&m);
	if (!steam_msg_friend_messages_message_reaction_decode(&m, body, len) ||
	    !m.has_steamid_friend || !m.has_server_timestamp || !m.has_reactor ||
	    m.reaction == NULL || *m.reaction == '\0') {
		purple_debug_warning("steam", "CM: malformed MessageReaction\n");
		steam_msg_friend_messages_message_reaction_clear(&m);
		return;
	}

	memset(&reaction, 0, sizeof(reaction));
	reaction.steamid_friend = m.steamid_friend;
	reaction.server_timestamp = m.server_timestamp;
	reaction.ordinal = m.ordinal;
	reaction.reactor = m.reactor;
	reaction.type = (SteamCMReactionType) m.reaction_type;
	reaction.reaction = m.reaction;
	reaction.is_add = m.is_add;
	purple_debug_misc("steam", "CM: reaction %s %s by %" G_GUINT64_FORMAT "\n",
	                  m.is_add ? "added" : "removed", m.reaction, m.reactor);

	if (CM_ALIVE(cm) && cm->cb.reaction)
		cm->cb.reaction(cm, &reaction, cm->user_data);
	steam_msg_friend_messages_message_reaction_clear(&m);
}

static void
cm_handle_service_method(SteamCM *cm, const SteamMsgProtoBufHeader *hdr,
                         const guint8 *body, gsize len)
{
	const gchar *name = hdr->target_job_name;

	if (name != NULL && strcmp(name, STEAM_NOTIFY_FRIEND_MESSAGES_INCOMING_MESSAGE) == 0)
		cm_handle_incoming_message(cm, body, len);
	else if (name != NULL && strcmp(name, STEAM_NOTIFY_FRIEND_MESSAGES_ACK_ECHO) == 0)
		cm_handle_ack_echo(cm, body, len);
	else if (name != NULL && strcmp(name, STEAM_NOTIFY_FRIEND_MESSAGES_MESSAGE_REACTION) == 0)
		cm_handle_message_reaction(cm, body, len);
	else
		purple_debug_misc("steam", "CM: ignoring notification %s\n", name ? name : "(unnamed)");
}

static void
cm_handle_service_response(SteamCM *cm, const SteamMsgProtoBufHeader *hdr,
                           const guint8 *body, gsize len, gboolean dest_failed)
{
	CMJob *job;
	guint64 jobid = hdr->jobid_target;
	SteamEResult eresult;

	job = g_hash_table_lookup(cm->jobs, &jobid);
	if (job == NULL) {
		purple_debug_misc("steam", "CM: response for unknown job %" G_GUINT64_FORMAT "\n", jobid);
		return;
	}

	eresult = dest_failed ? STEAM_ERESULT_FAIL : (SteamEResult) hdr->eresult;
	if (eresult != STEAM_ERESULT_OK)
		purple_debug_warning("steam", "CM job %" G_GUINT64_FORMAT " (%s): %d (%s)%s%s\n",
		                     jobid, job->method, eresult, cm_eresult_name(eresult),
		                     hdr->error_message ? " " : "", hdr->error_message ? hdr->error_message : "");

	cm_job_complete(cm, job, eresult, dest_failed ? NULL : body, dest_failed ? 0 : len);
}

/* ------------------------------------------------------------------ */
/* Packet dispatch                                                     */

static void cm_dispatch(SteamCM *cm, const guint8 *data, gsize len, gint nesting);

static void
cm_handle_multi(SteamCM *cm, const guint8 *body, gsize len, gint nesting)
{
	SteamMsgMulti multi;
	GPtrArray *packets;
	guint i, gen = cm->generation;

	if (nesting >= CM_MAX_MULTI_NESTING) {
		purple_debug_warning("steam", "CM: Multi nested too deeply\n");
		return;
	}

	steam_msg_multi_init(&multi);
	packets = g_ptr_array_new_with_free_func((GDestroyNotify) g_byte_array_unref);
	if (!steam_msg_multi_decode(&multi, body, len) || !steam_msg_multi_unpack(&multi, packets)) {
		purple_debug_warning("steam", "CM: malformed Multi\n");
	} else {
		purple_debug_misc("steam", "CM: Multi with %u packets\n", packets->len);
		for (i = 0; i < packets->len; i++) {
			GByteArray *pkt = g_ptr_array_index(packets, i);

			if (!CM_ALIVE(cm) || gen != cm->generation)
				break;
			cm_dispatch(cm, pkt->data, pkt->len, nesting + 1);
		}
	}
	g_ptr_array_free(packets, TRUE);
	steam_msg_multi_clear(&multi);
}

static void
cm_dispatch(SteamCM *cm, const guint8 *data, gsize len, gint nesting)
{
	SteamMsgProtoBufHeader hdr;
	guint32 emsg = 0;
	const guint8 *body;
	gsize body_len;

	if (!steam_msg_packet_parse(data, len, &emsg, &hdr, &body, &body_len)) {
		purple_debug_warning("steam", "CM: unparseable packet (EMsg %u %s, %" G_GSIZE_FORMAT " bytes)\n",
		                     emsg, cm_emsg_name(emsg), len);
		steam_msg_protobuf_header_clear(&hdr);
		return;
	}

	/* Everything but Multi and logon needs a logged-on session. */
	if (emsg != STEAM_EMSG_MULTI && emsg != STEAM_EMSG_CLIENT_LOG_ON_RESPONSE &&
	    emsg != STEAM_EMSG_CLIENT_LOGGED_OFF && !cm_is_logged_on(cm)) {
		purple_debug_misc("steam", "CM: ignoring %s (%u) before logon\n", cm_emsg_name(emsg), emsg);
		steam_msg_protobuf_header_clear(&hdr);
		return;
	}

	switch (emsg) {
	case STEAM_EMSG_MULTI:
		cm_handle_multi(cm, body, body_len, nesting);
		break;
	case STEAM_EMSG_CLIENT_LOG_ON_RESPONSE:
		cm_handle_logon_response(cm, &hdr, body, body_len);
		break;
	case STEAM_EMSG_CLIENT_LOGGED_OFF:
		cm_handle_logged_off(cm, body, body_len);
		break;
	case STEAM_EMSG_CLIENT_HEART_BEAT:
		cm->heartbeat_replies = TRUE;
		break;
	case STEAM_EMSG_CLIENT_FRIENDS_LIST:
		cm_handle_friends_list(cm, body, body_len);
		break;
	case STEAM_EMSG_CLIENT_PERSONA_STATE:
		cm_handle_persona_state(cm, body, body_len);
		break;
	case STEAM_EMSG_CLIENT_PLAYER_NICKNAME_LIST:
		cm_handle_nickname_list(cm, body, body_len);
		break;
	case STEAM_EMSG_CLIENT_ACCOUNT_INFO:
		cm_handle_account_info(cm, body, body_len);
		break;
	case STEAM_EMSG_CLIENT_ADD_FRIEND_RESPONSE:
		cm_handle_add_friend_response(cm, body, body_len);
		break;
	case STEAM_EMSG_SERVICE_METHOD:
		cm_handle_service_method(cm, &hdr, body, body_len);
		break;
	case STEAM_EMSG_SERVICE_METHOD_RESPONSE:
		cm_handle_service_response(cm, &hdr, body, body_len, FALSE);
		break;
	case STEAM_EMSG_DEST_JOB_FAILED:
		cm_handle_service_response(cm, &hdr, body, body_len, TRUE);
		break;
	default:
		purple_debug_misc("steam", "CM: ignoring %s (%u), %" G_GSIZE_FORMAT " bytes\n",
		                  cm_emsg_name(emsg), emsg, body_len);
		break;
	}

	steam_msg_protobuf_header_clear(&hdr);
}

/* ------------------------------------------------------------------ */
/* WebSocket callbacks                                                 */

static void
cm_ws_connected(SteamWebSocket *ws, gpointer user_data)
{
	SteamCM *cm = user_data;

	cm_enter(cm);
	if (CM_ALIVE(cm) && ws == cm->ws) {
		purple_debug_info("steam", "CM: WebSocket connected, logging on\n");
		cm->last_rx = g_get_monotonic_time();
		cm_send_hello_and_logon(cm);
	}
	cm_leave(cm);
}

static void
cm_ws_message(SteamWebSocket *ws, const guint8 *data, gsize len,
              gboolean binary, gpointer user_data)
{
	SteamCM *cm = user_data;

	cm_enter(cm);
	if (CM_ALIVE(cm) && ws == cm->ws) {
		cm->last_rx = g_get_monotonic_time();
		if (binary)
			cm_dispatch(cm, data, len, 0);
		else
			purple_debug_misc("steam", "CM: ignoring text frame\n");
	}
	cm_leave(cm);
}

static void
cm_ws_closed(SteamWebSocket *ws, const gchar *error, gpointer user_data)
{
	SteamCM *cm = user_data;

	cm_enter(cm);
	if (CM_ALIVE(cm) && ws == cm->ws)
		cm_connection_lost(cm, STEAM_ERESULT_NO_CONNECTION,
		                   error ? error : "Connection closed by the server");
	cm_leave(cm);
}

static const SteamWebSocketCallbacks cm_ws_callbacks = {
	cm_ws_connected,
	cm_ws_message,
	cm_ws_closed,
};

/* ------------------------------------------------------------------ */
/* Directory                                                           */

static gint
cm_server_compare(gconstpointer a, gconstpointer b)
{
	const CMServer *sa = *(const CMServer * const *) a;
	const CMServer *sb = *(const CMServer * const *) b;

	if (sa->load < sb->load)
		return -1;
	if (sa->load > sb->load)
		return 1;
	return 0;
}

static gdouble
cm_json_number(JsonObject *obj, const gchar *member)
{
	JsonNode *node;
	GType type;

	if (!json_object_has_member(obj, member))
		return 1e9;
	node = json_object_get_member(obj, member);
	if (!JSON_NODE_HOLDS_VALUE(node))
		return 1e9;
	type = json_node_get_value_type(node);
	if (type == G_TYPE_INT64)
		return (gdouble) json_node_get_int(node);
	if (type == G_TYPE_DOUBLE)
		return json_node_get_double(node);
	if (type == G_TYPE_STRING)
		return g_ascii_strtod(json_node_get_string(node), NULL);
	return 1e9;
}

/* Parses the GetCMListForConnect JSON; returns NULL if no usable entries. */
static GPtrArray *
cm_parse_directory(const guint8 *body, gsize len)
{
	JsonParser *parser;
	JsonNode *root;
	JsonObject *obj, *response;
	JsonArray *list;
	GPtrArray *servers;
	guint i;

	parser = json_parser_new();
	if (!json_parser_load_from_data(parser, (const gchar *) body, len, NULL)) {
		g_object_unref(parser);
		return NULL;
	}
	root = json_parser_get_root(parser);
	if (root == NULL || !JSON_NODE_HOLDS_OBJECT(root)) {
		g_object_unref(parser);
		return NULL;
	}
	obj = json_node_get_object(root);
	response = json_object_get_object_member(obj, "response");
	list = response ? json_object_get_array_member(response, "serverlist") : NULL;
	if (list == NULL) {
		g_object_unref(parser);
		return NULL;
	}

	servers = g_ptr_array_new_with_free_func(cm_server_free);
	for (i = 0; i < json_array_get_length(list); i++) {
		JsonNode *node = json_array_get_element(list, i);
		JsonObject *entry;
		const gchar *type, *endpoint, *colon;
		CMServer *srv;
		gint64 port = 443;

		if (!JSON_NODE_HOLDS_OBJECT(node))
			continue;
		entry = json_node_get_object(node);
		type = json_object_get_string_member(entry, "type");
		endpoint = json_object_get_string_member(entry, "endpoint");
		if (type == NULL || endpoint == NULL || strcmp(type, "websockets") != 0)
			continue;

		colon = strrchr(endpoint, ':');
		if (colon != NULL) {
			port = g_ascii_strtoll(colon + 1, NULL, 10);
			if (port <= 0 || port > 65535)
				continue;
		}
		srv = g_new0(CMServer, 1);
		srv->host = colon ? g_strndup(endpoint, colon - endpoint) : g_strdup(endpoint);
		srv->port = (guint16) port;
		srv->load = cm_json_number(entry, "load");
		g_ptr_array_add(servers, srv);
	}
	g_object_unref(parser);

	if (servers->len == 0) {
		g_ptr_array_free(servers, TRUE);
		return NULL;
	}
	g_ptr_array_sort(servers, cm_server_compare);
	return servers;
}

static void
cm_directory_cb(SteamAccount *sa, const gchar *headers, const guint8 *body,
                gsize body_len, gpointer user_data)
{
	CMDirRequest *req = user_data;
	SteamCM *cm = req->cm;
	GPtrArray *servers = NULL;
	guint status;

	g_free(req);
	if (cm == NULL)
		return; /* the CM went away */
	cm->dir_req = NULL;

	cm_enter(cm);
	if (!CM_ALIVE(cm) || cm->state != CM_STATE_DIRECTORY) {
		cm_leave(cm);
		return;
	}

	status = headers ? steam_connection_get_status(headers) : 0;
	if (status == 200 && body != NULL)
		servers = cm_parse_directory(body, body_len);

	if (servers == NULL) {
		gchar *msg = g_strdup_printf("CM directory lookup failed (HTTP %u)", status);
		cm_connection_lost(cm, STEAM_ERESULT_SERVICE_UNAVAILABLE, msg);
		g_free(msg);
	} else {
		purple_debug_info("steam", "CM directory: %u WebSocket servers, best %s:%u (load %g)\n",
		                  servers->len, ((CMServer *) servers->pdata[0])->host,
		                  ((CMServer *) servers->pdata[0])->port, ((CMServer *) servers->pdata[0])->load);
		if (cm->servers)
			g_ptr_array_free(cm->servers, TRUE);
		cm->servers = servers;
		cm->server_index = 0;
		cm_connect_next(cm);
	}
	cm_leave(cm);
}

static void
cm_fetch_directory(SteamCM *cm)
{
	SteamConnection *conn;
	CMDirRequest *req;

	cm_cancel_directory(cm);
	cm->state = CM_STATE_DIRECTORY;

	req = g_new0(CMDirRequest, 1);
	req->cm = cm;
	cm->dir_req = req;

	purple_debug_info("steam", "CM: fetching the server directory\n");
	conn = steam_post_or_get(cm->sa, STEAM_METHOD_GET | STEAM_METHOD_SSL,
	                         CM_DIRECTORY_HOST, CM_DIRECTORY_PATH, NULL, NULL, req, FALSE);
	if (conn != NULL)
		conn->raw_callback = cm_directory_cb;
}

/* Connects to the next server in the cached list, or refetches the list. */
static void
cm_connect_next(SteamCM *cm)
{
	CMServer *srv;

	cm_drop_transport(cm);

	if (cm->servers == NULL || cm->server_index >= cm->servers->len) {
		if (cm->servers) {
			g_ptr_array_free(cm->servers, TRUE);
			cm->servers = NULL;
		}
		cm->server_index = 0;
		cm_fetch_directory(cm);
		return;
	}

	srv = g_ptr_array_index(cm->servers, cm->server_index);
	purple_debug_info("steam", "CM: connecting to %s:%u\n", srv->host, srv->port);
	cm->state = CM_STATE_CONNECTING;
	cm->ws = steam_ws_connect(cm->sa, srv->host, srv->port, CM_WS_PATH, NULL,
	                          &cm_ws_callbacks, cm);
}

/* ------------------------------------------------------------------ */
/* Public API: lifecycle                                               */

SteamCM *
steam_cm_new(SteamAccount *sa, const SteamCMCallbacks *callbacks, gpointer user_data)
{
	SteamCM *cm = g_new0(SteamCM, 1);

	cm->sa = sa;
	if (callbacks)
		cm->cb = *callbacks;
	cm->user_data = user_data;
	cm->jobs = g_hash_table_new(g_int64_hash, g_int64_equal);
	cm->next_jobid = 1;
	cm->heartbeat_seconds = CM_DEFAULT_HEARTBEAT_SECONDS;
	cm->state = CM_STATE_IDLE;
	return cm;
}

void
steam_cm_connect(SteamCM *cm, const gchar *refresh_token, guint64 steamid)
{
	g_return_if_fail(cm != NULL);
	if (cm->free_pending)
		return;

	cm_shutdown_quiet(cm);

	if (cm->refresh_token) {
		memset(cm->refresh_token, 0, strlen(cm->refresh_token));
		g_free(cm->refresh_token);
	}
	cm->refresh_token = g_strdup(refresh_token);
	cm->steamid = steamid;
	cm->session_steamid = 0;
	cm->failures = 0;
	cm->finished = FALSE;

	purple_debug_info("steam", "CM: connecting as %" G_GUINT64_FORMAT "\n", steamid);
	cm_connect_next(cm);
}

void
steam_cm_disconnect(SteamCM *cm)
{
	if (cm == NULL)
		return;

	if (cm_is_logged_on(cm) && (cm->test_mode || steam_ws_is_connected(cm->ws))) {
		purple_debug_info("steam", "CM: logging off\n");
		cm_send(cm, STEAM_EMSG_CLIENT_LOG_OFF, NULL, STEAM_JOBID_NONE, NULL);
	}
	if (cm->ws)
		steam_ws_close(cm->ws);
	cm_shutdown_quiet(cm);
	cm->finished = TRUE;
}

static void
cm_really_free(SteamCM *cm)
{
	if (cm->servers)
		g_ptr_array_free(cm->servers, TRUE);
	if (cm->jobs)
		g_hash_table_destroy(cm->jobs);
	if (cm->test_sent)
		g_ptr_array_free(cm->test_sent, TRUE);
	if (cm->refresh_token) {
		memset(cm->refresh_token, 0, strlen(cm->refresh_token));
		g_free(cm->refresh_token);
	}
	g_free(cm);
}

void
steam_cm_free(SteamCM *cm)
{
	if (cm == NULL || cm->free_pending)
		return;

	steam_cm_disconnect(cm);
	memset(&cm->cb, 0, sizeof(cm->cb));

	if (cm->depth > 0)
		cm->free_pending = TRUE; /* released by the outermost cm_leave() */
	else
		cm_really_free(cm);
}

gboolean
steam_cm_is_logged_on(const SteamCM *cm)
{
	return cm != NULL && !cm->free_pending && !cm->finished && cm_is_logged_on(cm);
}

guint64
steam_cm_get_steamid(const SteamCM *cm)
{
	if (cm == NULL)
		return 0;
	return cm->session_steamid ? cm->session_steamid : cm->steamid;
}

/* ------------------------------------------------------------------ */
/* Public API: requests                                                */

static gboolean
cm_check_logged_on(SteamCM *cm, const gchar *what)
{
	if (cm == NULL)
		return FALSE;
	if (!steam_cm_is_logged_on(cm)) {
		purple_debug_info("steam", "CM: %s ignored, not logged on\n", what);
		return FALSE;
	}
	return TRUE;
}

void
steam_cm_set_persona_state(SteamCM *cm, SteamPersonaState state, const gchar *player_name)
{
	SteamMsgClientChangeStatus m;
	GByteArray *body;

	if (!cm_check_logged_on(cm, "set_persona_state"))
		return;

	steam_msg_client_change_status_init(&m);
	STEAM_MSG_SET(&m, persona_state, (guint32) state);
	if (player_name != NULL)
		m.player_name = g_strdup(player_name);
	STEAM_MSG_SET(&m, persona_set_by_user, TRUE);
	body = g_byte_array_new();
	steam_msg_client_change_status_encode(&m, body);
	purple_debug_info("steam", "CM: setting persona state %d\n", state);
	cm_send(cm, STEAM_EMSG_CLIENT_CHANGE_STATUS, NULL, STEAM_JOBID_NONE, body);
	g_byte_array_unref(body);
	steam_msg_client_change_status_clear(&m);
}

void
steam_cm_request_friend_data(SteamCM *cm, const guint64 *steamids, guint n)
{
	if (!cm_check_logged_on(cm, "request_friend_data"))
		return;
	if (steamids == NULL || n == 0)
		return;
	cm_request_friend_data_internal(cm, steamids, n);
}

void
steam_cm_add_friend(SteamCM *cm, guint64 steamid)
{
	SteamMsgClientAddFriend m;
	GByteArray *body;

	if (!cm_check_logged_on(cm, "add_friend"))
		return;

	steam_msg_client_add_friend_init(&m);
	STEAM_MSG_SET(&m, steamid_to_add, steamid);
	body = g_byte_array_new();
	steam_msg_client_add_friend_encode(&m, body);
	cm_send(cm, STEAM_EMSG_CLIENT_ADD_FRIEND, NULL, STEAM_JOBID_NONE, body);
	g_byte_array_unref(body);
	steam_msg_client_add_friend_clear(&m);
}

void
steam_cm_remove_friend(SteamCM *cm, guint64 steamid)
{
	SteamMsgClientRemoveFriend m;
	GByteArray *body;

	if (!cm_check_logged_on(cm, "remove_friend"))
		return;

	steam_msg_client_remove_friend_init(&m);
	STEAM_MSG_SET(&m, friendid, steamid);
	body = g_byte_array_new();
	steam_msg_client_remove_friend_encode(&m, body);
	cm_send(cm, STEAM_EMSG_CLIENT_REMOVE_FRIEND, NULL, STEAM_JOBID_NONE, body);
	g_byte_array_unref(body);
	steam_msg_client_remove_friend_clear(&m);
}

/* --- FriendMessages.SendMessage --- */

static void
cm_send_message_done(SteamCM *cm, CMJob *job, SteamEResult eresult,
                     const guint8 *body, gsize len)
{
	SteamCMSendMessageFunc callback = (SteamCMSendMessageFunc) job->callback;
	guint32 server_timestamp = 0;

	if (body != NULL) {
		SteamMsgFriendMessagesSendMessageResponse resp;

		steam_msg_friend_messages_send_message_response_init(&resp);
		steam_msg_friend_messages_send_message_response_decode(&resp, body, len);
		server_timestamp = resp.server_timestamp;
		steam_msg_friend_messages_send_message_response_clear(&resp);
	}
	if (callback)
		callback(cm, eresult, server_timestamp, job->user_data);
}

static void
cm_send_message_full_done(SteamCM *cm, CMJob *job, SteamEResult eresult,
                          const guint8 *body, gsize len)
{
	SteamCMSendMessageFullFunc callback = (SteamCMSendMessageFullFunc) job->callback;
	guint32 server_timestamp = 0, ordinal = 0;

	if (body != NULL) {
		SteamMsgFriendMessagesSendMessageResponse resp;

		steam_msg_friend_messages_send_message_response_init(&resp);
		steam_msg_friend_messages_send_message_response_decode(&resp, body, len);
		server_timestamp = resp.server_timestamp;
		ordinal = resp.ordinal;
		steam_msg_friend_messages_send_message_response_clear(&resp);
	}
	if (callback)
		callback(cm, eresult, server_timestamp, ordinal, job->user_data);
}

static void
cm_send_message_internal(SteamCM *cm, guint64 steamid, SteamChatEntryType type,
                         const gchar *message, CMJobHandler handler, gpointer callback,
                         gpointer user_data);

void
steam_cm_send_message(SteamCM *cm, guint64 steamid, SteamChatEntryType type,
                      const gchar *message, SteamCMSendMessageFunc callback,
                      gpointer user_data)
{
	cm_send_message_internal(cm, steamid, type, message,
	                         callback ? cm_send_message_done : NULL, (gpointer) callback, user_data);
}

void
steam_cm_send_message_full(SteamCM *cm, guint64 steamid, SteamChatEntryType type,
                           const gchar *message, SteamCMSendMessageFullFunc callback,
                           gpointer user_data)
{
	cm_send_message_internal(cm, steamid, type, message,
	                         callback ? cm_send_message_full_done : NULL, (gpointer) callback, user_data);
}

static void
cm_send_message_internal(SteamCM *cm, guint64 steamid, SteamChatEntryType type,
                         const gchar *message, CMJobHandler handler, gpointer callback,
                         gpointer user_data)
{
	SteamMsgFriendMessagesSendMessageRequest m;
	GByteArray *body;

	if (!cm_check_logged_on(cm, "send_message"))
		return;

	steam_msg_friend_messages_send_message_request_init(&m);
	STEAM_MSG_SET(&m, steamid, steamid);
	STEAM_MSG_SET(&m, chat_entry_type, (gint32) type);
	m.message = g_strdup(message ? message : "");
	STEAM_MSG_SET(&m, contains_bbcode, FALSE);
	STEAM_MSG_SET(&m, echo_to_sender, FALSE);
	m.client_message_id = g_strdup_printf("%08x%08x", g_random_int(), ++cm->message_counter);
	body = g_byte_array_new();
	steam_msg_friend_messages_send_message_request_encode(&m, body);
	cm_call_service(cm, STEAM_METHOD_FRIEND_MESSAGES_SEND_MESSAGE, body,
	                handler, callback, user_data, steamid);
	g_byte_array_unref(body);
	steam_msg_friend_messages_send_message_request_clear(&m);
}

/* --- FriendMessages.AckMessage --- */

void
steam_cm_ack_message(SteamCM *cm, guint64 steamid_partner, guint32 timestamp)
{
	SteamMsgFriendMessagesAckMessage m;
	GByteArray *body;

	if (!cm_check_logged_on(cm, "ack_message"))
		return;

	steam_msg_friend_messages_ack_message_init(&m);
	STEAM_MSG_SET(&m, steamid_partner, steamid_partner);
	STEAM_MSG_SET(&m, timestamp, timestamp);
	body = g_byte_array_new();
	steam_msg_friend_messages_ack_message_encode(&m, body);
	cm_call_service(cm, STEAM_METHOD_FRIEND_MESSAGES_ACK_MESSAGE, body, NULL, NULL, NULL, steamid_partner);
	g_byte_array_unref(body);
	steam_msg_friend_messages_ack_message_clear(&m);
}

/* --- FriendMessages.UpdateMessageReaction --- */

static void
cm_update_reaction_done(SteamCM *cm, CMJob *job, SteamEResult eresult,
                        const guint8 *body, gsize len)
{
	SteamCMReactionDoneFunc callback = (SteamCMReactionDoneFunc) job->callback;

	if (callback)
		callback(cm, body != NULL ? eresult : (eresult == STEAM_ERESULT_OK ? STEAM_ERESULT_FAIL : eresult),
		         job->user_data);
}

void
steam_cm_update_message_reaction(SteamCM *cm, guint64 steamid, guint32 server_timestamp,
                                 guint32 ordinal, SteamCMReactionType type,
                                 const gchar *reaction, gboolean is_add,
                                 SteamCMReactionDoneFunc callback, gpointer user_data)
{
	SteamMsgFriendMessagesUpdateMessageReactionRequest m;
	GByteArray *body;

	if (!cm_check_logged_on(cm, "update_message_reaction"))
		return;

	steam_msg_friend_messages_update_message_reaction_request_init(&m);
	STEAM_MSG_SET(&m, steamid, steamid);
	STEAM_MSG_SET(&m, server_timestamp, server_timestamp);
	if (ordinal)
		STEAM_MSG_SET(&m, ordinal, ordinal);
	STEAM_MSG_SET(&m, reaction_type, (gint32) type);
	m.reaction = g_strdup(reaction);
	STEAM_MSG_SET(&m, is_add, is_add);
	body = g_byte_array_new();
	steam_msg_friend_messages_update_message_reaction_request_encode(&m, body);
	cm_call_service(cm, STEAM_METHOD_FRIEND_MESSAGES_UPDATE_REACTION, body,
	                callback ? cm_update_reaction_done : NULL, (gpointer) callback, user_data, steamid);
	g_byte_array_unref(body);
	steam_msg_friend_messages_update_message_reaction_request_clear(&m);
}

/* --- FriendMessages.GetRecentMessages --- */

static void
cm_recent_messages_done(SteamCM *cm, CMJob *job, SteamEResult eresult,
                        const guint8 *body, gsize len)
{
	SteamCMHistoryFunc callback = (SteamCMHistoryFunc) job->callback;
	SteamMsgFriendMessagesGetRecentMessagesResponse resp;
	SteamCMHistoryMessage *msgs = NULL;
	GPtrArray *reactions = g_ptr_array_new_with_free_func(g_free);
	gboolean more = FALSE;
	guint i, j, n = 0;

	steam_msg_friend_messages_get_recent_messages_response_init(&resp);
	if (body != NULL && eresult == STEAM_ERESULT_OK &&
	    steam_msg_friend_messages_get_recent_messages_response_decode(&resp, body, len)) {
		n = resp.messages->len;
		more = resp.more_available;
		msgs = g_new0(SteamCMHistoryMessage, n + 1);
		for (i = 0; i < n; i++) {
			const SteamMsgFriendMessage *src = &g_array_index(resp.messages, SteamMsgFriendMessage, i);
			msgs[i].accountid = src->accountid;
			msgs[i].timestamp = src->timestamp;
			msgs[i].message = src->message;
			msgs[i].ordinal = src->ordinal;
			if (src->reactions != NULL && src->reactions->len > 0) {
				SteamCMHistoryReaction *r = g_new0(SteamCMHistoryReaction, src->reactions->len);

				for (j = 0; j < src->reactions->len; j++) {
					const SteamMsgFriendMessageReaction *mr =
						&g_array_index(src->reactions, SteamMsgFriendMessageReaction, j);

					r[j].type = (SteamCMReactionType) mr->reaction_type;
					r[j].reaction = mr->reaction ? mr->reaction : "";
					r[j].reactors = (const guint32 *) mr->reactors->data;
					r[j].n_reactors = mr->reactors->len;
				}
				msgs[i].reactions = r;
				msgs[i].n_reactions = src->reactions->len;
				g_ptr_array_add(reactions, r);
			}
		}
	}
	if (callback)
		callback(cm, job->friend_steamid, msgs, n, more, job->user_data);
	g_ptr_array_free(reactions, TRUE);
	g_free(msgs);
	steam_msg_friend_messages_get_recent_messages_response_clear(&resp);
}

void
steam_cm_get_recent_messages(SteamCM *cm, guint64 friend_steamid, guint32 since,
                             guint count, SteamCMHistoryFunc callback, gpointer user_data)
{
	SteamCMHistoryQuery query;

	memset(&query, 0, sizeof(query));
	query.count = count;
	query.most_recent_conversation = since == 0;
	query.start_time = since;
	steam_cm_get_recent_messages_query(cm, friend_steamid, &query, callback, user_data);
}

void
steam_cm_get_recent_messages_query(SteamCM *cm, guint64 friend_steamid,
                                   const SteamCMHistoryQuery *query,
                                   SteamCMHistoryFunc callback, gpointer user_data)
{
	SteamMsgFriendMessagesGetRecentMessagesRequest m;
	GByteArray *body;

	if (!cm_check_logged_on(cm, "get_recent_messages"))
		return;

	steam_msg_friend_messages_get_recent_messages_request_init(&m);
	STEAM_MSG_SET(&m, steamid1, cm->session_steamid);
	STEAM_MSG_SET(&m, steamid2, friend_steamid);
	STEAM_MSG_SET(&m, count, query->count);
	STEAM_MSG_SET(&m, most_recent_conversation, query->most_recent_conversation);
	STEAM_MSG_SET(&m, rtime32_start_time, query->start_time);
	STEAM_MSG_SET(&m, bbcode_format, query->bbcode);
	if (query->start_ordinal)
		STEAM_MSG_SET(&m, start_ordinal, query->start_ordinal);
	if (query->time_last)
		STEAM_MSG_SET(&m, time_last, query->time_last);
	if (query->ordinal_last)
		STEAM_MSG_SET(&m, ordinal_last, query->ordinal_last);
	body = g_byte_array_new();
	steam_msg_friend_messages_get_recent_messages_request_encode(&m, body);
	cm_call_service(cm, STEAM_METHOD_FRIEND_MESSAGES_GET_RECENT_MESSAGES, body,
	                cm_recent_messages_done, (gpointer) callback, user_data, friend_steamid);
	g_byte_array_unref(body);
	steam_msg_friend_messages_get_recent_messages_request_clear(&m);
}

/* --- FriendMessages.GetActiveMessageSessions --- */

static void
cm_active_sessions_done(SteamCM *cm, CMJob *job, SteamEResult eresult,
                        const guint8 *body, gsize len)
{
	SteamCMSessionsFunc callback = (SteamCMSessionsFunc) job->callback;
	SteamMsgFriendMessagesGetActiveMessageSessionsResponse resp;
	SteamCMMessageSession *sessions = NULL;
	guint32 timestamp = 0;
	guint i, n = 0;

	steam_msg_friend_messages_get_active_message_sessions_response_init(&resp);
	if (body != NULL && eresult == STEAM_ERESULT_OK &&
	    steam_msg_friend_messages_get_active_message_sessions_response_decode(&resp, body, len)) {
		n = resp.message_sessions->len;
		timestamp = resp.timestamp;
		sessions = g_new0(SteamCMMessageSession, n + 1);
		for (i = 0; i < n; i++) {
			const SteamMsgFriendMessageSession *src =
				&g_array_index(resp.message_sessions, SteamMsgFriendMessageSession, i);
			sessions[i].accountid_friend = src->accountid_friend;
			sessions[i].last_message = src->last_message;
			sessions[i].last_view = src->last_view;
			sessions[i].unread_message_count = src->unread_message_count;
		}
	}
	if (callback)
		callback(cm, sessions, n, timestamp, job->user_data);
	g_free(sessions);
	steam_msg_friend_messages_get_active_message_sessions_response_clear(&resp);
}

void
steam_cm_get_active_message_sessions(SteamCM *cm, guint32 since,
                                     SteamCMSessionsFunc callback, gpointer user_data)
{
	SteamMsgFriendMessagesGetActiveMessageSessionsRequest m;
	GByteArray *body;

	if (!cm_check_logged_on(cm, "get_active_message_sessions"))
		return;

	steam_msg_friend_messages_get_active_message_sessions_request_init(&m);
	STEAM_MSG_SET(&m, lastmessage_since, since);
	STEAM_MSG_SET(&m, only_sessions_with_messages, TRUE);
	body = g_byte_array_new();
	steam_msg_friend_messages_get_active_message_sessions_request_encode(&m, body);
	cm_call_service(cm, STEAM_METHOD_FRIEND_MESSAGES_GET_ACTIVE_SESSIONS, body,
	                cm_active_sessions_done, (gpointer) callback, user_data, 0);
	g_byte_array_unref(body);
	steam_msg_friend_messages_get_active_message_sessions_request_clear(&m);
}

/* --- Player.GetNicknameList --- */

static void
cm_nickname_list_done(SteamCM *cm, CMJob *job, SteamEResult eresult,
                      const guint8 *body, gsize len)
{
	SteamMsgPlayerGetNicknameListResponse resp;
	SteamCMNickname *nicks;
	guint i;

	if (body == NULL || eresult != STEAM_ERESULT_OK) {
		purple_debug_warning("steam", "CM: Player.GetNicknameList failed: %d (%s)\n",
		                     eresult, cm_eresult_name(eresult));
		return;
	}

	steam_msg_player_get_nickname_list_response_init(&resp);
	if (!steam_msg_player_get_nickname_list_response_decode(&resp, body, len)) {
		purple_debug_warning("steam", "CM: malformed GetNicknameList response\n");
		steam_msg_player_get_nickname_list_response_clear(&resp);
		return;
	}

	nicks = g_new0(SteamCMNickname, resp.nicknames->len + 1);
	for (i = 0; i < resp.nicknames->len; i++) {
		const SteamMsgPlayerNickname *src = &g_array_index(resp.nicknames, SteamMsgPlayerNickname, i);
		nicks[i].steamid = steam_cm_accountid_to_steamid(src->accountid);
		nicks[i].nickname = src->nickname;
	}
	if (cm->cb.nicknames)
		cm->cb.nicknames(cm, nicks, resp.nicknames->len, FALSE, FALSE, cm->user_data);
	g_free(nicks);
	steam_msg_player_get_nickname_list_response_clear(&resp);
}

void
steam_cm_get_nickname_list(SteamCM *cm)
{
	SteamMsgPlayerGetNicknameListRequest m;
	GByteArray *body;

	if (!cm_check_logged_on(cm, "get_nickname_list"))
		return;

	steam_msg_player_get_nickname_list_request_init(&m);
	body = g_byte_array_new();
	steam_msg_player_get_nickname_list_request_encode(&m, body);
	cm_call_service(cm, STEAM_METHOD_PLAYER_GET_NICKNAME_LIST, body,
	                cm_nickname_list_done, NULL, NULL, 0);
	g_byte_array_unref(body);
	steam_msg_player_get_nickname_list_request_clear(&m);
}

/* ------------------------------------------------------------------ */
/* Test hooks (not part of the public API; used by tests/test_cm.c).
 * A test-mode CM has no socket: packets are fed with
 * steam_cm__test_dispatch() and everything it sends is collected as
 * complete packets in steam_cm__test_sent(). It starts in the
 * "ClientLogon sent" state. */

SteamCM *steam_cm__test_new(SteamAccount *sa, const SteamCMCallbacks *callbacks,
                            gpointer user_data, guint64 steamid);
void steam_cm__test_dispatch(SteamCM *cm, const guint8 *data, gsize len);
GPtrArray *steam_cm__test_sent(SteamCM *cm);
void steam_cm__test_connection_lost(SteamCM *cm);

SteamCM *
steam_cm__test_new(SteamAccount *sa, const SteamCMCallbacks *callbacks,
                   gpointer user_data, guint64 steamid)
{
	SteamCM *cm = steam_cm_new(sa, callbacks, user_data);

	cm->test_mode = TRUE;
	cm->test_sent = g_ptr_array_new_with_free_func((GDestroyNotify) g_byte_array_unref);
	cm->steamid = steamid;
	cm->refresh_token = g_strdup("test.token.value");
	cm_send_hello_and_logon(cm);
	return cm;
}

void
steam_cm__test_dispatch(SteamCM *cm, const guint8 *data, gsize len)
{
	cm_enter(cm);
	if (CM_ALIVE(cm)) {
		cm->last_rx = g_get_monotonic_time();
		cm_dispatch(cm, data, len, 0);
	}
	cm_leave(cm);
}

GPtrArray *
steam_cm__test_sent(SteamCM *cm)
{
	return cm->test_sent;
}

/* Simulates a transport failure (fails jobs, schedules a reconnect). */
void
steam_cm__test_connection_lost(SteamCM *cm)
{
	cm_enter(cm);
	cm_connection_lost(cm, STEAM_ERESULT_NO_CONNECTION, "test");
	cm_leave(cm);
}
