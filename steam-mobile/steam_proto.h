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
 * Minimal protobuf wire-format codec.
 *
 * Steam's CM protocol and IAuthenticationService speak protobuf. Rather than
 * depend on protobuf-c and generated code, this plugin hand-encodes the
 * handful of messages it needs on top of these primitives. Message-level
 * structs and their encode/decode functions live in steam_msgs.h.
 *
 * Writer: append fields to a GByteArray. Nested messages are built in their
 * own GByteArray and embedded with steam_proto_put_message().
 *
 * Reader: iterate fields with steam_proto_next(); switch on r->field, read
 * the value from the union member matching r->wt, and ignore unknown fields.
 */

#ifndef STEAM_PROTO_H
#define STEAM_PROTO_H

#include <glib.h>

typedef enum {
	STEAM_PROTO_WT_VARINT  = 0,
	STEAM_PROTO_WT_FIXED64 = 1,
	STEAM_PROTO_WT_LEN     = 2,
	STEAM_PROTO_WT_FIXED32 = 5
} SteamProtoWireType;

/* ---- Writer ---- */

void steam_proto_put_varint_raw(GByteArray *b, guint64 v);
void steam_proto_put_tag(GByteArray *b, guint field, SteamProtoWireType wt);

/* uint32/uint64/int64/enum fields */
void steam_proto_put_varint(GByteArray *b, guint field, guint64 v);
/* int32 fields: negative values are sign-extended to 10 bytes as protobuf requires */
void steam_proto_put_int32(GByteArray *b, guint field, gint32 v);
void steam_proto_put_bool(GByteArray *b, guint field, gboolean v);
void steam_proto_put_fixed64(GByteArray *b, guint field, guint64 v);
void steam_proto_put_fixed32(GByteArray *b, guint field, guint32 v);
void steam_proto_put_bytes(GByteArray *b, guint field, const guint8 *data, gsize len);
/* NULL string is skipped (field not present) */
void steam_proto_put_string(GByteArray *b, guint field, const gchar *s);
/* Embeds a sub-message built in another GByteArray (length-delimited). */
void steam_proto_put_message(GByteArray *b, guint field, const GByteArray *sub);

/* ---- Reader ---- */

typedef struct {
	const guint8 *data;
	gsize len;
	gsize pos;
	gboolean error;   /* set when input is malformed; steam_proto_next() returns FALSE */

	/* Populated by steam_proto_next() */
	guint field;
	SteamProtoWireType wt;
	guint64 varint;        /* STEAM_PROTO_WT_VARINT */
	guint64 fixed;         /* STEAM_PROTO_WT_FIXED32 / _FIXED64 (zero-extended) */
	const guint8 *bytes;   /* STEAM_PROTO_WT_LEN: points into data */
	gsize bytes_len;
} SteamProtoReader;

void steam_proto_reader_init(SteamProtoReader *r, const guint8 *data, gsize len);

/* Reads the next field. Returns FALSE at end of input or on malformed data
 * (check r->error to tell them apart). Unknown or unwanted fields are simply
 * skipped by calling steam_proto_next() again. */
gboolean steam_proto_next(SteamProtoReader *r);

/* Helpers for the current field */
#define steam_proto_int32(r)  ((gint32)(guint32)(r)->varint)
#define steam_proto_uint32(r) ((guint32)(r)->varint)
#define steam_proto_bool(r)   ((r)->varint != 0)
/* Returns a NUL-terminated copy of the current LEN field (caller frees). */
gchar *steam_proto_dup_string(const SteamProtoReader *r);
/* Returns a copy of the current LEN field as a GByteArray (caller frees). */
GByteArray *steam_proto_dup_bytes(const SteamProtoReader *r);
/* Initialises a sub-reader over the current LEN field (embedded message). */
void steam_proto_sub_reader(const SteamProtoReader *r, SteamProtoReader *sub);

/* Hex dump for debug logs (caller frees) */
gchar *steam_proto_hexdump(const guint8 *data, gsize len);

#endif /* STEAM_PROTO_H */
