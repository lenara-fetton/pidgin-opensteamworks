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
 * IAuthenticationService login. See steam_auth.h and docs/architecture.md.
 *
 * All requests go to https://api.steampowered.com/IAuthenticationService/
 * with the request protobuf base64-encoded in `input_protobuf_encoded`
 * (query string for GET, form body for POST). Responses are binary protobuf
 * with the EResult in the `x-eresult` header. Field numbers follow
 * steam/steammessages_auth.steamclient.proto in SteamDatabase/Protobufs.
 */

#include "steam_auth.h"
#include "steam_connection.h"
#include "steam_proto.h"

#include <string.h>
#include <stdlib.h>

/* steam_rsa.c is not a compilation unit of its own: libsteam.c has always
 * #included it. Include it here as well, renaming its (non-static) globals
 * so the two copies never clash at link time. */
#define hexstring_to_binary       steam_auth_rsa_hexstring_to_binary
#define pkcs1pad2                 steam_auth_rsa_pkcs1pad2
#define steam_encrypt_password    steam_auth_rsa_encrypt_password
#define steam_util_str_hex2bytes  steam_auth_rsa_str_hex2bytes
#define steam_crypt_rsa_enc       steam_auth_rsa_crypt_rsa_enc
#include "steam_rsa.c"

#define STEAM_AUTH_HOST            "api.steampowered.com"
#define STEAM_AUTH_PATH            "/IAuthenticationService/"

/* EAuthTokenPlatformType: SteamClient tokens have aud ["web","client"] and
 * are accepted by CM logon. */
#define STEAM_AUTH_PLATFORM_STEAMCLIENT  1
/* ESessionPersistence */
#define STEAM_AUTH_PERSISTENT            1
/* EOSType LinuxUnknown */
#define STEAM_AUTH_OS_TYPE               (-203)
/* EGamingDeviceType StandardPC */
#define STEAM_AUTH_GAMING_DEVICE_PC      1

#define STEAM_AUTH_POLL_TIMEOUT_SECS     300
#define STEAM_AUTH_DEFAULT_INTERVAL      5.0

/* EResults not (yet) in steam_eresult.h */
#define STEAM_AUTH_ERESULT_FILE_NOT_FOUND  9

typedef struct _SteamAuthRequest SteamAuthRequest;

/* Handler for a decoded response. `eresult` is from x-eresult (or
 * NO_CONNECTION on transport failure, in which case body is NULL). */
typedef void (*SteamAuthResponseFunc)(SteamAuth *auth, SteamAuthRequest *req,
                                      SteamEResult eresult, const gchar *message,
                                      const guint8 *body, gsize body_len);

struct _SteamAuthRequest {
	SteamAuth *auth;              /* NULL once cancelled / detached */
	SteamAuthResponseFunc func;
	const gchar *method;          /* static string, for logs */

	/* steam_auth_refresh_access_token() only (auth == NULL) */
	SteamAccount *sa;
	SteamAuthTokenFunc token_cb;
	gpointer token_user_data;
};

struct _SteamAuth {
	SteamAccount *sa;
	SteamAuthCallbacks cb;
	gpointer user_data;

	gchar *username;
	gchar *password;              /* wiped once encrypted */
	gchar *device_name;

	/* Session from BeginAuthSessionViaCredentials */
	guint64 client_id;
	GByteArray *request_id;
	guint64 steamid;
	gdouble interval;

	/* Allowed Steam Guard types and email domain from the same response,
	 * kept so a wrong guard code can re-trigger guard_required without
	 * needing a fresh BeginAuthSessionViaCredentials round trip. */
	GArray *allowed_guard_types;  /* SteamGuardType */
	gchar *email_domain;

	/* Polling */
	gboolean polling;             /* PollAuthSessionStatus loop active */
	gboolean poll_in_flight;
	guint poll_timer;
	gint64 poll_deadline;         /* g_get_monotonic_time() microseconds */

	GSList *pending;              /* SteamAuthRequest* in flight */

	/* Re-entrancy guards */
	guint callback_depth;         /* inside guard_required */
	gboolean cancelled;           /* steam_auth_cancel() during a callback */
	gboolean finished;            /* success/error callback running */
};

/* ------------------------------------------------------------------ */
/* Helpers                                                              */
/* ------------------------------------------------------------------ */

