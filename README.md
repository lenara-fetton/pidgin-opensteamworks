# pidgin-opensteamworks

A Steam chat protocol plugin for libpurple (Pidgin, Finch, and other
libpurple-based messengers).

Supports:

  * Friends list, presence and in-game status
  * Private messages and typing notifications
  * Friend requests (incoming and outgoing)
  * Offline message history
  * Steam nicknames

Logging in uses your Steam account password plus Steam Guard (a code from
the mobile app, a code emailed to you, or approving the login from the
mobile app). After the first successful login, a refresh token is stored
in your system keyring, so you won't be asked to log in again for about
200 days.

Status
======

Version 2.0 is a rewrite of the protocol layer against Steam's current
`IAuthenticationService` login and its Connection Manager (CM) WebSocket
protocol. The plugin's previous transport, the Steam mobile web API, was
shut down and the plugin stopped working. See
[`docs/steam-api-status.md`](docs/steam-api-status.md) for the details of
what broke and how the rewrite is built.

Building from source
=====================

Dependencies (pkg-config names): `purple`, `glib-2.0`, `json-glib-1.0`,
`zlib`, `nss`. `libsecret-1` headers are used at build time but are
optional; the keyring integration is loaded at runtime if available.

On Debian/Ubuntu:

```
sudo apt install libpurple-dev libglib2.0-dev libjson-glib-dev libnss3-dev libsecret-1-dev
git clone https://github.com/EionRobb/pidgin-opensteamworks
cd pidgin-opensteamworks/steam-mobile
make
sudo make install
```

To use a different crypto backend for RSA-encrypting your password at
login, build with `make STEAM_CRYPT_BACKEND=gcrypt`, `=mbedtls`, or
`=openssl` (defaults to `nss`).

Adding friends
==============

Buddy names are SteamID64 numbers, e.g. `7656119xxxxxxxxxx`. You can find
your own SteamID64 on your Steam profile page.

Packaging
=========

  * Debian packages for Debian 10/11 are automatically built and published
    to the [package registry](https://gitlab.com/nodiscc/pidgin-opensteamworks/-/packages?type=&sort=desc&orderBy=version&search[]=)
    for every tag.
  * An RPM spec for Fedora/openSUSE/CentOS/RHEL is at
    [`steam-mobile/purple-libsteam.spec`](steam-mobile/purple-libsteam.spec).

Development
===========

See [`docs/architecture.md`](docs/architecture.md) for how the rewritten
plugin's modules fit together, and [`steam-mobile/tests`](steam-mobile/tests)
for the standalone test programs used to exercise the login and CM code
without a full Pidgin install.

License
=======

GPLv3. Originally written by Eion Robb.
