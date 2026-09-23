# Architecture of the rewritten Steam plugin

Companion to `steam-api-status.md` (which explains *why* the old plugin died).
This document is the contract shared by everyone working on the rewrite.
Interfaces are fixed in the headers; this file explains how the modules fit
together and collects protocol reference material.

## Modules (all in `steam-mobile/`)

| File | Role | Depends on |
|---|---|---|
| `steam_proto.[ch]` | Hand-written protobuf **wire** codec (varint/fixed/bytes, reader/writer). Done. | glib |
| `steam_msgs.[ch]` | Message structs + encode/decode for every CM message the plugin uses, EMsg numbers, `steam_eresult_to_string()`. Done. | `steam_proto` |
| `steam_ws.[ch]` | RFC 6455 WebSocket client over `purple_ssl_connect()`. Done. | libpurple |
| `steam_connection.[ch]` | Existing HTTPS request helper (`steam_post_or_get`). Gains a raw-body callback for binary (protobuf) responses. Done. | libpurple, json-glib, zlib |
| `steam_auth.[ch]` | `IAuthenticationService` login: password + Steam Guard → refresh token. JWT decode. Done. | `steam_connection`, `steam_proto`, `steam_rsa` |
| `steam_cm.[ch]` | CM session: directory lookup, framing, `Multi`, logon, heartbeat, jobs, reconnect. Translates protobuf into plain structs. Done. | `steam_ws`, `steam_msgs`, `steam_connection` (for directory) |
| `libsteam.[ch]` | libpurple prpl: accounts, buddy list, conversations, menus, keyring. Talks only to `steam_auth` and `steam_cm`. Done. | everything above |
| `steam_rsa.c` | RSA-encrypts the password (NSS/gcrypt/mbedtls/openssl). Unchanged. | crypto backend |
| `tests/` | Standalone test programs (headless libpurple harness, codec round-trip tests). Not installed. `steam_ws.c` and `steam_cm.c` export `steam_ws__test_*` / `steam_cm__test_*` hooks (declared locally in the test files, not in the public headers) purely so the tests can drive them without a real socket; nothing else should call them. See `testing.md`. | |

Rules:

- **No protobuf outside `steam_msgs.c` and `steam_auth.c`.** `libsteam.c` only
  sees the structs in `steam_cm.h` / `steam_auth.h`.
- **No libpurple UI calls below `libsteam.c`** (no `purple_request_*`,
  `purple_notify_*`, `purple_connection_error`). Lower layers report through
  callbacks; `purple_debug_*` is fine everywhere.
- Every module owns its files; don't edit another module's `.c` file. Header
  changes to a contract header (`steam_ws.h`, `steam_auth.h`, `steam_cm.h`)
  need to be called out explicitly so dependents can adapt.
- Build: `make` in `steam-mobile/`. Each object can be built alone with
  `make steam_ws.o` etc. Keep `-Wall` clean.
- glib deprecations: use `g_memdup2`, not `g_memdup`.
- C89-ish style as in the existing code: tabs, `purple_debug_info("steam", ...)`.

## Login flow (`steam_auth`)

All calls are `POST https://api.steampowered.com/IAuthenticationService/<Method>/v1`
with a form field `input_protobuf_encoded=<base64 of the request protobuf>`.
The response body is a binary protobuf and the HTTP header `x-eresult`
carries the EResult (`x-error_message` may carry text). Only
`GetPasswordRSAPublicKey` is a `GET` (`?account_name=...`), and it returns
JSON if no protobuf input is given (`{"response":{"publickey_mod":"<hex>",
"publickey_exp":"<hex>","timestamp":"<uint64>"}}`). It also accepts
`?input_protobuf_encoded=` on the `GET` and then returns protobuf with
`x-eresult` like the other four calls; `steam_auth.c` uses that form so all
five calls share one code path.

Messages (`steammessages_auth.steamclient.proto` in SteamDatabase/Protobufs):