static const gchar *
steam_auth_eresult_name(SteamEResult eresult)
{
	switch ((int)eresult) {
		case STEAM_ERESULT_OK: return "OK";
		case STEAM_ERESULT_FAIL: return "Fail";
		case STEAM_ERESULT_NO_CONNECTION: return "NoConnection";
		case STEAM_ERESULT_INVALID_PASSWORD: return "InvalidPassword";
		case STEAM_ERESULT_INVALID_PARAM: return "InvalidParam";
		case STEAM_AUTH_ERESULT_FILE_NOT_FOUND: return "FileNotFound";
		case STEAM_ERESULT_BUSY: return "Busy";
		case STEAM_ERESULT_INVALID_STATE: return "InvalidState";
		case STEAM_ERESULT_INVALID_NAME: return "InvalidName";
		case STEAM_ERESULT_ACCESS_DENIED: return "AccessDenied";
		case STEAM_ERESULT_TIMEOUT: return "Timeout";
		case STEAM_ERESULT_BANNED: return "Banned";
		case STEAM_ERESULT_ACCOUNT_NOT_FOUND: return "AccountNotFound";
		case STEAM_ERESULT_SERVICE_UNAVAILABLE: return "ServiceUnavailable";
		case STEAM_ERESULT_LIMIT_EXCEEDED: return "LimitExceeded";
		case STEAM_ERESULT_REVOKED: return "Revoked";
		case STEAM_ERESULT_EXPIRED: return "Expired";
		case STEAM_ERESULT_DUPLICATE_REQUEST: return "DuplicateRequest";
		case STEAM_ERESULT_ACCOUNT_LOGON_DENIED: return "AccountLogonDenied";
		case STEAM_ERESULT_INVALID_LOGIN_AUTH_CODE: return "InvalidLoginAuthCode";
		case STEAM_ERESULT_RATE_LIMIT_EXCEEDED: return "RateLimitExceeded";
		case STEAM_ERESULT_ACCOUNT_LOGIN_DENIED_NEED_TWO_FACTOR: return "AccountLoginDeniedNeedTwoFactor";
		case STEAM_ERESULT_ACCOUNT_LOGIN_DENIED_THROTTLE: return "AccountLoginDeniedThrottle";
		case STEAM_ERESULT_TWO_FACTOR_CODE_MISMATCH: return "TwoFactorCodeMismatch";
		default: return "Unknown";
	}
}

/* Human-readable fallback message when Steam sends no x-error_message. */
static const gchar *
steam_auth_default_message(SteamEResult eresult)
{
	switch ((int)eresult) {
		case STEAM_ERESULT_INVALID_PASSWORD:
			return _("Incorrect account name or password");
		case STEAM_ERESULT_ACCOUNT_NOT_FOUND:
		case STEAM_ERESULT_INVALID_NAME:
			return _("Unknown Steam account name");
		case STEAM_ERESULT_TWO_FACTOR_CODE_MISMATCH:
		case STEAM_ERESULT_INVALID_LOGIN_AUTH_CODE:
			return _("Incorrect Steam Guard code");
		case STEAM_ERESULT_RATE_LIMIT_EXCEEDED:
		case STEAM_ERESULT_ACCOUNT_LOGIN_DENIED_THROTTLE:
		case STEAM_ERESULT_LIMIT_EXCEEDED:
			return _("Too many login attempts; try again later");
		case STEAM_ERESULT_NO_CONNECTION:
			return _("Could not connect to the Steam authentication server");
		case STEAM_ERESULT_TIMEOUT:
			return _("Timed out waiting for Steam Guard confirmation");
		case STEAM_ERESULT_EXPIRED:
		case STEAM_AUTH_ERESULT_FILE_NOT_FOUND:
			return _("The Steam login session expired");
		case STEAM_ERESULT_ACCESS_DENIED:
			return _("Steam denied access");
		case STEAM_ERESULT_BANNED:
			return _("This Steam account is banned");
		case STEAM_ERESULT_SERVICE_UNAVAILABLE:
		case STEAM_ERESULT_BUSY:
			return _("The Steam authentication service is unavailable");
		default:
			return _("Steam login failed");
	}
}

/* Does this EResult mean "the user typed something wrong"? */
static gboolean
steam_auth_is_bad_credentials(SteamEResult eresult)
{
	switch ((int)eresult) {
		case STEAM_ERESULT_INVALID_PASSWORD:
		case STEAM_ERESULT_ACCOUNT_NOT_FOUND:
		case STEAM_ERESULT_INVALID_NAME:
		case STEAM_ERESULT_TWO_FACTOR_CODE_MISMATCH:
		case STEAM_ERESULT_INVALID_LOGIN_AUTH_CODE:
			return TRUE;
		default:
			return FALSE;
	}
}

static gchar *
steam_auth_b64url_to_b64(const gchar *in)
{
	GString *s = g_string_new(in);
	gsize i;

	for (i = 0; i < s->len; i++) {
		if (s->str[i] == '-')
			s->str[i] = '+';
		else if (s->str[i] == '_')
			s->str[i] = '/';
	}
	while (s->len % 4 != 0)
		g_string_append_c(s, '=');

	return g_string_free(s, FALSE);
}

/* ------------------------------------------------------------------ */
/* Request plumbing                                                     */
/* ------------------------------------------------------------------ */

static void
steam_auth_raw_cb(SteamAccount *sa, const gchar *headers, const guint8 *body,
                  gsize body_len, gpointer user_data)
{
	SteamAuthRequest *req = user_data;
	SteamAuth *auth = req->auth;
	SteamEResult eresult;
	gchar *message = NULL;

	if (req->func == NULL) {
		/* cancelled after the fact */
		g_free(req);
		return;
	}

	if (auth != NULL)
		auth->pending = g_slist_remove(auth->pending, req);

	if (headers == NULL) {
		eresult = STEAM_ERESULT_NO_CONNECTION;
		body = NULL;
		body_len = 0;
	} else {
		gchar *hdr = steam_connection_get_header(headers, "x-eresult");
		guint status = steam_connection_get_status(headers);

		if (hdr != NULL && *hdr) {
			eresult = (SteamEResult)atoi(hdr);
		} else if (status == 200) {
			eresult = STEAM_ERESULT_OK;
		} else if (status == 429) {
			eresult = STEAM_ERESULT_RATE_LIMIT_EXCEEDED;
		} else if (status >= 500) {
			eresult = STEAM_ERESULT_SERVICE_UNAVAILABLE;
		} else {
			eresult = STEAM_ERESULT_FAIL;
		}
		g_free(hdr);

		message = steam_connection_get_header(headers, "x-error_message");
		if (message != NULL && *message == '\0') {
			g_free(message);
			message = NULL;
		}

		purple_debug_info("steam", "auth %s: HTTP %u, eresult %d (%s)%s%s\n",
				req->method, status, (int)eresult,
				steam_auth_eresult_name(eresult),
				message ? ", " : "", message ? message : "");
	}

	req->func(auth, req, eresult, message, body, body_len);

	g_free(message);
	g_free(req);
}

