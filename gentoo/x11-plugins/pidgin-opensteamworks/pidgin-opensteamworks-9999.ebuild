# Copyright 2026 Lenara Fetton
# Distributed under the terms of the GNU General Public License v2

EAPI=8

inherit git-r3 toolchain-funcs

DESCRIPTION="Steam protocol plugin for libpurple (Pidgin)"
HOMEPAGE="https://github.com/lenara-fetton/pidgin-opensteamworks"
EGIT_REPO_URI="https://github.com/lenara-fetton/pidgin-opensteamworks.git"
S="${WORKDIR}/${P}/steam-mobile"

LICENSE="GPL-3"
SLOT="0"
# The tests link a purple harness and are run by hand (steam-mobile/tests).
RESTRICT="test"

# Only libpurple 2.14 API is used, so it also loads in the stock Pidgin
# 2.14.14; the pidgin-gtk4 net-im/pidgin uses the extra message metadata.
RDEPEND="
	app-crypt/libsecret
	dev-libs/glib:2
	dev-libs/json-glib
	dev-libs/nss
	net-im/pidgin:0
	sys-libs/zlib
"
DEPEND="${RDEPEND}"
BDEPEND="virtual/pkgconfig"

src_configure() {
	tc-export CC PKG_CONFIG
}

src_install() {
	default
	dodoc ../README.md
}