1. `CAuthentication_GetPasswordRSAPublicKey_Request { string account_name = 1; }`
   → `Response { string publickey_mod = 1; string publickey_exp = 2; uint64 timestamp = 3; }`
2. `CAuthentication_BeginAuthSessionViaCredentials_Request`:
   `device_friendly_name = 1`, `account_name = 2`, `encrypted_password = 3`
   (base64 of the RSA-PKCS1v15 ciphertext, i.e. what `steam_rsa` already
   produces), `encryption_timestamp = 4` (uint64), `remember_login = 5`
   (bool, true), `platform_type = 6` (EAuthTokenPlatformType, use
   `1 = SteamClient` so the refresh token is valid for CM logon),
   `persistence = 7` (ESessionPersistence, `1 = Persistent`),
   `website_id = 8` ("Client"), `device_details = 9` (message:
   `device_friendly_name = 1`, `platform_type = 2` (1), `os_type = 3`
   (int32, e.g. `-203` LinuxUnknown or `16` Windows10), `gaming_device_type = 4` (1);
   also has `client_count = 5`, `machine_id = 6` (bytes), `app_type = 7`, unused),
   `guard_data = 10` (string; optional, from a previous login). Also has
   `language = 11` (uint32) and `qos_level = 12` (int32), unused.
   → `Response { uint64 client_id = 1; bytes request_id = 2; float interval = 3;
   repeated AllowedConfirmation allowed_confirmations = 4 { EAuthSessionGuardType confirmation_type = 1; string associated_message = 2; };
   uint64 steamid = 5; string weak_token = 6; string agreement_session_url = 7; string extended_error_message = 8; }`
   Note `request_id` is **bytes**, not string.
3. `CAuthentication_UpdateAuthSessionWithSteamGuardCode_Request { uint64 client_id = 1; fixed64 steamid = 2; string code = 3; EAuthSessionGuardType code_type = 4; }`
   → `Response { string agreement_session_url = 7; }`
4. `CAuthentication_PollAuthSessionStatus_Request { uint64 client_id = 1; bytes request_id = 2; }`
   (echoing the `request_id` bytes from step 2 verbatim; also has
   `token_to_revoke = 3` (fixed64), unused)
   → `Response { uint64 new_client_id = 1; string new_challenge_url = 2; string refresh_token = 3; string access_token = 4; bool had_remote_interaction = 5; string account_name = 6; string new_guard_data = 7; string agreement_session_url = 8; }`
   Poll every `interval` seconds until `refresh_token` is non-empty. An
   `x-eresult` other than OK (or empty response with `had_remote_interaction`
   false for too long) is a failure. `new_guard_data` is not currently
   surfaced by `steam_auth.h`, so email-Steam-Guard users are asked for a
   code on every *password* login (rare: the refresh token lasts ~200 days).
5. `CAuthentication_AccessToken_GenerateForApp_Request { string refresh_token = 1; fixed64 steamid = 2; ETokenRenewalType renewal_type = 3; }`
   → `Response { string access_token = 1; string refresh_token = 2; }`
   Method name: `GenerateAccessTokenForApp`. Over the WebAPI this call
   rejects **SteamClient** refresh tokens with `AccessDenied` (15) — such
   calls only succeed over an authenticated CM session. So
   `steam_auth_refresh_access_token()` is not a token validity check;
   decide token validity from the JWT `exp` and the CM logon result
   instead.

The refresh token is a JWT; its payload has `sub` (SteamID as a string),
`exp`, and `aud` (an array; must contain `"client"` for CM logon). Steam
JWTs use base64url without padding.

Steam Guard mapping to the libpurple UI in `libsteam.c`:

- `DEVICE_CODE` → "Enter your Steam Guard code from the mobile app".
- `EMAIL_CODE` → "Enter the code sent to your email (domain)".
- `DEVICE_CONFIRMATION` → tell the user to approve in the app, keep polling.
- If several are allowed, show a code prompt and keep polling at the same
  time so either path works.

