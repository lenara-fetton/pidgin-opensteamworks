/*
 * Tests for steam_auth (IAuthenticationService login).
 *
 *   make -C tests test_auth && ./tests/test_auth
 *
 * 1. steam_auth_jwt_decode() on synthetic tokens (offline).
 * 2. Password login with a bogus account: expects the error callback with a
 *    non-OK EResult, which proves request encoding + response decoding.
 * 3. steam_auth_refresh_access_token() with a garbage token: non-OK EResult.
 * 4. Only if STEAM_TEST_USER and STEAM_TEST_PASS are set: a real login.
 *    Steam Guard codes are read from stdin; device confirmations just poll.
 *
 * Set STEAM_DEBUG=1 for libpurple debug output.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "purple_harness.h"
#include "steam_auth.h"

static int failures = 0;
static SteamAccount *sa = NULL;

#define CHECK(cond, ...) do { \
	if (cond) { printf("  ok: "); } else { printf("  FAIL: "); failures++; } \
	printf(__VA_ARGS__); printf("\n"); \
} while (0)

/* ---- 1. JWT ---- */

static gchar *
b64url(const gchar *s)
{
	gchar *b = g_base64_encode((const guchar *)s, strlen(s));
	gchar *p;
	gsize n;

	for (p = b; *p; p++) {
		if (*p == '+') *p = '-';
		else if (*p == '/') *p = '_';
	}
	n = strlen(b);
	while (n > 0 && b[n - 1] == '=')
		b[--n] = '\0';
	return b;
}

static gchar *
make_jwt(const gchar *payload_json)
{
	gchar *h = b64url("{\"typ\":\"JWT\",\"alg\":\"EdDSA\"}");
	gchar *p = b64url(payload_json);
	gchar *t = g_strdup_printf("%s.%s.c2lnbmF0dXJl", h, p);
	g_free(h);
	g_free(p);
	return t;
}

static void
test_jwt(void)
{
	gchar *tok;
	guint64 steamid;
	gint64 exp;
	gboolean client;
	gboolean ok;
	gint len;

	printf("JWT decode\n");

	/* payload lengths chosen so the base64url needs padding restored */
	for (len = 0; len < 3; len++) {
		gchar *payload = g_strdup_printf(
			"{\"iss\":\"steam\",\"sub\":\"76561197960287930\",\"aud\":[\"web\",\"client\"],"
			"\"exp\":1790000000,\"nbf\":1760000000,\"pad\":\"%.*s\"}", len, "xyz");
		tok = make_jwt(payload);
		ok = steam_auth_jwt_decode(tok, &steamid, &exp, &client);
		CHECK(ok && steamid == G_GUINT64_CONSTANT(76561197960287930) && exp == 1790000000 && client,
		      "client token (pad %d): ok=%d sub=%" G_GUINT64_FORMAT " exp=%" G_GINT64_FORMAT " client=%d",
		      len, ok, steamid, exp, client);
		g_free(tok);
		g_free(payload);
	}

	tok = make_jwt("{\"sub\":\"76561197960287931\",\"aud\":[\"web\"],\"exp\":1700000000}");
	ok = steam_auth_jwt_decode(tok, &steamid, &exp, &client);
	CHECK(ok && steamid == G_GUINT64_CONSTANT(76561197960287931) && exp == 1700000000 && !client,
	      "web-only token: client=%d", client);
	/* NULL out-params are allowed */
	CHECK(steam_auth_jwt_decode(tok, NULL, NULL, NULL), "NULL out-params");
	g_free(tok);

	tok = make_jwt("{\"sub\":\"1\",\"aud\":\"client\",\"exp\":5}");
	ok = steam_auth_jwt_decode(tok, &steamid, &exp, &client);
	CHECK(ok && client && steamid == 1 && exp == 5, "string aud");
	g_free(tok);

	CHECK(!steam_auth_jwt_decode("garbage", &steamid, &exp, &client), "rejects 'garbage'");
	CHECK(!steam_auth_jwt_decode("a.b", NULL, NULL, NULL), "rejects two segments");
	CHECK(!steam_auth_jwt_decode("a.!!!!.c", NULL, NULL, NULL), "rejects bad base64");
	CHECK(!steam_auth_jwt_decode(NULL, NULL, NULL, NULL), "rejects NULL");
	tok = make_jwt("[1,2,3]");
	CHECK(!steam_auth_jwt_decode(tok, NULL, NULL, NULL), "rejects non-object payload");
	g_free(tok);
	tok = make_jwt("not json");
	CHECK(!steam_auth_jwt_decode(tok, NULL, NULL, NULL), "rejects non-JSON payload");
	g_free(tok);
}