/* Sends `msg` to IAuthenticationService/<method>/v1. `auth` may be NULL
 * (then req must already carry sa). Takes ownership of req. */
static void
steam_auth_send(SteamAccount *sa, SteamAuth *auth, SteamAuthRequest *req,
                const gchar *method, gboolean post, const GByteArray *msg)
{
	SteamConnection *sconn;
	gchar *b64, *escaped, *url, *postdata = NULL;

	req->auth = auth;
	req->method = method;

	b64 = g_base64_encode(msg->data, msg->len);
	escaped = g_uri_escape_string(b64, NULL, FALSE);
	g_free(b64);

	if (post) {
		url = g_strdup_printf(STEAM_AUTH_PATH "%s/v1/", method);
		postdata = g_strdup_printf("input_protobuf_encoded=%s", escaped);
	} else {
		url = g_strdup_printf(STEAM_AUTH_PATH "%s/v1/?input_protobuf_encoded=%s", method, escaped);
	}
	g_free(escaped);

	if (auth != NULL)
		auth->pending = g_slist_prepend(auth->pending, req);

	sconn = steam_post_or_get(sa,
			(post ? STEAM_METHOD_POST : STEAM_METHOD_GET) | STEAM_METHOD_SSL,
			STEAM_AUTH_HOST, url, postdata, NULL, req, FALSE);
	sconn->raw_callback = steam_auth_raw_cb;

	if (postdata != NULL) {
		/* may contain the encrypted password */
		memset(postdata, 0, strlen(postdata));
		g_free(postdata);
	}
	g_free(url);
}

/* Detaches every in-flight request and stops the poll timer. */
static void
steam_auth_detach(SteamAuth *auth)
{
	GSList *l;

	if (auth->poll_timer) {
		purple_timeout_remove(auth->poll_timer);
		auth->poll_timer = 0;
	}
	auth->polling = FALSE;

	for (l = auth->pending; l != NULL; l = l->next) {
		SteamAuthRequest *req = l->data;

		if (steam_connection_cancel_by_user_data(auth->sa, req) > 0) {
			g_free(req);
		} else {
			/* Not found (e.g. in the HTTP 429 back-off): leave it for
			 * steam_auth_raw_cb to free when it eventually fires. */
			req->auth = NULL;
			req->func = NULL;
		}
	}
	g_slist_free(auth->pending);
	auth->pending = NULL;
}

static void
steam_auth_free(SteamAuth *auth)
{
	steam_auth_detach(auth);

	if (auth->password != NULL) {
		memset(auth->password, 0, strlen(auth->password));
		g_free(auth->password);
	}
	g_free(auth->username);
	g_free(auth->device_name);
	if (auth->request_id != NULL)
		g_byte_array_free(auth->request_id, TRUE);
	if (auth->allowed_guard_types != NULL)
		g_array_free(auth->allowed_guard_types, TRUE);
	g_free(auth->email_domain);
	g_free(auth);
}

static void
steam_auth_fail(SteamAuth *auth, SteamEResult eresult, const gchar *message,
                gboolean bad_credentials)
{
	if (auth->finished || auth->cancelled)
		return;

	if (message == NULL || *message == '\0')
		message = steam_auth_default_message(eresult);

	purple_debug_warning("steam", "auth failed: eresult %d (%s): %s\n",
			(int)eresult, steam_auth_eresult_name(eresult), message);

	steam_auth_detach(auth);
	auth->finished = TRUE;
	if (auth->cb.error != NULL)
		auth->cb.error(auth, eresult, message, bad_credentials, auth->user_data);
	steam_auth_free(auth);
}

/* Fails with the EResult of a failed call. */
static void
steam_auth_fail_eresult(SteamAuth *auth, SteamEResult eresult, const gchar *message)
{
	steam_auth_fail(auth, eresult, message, steam_auth_is_bad_credentials(eresult));
}

/* ------------------------------------------------------------------ */
/* Polling                                                              */
/* ------------------------------------------------------------------ */

static void steam_auth_send_poll(SteamAuth *auth);

static gboolean
steam_auth_poll_timeout_cb(gpointer data)
{
	SteamAuth *auth = data;

	auth->poll_timer = 0;
	steam_auth_send_poll(auth);

	return FALSE;
}

/* (Re)schedules the next poll `delay_ms` from now, unless one is in flight
 * (its response reschedules). */
static void
steam_auth_schedule_poll(SteamAuth *auth, guint delay_ms)
{
	if (!auth->polling || auth->poll_in_flight)
		return;

	if (auth->poll_timer)
		purple_timeout_remove(auth->poll_timer);
	auth->poll_timer = purple_timeout_add(delay_ms, steam_auth_poll_timeout_cb, auth);
}

static guint
steam_auth_interval_ms(SteamAuth *auth)
{
	gdouble interval = auth->interval;

	if (interval < 1.0 || interval > 60.0)
		interval = STEAM_AUTH_DEFAULT_INTERVAL;

	return (guint)(interval * 1000.0);
}