The old plugin stored the OAuth token in the keyring under the account key
`access_token`. The new plugin stores the **refresh token** in the same
place (via the existing libsecret/gnome-keyring code, or
`purple_account_set_string` fallback) and the SteamID in the `steamid`
account setting. On connect: if a refresh token exists and its `exp` is in
the future, go straight to `steam_cm_connect()`; otherwise run the password
login. If the CM rejects the token (`logon_failed`), clear it and fall back
to password login once.

## CM session (`steam_cm`)

### Directory

`GET https://api.steampowered.com/ISteamDirectory/GetCMListForConnect/v1/?cellid=0&format=json`
→ `{"response":{"serverlist":[{"endpoint":"cmp1-lax1.steamserver.net:27038","legacy_endpoint":...,"type":"websockets","dc":"lax1","realm":"steamglobal","load":30,"wtd_load":...}, ...]}}`.
Use entries with `"type":"websockets"`, sorted by `load`. Connect to
`wss://<endpoint>/cmsocket/`. Cache the list per session and try the next
server on failure.

### Framing

Every WebSocket binary message is one packet:

```
uint32 LE  emsg | 0x80000000   (the high bit marks a protobuf packet; all packets we use are protobuf)
uint32 LE  header_len
bytes      CMsgProtoBufHeader (header_len bytes)
bytes      body (protobuf message for that EMsg)
```

`CMsgProtoBufHeader` fields we use: `steamid = 1` (fixed64), `client_sessionid = 2`
(int32), `routing_appid = 3` (uint32, unused), `jobid_source = 10` (fixed64),
`jobid_target = 11` (fixed64), `target_job_name = 12` (string),
`eresult = 13` (int32, default 2), `error_message = 14` (string),
`realm = 32` (uint32). Absent job ids mean `0xFFFFFFFFFFFFFFFF`. After
logon, every outgoing header must carry our `steamid` and the
`client_sessionid` from the logon response.

### EMsg numbers