/* ---- async tests ---- */

typedef enum { STEP_BOGUS_LOGIN, STEP_REFRESH, STEP_REAL_LOGIN, STEP_DONE } Step;
static Step step;
static void next_step(void);

static void
on_guard(SteamAuth *auth, const SteamGuardType *allowed, guint n,
         const gchar *email_domain, gpointer user_data)
{
	guint i;
	SteamGuardType code_type = STEAM_GUARD_UNKNOWN;

	printf("  guard_required: %u type(s):", n);
	for (i = 0; i < n; i++) {
		printf(" %d", allowed[i]);
		if (allowed[i] == STEAM_GUARD_DEVICE_CODE || allowed[i] == STEAM_GUARD_EMAIL_CODE)
			if (code_type == STEAM_GUARD_UNKNOWN)
				code_type = allowed[i];
	}
	printf("%s%s\n", email_domain ? ", email domain " : "", email_domain ? email_domain : "");

	if (step != STEP_REAL_LOGIN) {
		printf("  FAIL: unexpected guard_required for bogus account\n");
		failures++;
		steam_auth_cancel(auth);
		next_step();
		return;
	}

	if (code_type != STEAM_GUARD_UNKNOWN) {
		char buf[64];
		printf("  enter %s code (or empty line to wait for app confirmation): ",
		       code_type == STEAM_GUARD_DEVICE_CODE ? "mobile authenticator" : "email");
		fflush(stdout);
		if (fgets(buf, sizeof(buf), stdin) != NULL) {
			g_strstrip(buf);
			if (*buf)
				steam_auth_submit_guard_code(auth, code_type, buf);
		}
	} else {
		printf("  approve the login in the Steam mobile app / email...\n");
	}
}

static void
on_success(SteamAuth *auth, const gchar *refresh_token, const gchar *access_token,
           guint64 steamid, const gchar *account_name, gpointer user_data)
{
	guint64 sub = 0;
	gint64 exp = 0;
	gboolean client = FALSE;
	gboolean ok;

	if (step != STEP_REAL_LOGIN) {
		printf("  FAIL: unexpected success\n");
		failures++;
		next_step();
		return;
	}

	ok = steam_auth_jwt_decode(refresh_token, &sub, &exp, &client);
	printf("  success: account %s steamid %" G_GUINT64_FORMAT ", access token %s\n",
	       account_name, steamid, access_token ? "present" : "absent");
	CHECK(ok, "refresh token decodes");
	printf("  refresh token: sub=%" G_GUINT64_FORMAT " exp=%" G_GINT64_FORMAT " (in %" G_GINT64_FORMAT " days) aud has client=%d\n",
	       sub, exp, (exp - (gint64)time(NULL)) / 86400, client);
	CHECK(client, "refresh token audience contains \"client\"");
	CHECK(sub == steamid, "sub matches steamid");
	next_step();
}

static void
on_error(SteamAuth *auth, SteamEResult eresult, const gchar *message,
         gboolean bad_credentials, gpointer user_data)
{
	printf("  error callback: eresult %d, bad_credentials %d, message \"%s\"\n",
	       (int)eresult, bad_credentials, message ? message : "(null)");

	if (step == STEP_BOGUS_LOGIN) {
		CHECK(eresult != STEAM_ERESULT_OK && eresult != STEAM_ERESULT_NO_CONNECTION &&
		      eresult != STEAM_ERESULT_INVALID, "bogus login rejected by Steam (not a transport error)");
		if (eresult == STEAM_ERESULT_INVALID_PASSWORD)
			CHECK(bad_credentials, "InvalidPassword => bad_credentials");
		CHECK(message != NULL && *message, "has a message");
	} else if (step == STEP_REAL_LOGIN) {
		printf("  FAIL: real login failed\n");
		failures++;
	}
	next_step();
}