static void
steam_auth_start_polling(SteamAuth *auth, guint first_delay_ms)
{
	gint64 min_deadline = g_get_monotonic_time() + (gint64)60 * G_USEC_PER_SEC;

	if (!auth->polling) {
		auth->polling = TRUE;
		auth->poll_deadline = g_get_monotonic_time() +
				(gint64)STEAM_AUTH_POLL_TIMEOUT_SECS * G_USEC_PER_SEC;
	} else if (auth->poll_deadline < min_deadline) {
		/* a code was just submitted: give it time to go through */
		auth->poll_deadline = min_deadline;
	}

	steam_auth_schedule_poll(auth, first_delay_ms);
}

static void
steam_auth_poll_cb(SteamAuth *auth, SteamAuthRequest *req, SteamEResult eresult,
                   const gchar *message, const guint8 *body, gsize body_len)
{
	SteamProtoReader r;
	gchar *refresh_token = NULL, *access_token = NULL, *account_name = NULL;
	gboolean had_remote_interaction = FALSE;

	auth->poll_in_flight = FALSE;

	if (eresult == STEAM_ERESULT_NO_CONNECTION || eresult == STEAM_ERESULT_BUSY ||
			eresult == STEAM_ERESULT_SERVICE_UNAVAILABLE) {
		/* transient; keep trying until the deadline */
		purple_debug_warning("steam", "auth poll: transient failure %d\n", (int)eresult);
		if (g_get_monotonic_time() >= auth->poll_deadline) {
			steam_auth_fail(auth, eresult, message, FALSE);
		} else {
			steam_auth_schedule_poll(auth, steam_auth_interval_ms(auth));
		}
		return;
	}

	if (eresult != STEAM_ERESULT_OK) {
		steam_auth_fail_eresult(auth, eresult, message);
		return;
	}

	steam_proto_reader_init(&r, body, body_len);
	while (steam_proto_next(&r)) {
		switch (r.field) {
			case 1: /* new_client_id */
				if (r.wt == STEAM_PROTO_WT_VARINT && r.varint != 0) {
					purple_debug_info("steam", "auth poll: new client id\n");
					auth->client_id = r.varint;
				}
				break;
			case 3: /* refresh_token */
				if (r.wt == STEAM_PROTO_WT_LEN) {
					g_free(refresh_token);
					refresh_token = steam_proto_dup_string(&r);
				}
				break;
			case 4: /* access_token */
				if (r.wt == STEAM_PROTO_WT_LEN) {
					g_free(access_token);
					access_token = steam_proto_dup_string(&r);
				}
				break;
			case 5: /* had_remote_interaction */
				if (r.wt == STEAM_PROTO_WT_VARINT)
					had_remote_interaction = steam_proto_bool(&r);
				break;
			case 6: /* account_name */
				if (r.wt == STEAM_PROTO_WT_LEN) {
					g_free(account_name);
					account_name = steam_proto_dup_string(&r);
				}
				break;
			/* 2 new_challenge_url (QR only), 7 new_guard_data,
			 * 8 agreement_session_url: unused */
			default:
				break;
		}
	}

	if (r.error) {
		purple_debug_error("steam", "auth poll: malformed response\n");
		steam_auth_fail(auth, STEAM_ERESULT_FAIL, _("Malformed response from Steam"), FALSE);
	} else if (refresh_token != NULL && *refresh_token) {
		guint64 steamid = auth->steamid;
		const gchar *name = (account_name && *account_name) ? account_name : auth->username;

		if (steamid == 0)
			steam_auth_jwt_decode(refresh_token, &steamid, NULL, NULL);

		purple_debug_info("steam", "auth: login complete for %s\n", name);

		steam_auth_detach(auth);
		auth->finished = TRUE;
		if (auth->cb.success != NULL)
			auth->cb.success(auth, refresh_token,
					(access_token && *access_token) ? access_token : NULL,
					steamid, name, auth->user_data);
		steam_auth_free(auth);
	} else if (g_get_monotonic_time() >= auth->poll_deadline) {
		steam_auth_fail(auth, STEAM_ERESULT_TIMEOUT, NULL, FALSE);
	} else {
		purple_debug_info("steam", "auth poll: not yet (remote interaction: %d)\n",
				had_remote_interaction);
		steam_auth_schedule_poll(auth, steam_auth_interval_ms(auth));
	}

	if (refresh_token != NULL) {
		memset(refresh_token, 0, strlen(refresh_token));
		g_free(refresh_token);
	}
	g_free(access_token);
	g_free(account_name);
}

static void
steam_auth_send_poll(SteamAuth *auth)
{
	GByteArray *msg;
	SteamAuthRequest *req;

	if (auth->poll_in_flight)
		return;

	msg = g_byte_array_new();
	steam_proto_put_varint(msg, 1, auth->client_id);
	if (auth->request_id != NULL)
		steam_proto_put_bytes(msg, 2, auth->request_id->data, auth->request_id->len);

	req = g_new0(SteamAuthRequest, 1);
	req->func = steam_auth_poll_cb;
	auth->poll_in_flight = TRUE;
	steam_auth_send(auth->sa, auth, req, "PollAuthSessionStatus", TRUE, msg);

	g_byte_array_free(msg, TRUE);
}