| Name | # | Body |
|---|---|---|
| Multi | 1 | `CMsgMulti { uint32 size_unzipped = 1; bytes message_body = 2; }` — if `size_unzipped` > 0 the body is gzip; the payload is a sequence of `uint32 LE length` + packet |
| ClientHeartBeat | 703 | `CMsgClientHeartBeat { bool send_reply = 1; }` |
| ClientRemoveFriend | 714 | `CMsgClientRemoveFriend { fixed64 friendid = 1; }` |
| ClientChangeStatus | 716 | `CMsgClientChangeStatus { uint32 persona_state = 1; string player_name = 2; bool is_auto_generated_name = 3; bool high_priority = 4; bool persona_set_by_user = 5; uint32 persona_state_flags = 6; bool need_persona_response = 7; bool is_client_idle = 8; }` |
| ClientLogOnResponse | 751 | `CMsgClientLogonResponse { int32 eresult = 1; int32 heartbeat_seconds = 3; fixed32 rtime32_server_time = 5; int32 eresult_extended = 10; fixed64 client_supplied_steamid = 20; string agreement_session_url = 29; }`. There is no `extended_error_msg` field. Our `client_sessionid` and SteamID come in the packet **header**, not the body. |
| ClientLoggedOff | 757 | `CMsgClientLoggedOff { int32 eresult = 1; }` |
| ClientPersonaState | 766 | `CMsgClientPersonaState { uint32 status_flags = 1; repeated Friend friends = 2; }` with `Friend { fixed64 friendid = 1; uint32 persona_state = 2; uint32 game_played_app_id = 3; uint32 game_server_ip = 4; uint32 game_server_port = 5; uint32 persona_state_flags = 6; uint32 online_session_instances = 7; bool persona_set_by_user = 10; string player_name = 15; uint32 query_port = 20; fixed64 steamid_source = 25; bytes avatar_hash = 31; uint32 last_logoff = 45; uint32 last_logon = 46; uint32 last_seen_online = 47; uint32 clan_rank = 50; string game_name = 55; fixed64 gameid = 56; bytes game_data_blob = 60; ClanData clan_data = 64; string clan_tag = 65; repeated KV rich_presence = 71 { string key = 1; string value = 2; }; fixed64 broadcast_id = 72; fixed64 game_lobby_id = 73; uint32 watching_broadcast_accountid = 74; ...(75-77); bools 78-81; other_game_data = 82; gaming_device_type = 83 }`. There is no game-server SteamID field; `steamid_source = 25` is the nearest equivalent to `SteamCMPersona.game_server_steamid` in `steam_cm.h`. |
| ClientFriendsList | 767 | `CMsgClientFriendsList { bool bincremental = 1; repeated Friend friends = 2 { fixed64 ulfriendid = 1; uint32 efriendrelationship = 2; }; uint32 max_friend_count = 3; uint32 active_friend_count = 4; bool friends_limit_hit = 5; }` |
| ClientAccountInfo | 768 | `CMsgClientAccountInfo { string persona_name = 1; string ip_country = 2; ... }` |
| ClientAddFriend | 791 | `CMsgClientAddFriend { fixed64 steamid_to_add = 1; string accountname_or_email_to_add = 2; }` |
| ClientAddFriendResponse | 792 | `CMsgClientAddFriendResponse { int32 eresult = 1; fixed64 steam_id_added = 2; string persona_name_added = 3; }` |
| ClientRequestFriendData | 815 | `CMsgClientRequestFriendData { uint32 persona_state_requested = 1; repeated fixed64 friends = 2; }`. `friends` is a proto2 repeated fixed64 without `[packed=true]`, so it's written as one tag per element (our decoder accepts both forms). |
| ClientLogon | 5514 | `CMsgClientLogon` (below) |
| ClientLogOff | 706 | `CMsgClientLogOff {}` |
| ClientPlayerNicknameList | 5587 | `CMsgClientPlayerNicknameList { bool removal = 1; bool incremental = 2; repeated PlayerNickname nicknames = 3 { fixed64 steamid = 1; string nickname = 2; }; }`. Note `nickname` is field **3** in `PlayerNickname`, not 2 (`removal`/`incremental` on the outer message are unaffected). |
| ClientHello | 9805 | `CMsgClientHello { uint32 protocol_version = 1; }` — send first thing after the WebSocket connects |
| ClientServerUnavailable / ClientSessionToken / ClientLicenseList / ClientEmailAddrInfo / ClientWalletInfo / ClientGameConnectTokens / ClientIsLimitedAccount / ClientVACBanStatus / ClientUpdateGuestPassesList / ClientPlayingSessionState / ClientClanState (822) / ClientFriendsGroupsList (5553) / ClientPICS*, etc. | | ignore silently at debug level |

`ClientCMList` (783) is no longer in the current EMsg enum (5515 is
`ClientGetClientDetails`, not `ClientLogOff`), so it's just an unknown EMsg
we ignore like the others in the last row above.

`CMsgClientLogon` fields: `protocol_version = 1` (65580), `client_language = 6`
("english"), `client_package_version = 5` (1771), `client_os_type = 7`
(**uint32**; e.g. `-203` LinuxUnknown or `16` Windows10 — negative values go
on the wire as their unsigned two's-complement form, e.g. -203 as
4294967093, a 5-byte varint, not a 10-byte int32), `should_remember_password = 8`
(true), `cell_id = 3`, `qos_level = 21`, `client_supplied_steam_id = 22`
(fixed64), `machine_id = 30` (bytes; optional), `machine_name = 96`,
`steam2_ticket_request = 88` (not needed), `supports_rate_limit_response = 102`
(true), `account_name = 50`, `password = 51` (leave empty),
`access_token = 108` (**the refresh token goes here**), `chat_mode = 33`
(2 = new chat).
The header for the logon packet must have `steamid` = our SteamID and `client_sessionid` = 0.

