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

/* Steam EResult codes used by this plugin. Full list: SteamKit2 EResult. */

#ifndef STEAM_ERESULT_H
#define STEAM_ERESULT_H

typedef enum {
	STEAM_ERESULT_INVALID = 0,
	STEAM_ERESULT_OK = 1,
	STEAM_ERESULT_FAIL = 2,
	STEAM_ERESULT_NO_CONNECTION = 3,
	STEAM_ERESULT_INVALID_PASSWORD = 5,
	STEAM_ERESULT_LOGGED_IN_ELSEWHERE = 6,
	STEAM_ERESULT_INVALID_PROTOCOL_VER = 7,
	STEAM_ERESULT_INVALID_PARAM = 8,
	STEAM_ERESULT_BUSY = 10,
	STEAM_ERESULT_INVALID_STATE = 11,
	STEAM_ERESULT_INVALID_NAME = 12,
	STEAM_ERESULT_ACCESS_DENIED = 15,
	STEAM_ERESULT_TIMEOUT = 16,
	STEAM_ERESULT_BANNED = 17,
	STEAM_ERESULT_ACCOUNT_NOT_FOUND = 18,
	STEAM_ERESULT_INVALID_STEAMID = 19,
	STEAM_ERESULT_SERVICE_UNAVAILABLE = 20,
	STEAM_ERESULT_NOT_LOGGED_ON = 21,
	STEAM_ERESULT_PENDING = 22,
	STEAM_ERESULT_LIMIT_EXCEEDED = 25,
	STEAM_ERESULT_REVOKED = 26,
	STEAM_ERESULT_EXPIRED = 27,
	STEAM_ERESULT_DUPLICATE_REQUEST = 29,
	STEAM_ERESULT_ALREADY_OWNED = 30,
	STEAM_ERESULT_LOGON_SESSION_REPLACED = 34,
	STEAM_ERESULT_CONNECT_FAILED = 35,
	STEAM_ERESULT_REMOTE_DISCONNECT = 38,
	STEAM_ERESULT_BLOCKED = 40,
	STEAM_ERESULT_ACCOUNT_DISABLED = 42,
	STEAM_ERESULT_TRY_ANOTHER_CM = 48,
	STEAM_ERESULT_ACCOUNT_LOGON_DENIED = 63,         /* email Steam Guard code needed */
	STEAM_ERESULT_INVALID_LOGIN_AUTH_CODE = 65,
	STEAM_ERESULT_ACCOUNT_LOGON_DENIED_NO_MAIL = 66,
	STEAM_ERESULT_RATE_LIMIT_EXCEEDED = 84,
	STEAM_ERESULT_ACCOUNT_LOGIN_DENIED_NEED_TWO_FACTOR = 85,
	STEAM_ERESULT_ACCOUNT_LOGIN_DENIED_THROTTLE = 87,
	STEAM_ERESULT_TWO_FACTOR_CODE_MISMATCH = 88,
	STEAM_ERESULT_TWO_FACTOR_ACTIVATION_CODE_MISMATCH = 89,
	STEAM_ERESULT_IP_BANNED = 105,
} SteamEResult;

/* Implemented in steam_msgs.c. Returns a static English string. */
const char *steam_eresult_to_string(int eresult);

#endif /* STEAM_ERESULT_H */