/* ------------------------------------------------------------------ */
/* Steam Guard code                                                     */
/* ------------------------------------------------------------------ */

static void
steam_auth_guard_code_cb(SteamAuth *auth, SteamAuthRequest *req, SteamEResult eresult,
                         const gchar *message, const guint8 *body, gsize body_len)
{
	if (eresult == STEAM_ERESULT_OK || eresult == STEAM_ERESULT_DUPLICATE_REQUEST) {
		/* DUPLICATE_REQUEST: session already confirmed (e.g. approved in
		 * the app at the same time). Either way the tokens are ready. */
		purple_debug_info("steam", "auth: Steam Guard code accepted\n");
		/* (re)starts polling and polls right away */
		steam_auth_start_polling(auth, 0);
		return;
	}

	if (eresult == STEAM_ERESULT_TWO_FACTOR_CODE_MISMATCH ||
	    eresult == STEAM_ERESULT_INVALID_LOGIN_AUTH_CODE) {
		/* Wrong code: Steam keeps the auth session (client_id/request_id)
		 * valid across a bad guard code, so re-prompt instead of failing
		 * the whole login. If DEVICE_CONFIRMATION/EMAIL_CONFIRMATION was
		 * among the allowed types, polling is already running and keeps
		 * going unchanged; otherwise the caller can just submit another
		 * code via steam_auth_submit_guard_code(). */
		purple_debug_info("steam", "auth: Steam Guard code rejected (%s), re-prompting\n",
				steam_auth_eresult_name(eresult));

		if (auth->cb.guard_required != NULL) {
			auth->callback_depth++;
			auth->cb.guard_required(auth,
					auth->allowed_guard_types != NULL ?
						(const SteamGuardType *)(void *)auth->allowed_guard_types->data : NULL,
					auth->allowed_guard_types != NULL ? auth->allowed_guard_types->len : 0,
					auth->email_domain, auth->user_data);
			auth->callback_depth--;
			if (auth->cancelled && auth->callback_depth == 0)
				steam_auth_free(auth);
		}
		return;
	}

	steam_auth_fail_eresult(auth, eresult, message);
}

void
steam_auth_submit_guard_code(SteamAuth *auth, SteamGuardType type, const gchar *code)
{
	GByteArray *msg;
	SteamAuthRequest *req;
	gchar *clean;

	g_return_if_fail(auth != NULL);

	if (auth->finished || auth->cancelled || auth->client_id == 0) {
		purple_debug_warning("steam", "auth: guard code submitted in wrong state\n");
		return;
	}
	if (type != STEAM_GUARD_EMAIL_CODE && type != STEAM_GUARD_DEVICE_CODE) {
		purple_debug_warning("steam", "auth: guard type %d does not take a code\n", type);
		return;
	}

	clean = g_strdup(code ? code : "");
	g_strstrip(clean);
	if (type == STEAM_GUARD_DEVICE_CODE || type == STEAM_GUARD_EMAIL_CODE) {
		/* codes are case-insensitive upper-case alphanumerics */
		gchar *up = g_ascii_strup(clean, -1);
		g_free(clean);
		clean = up;
	}

	msg = g_byte_array_new();
	steam_proto_put_varint(msg, 1, auth->client_id);
	steam_proto_put_fixed64(msg, 2, auth->steamid);
	steam_proto_put_string(msg, 3, clean);
	steam_proto_put_varint(msg, 4, (guint64)type);
	g_free(clean);

	req = g_new0(SteamAuthRequest, 1);
	req->func = steam_auth_guard_code_cb;
	steam_auth_send(auth->sa, auth, req, "UpdateAuthSessionWithSteamGuardCode", TRUE, msg);

	g_byte_array_free(msg, TRUE);
}

/* ------------------------------------------------------------------ */
/* BeginAuthSessionViaCredentials                                       */
/* ------------------------------------------------------------------ */