Persona state flow: after `ClientLogOnResponse` OK → start heartbeat at
`heartbeat_seconds` → server pushes `ClientAccountInfo`, `ClientFriendsList`
(full), `ClientPlayerNicknameList`. Friends' `ClientPersonaState` only
starts arriving after we send `ClientChangeStatus` with a non-offline state.
Request names/avatars/game info explicitly with `ClientRequestFriendData`,
`persona_state_requested = Status(1)|PlayerName(2)|Presence(16)|LastSeen(64)|GameExtraInfo(256)`
(`EClientPersonaStateFlag.GameExtraInfo` is 256, not 512 — 512 is
`GameDataBlob`). `steam_msgs.h` defines `STEAM_PERSONA_REQ_*` constants plus
`STEAM_PERSONA_REQ_DEFAULT`, which also includes RichPresence (4096).
`avatar_hash` all-zero means the default avatar; otherwise the picture is
`https://avatars.steamstatic.com/<hex>_full.jpg`.

### Service methods (unified messages)

Sent as `ServiceMethodCallFromClient` with `target_job_name` as shown, body =
the request message. Responses come back as `ServiceMethodResponse` matched
on `jobid_target`. Notifications come as `ServiceMethod`.

- `FriendMessages.SendMessage#1`: `CFriendMessages_SendMessage_Request { fixed64 steamid = 1; int32 chat_entry_type = 2; string message = 3; bool contains_bbcode = 4; bool echo_to_sender = 5; bool low_priority = 6; string client_message_id = 8; }`
  → `Response { string modified_message = 1; uint32 server_timestamp = 2; uint32 ordinal = 3; string message_without_bb_code = 4; }`.
  Typing notifications: `chat_entry_type = 2`, empty message.
- `FriendMessagesClient.IncomingMessage#1` (notification): `CFriendMessages_IncomingMessage_Notification { fixed64 steamid_friend = 1; int32 chat_entry_type = 2; bool from_limited_account = 3; string message = 4; fixed32 rtime32_server_timestamp = 5; uint32 ordinal = 6; bool local_echo = 7; string message_no_bbcode = 8; bool low_priority = 9; }`
- `FriendMessages.GetRecentMessages#1`: `Request { fixed64 steamid1 = 1; fixed64 steamid2 = 2; uint32 count = 3; bool most_recent_conversation = 4; fixed32 rtime32_start_time = 5; bool bbcode_format = 6; uint32 start_ordinal = 7; uint32 time_last = 8; uint32 ordinal_last = 9; }`
  → `Response { repeated FriendMessage messages = 1 { uint32 accountid = 1; uint32 timestamp = 2; string message = 3; uint32 ordinal = 4; ... }; bool more_available = 4; }`
- `FriendMessages.GetActiveMessageSessions#1`: `Request { uint32 lastmessage_since = 1; bool only_sessions_with_messages = 2; }`
  → `Response { repeated ActiveMessageSession message_sessions = 1 { uint32 accountid_friend = 1; uint32 last_message = 2; uint32 last_view = 3; uint32 unread_message_count = 4; repeated EChatSessionNotice notices = 5; }; uint32 timestamp = 2; }`.
  In the .proto these messages are spelled
  `CFriendsMessages_GetActiveMessageSessions_Request` /
  `_Response` (note `Friends`, plural); the wire method name stays
  `FriendMessages.GetActiveMessageSessions#1`. The `notices` field is
  ignored.
- `Player.GetNicknameList#1`: `Request {}` → `Response { repeated PlayerNickname nicknames = 1 { fixed32 accountid = 1; string nickname = 2; } }`. Note `accountid` is **fixed32**, not uint32.
- `FriendMessages.AckMessage#1` (sent, no response) and `FriendMessagesClient.NotifyAckMessageEcho#1` (received): `CFriendMessages_AckMessage_Notification { fixed64 steamid_partner = 1; uint32 timestamp = 2; }`. Used only for message-meta UIs (see below).

