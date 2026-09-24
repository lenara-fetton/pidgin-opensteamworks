# Steam API status and revival plan

Review date: 2026-09-22. Last upstream commit reviewed: `4274774` (2021-11-01).

## Status update (2026-09-22)

The rewrite this document calls for below has been implemented. See
[`architecture.md`](architecture.md) for how the modules fit together and
the protocol reference, and [`testing.md`](testing.md) for how to build and
run the test suite.

What's verified:

- `make` in `steam-mobile/` builds `libsteam.so` cleanly, no warnings.
- The unit tests in `steam-mobile/tests/` (`test_proto`, `test_ws`,
  `test_auth`, `test_cm`) all pass: codec round-trips, WebSocket framing,
  and offline CM packet handling.
- Live round trips against the real Steam servers with bogus credentials:
  the `IAuthenticationService` handshake and login rejection
  (`BeginAuthSessionViaCredentials` with a bad password), and a CM
  directory lookup + WebSocket connect + `ClientLogon` with a bogus
  refresh token, both correctly come back with a non-OK EResult. This
  proves request encoding, response decoding, framing, and header
  handling end to end against production Steam.

What's not verified yet:

- A real end-to-end login with a live Steam account (password + Steam
  Guard, or refresh token) has not been done.
- Friends list population, persona state updates, and nicknames from a
  real account.
- Sending and receiving chat messages, typing notifications, and offline
  message history.

The rest of this document is the original analysis that motivated the
rewrite; it is kept as-is for historical reference.

## Summary

**The plugin no longer works.** It still compiles against current libraries
(libpurple 2.14.14, json-glib 1.10.8, NSS 3.129), but the Steam backend it
depends on has been removed. It **can** be made to work again, but that
requires rewriting the protocol layer. The libpurple integration can
largely be kept.

## The two plugins in this repository

### `libsteamworks.cpp` / `libsteamworks.h` (Open Steamworks, versions before 1.0)

- Loads `steamclient` from a running Steam client and calls interfaces
  pinned to versions from around 2010: `ISteamClient008`,
  `ISteamFriends002`, `ISteamUtils002`, `ISteamUser005`, `IClientFriends`,
  `ISteamApps001`, `ISteamMatchmaking008`.
- The Steam client has changed those interfaces many times since.
  `IClient*` interfaces are private and change between client builds, so
  they could never be relied on long-term.
- opensteamworks.org no longer exists, and the Makefile doesn't build these
  files.
- **Verdict:** can't be salvaged. Remove it, or at least mark it as
  historical in the README.

### `steam-mobile/` (Steam mobile API, versions 1.0 and later)

This is the plugin that gets built and packaged. It uses the old Steam
mobile-app HTTP APIs.

## Endpoint status

Checked on 2026-09-22 with unauthenticated or bogus-token requests:

| Purpose | Endpoint (used in) | Result |
|---|---|---|
| Sign on, poll for messages/presence, send messages, sign off | `api.steampowered.com/ISteamWebUserPresenceOAuth/{Logon,Poll,PollStatus,Message,Logoff}/v0001` (`libsteam.c`) | **404 `Interface 'ISteamWebUserPresenceOAuth' not found`**. The interface has been removed. |
| Get the RSA key for password login | `steamcommunity.com/mobilelogin/getrsakey` | 200, still answers |
| Password login | `steamcommunity.com/mobilelogin/dologin/` with `oauth_client_id=3638BFB1` | Still answers (`{"success":false}`), but it no longer issues the legacy `oauth_token`. Replaced by the 2023 login overhaul (see below). |
| Friends list, profiles, user search | `ISteamUserOAuth/{GetFriendList,GetUserSummaries,Search}/v0001` | 401. These depend on the legacy OAuth token, which Steam no longer issues. |
| Chat history | `IFriendMessagesService/{GetRecentMessages,GetActiveMessageSessions}` | 401 with a bogus token. The service still exists but needs a current access token. |
| Nicknames | `IPlayerService/GetNicknameList` | Service still exists; needs a current access token |
| Buddy info page | `steamcommunity.com/chat/friendstate/<id>` | 302 redirect to the login page. The legacy web chat was retired in 2018. |
| Session ID | `steamcommunity.com/mobilesettings/GetManifest/v0001` with a `steamLogin=<id>\|\|oauth:<token>` cookie | Obsolete cookie format; can't authenticate this way anymore |
| Add/remove friend, redeem key | `/actions/{Add,Remove}FriendAjax`, `/profiles/<id>/home_process`, `store.steampowered.com/account/ajaxregisterkey/` | Only work with a valid community/store web session. The cookie derived from the old token no longer provides one. |