static void
steam_auth_begin_cb(SteamAuth *auth, SteamAuthRequest *req, SteamEResult eresult,
                    const gchar *message, const guint8 *body, gsize body_len)
{
	SteamProtoReader r;
	GArray *allowed;
	gchar *email_domain = NULL;
	gchar *extended_error = NULL;
	gboolean need_poll = FALSE, need_code = FALSE, no_guard = FALSE;
	guint i;

	if (eresult != STEAM_ERESULT_OK) {
		/* extended_error_message may be in the body even on failure */
		if (body != NULL && body_len > 0) {
			steam_proto_reader_init(&r, body, body_len);
			while (steam_proto_next(&r)) {
				if (r.field == 8 && r.wt == STEAM_PROTO_WT_LEN) {
					g_free(extended_error);
					extended_error = steam_proto_dup_string(&r);
				}
			}
		}
		steam_auth_fail_eresult(auth, eresult,
				(extended_error && *extended_error) ? extended_error : message);
		g_free(extended_error);
		return;
	}

	allowed = g_array_new(FALSE, FALSE, sizeof(SteamGuardType));

	steam_proto_reader_init(&r, body, body_len);
	while (steam_proto_next(&r)) {
		switch (r.field) {
			case 1: /* client_id */
				if (r.wt == STEAM_PROTO_WT_VARINT)
					auth->client_id = r.varint;
				break;
			case 2: /* request_id (bytes) */
				if (r.wt == STEAM_PROTO_WT_LEN) {
					if (auth->request_id != NULL)
						g_byte_array_free(auth->request_id, TRUE);
					auth->request_id = steam_proto_dup_bytes(&r);
				}
				break;
			case 3: /* interval (float) */
				if (r.wt == STEAM_PROTO_WT_FIXED32) {
					union { guint32 u; gfloat f; } conv;
					conv.u = (guint32)r.fixed;
					auth->interval = conv.f;
				}
				break;
			case 4: /* allowed_confirmations */
				if (r.wt == STEAM_PROTO_WT_LEN) {
					SteamProtoReader sub;
					SteamGuardType type = STEAM_GUARD_UNKNOWN;
					gchar *assoc = NULL;

					steam_proto_sub_reader(&r, &sub);
					while (steam_proto_next(&sub)) {
						if (sub.field == 1 && sub.wt == STEAM_PROTO_WT_VARINT) {
							type = (SteamGuardType)sub.varint;
						} else if (sub.field == 2 && sub.wt == STEAM_PROTO_WT_LEN) {
							g_free(assoc);
							assoc = steam_proto_dup_string(&sub);
						}
					}
					purple_debug_info("steam", "auth: allowed confirmation %d%s%s\n",
							type, assoc ? " " : "", assoc ? assoc : "");
					if (type == STEAM_GUARD_EMAIL_CODE && assoc && *assoc && email_domain == NULL) {
						email_domain = assoc;
						assoc = NULL;
					}
					g_free(assoc);
					g_array_append_val(allowed, type);
				}
				break;
			case 5: /* steamid */
				if (r.wt == STEAM_PROTO_WT_VARINT)
					auth->steamid = r.varint;
				break;
			case 8: /* extended_error_message */
				if (r.wt == STEAM_PROTO_WT_LEN) {
					g_free(extended_error);
					extended_error = steam_proto_dup_string(&r);
				}
				break;
			/* 6 weak_token, 7 agreement_session_url: unused */
			default:
				break;
		}
	}

	if (r.error || auth->client_id == 0) {
		purple_debug_error("steam", "auth: bad BeginAuthSessionViaCredentials response\n");
		steam_auth_fail(auth, STEAM_ERESULT_FAIL,
				(extended_error && *extended_error) ? extended_error : _("Malformed response from Steam"),
				FALSE);
		goto out;
	}

	purple_debug_info("steam", "auth: session started, interval %.1fs, %u confirmation type(s)\n",
			auth->interval, allowed->len);

	/* Remember these for steam_auth_guard_code_cb(), which needs to
	 * re-trigger guard_required() on a wrong code without a fresh
	 * BeginAuthSessionViaCredentials round trip. */
	if (auth->allowed_guard_types != NULL)
		g_array_free(auth->allowed_guard_types, TRUE);
	auth->allowed_guard_types = g_array_new(FALSE, FALSE, sizeof(SteamGuardType));
	g_array_append_vals(auth->allowed_guard_types, allowed->data, allowed->len);
	g_free(auth->email_domain);
	auth->email_domain = g_strdup(email_domain);

	for (i = 0; i < allowed->len; i++) {
		switch (g_array_index(allowed, SteamGuardType, i)) {
			case STEAM_GUARD_NONE:
				no_guard = TRUE;
				break;
			case STEAM_GUARD_DEVICE_CONFIRMATION:
			case STEAM_GUARD_EMAIL_CONFIRMATION:
				need_poll = TRUE;
				break;
			case STEAM_GUARD_EMAIL_CODE:
			case STEAM_GUARD_DEVICE_CODE:
				need_code = TRUE;
				break;
			default:
				break;
		}
	}

	if (no_guard || allowed->len == 0) {
		/* No Steam Guard (or a remembered machine): tokens are ready */
		steam_auth_start_polling(auth, 0);
		goto out;
	}

	if (!need_poll && !need_code) {
		/* Only types we cannot satisfy (machine token etc.). Polling is
		 * the best we can do; it times out if nothing happens. */
		purple_debug_warning("steam", "auth: no supported Steam Guard type offered\n");
		need_poll = TRUE;
	}

	if (need_poll)
		steam_auth_start_polling(auth, steam_auth_interval_ms(auth));

	if (auth->cb.guard_required != NULL) {
		auth->callback_depth++;
		auth->cb.guard_required(auth, (const SteamGuardType *)(void *)allowed->data,
				allowed->len, email_domain, auth->user_data);
		auth->callback_depth--;
		if (auth->cancelled && auth->callback_depth == 0) {
			steam_auth_free(auth);
			goto out;
		}
	} else if (!need_poll) {
		steam_auth_fail(auth, STEAM_ERESULT_ACCOUNT_LOGIN_DENIED_NEED_TWO_FACTOR,
				_("A Steam Guard code is required"), FALSE);
		goto out;
	}

out:
	g_array_free(allowed, TRUE);
	g_free(email_domain);
	g_free(extended_error);
}

/* ------------------------------------------------------------------ */
/* GetPasswordRSAPublicKey                                              */
/* ------------------------------------------------------------------ */

