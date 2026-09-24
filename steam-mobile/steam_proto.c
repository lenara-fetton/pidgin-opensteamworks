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

#include "steam_proto.h"

#include <string.h>

/* ---- Writer ---- */

void
steam_proto_put_varint_raw(GByteArray *b, guint64 v)
{
	guint8 tmp[10];
	gsize n = 0;

	do {
		guint8 byte = v & 0x7f;
		v >>= 7;
		if (v)
			byte |= 0x80;
		tmp[n++] = byte;
	} while (v);

	g_byte_array_append(b, tmp, n);
}

void
steam_proto_put_tag(GByteArray *b, guint field, SteamProtoWireType wt)
{
	steam_proto_put_varint_raw(b, ((guint64)field << 3) | (guint)wt);
}

void
steam_proto_put_varint(GByteArray *b, guint field, guint64 v)
{
	steam_proto_put_tag(b, field, STEAM_PROTO_WT_VARINT);
	steam_proto_put_varint_raw(b, v);
}

void
steam_proto_put_int32(GByteArray *b, guint field, gint32 v)
{
	/* protobuf encodes negative int32 as the 64-bit two's complement */
	steam_proto_put_varint(b, field, (guint64)(gint64)v);
}

void
steam_proto_put_bool(GByteArray *b, guint field, gboolean v)
{
	steam_proto_put_varint(b, field, v ? 1 : 0);
}

void
steam_proto_put_fixed64(GByteArray *b, guint field, guint64 v)
{
	guint8 tmp[8];
	gint i;

	steam_proto_put_tag(b, field, STEAM_PROTO_WT_FIXED64);
	for (i = 0; i < 8; i++)
		tmp[i] = (v >> (8 * i)) & 0xff;
	g_byte_array_append(b, tmp, 8);
}

void
steam_proto_put_fixed32(GByteArray *b, guint field, guint32 v)
{
	guint8 tmp[4];
	gint i;

	steam_proto_put_tag(b, field, STEAM_PROTO_WT_FIXED32);
	for (i = 0; i < 4; i++)
		tmp[i] = (v >> (8 * i)) & 0xff;
	g_byte_array_append(b, tmp, 4);
}

void
steam_proto_put_bytes(GByteArray *b, guint field, const guint8 *data, gsize len)
{
	steam_proto_put_tag(b, field, STEAM_PROTO_WT_LEN);
	steam_proto_put_varint_raw(b, len);
	if (len)
		g_byte_array_append(b, data, len);
}

void
steam_proto_put_string(GByteArray *b, guint field, const gchar *s)
{
	if (s == NULL)
		return;
	steam_proto_put_bytes(b, field, (const guint8 *)s, strlen(s));
}

void
steam_proto_put_message(GByteArray *b, guint field, const GByteArray *sub)
{
	steam_proto_put_bytes(b, field, sub ? sub->data : NULL, sub ? sub->len : 0);
}

/* ---- Reader ---- */

void
steam_proto_reader_init(SteamProtoReader *r, const guint8 *data, gsize len)
{
	memset(r, 0, sizeof(*r));
	r->data = data;
	r->len = len;
}

static gboolean
read_varint(SteamProtoReader *r, guint64 *out)
{
	guint64 v = 0;
	guint shift = 0;

	while (r->pos < r->len) {
		guint8 byte = r->data[r->pos++];
		if (shift < 64)
			v |= (guint64)(byte & 0x7f) << shift;
		shift += 7;
		if (!(byte & 0x80)) {
			*out = v;
			return TRUE;
		}
		if (shift >= 70) /* more than 10 bytes */
			break;
	}

	r->error = TRUE;
	return FALSE;
}

gboolean
steam_proto_next(SteamProtoReader *r)
{
	guint64 tag;
	gsize i;

	if (r->error || r->pos >= r->len)
		return FALSE;

	if (!read_varint(r, &tag))
		return FALSE;

	r->field = (guint)(tag >> 3);
	r->wt = (SteamProtoWireType)(tag & 7);
	r->varint = 0;
	r->fixed = 0;
	r->bytes = NULL;
	r->bytes_len = 0;

	if (r->field == 0 || (tag >> 3) > 0x1FFFFFFF) {
		r->error = TRUE;
		return FALSE;
	}

	switch (r->wt) {
	case STEAM_PROTO_WT_VARINT:
		return read_varint(r, &r->varint);

	case STEAM_PROTO_WT_FIXED64:
		if (r->len - r->pos < 8) {
			r->error = TRUE;
			return FALSE;
		}
		for (i = 0; i < 8; i++)
			r->fixed |= (guint64)r->data[r->pos + i] << (8 * i);
		r->pos += 8;
		return TRUE;

	case STEAM_PROTO_WT_FIXED32:
		if (r->len - r->pos < 4) {
			r->error = TRUE;
			return FALSE;
		}
		for (i = 0; i < 4; i++)
			r->fixed |= (guint64)r->data[r->pos + i] << (8 * i);
		r->pos += 4;
		return TRUE;

	case STEAM_PROTO_WT_LEN: {
		guint64 blen;
		if (!read_varint(r, &blen))
			return FALSE;
		if (blen > r->len - r->pos) {
			r->error = TRUE;
			return FALSE;
		}
		r->bytes = r->data + r->pos;
		r->bytes_len = (gsize)blen;
		r->pos += (gsize)blen;
		return TRUE;
	}

	default:
		/* start/end group (3/4) are deprecated and unused by Steam */
		r->error = TRUE;
		return FALSE;
	}
}

gchar *
steam_proto_dup_string(const SteamProtoReader *r)
{
	if (r->wt != STEAM_PROTO_WT_LEN)
		return NULL;
	return g_strndup((const gchar *)r->bytes, r->bytes_len);
}

GByteArray *
steam_proto_dup_bytes(const SteamProtoReader *r)
{
	GByteArray *b = g_byte_array_new();
	if (r->wt == STEAM_PROTO_WT_LEN && r->bytes_len)
		g_byte_array_append(b, r->bytes, r->bytes_len);
	return b;
}

void
steam_proto_sub_reader(const SteamProtoReader *r, SteamProtoReader *sub)
{
	if (r->wt == STEAM_PROTO_WT_LEN)
		steam_proto_reader_init(sub, r->bytes, r->bytes_len);
	else
		steam_proto_reader_init(sub, NULL, 0);
}

gchar *
steam_proto_hexdump(const guint8 *data, gsize len)
{
	static const char hex[] = "0123456789abcdef";
	gchar *out = g_malloc(len * 2 + 1);
	gsize i;

	for (i = 0; i < len; i++) {
		out[i * 2] = hex[data[i] >> 4];
		out[i * 2 + 1] = hex[data[i] & 0xf];
	}
	out[len * 2] = '\0';
	return out;
}
