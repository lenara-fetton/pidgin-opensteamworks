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
 * Steam login via IAuthenticationService (api.steampowered.com, HTTPS).
 *
 * Flow (see docs/architecture.md):
 *   GetPasswordRSAPublicKey -> BeginAuthSessionViaCredentials ->
 *   [UpdateAuthSessionWithSteamGuardCode] -> PollAuthSessionStatus
 * The result is a long-lived REFRESH TOKEN (JWT, ~200 days) which the CM
 * layer uses to log on, plus a short-lived access token for web calls.
 *
 * Everything is asynchronous on top of steam_post_or_get(); results are
 * delivered through SteamAuthCallbacks.
 */

#ifndef STEAM_AUTH_H
#define STEAM_AUTH_H

#include "libsteam.h"
#include "steam_eresult.h"

typedef struct _SteamAuth SteamAuth;

/* EAuthSessionGuardType */
typedef enum {
	STEAM_GUARD_UNKNOWN = 0,
	STEAM_GUARD_NONE = 1,
	STEAM_GUARD_EMAIL_CODE = 2,           /* code sent by email */
	STEAM_GUARD_DEVICE_CODE = 3,          /* TOTP from the Steam mobile app */
	STEAM_GUARD_DEVICE_CONFIRMATION = 4,  /* approve in the mobile app, no code */
	STEAM_GUARD_EMAIL_CONFIRMATION = 5,   /* approve via email link */
	STEAM_GUARD_MACHINE_TOKEN = 6,
	STEAM_GUARD_LEGACY_MACHINE_AUTH = 7
} SteamGuardType;

typedef struct {
	/* Steam wants a Steam Guard confirmation. `allowed` lists the types the
	 * server accepts for this session. If DEVICE_CONFIRMATION or
	 * EMAIL_CONFIRMATION is among them, the auth object keeps polling and
	 * will call `success` once the user approves out-of-band; the caller
	 * may still submit a code via steam_auth_submit_guard_code() (for
	 * EMAIL_CODE / DEVICE_CODE). `email_domain` may be NULL. */
	void (*guard_required)(SteamAuth *auth, const SteamGuardType *allowed,
	                       guint n_allowed, const gchar *email_domain,
	                       gpointer user_data);

	/* Login finished. `refresh_token` must be persisted by the caller;
	 * `access_token` may be NULL. `steamid` is the 64-bit SteamID. The
	 * SteamAuth object is freed after this callback returns. */
	void (*success)(SteamAuth *auth, const gchar *refresh_token,
	                const gchar *access_token, guint64 steamid,
	                const gchar *account_name, gpointer user_data);

	/* Login failed. `bad_credentials` is TRUE for wrong password / wrong
	 * guard code, so the caller can decide whether to re-prompt or give
	 * up. The SteamAuth object is freed after this callback returns. */
	void (*error)(SteamAuth *auth, SteamEResult eresult, const gchar *message,
	              gboolean bad_credentials, gpointer user_data);
} SteamAuthCallbacks;

/* Starts a password login. Returns NULL only on immediate failure. */
SteamAuth *steam_auth_login_password(SteamAccount *sa, const gchar *username,
                                     const gchar *password,
                                     const SteamAuthCallbacks *callbacks,
                                     gpointer user_data);

/* Submits an email or TOTP code after guard_required. */
void steam_auth_submit_guard_code(SteamAuth *auth, SteamGuardType type,
                                  const gchar *code);

/* Aborts an in-progress login and frees the object. No callbacks fire. */
void steam_auth_cancel(SteamAuth *auth);

/* Exchanges a refresh token for a fresh access token
 * (GenerateAccessTokenForApp). `access_token` is NULL on failure, in which
 * case `eresult` explains why (EXPIRED / ACCESS_DENIED mean the refresh
 * token is dead and the caller must do a password login again).
 * CAUTION: per node-steam-session, refresh tokens issued for platform
 * SteamClient (which is what steam_auth_login_password() requests, since
 * only those work for CM logon) are refused with ACCESS_DENIED by this
 * WebAPI call; for them it only works over an authenticated CM session. Do
 * NOT use this to validate the login token -- check its expiry with
 * steam_auth_jwt_decode() and let the CM logon decide. */
typedef void (*SteamAuthTokenFunc)(SteamAccount *sa, const gchar *access_token,
                                   SteamEResult eresult, gpointer user_data);
void steam_auth_refresh_access_token(SteamAccount *sa, const gchar *refresh_token,
                                     SteamAuthTokenFunc callback, gpointer user_data);

/* Decodes the payload of a Steam JWT without verifying it. Any out-param may
 * be NULL. `steamid` comes from "sub", `expiry` from "exp" (unix time),
 * `is_client_token` is TRUE when "aud" contains "client" (required for CM
 * logon). Returns FALSE if the token is not a parseable JWT. */
gboolean steam_auth_jwt_decode(const gchar *token, guint64 *steamid,
                               gint64 *expiry, gboolean *is_client_token);

#endif /* STEAM_AUTH_H */