static void
steam_auth_rsa_cb(SteamAuth *auth, SteamAuthRequest *req, SteamEResult eresult,
                  const gchar *message, const guint8 *body, gsize body_len)
{
	SteamProtoReader r;
	gchar *mod = NULL, *exp = NULL, *encrypted = NULL;
	guint64 timestamp = 0;
	GByteArray *msg, *details;
	SteamAuthRequest *breq;

	if (eresult != STEAM_ERESULT_OK) {
		steam_auth_fail_eresult(auth, eresult, message);
		return;
	}

	steam_proto_reader_init(&r, body, body_len);
	while (steam_proto_next(&r)) {
		if (r.field == 1 && r.wt == STEAM_PROTO_WT_LEN) {
			g_free(mod);
			mod = steam_proto_dup_string(&r);
		} else if (r.field == 2 && r.wt == STEAM_PROTO_WT_LEN) {
			g_free(exp);
			exp = steam_proto_dup_string(&r);
		} else if (r.field == 3 && r.wt == STEAM_PROTO_WT_VARINT) {
			timestamp = r.varint;
		}
	}

	if (r.error || mod == NULL || exp == NULL || !*mod || !*exp ||
			strlen(mod) % 2 != 0 || strlen(exp) % 2 != 0) {
		purple_debug_error("steam", "auth: bad RSA key response\n");
		steam_auth_fail(auth, STEAM_ERESULT_FAIL, _("Malformed response from Steam"), FALSE);
		goto out;
	}

	encrypted = steam_encrypt_password(mod, exp, auth->password);
	memset(auth->password, 0, strlen(auth->password));
	g_free(auth->password);
	auth->password = NULL;

	if (encrypted == NULL) {
		steam_auth_fail(auth, STEAM_ERESULT_FAIL, _("Could not encrypt the password"), FALSE);
		goto out;
	}

	details = g_byte_array_new();
	steam_proto_put_string(details, 1, auth->device_name);
	steam_proto_put_varint(details, 2, STEAM_AUTH_PLATFORM_STEAMCLIENT);
	steam_proto_put_int32(details, 3, STEAM_AUTH_OS_TYPE);
	steam_proto_put_varint(details, 4, STEAM_AUTH_GAMING_DEVICE_PC);

	msg = g_byte_array_new();
	steam_proto_put_string(msg, 1, auth->device_name);
	steam_proto_put_string(msg, 2, auth->username);
	steam_proto_put_string(msg, 3, encrypted);
	steam_proto_put_varint(msg, 4, timestamp);
	steam_proto_put_bool(msg, 5, TRUE);
	steam_proto_put_varint(msg, 6, STEAM_AUTH_PLATFORM_STEAMCLIENT);
	steam_proto_put_varint(msg, 7, STEAM_AUTH_PERSISTENT);
	steam_proto_put_string(msg, 8, "Client");
	steam_proto_put_message(msg, 9, details);
	g_byte_array_free(details, TRUE);

	breq = g_new0(SteamAuthRequest, 1);
	breq->func = steam_auth_begin_cb;
	steam_auth_send(auth->sa, auth, breq, "BeginAuthSessionViaCredentials", TRUE, msg);

	memset(msg->data, 0, msg->len);
	g_byte_array_free(msg, TRUE);

out:
	g_free(mod);
	g_free(exp);
	g_free(encrypted);
}

SteamAuth *
steam_auth_login_password(SteamAccount *sa, const gchar *username,
                          const gchar *password,
                          const SteamAuthCallbacks *callbacks,
                          gpointer user_data)
{
	SteamAuth *auth;
	GByteArray *msg;
	SteamAuthRequest *req;

	g_return_val_if_fail(sa != NULL, NULL);
	g_return_val_if_fail(callbacks != NULL, NULL);

	if (username == NULL || !*username || password == NULL || !*password) {
		purple_debug_error("steam", "auth: empty username or password\n");
		return NULL;
	}

	auth = g_new0(SteamAuth, 1);
	auth->sa = sa;
	auth->cb = *callbacks;
	auth->user_data = user_data;
	auth->username = g_strdup(username);
	auth->password = g_strdup(password);
	auth->device_name = g_strdup_printf("Pidgin (%s)", g_get_host_name());
	auth->interval = STEAM_AUTH_DEFAULT_INTERVAL;

	purple_debug_info("steam", "auth: starting password login for %s\n", username);

	msg = g_byte_array_new();
	steam_proto_put_string(msg, 1, username);

	req = g_new0(SteamAuthRequest, 1);
	req->func = steam_auth_rsa_cb;
	steam_auth_send(sa, auth, req, "GetPasswordRSAPublicKey", FALSE, msg);

	g_byte_array_free(msg, TRUE);

	return auth;
}

void
steam_auth_cancel(SteamAuth *auth)
{
	if (auth == NULL)
		return;

	if (auth->finished) {
		/* Called from inside success/error: the object is freed as soon
		 * as that callback returns. */
		return;
	}

	if (auth->callback_depth > 0) {
		/* Called from inside guard_required: free on return. */
		auth->cancelled = TRUE;
		steam_auth_detach(auth);
		return;
	}

	purple_debug_info("steam", "auth: cancelled\n");
	steam_auth_free(auth);
}

/* ------------------------------------------------------------------ */
/* GenerateAccessTokenForApp                                            */
/* ------------------------------------------------------------------ */