All field numbers above have been verified against the `.proto` files in
https://github.com/SteamDatabase/Protobufs @ `60d634a` (directory `steam/`):
`steammessages_base.proto`, `steammessages_clientserver_login.proto`,
`steammessages_clientserver_friends.proto`, `steammessages_clientserver_2.proto`,
`steammessages_friendmessages.steamclient.proto`,
`steammessages_player.steamclient.proto`, `steammessages_auth.steamclient.proto`,
`enums_clientserver.proto` (EMsg), `enums.proto`; `tests/test_proto.c` checks
them byte-for-byte against protoc's own Python output
(`tests/gen_vectors.py`).

Reference implementations: SteamKit2 (`SteamKit2/SteamKit2/Steam/Handlers/SteamFriends`,
`SteamUser`, `Networking/Connections/WebSocketConnection.cs`) and
node-steam-user (`components/connection_protocol/websocket.js`, `components/logon.js`,
`components/friends.js`, `components/chat.js`).

## libpurple integration (`libsteam.c`)

Keep: account options, keyring code, buddy-list helpers, status types,
tooltips/emblems, blist menu (launch game / join game / view profile), plugin
actions, icon fetching via `purple_util_fetch_url`.

Replace: everything that called `ISteamWebUserPresenceOAuth`,
`ISteamUserOAuth`, `mobilelogin`, the poll loop and captcha handling.

Mapping:

| libpurple | Steam |
|---|---|
| `steam_login` | token present → `steam_cm_connect`; else `steam_auth_login_password` |
| `steam_close` | `steam_cm_disconnect`, `steam_auth_cancel` |
| `logged_on` cb | `purple_connection_set_state(CONNECTED)`, then `steam_cm_set_persona_state` from the current saved status, then `steam_cm_get_active_message_sessions(since = last_message_timestamp)` for offline history |
| `friends_list` cb | add/remove `PurpleBuddy`s (group "Steam"); `REQUEST_RECIPIENT` → `purple_account_request_authorization`; `REQUEST_INITIATOR` → buddy shown offline with "request sent" |
| `persona_state` cb | `purple_prpl_got_user_status` (state → status id: online/away/snooze/busy/offline/trade/play), alias from `player_name`, avatar fetch on hash change, game info into `SteamBuddy` for tooltip / emblem |
| `message` cb | `CHAT_MSG` → `serv_got_im` (or, for `local_echo`, write to the conversation as sent by us); `TYPING` → `serv_got_typing`; `LEFT_CONVERSATION` → typing stopped |
| `steam_send_im` | `steam_cm_send_message(CHAT_MSG)`; unescape HTML, convert `/me` to `/me ...` text |
| `steam_send_typing` | `steam_cm_send_message(TYPING, "")`, rate-limited |
| `steam_set_status` / `steam_set_idle` | `steam_cm_set_persona_state` |
| `steam_add_buddy` | `steam_cm_add_friend(steamid64)` (buddy name must be a SteamID64) |
| `steam_buddy_remove` | `steam_cm_remove_friend` |
| authorization accept/deny | `steam_cm_add_friend` / `steam_cm_remove_friend` |
| `nicknames` cb | `purple_blist_alias_buddy` / server alias |