`ISteamWebUserPresenceOAuth` was the entire real-time channel (logon,
long-poll for incoming messages and presence, sending messages). Nothing
replaces it over plain HTTP, so this outage is fatal on its own.

## How Steam works today

### Authentication: `IAuthenticationService` (2023 overhaul)

1. `IAuthenticationService/GetPasswordRSAPublicKey/v1?account_name=...`
   returns the RSA modulus and exponent. This works (checked), and the
   encryption code in `steam_rsa.c` can be reused.
2. `IAuthenticationService/BeginAuthSessionViaCredentials/v1` sends the
   username and the RSA-encrypted password. The response is a
   `client_id`, `request_id`, `steamid`, and the allowed Steam Guard
   confirmation types.
3. If a code is needed,
   `IAuthenticationService/UpdateAuthSessionWithSteamGuardCode/v1` sends the
   TOTP or email code. The mobile app can also approve the login with a
   push confirmation.
4. `IAuthenticationService/PollAuthSessionStatus/v1` returns a
   **refresh token** (a JWT that lasts about 200 days) and an **access
   token** (a short-lived JWT).
5. `IAuthenticationService/GenerateAccessTokenForApp/v1` gets a new access
   token from the refresh token.

A QR-code alternative (`BeginAuthSessionViaQR`) lets the user approve the
login from the Steam mobile app without the plugin handling the password.

These methods accept form parameters or `input_protobuf_encoded`
(protobuf, base64-encoded) over HTTPS.

### Real-time chat: CM over WebSocket

The Steam web and desktop clients, SteamKit2, and node-steam-user all
connect to a Connection Manager (CM) server using protobuf messages over a
WebSocket:

- `ISteamDirectory/GetCMListForConnect/v1/?cellid=0` lists servers of
  `"type":"websockets"`, for example `cmp1-lax1.steamserver.net:443`.
  Checked and working.
- Connect to `wss://<endpoint>/cmsocket/`.
- Frames: a 4-byte `EMsg` (with the protobuf flag `0x80000000`), a 4-byte
  header length, a `CMsgProtoBufHeader`, and then the message body.
- Key messages:
  - `EMsg.ClientLogon` (`CMsgClientLogon`), with the refresh token passed
    as `access_token`
  - `EMsg.ClientLogOnResponse`
  - `EMsg.ClientHeartBeat`, sent at the interval from the logon response
  - `EMsg.ClientFriendsList` (`CMsgClientFriendsList`): friends and
    relationship changes (friend requests)
  - `EMsg.ClientRequestFriendData` / `EMsg.ClientPersonaState`: names,
    avatars, online status, in-game info
  - `EMsg.ClientChangeStatus`: set your own persona state
  - Service methods, sent as `EMsg.ServiceMethodCallFromClient` and received
    as `EMsg.ServiceMethod`:
    - `FriendMessages.SendMessage#1` (also used for typing notifications
      and `/me` emotes)
    - `FriendMessagesClient.IncomingMessage#1` (incoming messages and typing)
    - `FriendMessages.GetRecentMessages#1`, `FriendMessages.GetActiveMessageSessions#1`
    - `Player.GetNicknameList#1`
  - `EMsg.ClientAddFriend` / `EMsg.ClientRemoveFriend`