static void
steam_auth_refresh_cb(SteamAuth *auth, SteamAuthRequest *req, SteamEResult eresult,
                      const gchar *message, const guint8 *body, gsize body_len)
{
	SteamProtoReader r;
	gchar *access_token = NULL;

	if (eresult == STEAM_ERESULT_OK) {
		steam_proto_reader_init(&r, body, body_len);
		while (steam_proto_next(&r)) {
			if (r.field == 1 && r.wt == STEAM_PROTO_WT_LEN) {
				g_free(access_token);
				access_token = steam_proto_dup_string(&r);
			}
			/* 2 refresh_token: only with renewal_type Allow; unused */
		}
		if (r.error || access_token == NULL || !*access_token) {
			purple_debug_error("steam", "auth: no access token in response\n");
			g_free(access_token);
			access_token = NULL;
			eresult = STEAM_ERESULT_FAIL;
		}
	} else {
		purple_debug_warning("steam", "auth: GenerateAccessTokenForApp failed: %d (%s)%s%s\n",
				(int)eresult, steam_auth_eresult_name(eresult),
				message ? ": " : "", message ? message : "");
	}

	if (req->token_cb != NULL)
		req->token_cb(req->sa, access_token, eresult, req->token_user_data);

	g_free(access_token);
}

void
steam_auth_refresh_access_token(SteamAccount *sa, const gchar *refresh_token,
                                SteamAuthTokenFunc callback, gpointer user_data)
{
	GByteArray *msg;
	SteamAuthRequest *req;
	guint64 steamid = 0;

	g_return_if_fail(sa != NULL);

	if (refresh_token == NULL || !*refresh_token) {
		if (callback != NULL)
			callback(sa, NULL, STEAM_ERESULT_INVALID_PARAM, user_data);
		return;
	}

	/* steamid is optional; a garbage token just leaves it out */
	steam_auth_jwt_decode(refresh_token, &steamid, NULL, NULL);

	msg = g_byte_array_new();
	steam_proto_put_string(msg, 1, refresh_token);
	if (steamid != 0)
		steam_proto_put_fixed64(msg, 2, steamid);
	steam_proto_put_varint(msg, 3, 0); /* ETokenRenewalType None */

	req = g_new0(SteamAuthRequest, 1);
	req->func = steam_auth_refresh_cb;
	req->sa = sa;
	req->token_cb = callback;
	req->token_user_data = user_data;
	steam_auth_send(sa, NULL, req, "GenerateAccessTokenForApp", TRUE, msg);

	memset(msg->data, 0, msg->len);
	g_byte_array_free(msg, TRUE);
}

/* ------------------------------------------------------------------ */
/* JWT                                                                  */
/* ------------------------------------------------------------------ */

gboolean
steam_auth_jwt_decode(const gchar *token, guint64 *steamid,
                      gint64 *expiry, gboolean *is_client_token)
{
	gchar **parts;
	gchar *b64;
	guchar *payload;
	gsize payload_len = 0;
	JsonParser *parser;
	JsonNode *root;
	JsonObject *obj;
	gboolean ok = FALSE;

	if (steamid) *steamid = 0;
	if (expiry) *expiry = 0;
	if (is_client_token) *is_client_token = FALSE;

	if (token == NULL)
		return FALSE;

	parts = g_strsplit(token, ".", 4);
	if (g_strv_length(parts) != 3 || !*parts[1]) {
		g_strfreev(parts);
		return FALSE;
	}

	b64 = steam_auth_b64url_to_b64(parts[1]);
	g_strfreev(parts);
	payload = g_base64_decode(b64, &payload_len);
	g_free(b64);
	if (payload == NULL || payload_len == 0) {
		g_free(payload);
		return FALSE;
	}

	parser = json_parser_new();
	if (json_parser_load_from_data(parser, (const gchar *)payload, payload_len, NULL) &&
			(root = json_parser_get_root(parser)) != NULL &&
			JSON_NODE_HOLDS_OBJECT(root)) {
		JsonNode *node;

		obj = json_node_get_object(root);
		ok = TRUE;

		node = json_object_get_member(obj, "sub");
		if (steamid && node && JSON_NODE_HOLDS_VALUE(node)) {
			if (json_node_get_value_type(node) == G_TYPE_STRING)
				*steamid = g_ascii_strtoull(json_node_get_string(node), NULL, 10);
			else if (json_node_get_value_type(node) == G_TYPE_INT64)
				*steamid = (guint64)json_node_get_int(node);
		}

		node = json_object_get_member(obj, "exp");
		if (expiry && node && JSON_NODE_HOLDS_VALUE(node)) {
			if (json_node_get_value_type(node) == G_TYPE_INT64)
				*expiry = json_node_get_int(node);
			else if (json_node_get_value_type(node) == G_TYPE_DOUBLE)
				*expiry = (gint64)json_node_get_double(node);
			else if (json_node_get_value_type(node) == G_TYPE_STRING)
				*expiry = g_ascii_strtoll(json_node_get_string(node), NULL, 10);
		}

		node = json_object_get_member(obj, "aud");
		if (is_client_token && node) {
			if (JSON_NODE_HOLDS_ARRAY(node)) {
				JsonArray *arr = json_node_get_array(node);
				guint i, n = json_array_get_length(arr);
				for (i = 0; i < n; i++) {
					JsonNode *el = json_array_get_element(arr, i);
					if (JSON_NODE_HOLDS_VALUE(el) &&
							json_node_get_value_type(el) == G_TYPE_STRING &&
							g_str_equal(json_node_get_string(el), "client")) {
						*is_client_token = TRUE;
						break;
					}
				}
			} else if (JSON_NODE_HOLDS_VALUE(node) &&
					json_node_get_value_type(node) == G_TYPE_STRING) {
				*is_client_token = g_str_equal(json_node_get_string(node), "client");
			}
		}
	}
	g_object_unref(parser);
	g_free(payload);

	return ok;
}