Dropped features (no longer feasible without a web session): user search
by name, "redeem game key" action, captcha. The "view profile" menu item
stays (it's just a URL).

## Message-meta UIs (pidgin4)

A UI that renders message metadata itself says so with
`purple_core_get_ui_info()["message-meta"] == "1"` (pidgin4 does; stock
Pidgin doesn't). `steam_native_init()` reads it at login into
`SteamAccount.native_meta`, and every behaviour below checks it. Without it
nothing new runs, and the output is exactly what it was.

- **Inline images and emoticons.** Live messages are rendered from the
  BBCode form (`message`, field 4 of `IncomingMessage`), and history is
  fetched with `bbcode_format`. `steam_rich_to_html()` turns
  `[emoticon]name[/emoticon]` (and `ːnameː`/`:name:` in plain text) into
  `<img src="https://steamcommunity-a.akamaihd.net/economy/emoticon/name"
  alt=":name:">`, and images on `images.steamusercontent.com` /
  `steamusercontent-a.akamaihd.net` (`[img src=…]`, `[url=…]` or a bare
  link) into the link, `<br/>` and an `<img>`. Stickers become the text
  `[sticker: Name]` (their CDN path isn't verified); other tags are dropped
  with their content kept. Messages sent from another client of ours get
  the same treatment.

- **Message ids and metadata.** Before each message it shows, the plugin
  emits `receiving-message-meta(account, conv name, GHashTable *meta)`
  (registered by pidgin4's libpurple) with `conv-type` = `im`, `sender`
  (the friend's SteamID, or our account's username for our own messages),
  `timestamp`, `stanza-id` and `server-id` (both the message id),
  `outgoing` = `1` for our own messages from another client, `markable` =
  `1` for the friend's messages, and `mam` = `1` + `mam-query` =
  `catchup` for the offline history fetched at sign-on. A handler that sets
  `discard` = `1` drops the message (the UI has it). **Message ids** are
  `<friend SteamID>:<server timestamp>`, plus `:<ordinal>` when the ordinal
  isn't 0: the conversation, timestamp and ordinal are what Steam's own
  requests and notifications use to name a message.
- **Own sends.** `send_im` returns 0 (libpurple writes nothing) and the
  message is written from the `SendMessage` reply, right after
  `sending-message-meta` with `conv-type`, `timestamp`, `stanza-id` and
  `server-id` (the reply's `server_timestamp` and `ordinal`), so the UI
  attaches the id to it. A failed send writes the text into the error.
- **Read markers.** IPC `send-marker(account, conv name, message id,
  marker)` (registered on the plugin only when the UI has message-meta)
  sends `FriendMessages.AckMessage#1` (`CFriendMessages_AckMessage_Notification
  { fixed64 steamid_partner = 1; uint32 timestamp = 2; }`, no response) for
  `displayed`/`acknowledged`; Steam has no delivery receipts, so `received`
  returns FALSE. The echo of an ack from another session of ours,
  `FriendMessagesClient.NotifyAckMessageEcho#1` (same message), becomes
  `message-receipt(account, friend, "<friend>:<timestamp>", "displayed",
  <our username>)`. Steam doesn't tell us when the friend reads our
  messages.

## Testing without a Steam account

- `tests/test_proto.c`: round-trips for `steam_proto` and `steam_msgs`;
  cross-check encodings with `protoc` + Python protobuf (both installed).
- `tests/purple_harness.c`: headless libpurple core init (GLib main loop
  eventloop ops, null UI) so `steam_ws`, `steam_auth`, `steam_cm` can be
  driven from a command-line program.
- Auth: `BeginAuthSessionViaCredentials` with a bogus account should come
  back with `x-eresult: 5` (InvalidPassword) or similar; that proves the
  request encoding and response decoding.
- CM: connecting and sending `ClientLogon` with a bogus token should yield
  `ClientLogOnResponse` with a non-OK eresult; that proves directory lookup,
  WebSocket, framing and header handling.
- `steam_ws.c` and `steam_cm.c` each export a handful of `steam_ws__test_*` /
  `steam_cm__test_*` functions (constructing an instance in "test mode" with
  no real socket, feeding it raw bytes, inspecting what it queued to send).
  They are declared only in the test `.c` files, not in `steam_ws.h` /
  `steam_cm.h`, and exist solely so the offline tests can drive the state
  machines deterministically; nothing outside `tests/` should call them.
- A real end-to-end test needs a Steam account; that is a manual step for
  the maintainer.

See `testing.md` for exact build/run commands, what each test program
covers, the environment variables that enable the real-account paths, and
the manual Pidgin test plan.