The `.proto` definitions are available from
[SteamDatabase/Protobufs](https://github.com/SteamDatabase/Protobufs), and
[SteamKit2](https://github.com/SteamRE/SteamKit) and
[node-steam-user](https://github.com/DoctorMcKay/node-steam-user) are
useful references for how each call behaves.

## What it would take to make it work

### Keep

- The libpurple plugin setup, account options, buddy list handling, and
  buddy/account menu actions in `libsteam.c`
- Refresh-token storage in the keyring/libsecret. Store the refresh token
  in place of the old `access_token`.
- `steam_rsa.c`, for encrypting the password (all four crypto backends)
- The Makefile and packaging, updated for the new dependencies

### Replace

1. **Login state machine.** Replace `steam_login_got_rsakey` /
   `steam_login_cb` / `steam_login_with_access_token` with the
   `IAuthenticationService` flow above. Map Steam Guard confirmation types
   onto the existing two-factor/email-code request dialogs, and remove the
   captcha path.
2. **Transport.** Replace the HTTP long-poll in `steam_connection.c` with a
   WebSocket client. libpurple 2.x has none built in; one can be written on
   top of `purple_ssl_connect` (several of Eion Robb's other prpls, such as
   purple-discord, have one to borrow). Keep the plain HTTPS helper for
   `IAuthenticationService` and `ISteamDirectory`.
3. **Protobuf.** Add protobuf-c (or a small hand-written encoder/decoder
   for the handful of messages needed) and generate code from the relevant
   `.proto` files: `steammessages_base`, `steammessages_clientserver_login`,
   `steammessages_clientserver_friends`, `steammessages_friendmessages`,
   `steammessages_auth`, `steammessages_player`.
4. **Protocol handlers.** Rebuild on CM messages:
   - logon, heartbeat, reconnect to another CM on disconnect
   - friends list, friend requests (accept/deny), add/remove friend
   - persona state: status, names, nicknames, in-game app name and ID
   - set own status (online/away/snooze/invisible/offline)
   - send and receive messages, typing notifications, `/me`
   - offline message history on login
5. **Web-session features** (redeem key, profile-page actions): if these
   are kept, get web cookies (`steamLoginSecure=<steamid>||<access token>`)
   from the new tokens, or drop the features.

### Cleanups found while building

- `g_memdup` is deprecated; use `g_memdup2` (`steam_connection.c:217`,
  `steam_connection.c:238`, `libsteam.c:427`).
- `g_str_equal` can receive NULL from `json_object_get_string_member`
  (`libsteam.c:889`, `libsteam.c:1334`). This can crash on an unexpected
  server response.
- Remove `libsteamworks.cpp` / `libsteamworks.h` and the bundled Windows
  `libjson-glib-1.0.dll`.
- README: remove the Open Steamworks, googlecode.com and adiumxtras
  references, and the duplicated "How to install on Ubuntu/Debian"
  section.

### Suggested order of work

1. Standalone `IAuthenticationService` login (password + Steam Guard),
   saving and refreshing the token. Can be tested without any CM code.
2. WebSocket client + CM directory lookup + `ClientLogon`/heartbeat.
3. Friends list and persona state, which fill the buddy list.
4. Send and receive messages, typing notifications.
5. History, nicknames, friend requests, in-game status, status options.
6. Remove the dead code paths and update the README/changelog.

### Effort and risk

This is roughly a new protocol plugin that reuses the existing UI layer.
It's a substantial project, but a well-understood one: the CM protocol and
the new authentication flow are thoroughly documented by SteamKit2 and
node-steam-user. The main risks are Valve changing things again (the CM
protocol has been stable for a long time; the web APIs have not) and
rate limiting or security checks on repeated password logins. Keeping the
refresh token keeps password logins rare.