static const SteamAuthCallbacks callbacks = { on_guard, on_success, on_error };

static void
on_refresh(SteamAccount *acct, const gchar *access_token, SteamEResult eresult, gpointer user_data)
{
	printf("  refresh callback: access_token=%s eresult %d\n",
	       access_token ? "(set)" : "NULL", (int)eresult);
	CHECK(access_token == NULL && eresult != STEAM_ERESULT_OK &&
	      eresult != STEAM_ERESULT_NO_CONNECTION, "garbage refresh token rejected by Steam");
	next_step();
}

static gboolean
start_step(gpointer data)
{
	switch (step) {
	case STEP_BOGUS_LOGIN:
		printf("Password login, bogus account\n");
		CHECK(steam_auth_login_password(sa, "pidgin_test_nonexistent_9f3a", "x",
		                                &callbacks, NULL) != NULL, "login started");
		break;
	case STEP_REFRESH:
		printf("GenerateAccessTokenForApp, garbage token\n");
		steam_auth_refresh_access_token(sa, "eyJnYXJiYWdlIjoxfQ.eyJzdWIiOiI3NjU2MTE5Nzk2MDI4NzkzMCJ9.AAAA",
		                                on_refresh, NULL);
		break;
	case STEP_REAL_LOGIN:
		printf("Real login as %s\n", getenv("STEAM_TEST_USER"));
		steam_auth_login_password(sa, getenv("STEAM_TEST_USER"), getenv("STEAM_TEST_PASS"),
		                          &callbacks, NULL);
		break;
	case STEP_DONE:
		purple_harness_quit(failures ? 1 : 0);
		break;
	}
	return FALSE;
}

static void
next_step(void)
{
	step++;
	if (step == STEP_REAL_LOGIN && !(getenv("STEAM_TEST_USER") && getenv("STEAM_TEST_PASS"))) {
		printf("Real login: skipped (set STEAM_TEST_USER / STEAM_TEST_PASS)\n");
		step++;
	}
	/* run outside the current callback stack */
	g_idle_add(start_step, NULL);
}

static gboolean
watchdog(gpointer data)
{
	printf("FAIL: timed out in step %d\n", step);
	failures++;
	purple_harness_quit(1);
	return FALSE;
}

int
main(int argc, char **argv)
{
	/* the real login may wait for a Steam Guard confirmation (up to 5 min) */
	guint limit = (getenv("STEAM_TEST_USER") ? 400 : 90);

	test_jwt();

	/* guards against nonsense input */
	sa = purple_harness_init("steam-test");
	CHECK(steam_auth_login_password(sa, "", "x", &callbacks, NULL) == NULL, "empty username -> NULL");
	CHECK(steam_auth_login_password(sa, "u", NULL, &callbacks, NULL) == NULL, "NULL password -> NULL");
	steam_auth_cancel(NULL);

	/* cancel right after start: no callback may fire */
	{
		SteamAuth *a = steam_auth_login_password(sa, "pidgin_test_nonexistent_9f3a", "x", &callbacks, NULL);
		CHECK(a != NULL, "login started (to be cancelled)");
		steam_auth_cancel(a);
		CHECK(sa->conns == NULL && g_queue_is_empty(sa->waiting_conns), "cancel removed the request");
	}

	step = STEP_BOGUS_LOGIN;
	g_idle_add(start_step, NULL);
	g_timeout_add_seconds(limit, watchdog, NULL);
	purple_harness_run();

	purple_harness_shutdown();
	printf("%s (%d failure%s)\n", failures ? "FAILED" : "PASSED", failures, failures == 1 ? "" : "s");
	return failures ? 1 : 0;
}
