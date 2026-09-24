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

/* Message structs and codecs. See steam_msgs.h for conventions.
 * No libpurple here: this file is unit tested standalone. */

#include "steam_msgs.h"
#include "steam_proto.h"

#include <string.h>
#include <zlib.h>

/* Largest inflated CMsgMulti body we accept. */
#define STEAM_MSG_MAX_UNZIPPED (64 * 1024 * 1024)

/* ======================================================================
 * Field helpers
 * ====================================================================== */

/* Readers: store the current field if its wire type matches; a field with
 * an unexpected wire type is ignored like an unknown field. */

static void
rd_u32(const SteamProtoReader *r, gboolean *has, guint32 *v)
{
	if (r->wt == STEAM_PROTO_WT_VARINT) {
		*has = TRUE;
		*v = (guint32)r->varint;
	}
}

static void
rd_i32(const SteamProtoReader *r, gboolean *has, gint32 *v)
{
	if (r->wt == STEAM_PROTO_WT_VARINT) {
		*has = TRUE;
		*v = (gint32)(guint32)r->varint;
	}
}

static void
rd_u64(const SteamProtoReader *r, gboolean *has, guint64 *v)
{
	if (r->wt == STEAM_PROTO_WT_VARINT) {
		*has = TRUE;
		*v = r->varint;
	}
}

static void
rd_bool(const SteamProtoReader *r, gboolean *has, gboolean *v)
{
	if (r->wt == STEAM_PROTO_WT_VARINT) {
		*has = TRUE;
		*v = r->varint != 0;
	}
}

static void
rd_fixed64(const SteamProtoReader *r, gboolean *has, guint64 *v)
{
	if (r->wt == STEAM_PROTO_WT_FIXED64) {
		*has = TRUE;
		*v = r->fixed;
	}
}

static void
rd_fixed32(const SteamProtoReader *r, gboolean *has, guint32 *v)
{
	if (r->wt == STEAM_PROTO_WT_FIXED32) {
		*has = TRUE;
		*v = (guint32)r->fixed;
	}
}

static void
rd_string(const SteamProtoReader *r, gchar **s)
{
	if (r->wt == STEAM_PROTO_WT_LEN) {
		g_free(*s);
		*s = steam_proto_dup_string(r);
	}
}

static void
rd_bytes(const SteamProtoReader *r, GByteArray **b)
{
	if (r->wt == STEAM_PROTO_WT_LEN) {
		if (*b)
			g_byte_array_unref(*b);
		*b = steam_proto_dup_bytes(r);
	}
}

#define R_U32(r, m, f)     rd_u32(r, &(m)->has_##f, &(m)->f)
#define R_I32(r, m, f)     rd_i32(r, &(m)->has_##f, &(m)->f)
#define R_U64(r, m, f)     rd_u64(r, &(m)->has_##f, &(m)->f)
#define R_BOOL(r, m, f)    rd_bool(r, &(m)->has_##f, &(m)->f)
#define R_FIXED64(r, m, f) rd_fixed64(r, &(m)->has_##f, &(m)->f)
#define R_FIXED32(r, m, f) rd_fixed32(r, &(m)->has_##f, &(m)->f)

/* Writers: emit the field only when present. */

static void
wr_u32(GByteArray *o, guint field, gboolean has, guint32 v)
{
	if (has)
		steam_proto_put_varint(o, field, v);
}

static void
wr_i32(GByteArray *o, guint field, gboolean has, gint32 v)
{
	if (has)
		steam_proto_put_int32(o, field, v);
}

static void
wr_u64(GByteArray *o, guint field, gboolean has, guint64 v)
{
	if (has)
		steam_proto_put_varint(o, field, v);
}

static void
wr_bool(GByteArray *o, guint field, gboolean has, gboolean v)
{
	if (has)
		steam_proto_put_bool(o, field, v);
}

static void
wr_fixed64(GByteArray *o, guint field, gboolean has, guint64 v)
{
	if (has)
		steam_proto_put_fixed64(o, field, v);
}

static void
wr_fixed32(GByteArray *o, guint field, gboolean has, guint32 v)
{
	if (has)
		steam_proto_put_fixed32(o, field, v);
}

static void
wr_bytes(GByteArray *o, guint field, const GByteArray *b)
{
	if (b)
		steam_proto_put_bytes(o, field, b->data, b->len);
}

#define W_U32(o, n, m, f)     wr_u32(o, n, (m)->has_##f, (m)->f)
#define W_I32(o, n, m, f)     wr_i32(o, n, (m)->has_##f, (m)->f)
#define W_U64(o, n, m, f)     wr_u64(o, n, (m)->has_##f, (m)->f)
#define W_BOOL(o, n, m, f)    wr_bool(o, n, (m)->has_##f, (m)->f)
#define W_FIXED64(o, n, m, f) wr_fixed64(o, n, (m)->has_##f, (m)->f)
#define W_FIXED32(o, n, m, f) wr_fixed32(o, n, (m)->has_##f, (m)->f)
#define W_STR(o, n, m, f)     steam_proto_put_string(o, n, (m)->f)
#define W_BYTES(o, n, m, f)   wr_bytes(o, n, (m)->f)

#define FREE_STR(p) G_STMT_START { g_free(p); (p) = NULL; } G_STMT_END
#define FREE_BYTES(p) G_STMT_START { if (p) g_byte_array_unref(p); (p) = NULL; } G_STMT_END
#define FREE_ARRAY(p) G_STMT_START { if (p) g_array_unref(p); (p) = NULL; } G_STMT_END

/* Encodes a sub-message with `fn` and embeds it as field `field`. */
#define W_SUB(o, field, fn, sub) G_STMT_START { \
	GByteArray *_tmp = g_byte_array_new(); \
	fn((sub), _tmp); \
	steam_proto_put_message((o), (field), _tmp); \
	g_byte_array_unref(_tmp); \
} G_STMT_END

/* Standard decode loop head/tail */
#define DECODE_BEGIN(r, data, len) \
	SteamProtoReader r; \
	steam_proto_reader_init(&r, data, len); \
	while (steam_proto_next(&r)) { \
		switch (r.field) {
#define DECODE_END(r) \
		default: \
			break; \
		} \
	} \
	return !r.error;

/* ======================================================================
 * CMsgProtoBufHeader
 * ====================================================================== */

void
steam_msg_protobuf_header_init(SteamMsgProtoBufHeader *m)
{
	memset(m, 0, sizeof(*m));
	m->jobid_source = STEAM_JOBID_NONE;
	m->jobid_target = STEAM_JOBID_NONE;
	m->eresult = STEAM_ERESULT_FAIL;
}

void
steam_msg_protobuf_header_clear(SteamMsgProtoBufHeader *m)
{
	FREE_STR(m->target_job_name);
	FREE_STR(m->error_message);
}

void
steam_msg_protobuf_header_encode(const SteamMsgProtoBufHeader *m, GByteArray *o)
{
	W_FIXED64(o, 1, m, steamid);
	W_I32(o, 2, m, client_sessionid);
	W_U32(o, 3, m, routing_appid);
	W_FIXED64(o, 10, m, jobid_source);
	W_FIXED64(o, 11, m, jobid_target);
	W_STR(o, 12, m, target_job_name);
	W_I32(o, 13, m, eresult);
	W_STR(o, 14, m, error_message);
	W_U32(o, 32, m, realm);
}

gboolean
steam_msg_protobuf_header_decode(SteamMsgProtoBufHeader *m, const guint8 *data, gsize len)
{
	steam_msg_protobuf_header_clear(m);
	steam_msg_protobuf_header_init(m);
	{
	DECODE_BEGIN(r, data, len)
		case 1: R_FIXED64(&r, m, steamid); break;
		case 2: R_I32(&r, m, client_sessionid); break;
		case 3: R_U32(&r, m, routing_appid); break;
		case 10: R_FIXED64(&r, m, jobid_source); break;
		case 11: R_FIXED64(&r, m, jobid_target); break;
		case 12: rd_string(&r, &m->target_job_name); break;
		case 13: R_I32(&r, m, eresult); break;
		case 14: rd_string(&r, &m->error_message); break;
		case 32: R_U32(&r, m, realm); break;
	DECODE_END(r)
	}
}

/* ======================================================================
 * CMsgMulti
 * ====================================================================== */

void
steam_msg_multi_init(SteamMsgMulti *m)
{
	memset(m, 0, sizeof(*m));
}

void
steam_msg_multi_clear(SteamMsgMulti *m)
{
	FREE_BYTES(m->message_body);
}

void
steam_msg_multi_encode(const SteamMsgMulti *m, GByteArray *o)
{
	W_U32(o, 1, m, size_unzipped);
	W_BYTES(o, 2, m, message_body);
}

gboolean
steam_msg_multi_decode(SteamMsgMulti *m, const guint8 *data, gsize len)
{
	steam_msg_multi_clear(m);
	steam_msg_multi_init(m);
	{
	DECODE_BEGIN(r, data, len)
		case 1: R_U32(&r, m, size_unzipped); break;
		case 2: rd_bytes(&r, &m->message_body); break;
	DECODE_END(r)
	}
}

/* Inflates gzip (or zlib) data that must expand to exactly `expected` bytes. */
static GByteArray *
steam_msg_gunzip(const guint8 *data, gsize len, gsize expected)
{
	z_stream zs;
	GByteArray *out;
	int ret;

	if (expected == 0 || expected > STEAM_MSG_MAX_UNZIPPED || len > G_MAXUINT)
		return NULL;

	memset(&zs, 0, sizeof(zs));
	/* +32: auto-detect gzip or zlib header */
	if (inflateInit2(&zs, MAX_WBITS + 32) != Z_OK)
		return NULL;

	out = g_byte_array_sized_new(expected);
	g_byte_array_set_size(out, expected);

	zs.next_in = (Bytef *)data;
	zs.avail_in = (uInt)len;
	zs.next_out = out->data;
	zs.avail_out = (uInt)expected;

	ret = inflate(&zs, Z_FINISH);
	if (ret != Z_STREAM_END || zs.total_out != expected) {
		inflateEnd(&zs);
		g_byte_array_unref(out);
		return NULL;
	}

	inflateEnd(&zs);
	return out;
}

static guint32
read_le32(const guint8 *p)
{
	return (guint32)p[0] | ((guint32)p[1] << 8) | ((guint32)p[2] << 16) | ((guint32)p[3] << 24);
}

static void
put_le32(GByteArray *b, guint32 v)
{
	guint8 tmp[4];

	tmp[0] = v & 0xff;
	tmp[1] = (v >> 8) & 0xff;
	tmp[2] = (v >> 16) & 0xff;
	tmp[3] = (v >> 24) & 0xff;
	g_byte_array_append(b, tmp, 4);
}

gboolean
steam_msg_multi_unpack(const SteamMsgMulti *m, GPtrArray *out_packets)
{
	GByteArray *inflated = NULL;
	GPtrArray *tmp;
	const guint8 *p;
	gsize len, pos = 0;
	gboolean ok = TRUE;
	guint i;

	if (m->size_unzipped > 0) {
		if (m->message_body == NULL)
			return FALSE;
		inflated = steam_msg_gunzip(m->message_body->data, m->message_body->len,
		                            m->size_unzipped);
		if (inflated == NULL)
			return FALSE;
		p = inflated->data;
		len = inflated->len;
	} else if (m->message_body) {
		p = m->message_body->data;
		len = m->message_body->len;
	} else {
		p = NULL;
		len = 0;
	}

	tmp = g_ptr_array_new();
	while (pos < len) {
		guint32 plen;
		GByteArray *pkt;

		if (len - pos < 4) {
			ok = FALSE;
			break;
		}
		plen = read_le32(p + pos);
		pos += 4;
		if (plen > len - pos) {
			ok = FALSE;
			break;
		}
		pkt = g_byte_array_sized_new(plen);
		g_byte_array_append(pkt, p + pos, plen);
		g_ptr_array_add(tmp, pkt);
		pos += plen;
	}

	for (i = 0; i < tmp->len; i++) {
		if (ok)
			g_ptr_array_add(out_packets, g_ptr_array_index(tmp, i));
		else
			g_byte_array_unref(g_ptr_array_index(tmp, i));
	}
	g_ptr_array_free(tmp, TRUE);
	if (inflated)
		g_byte_array_unref(inflated);
	return ok;
}

/* ======================================================================
 * Packet framing
 * ====================================================================== */

gboolean
steam_msg_packet_parse(const guint8 *data, gsize len, guint32 *emsg,
                       SteamMsgProtoBufHeader *hdr,
                       const guint8 **body, gsize *body_len)
{
	guint32 raw, hlen;

	steam_msg_protobuf_header_init(hdr);
	if (body)
		*body = NULL;
	if (body_len)
		*body_len = 0;

	if (data == NULL || len < 4)
		return FALSE;
	raw = read_le32(data);
	if (emsg)
		*emsg = raw & ~STEAM_EMSG_PROTO_MASK;
	if (!(raw & STEAM_EMSG_PROTO_MASK))
		return FALSE;
	if (len < 8)
		return FALSE;
	hlen = read_le32(data + 4);
	if (hlen > len - 8)
		return FALSE;
	if (!steam_msg_protobuf_header_decode(hdr, data + 8, hlen))
		return FALSE;

	if (body)
		*body = data + 8 + hlen;
	if (body_len)
		*body_len = len - 8 - hlen;
	return TRUE;
}

void
steam_msg_packet_build(guint32 emsg, const SteamMsgProtoBufHeader *hdr,
                       const GByteArray *body, GByteArray *out)
{
	GByteArray *h = g_byte_array_new();

	if (hdr)
		steam_msg_protobuf_header_encode(hdr, h);
	put_le32(out, emsg | STEAM_EMSG_PROTO_MASK);
	put_le32(out, h->len);
	if (h->len)
		g_byte_array_append(out, h->data, h->len);
	if (body && body->len)
		g_byte_array_append(out, body->data, body->len);
	g_byte_array_unref(h);
}

/* ======================================================================
 * CMsgClientHello
 * ====================================================================== */

void
steam_msg_client_hello_init(SteamMsgClientHello *m)
{
	memset(m, 0, sizeof(*m));
}

void
steam_msg_client_hello_clear(SteamMsgClientHello *m)
{
}

void
steam_msg_client_hello_encode(const SteamMsgClientHello *m, GByteArray *o)
{
	W_U32(o, 1, m, protocol_version);
}

gboolean
steam_msg_client_hello_decode(SteamMsgClientHello *m, const guint8 *data, gsize len)
{
	steam_msg_client_hello_init(m);
	{
	DECODE_BEGIN(r, data, len)
		case 1: R_U32(&r, m, protocol_version); break;
	DECODE_END(r)
	}
}

/* ======================================================================
 * CMsgClientLogon
 * ====================================================================== */

/* CMsgIPAddress { oneof ip { fixed32 v4 = 1; bytes v6 = 2; } }: keeps v4 only. */
static gboolean
ip_address_v4_decode(gboolean *has, guint32 *v4, const SteamProtoReader *parent)
{
	SteamProtoReader r;

	steam_proto_sub_reader(parent, &r);
	while (steam_proto_next(&r)) {
		switch (r.field) {
		case 1: rd_fixed32(&r, has, v4); break;
		case 2: *has = FALSE; break;
		default: break;
		}
	}
	return !r.error;
}

void
steam_msg_client_logon_init(SteamMsgClientLogon *m)
{
	memset(m, 0, sizeof(*m));
}

void
steam_msg_client_logon_clear(SteamMsgClientLogon *m)
{
	FREE_STR(m->client_language);
	FREE_BYTES(m->machine_id);
	FREE_STR(m->account_name);
	FREE_STR(m->password);
	FREE_STR(m->machine_name);
	FREE_STR(m->access_token);
}

void
steam_msg_client_logon_encode(const SteamMsgClientLogon *m, GByteArray *o)
{
	W_U32(o, 1, m, protocol_version);
	W_U32(o, 3, m, cell_id);
	W_U32(o, 5, m, client_package_version);
	W_STR(o, 6, m, client_language);
	W_U32(o, 7, m, client_os_type);
	W_BOOL(o, 8, m, should_remember_password);
	if (m->has_obfuscated_private_ip) {
		GByteArray *ip = g_byte_array_new();
		steam_proto_put_fixed32(ip, 1, m->obfuscated_private_ip);
		steam_proto_put_message(o, 11, ip);
		g_byte_array_unref(ip);
	}
	W_U32(o, 21, m, qos_level);
	W_FIXED64(o, 22, m, client_supplied_steam_id);
	W_BYTES(o, 30, m, machine_id);
	W_U32(o, 33, m, chat_mode);
	W_STR(o, 50, m, account_name);
	W_STR(o, 51, m, password);
	W_STR(o, 96, m, machine_name);
	W_BOOL(o, 102, m, supports_rate_limit_response);
	W_STR(o, 108, m, access_token);
}

gboolean
steam_msg_client_logon_decode(SteamMsgClientLogon *m, const guint8 *data, gsize len)
{
	steam_msg_client_logon_clear(m);
	steam_msg_client_logon_init(m);
	{
	DECODE_BEGIN(r, data, len)
		case 1: R_U32(&r, m, protocol_version); break;
		case 3: R_U32(&r, m, cell_id); break;
		case 5: R_U32(&r, m, client_package_version); break;
		case 6: rd_string(&r, &m->client_language); break;
		case 7: R_U32(&r, m, client_os_type); break;
		case 8: R_BOOL(&r, m, should_remember_password); break;
		case 11:
			if (r.wt == STEAM_PROTO_WT_LEN && !ip_address_v4_decode(&m->has_obfuscated_private_ip,
			                                                      &m->obfuscated_private_ip, &r))
				return FALSE;
			break;
		case 21: R_U32(&r, m, qos_level); break;
		case 22: R_FIXED64(&r, m, client_supplied_steam_id); break;
		case 30: rd_bytes(&r, &m->machine_id); break;
		case 33: R_U32(&r, m, chat_mode); break;
		case 50: rd_string(&r, &m->account_name); break;
		case 51: rd_string(&r, &m->password); break;
		case 96: rd_string(&r, &m->machine_name); break;
		case 102: R_BOOL(&r, m, supports_rate_limit_response); break;
		case 108: rd_string(&r, &m->access_token); break;
	DECODE_END(r)
	}
}

/* ======================================================================
 * CMsgClientLogonResponse
 * ====================================================================== */

void
steam_msg_client_logon_response_init(SteamMsgClientLogonResponse *m)
{
	memset(m, 0, sizeof(*m));
	m->eresult = STEAM_ERESULT_FAIL;
}

void
steam_msg_client_logon_response_clear(SteamMsgClientLogonResponse *m)
{
	FREE_STR(m->email_domain);
	FREE_STR(m->vanity_url);
	FREE_STR(m->ip_country_code);
	FREE_STR(m->agreement_session_url);
}

void
steam_msg_client_logon_response_encode(const SteamMsgClientLogonResponse *m, GByteArray *o)
{
	W_I32(o, 1, m, eresult);
	W_I32(o, 2, m, legacy_out_of_game_heartbeat_seconds);
	W_I32(o, 3, m, heartbeat_seconds);
	W_FIXED32(o, 5, m, rtime32_server_time);
	W_U32(o, 6, m, account_flags);
	W_U32(o, 7, m, cell_id);
	W_STR(o, 8, m, email_domain);
	W_I32(o, 10, m, eresult_extended);
	W_STR(o, 14, m, vanity_url);
	W_FIXED64(o, 20, m, client_supplied_steamid);
	W_STR(o, 21, m, ip_country_code);
	W_U64(o, 27, m, client_instance_id);
	W_STR(o, 29, m, agreement_session_url);
}

gboolean
steam_msg_client_logon_response_decode(SteamMsgClientLogonResponse *m, const guint8 *data, gsize len)
{
	steam_msg_client_logon_response_clear(m);
	steam_msg_client_logon_response_init(m);
	{
	DECODE_BEGIN(r, data, len)
		case 1: R_I32(&r, m, eresult); break;
		case 2: R_I32(&r, m, legacy_out_of_game_heartbeat_seconds); break;
		case 3: R_I32(&r, m, heartbeat_seconds); break;
		case 5: R_FIXED32(&r, m, rtime32_server_time); break;
		case 6: R_U32(&r, m, account_flags); break;
		case 7: R_U32(&r, m, cell_id); break;
		case 8: rd_string(&r, &m->email_domain); break;
		case 10: R_I32(&r, m, eresult_extended); break;
		case 14: rd_string(&r, &m->vanity_url); break;
		case 20: R_FIXED64(&r, m, client_supplied_steamid); break;
		case 21: rd_string(&r, &m->ip_country_code); break;
		case 27: R_U64(&r, m, client_instance_id); break;
		case 29: rd_string(&r, &m->agreement_session_url); break;
	DECODE_END(r)
	}
}

/* ======================================================================
 * CMsgClientLogOff (empty)
 * ====================================================================== */

void
steam_msg_client_logoff_init(SteamMsgClientLogOff *m)
{
	memset(m, 0, sizeof(*m));
}

void
steam_msg_client_logoff_clear(SteamMsgClientLogOff *m)
{
}

void
steam_msg_client_logoff_encode(const SteamMsgClientLogOff *m, GByteArray *o)
{
}

gboolean
steam_msg_client_logoff_decode(SteamMsgClientLogOff *m, const guint8 *data, gsize len)
{
	steam_msg_client_logoff_init(m);
	{
	DECODE_BEGIN(r, data, len)
	DECODE_END(r)
	}
}

/* ======================================================================
 * CMsgClientLoggedOff
 * ====================================================================== */

void
steam_msg_client_logged_off_init(SteamMsgClientLoggedOff *m)
{
	memset(m, 0, sizeof(*m));
	m->eresult = STEAM_ERESULT_FAIL;
}

void
steam_msg_client_logged_off_clear(SteamMsgClientLoggedOff *m)
{
}

void
steam_msg_client_logged_off_encode(const SteamMsgClientLoggedOff *m, GByteArray *o)
{
	W_I32(o, 1, m, eresult);
}

gboolean
steam_msg_client_logged_off_decode(SteamMsgClientLoggedOff *m, const guint8 *data, gsize len)
{
	steam_msg_client_logged_off_init(m);
	{
	DECODE_BEGIN(r, data, len)
		case 1: R_I32(&r, m, eresult); break;
	DECODE_END(r)
	}
}

/* ======================================================================
 * CMsgClientHeartBeat
 * ====================================================================== */

void
steam_msg_client_heartbeat_init(SteamMsgClientHeartBeat *m)
{
	memset(m, 0, sizeof(*m));
}

void
steam_msg_client_heartbeat_clear(SteamMsgClientHeartBeat *m)
{
}

void
steam_msg_client_heartbeat_encode(const SteamMsgClientHeartBeat *m, GByteArray *o)
{
	W_BOOL(o, 1, m, send_reply);
}

gboolean
steam_msg_client_heartbeat_decode(SteamMsgClientHeartBeat *m, const guint8 *data, gsize len)
{
	steam_msg_client_heartbeat_init(m);
	{
	DECODE_BEGIN(r, data, len)
		case 1: R_BOOL(&r, m, send_reply); break;
	DECODE_END(r)
	}
}

/* ======================================================================
 * CMsgClientAccountInfo
 * ====================================================================== */

void
steam_msg_client_account_info_init(SteamMsgClientAccountInfo *m)
{
	memset(m, 0, sizeof(*m));
}

void
steam_msg_client_account_info_clear(SteamMsgClientAccountInfo *m)
{
	FREE_STR(m->persona_name);
	FREE_STR(m->ip_country);
}

void
steam_msg_client_account_info_encode(const SteamMsgClientAccountInfo *m, GByteArray *o)
{
	W_STR(o, 1, m, persona_name);
	W_STR(o, 2, m, ip_country);
	W_I32(o, 5, m, count_authed_computers);
	W_U32(o, 7, m, account_flags);
	W_BOOL(o, 16, m, is_phone_verified);
	W_U32(o, 17, m, two_factor_state);
}

gboolean
steam_msg_client_account_info_decode(SteamMsgClientAccountInfo *m, const guint8 *data, gsize len)
{
	steam_msg_client_account_info_clear(m);
	steam_msg_client_account_info_init(m);
	{
	DECODE_BEGIN(r, data, len)
		case 1: rd_string(&r, &m->persona_name); break;
		case 2: rd_string(&r, &m->ip_country); break;
		case 5: R_I32(&r, m, count_authed_computers); break;
		case 7: R_U32(&r, m, account_flags); break;
		case 16: R_BOOL(&r, m, is_phone_verified); break;
		case 17: R_U32(&r, m, two_factor_state); break;
	DECODE_END(r)
	}
}

/* ======================================================================
 * CMsgClientFriendsList
 * ====================================================================== */

void
steam_msg_client_friends_list_init(SteamMsgClientFriendsList *m)
{
	memset(m, 0, sizeof(*m));
	m->friends = g_array_new(FALSE, TRUE, sizeof(SteamMsgClientFriendsListFriend));
}

void
steam_msg_client_friends_list_clear(SteamMsgClientFriendsList *m)
{
	FREE_ARRAY(m->friends);
}

static void
friends_list_friend_encode(const SteamMsgClientFriendsListFriend *f, GByteArray *o)
{
	W_FIXED64(o, 1, f, ulfriendid);
	W_U32(o, 2, f, efriendrelationship);
}

static gboolean
friends_list_friend_decode(SteamMsgClientFriendsListFriend *f, const SteamProtoReader *parent)
{
	SteamProtoReader r;

	steam_proto_sub_reader(parent, &r);
	while (steam_proto_next(&r)) {
		switch (r.field) {
		case 1: R_FIXED64(&r, f, ulfriendid); break;
		case 2: R_U32(&r, f, efriendrelationship); break;
		default: break;
		}
	}
	return !r.error;
}

void
steam_msg_client_friends_list_encode(const SteamMsgClientFriendsList *m, GByteArray *o)
{
	guint i;

	W_BOOL(o, 1, m, bincremental);
	for (i = 0; i < m->friends->len; i++)
		W_SUB(o, 2, friends_list_friend_encode,
		      &g_array_index(m->friends, SteamMsgClientFriendsListFriend, i));
	W_U32(o, 3, m, max_friend_count);
	W_U32(o, 4, m, active_friend_count);
	W_BOOL(o, 5, m, friends_limit_hit);
}

gboolean
steam_msg_client_friends_list_decode(SteamMsgClientFriendsList *m, const guint8 *data, gsize len)
{
	steam_msg_client_friends_list_clear(m);
	steam_msg_client_friends_list_init(m);
	{
	DECODE_BEGIN(r, data, len)
		case 1: R_BOOL(&r, m, bincremental); break;
		case 2:
			if (r.wt == STEAM_PROTO_WT_LEN) {
				SteamMsgClientFriendsListFriend f;
				memset(&f, 0, sizeof(f));
				if (!friends_list_friend_decode(&f, &r))
					return FALSE;
				g_array_append_val(m->friends, f);
			}
			break;
		case 3: R_U32(&r, m, max_friend_count); break;
		case 4: R_U32(&r, m, active_friend_count); break;
		case 5: R_BOOL(&r, m, friends_limit_hit); break;
	DECODE_END(r)
	}
}

/* ======================================================================
 * CMsgClientRequestFriendData
 * ====================================================================== */

void
steam_msg_client_request_friend_data_init(SteamMsgClientRequestFriendData *m)
{
	memset(m, 0, sizeof(*m));
	m->friends = g_array_new(FALSE, TRUE, sizeof(guint64));
}

void
steam_msg_client_request_friend_data_clear(SteamMsgClientRequestFriendData *m)
{
	FREE_ARRAY(m->friends);
}

void
steam_msg_client_request_friend_data_encode(const SteamMsgClientRequestFriendData *m, GByteArray *o)
{
	guint i;

	W_U32(o, 1, m, persona_state_requested);
	/* proto2 repeated without [packed=true]: one tag per element */
	for (i = 0; i < m->friends->len; i++)
		steam_proto_put_fixed64(o, 2, g_array_index(m->friends, guint64, i));
}

gboolean
steam_msg_client_request_friend_data_decode(SteamMsgClientRequestFriendData *m, const guint8 *data, gsize len)
{
	steam_msg_client_request_friend_data_clear(m);
	steam_msg_client_request_friend_data_init(m);
	{
	DECODE_BEGIN(r, data, len)
		case 1: R_U32(&r, m, persona_state_requested); break;
		case 2:
			if (r.wt == STEAM_PROTO_WT_FIXED64) {
				guint64 v = r.fixed;
				g_array_append_val(m->friends, v);
			} else if (r.wt == STEAM_PROTO_WT_LEN) {
				/* packed encoding: parsers must accept both */
				gsize i, j;
				if (r.bytes_len % 8)
					return FALSE;
				for (i = 0; i < r.bytes_len; i += 8) {
					guint64 v = 0;
					for (j = 0; j < 8; j++)
						v |= (guint64)r.bytes[i + j] << (8 * j);
					g_array_append_val(m->friends, v);
				}
			}
			break;
	DECODE_END(r)
	}
}

/* ======================================================================
 * CMsgClientPersonaState
 * ====================================================================== */

static void
persona_kv_clear(gpointer p)
{
	SteamMsgPersonaKV *kv = p;

	FREE_STR(kv->key);
	FREE_STR(kv->value);
}

static void
persona_friend_init(SteamMsgPersonaFriend *f)
{
	memset(f, 0, sizeof(*f));
	f->rich_presence = g_array_new(FALSE, TRUE, sizeof(SteamMsgPersonaKV));
	g_array_set_clear_func(f->rich_presence, persona_kv_clear);
}

static void
persona_friend_clear(gpointer p)
{
	SteamMsgPersonaFriend *f = p;

	FREE_STR(f->player_name);
	FREE_BYTES(f->avatar_hash);
	FREE_STR(f->game_name);
	FREE_BYTES(f->game_data_blob);
	FREE_STR(f->clan_tag);
	FREE_ARRAY(f->rich_presence);
	FREE_STR(f->watching_broadcast_title);
}

void
steam_msg_client_persona_state_init(SteamMsgClientPersonaState *m)
{
	memset(m, 0, sizeof(*m));
	m->friends = g_array_new(FALSE, TRUE, sizeof(SteamMsgPersonaFriend));
	g_array_set_clear_func(m->friends, persona_friend_clear);
}

void
steam_msg_client_persona_state_clear(SteamMsgClientPersonaState *m)
{
	FREE_ARRAY(m->friends);
}

SteamMsgPersonaFriend *
steam_msg_client_persona_state_add_friend(SteamMsgClientPersonaState *m)
{
	SteamMsgPersonaFriend f;

	persona_friend_init(&f);
	g_array_append_val(m->friends, f);
	return &g_array_index(m->friends, SteamMsgPersonaFriend, m->friends->len - 1);
}

void
steam_msg_persona_friend_add_rich_presence(SteamMsgPersonaFriend *f, const gchar *key, const gchar *value)
{
	SteamMsgPersonaKV kv;

	kv.key = g_strdup(key);
	kv.value = g_strdup(value);
	g_array_append_val(f->rich_presence, kv);
}

const gchar *
steam_msg_persona_friend_get_rich_presence(const SteamMsgPersonaFriend *f, const gchar *key)
{
	guint i;

	if (f->rich_presence == NULL || key == NULL)
		return NULL;
	for (i = 0; i < f->rich_presence->len; i++) {
		const SteamMsgPersonaKV *kv = &g_array_index(f->rich_presence, SteamMsgPersonaKV, i);
		if (kv->key && g_str_equal(kv->key, key))
			return kv->value;
	}
	return NULL;
}

static void
persona_kv_encode(const SteamMsgPersonaKV *kv, GByteArray *o)
{
	steam_proto_put_string(o, 1, kv->key);
	steam_proto_put_string(o, 2, kv->value);
}

static void
persona_clan_data_encode(const SteamMsgPersonaFriend *f, GByteArray *o)
{
	wr_u32(o, 1, f->has_clan_ogg_app_id, f->clan_ogg_app_id);
	wr_u64(o, 2, f->has_clan_chat_group_id, f->clan_chat_group_id);
}

static void
persona_friend_encode(const SteamMsgPersonaFriend *f, GByteArray *o)
{
	guint i;

	W_FIXED64(o, 1, f, friendid);
	W_U32(o, 2, f, persona_state);
	W_U32(o, 3, f, game_played_app_id);
	W_U32(o, 4, f, game_server_ip);
	W_U32(o, 5, f, game_server_port);
	W_U32(o, 6, f, persona_state_flags);
	W_U32(o, 7, f, online_session_instances);
	W_BOOL(o, 10, f, persona_set_by_user);
	W_STR(o, 15, f, player_name);
	W_U32(o, 20, f, query_port);
	W_FIXED64(o, 25, f, steamid_source);
	W_BYTES(o, 31, f, avatar_hash);
	W_U32(o, 45, f, last_logoff);
	W_U32(o, 46, f, last_logon);
	W_U32(o, 47, f, last_seen_online);
	W_U32(o, 50, f, clan_rank);
	W_STR(o, 55, f, game_name);
	W_FIXED64(o, 56, f, gameid);
	W_BYTES(o, 60, f, game_data_blob);
	if (f->has_clan_data)
		W_SUB(o, 64, persona_clan_data_encode, f);
	W_STR(o, 65, f, clan_tag);
	if (f->rich_presence)
		for (i = 0; i < f->rich_presence->len; i++)
			W_SUB(o, 71, persona_kv_encode,
			      &g_array_index(f->rich_presence, SteamMsgPersonaKV, i));
	W_FIXED64(o, 72, f, broadcast_id);
	W_FIXED64(o, 73, f, game_lobby_id);
	W_U32(o, 74, f, watching_broadcast_accountid);
	W_U32(o, 75, f, watching_broadcast_appid);
	W_U32(o, 76, f, watching_broadcast_viewers);
	W_STR(o, 77, f, watching_broadcast_title);
	W_BOOL(o, 78, f, is_community_banned);
	W_BOOL(o, 79, f, player_name_pending_review);
	W_BOOL(o, 80, f, avatar_pending_review);
	W_BOOL(o, 81, f, on_steam_deck);
	W_U32(o, 83, f, gaming_device_type);
}

static gboolean
persona_kv_decode(SteamMsgPersonaKV *kv, const SteamProtoReader *parent)
{
	SteamProtoReader r;

	steam_proto_sub_reader(parent, &r);
	while (steam_proto_next(&r)) {
		switch (r.field) {
		case 1: rd_string(&r, &kv->key); break;
		case 2: rd_string(&r, &kv->value); break;
		default: break;
		}
	}
	return !r.error;
}

static gboolean
persona_clan_data_decode(SteamMsgPersonaFriend *f, const SteamProtoReader *parent)
{
	SteamProtoReader r;

	f->has_clan_data = TRUE;
	steam_proto_sub_reader(parent, &r);
	while (steam_proto_next(&r)) {
		switch (r.field) {
		case 1: rd_u32(&r, &f->has_clan_ogg_app_id, &f->clan_ogg_app_id); break;
		case 2: rd_u64(&r, &f->has_clan_chat_group_id, &f->clan_chat_group_id); break;
		default: break;
		}
	}
	return !r.error;
}

static gboolean
persona_friend_decode(SteamMsgPersonaFriend *f, const SteamProtoReader *parent)
{
	SteamProtoReader r;

	steam_proto_sub_reader(parent, &r);
	while (steam_proto_next(&r)) {
		switch (r.field) {
		case 1: R_FIXED64(&r, f, friendid); break;
		case 2: R_U32(&r, f, persona_state); break;
		case 3: R_U32(&r, f, game_played_app_id); break;
		case 4: R_U32(&r, f, game_server_ip); break;
		case 5: R_U32(&r, f, game_server_port); break;
		case 6: R_U32(&r, f, persona_state_flags); break;
		case 7: R_U32(&r, f, online_session_instances); break;
		case 10: R_BOOL(&r, f, persona_set_by_user); break;
		case 15: rd_string(&r, &f->player_name); break;
		case 20: R_U32(&r, f, query_port); break;
		case 25: R_FIXED64(&r, f, steamid_source); break;
		case 31: rd_bytes(&r, &f->avatar_hash); break;
		case 45: R_U32(&r, f, last_logoff); break;
		case 46: R_U32(&r, f, last_logon); break;
		case 47: R_U32(&r, f, last_seen_online); break;
		case 50: R_U32(&r, f, clan_rank); break;
		case 55: rd_string(&r, &f->game_name); break;
		case 56: R_FIXED64(&r, f, gameid); break;
		case 60: rd_bytes(&r, &f->game_data_blob); break;
		case 64:
			if (r.wt == STEAM_PROTO_WT_LEN && !persona_clan_data_decode(f, &r))
				return FALSE;
			break;
		case 65: rd_string(&r, &f->clan_tag); break;
		case 71:
			if (r.wt == STEAM_PROTO_WT_LEN) {
				SteamMsgPersonaKV kv;
				memset(&kv, 0, sizeof(kv));
				if (!persona_kv_decode(&kv, &r)) {
					persona_kv_clear(&kv);
					return FALSE;
				}
				g_array_append_val(f->rich_presence, kv);
			}
			break;
		case 72: R_FIXED64(&r, f, broadcast_id); break;
		case 73: R_FIXED64(&r, f, game_lobby_id); break;
		case 74: R_U32(&r, f, watching_broadcast_accountid); break;
		case 75: R_U32(&r, f, watching_broadcast_appid); break;
		case 76: R_U32(&r, f, watching_broadcast_viewers); break;
		case 77: rd_string(&r, &f->watching_broadcast_title); break;
		case 78: R_BOOL(&r, f, is_community_banned); break;
		case 79: R_BOOL(&r, f, player_name_pending_review); break;
		case 80: R_BOOL(&r, f, avatar_pending_review); break;
		case 81: R_BOOL(&r, f, on_steam_deck); break;
		case 83: R_U32(&r, f, gaming_device_type); break;
		default: break;
		}
	}
	return !r.error;
}

void
steam_msg_client_persona_state_encode(const SteamMsgClientPersonaState *m, GByteArray *o)
{
	guint i;

	W_U32(o, 1, m, status_flags);
	for (i = 0; i < m->friends->len; i++)
		W_SUB(o, 2, persona_friend_encode,
		      &g_array_index(m->friends, SteamMsgPersonaFriend, i));
}

gboolean
steam_msg_client_persona_state_decode(SteamMsgClientPersonaState *m, const guint8 *data, gsize len)
{
	steam_msg_client_persona_state_clear(m);
	steam_msg_client_persona_state_init(m);
	{
	DECODE_BEGIN(r, data, len)
		case 1: R_U32(&r, m, status_flags); break;
		case 2:
			if (r.wt == STEAM_PROTO_WT_LEN) {
				/* append first so the array owns it even on failure */
				SteamMsgPersonaFriend *f = steam_msg_client_persona_state_add_friend(m);
				if (!persona_friend_decode(f, &r))
					return FALSE;
			}
			break;
	DECODE_END(r)
	}
}

/* ======================================================================
 * CMsgClientChangeStatus
 * ====================================================================== */

void
steam_msg_client_change_status_init(SteamMsgClientChangeStatus *m)
{
	memset(m, 0, sizeof(*m));
}

void
steam_msg_client_change_status_clear(SteamMsgClientChangeStatus *m)
{
	FREE_STR(m->player_name);
}

void
steam_msg_client_change_status_encode(const SteamMsgClientChangeStatus *m, GByteArray *o)
{
	W_U32(o, 1, m, persona_state);
	W_STR(o, 2, m, player_name);
	W_BOOL(o, 3, m, is_auto_generated_name);
	W_BOOL(o, 4, m, high_priority);
	W_BOOL(o, 5, m, persona_set_by_user);
	W_U32(o, 6, m, persona_state_flags);
	W_BOOL(o, 7, m, need_persona_response);
	W_BOOL(o, 8, m, is_client_idle);
}

gboolean
steam_msg_client_change_status_decode(SteamMsgClientChangeStatus *m, const guint8 *data, gsize len)
{
	steam_msg_client_change_status_clear(m);
	steam_msg_client_change_status_init(m);
	{
	DECODE_BEGIN(r, data, len)
		case 1: R_U32(&r, m, persona_state); break;
		case 2: rd_string(&r, &m->player_name); break;
		case 3: R_BOOL(&r, m, is_auto_generated_name); break;
		case 4: R_BOOL(&r, m, high_priority); break;
		case 5: R_BOOL(&r, m, persona_set_by_user); break;
		case 6: R_U32(&r, m, persona_state_flags); break;
		case 7: R_BOOL(&r, m, need_persona_response); break;
		case 8: R_BOOL(&r, m, is_client_idle); break;
	DECODE_END(r)
	}
}

/* ======================================================================
 * CMsgClientAddFriend / Response / RemoveFriend
 * ====================================================================== */

void
steam_msg_client_add_friend_init(SteamMsgClientAddFriend *m)
{
	memset(m, 0, sizeof(*m));
}

void
steam_msg_client_add_friend_clear(SteamMsgClientAddFriend *m)
{
	FREE_STR(m->accountname_or_email_to_add);
}

void
steam_msg_client_add_friend_encode(const SteamMsgClientAddFriend *m, GByteArray *o)
{
	W_FIXED64(o, 1, m, steamid_to_add);
	W_STR(o, 2, m, accountname_or_email_to_add);
}

gboolean
steam_msg_client_add_friend_decode(SteamMsgClientAddFriend *m, const guint8 *data, gsize len)
{
	steam_msg_client_add_friend_clear(m);
	steam_msg_client_add_friend_init(m);
	{
	DECODE_BEGIN(r, data, len)
		case 1: R_FIXED64(&r, m, steamid_to_add); break;
		case 2: rd_string(&r, &m->accountname_or_email_to_add); break;
	DECODE_END(r)
	}
}

void
steam_msg_client_add_friend_response_init(SteamMsgClientAddFriendResponse *m)
{
	memset(m, 0, sizeof(*m));
	m->eresult = STEAM_ERESULT_FAIL;
}

void
steam_msg_client_add_friend_response_clear(SteamMsgClientAddFriendResponse *m)
{
	FREE_STR(m->persona_name_added);
}

void
steam_msg_client_add_friend_response_encode(const SteamMsgClientAddFriendResponse *m, GByteArray *o)
{
	W_I32(o, 1, m, eresult);
	W_FIXED64(o, 2, m, steam_id_added);
	W_STR(o, 3, m, persona_name_added);
}

gboolean
steam_msg_client_add_friend_response_decode(SteamMsgClientAddFriendResponse *m, const guint8 *data, gsize len)
{
	steam_msg_client_add_friend_response_clear(m);
	steam_msg_client_add_friend_response_init(m);
	{
	DECODE_BEGIN(r, data, len)
		case 1: R_I32(&r, m, eresult); break;
		case 2: R_FIXED64(&r, m, steam_id_added); break;
		case 3: rd_string(&r, &m->persona_name_added); break;
	DECODE_END(r)
	}
}

void
steam_msg_client_remove_friend_init(SteamMsgClientRemoveFriend *m)
{
	memset(m, 0, sizeof(*m));
}

void
steam_msg_client_remove_friend_clear(SteamMsgClientRemoveFriend *m)
{
}

void
steam_msg_client_remove_friend_encode(const SteamMsgClientRemoveFriend *m, GByteArray *o)
{
	W_FIXED64(o, 1, m, friendid);
}

gboolean
steam_msg_client_remove_friend_decode(SteamMsgClientRemoveFriend *m, const guint8 *data, gsize len)
{
	steam_msg_client_remove_friend_init(m);
	{
	DECODE_BEGIN(r, data, len)
		case 1: R_FIXED64(&r, m, friendid); break;
	DECODE_END(r)
	}
}

/* ======================================================================
 * CMsgClientPlayerNicknameList
 * ====================================================================== */

static void
client_nickname_clear(gpointer p)
{
	SteamMsgClientPlayerNickname *n = p;

	FREE_STR(n->nickname);
}

void
steam_msg_client_player_nickname_list_init(SteamMsgClientPlayerNicknameList *m)
{
	memset(m, 0, sizeof(*m));
	m->nicknames = g_array_new(FALSE, TRUE, sizeof(SteamMsgClientPlayerNickname));
	g_array_set_clear_func(m->nicknames, client_nickname_clear);
}

void
steam_msg_client_player_nickname_list_clear(SteamMsgClientPlayerNicknameList *m)
{
	FREE_ARRAY(m->nicknames);
}

static void
client_nickname_encode(const SteamMsgClientPlayerNickname *n, GByteArray *o)
{
	W_FIXED64(o, 1, n, steamid);
	W_STR(o, 3, n, nickname);
}

void
steam_msg_client_player_nickname_list_encode(const SteamMsgClientPlayerNicknameList *m, GByteArray *o)
{
	guint i;

	W_BOOL(o, 1, m, removal);
	W_BOOL(o, 2, m, incremental);
	for (i = 0; i < m->nicknames->len; i++)
		W_SUB(o, 3, client_nickname_encode,
		      &g_array_index(m->nicknames, SteamMsgClientPlayerNickname, i));
}

gboolean
steam_msg_client_player_nickname_list_decode(SteamMsgClientPlayerNicknameList *m, const guint8 *data, gsize len)
{
	steam_msg_client_player_nickname_list_clear(m);
	steam_msg_client_player_nickname_list_init(m);
	{
	DECODE_BEGIN(r, data, len)
		case 1: R_BOOL(&r, m, removal); break;
		case 2: R_BOOL(&r, m, incremental); break;
		case 3:
			if (r.wt == STEAM_PROTO_WT_LEN) {
				SteamMsgClientPlayerNickname n;
				SteamProtoReader s;
				memset(&n, 0, sizeof(n));
				steam_proto_sub_reader(&r, &s);
				while (steam_proto_next(&s)) {
					switch (s.field) {
					case 1: R_FIXED64(&s, &n, steamid); break;
					case 3: rd_string(&s, &n.nickname); break;
					default: break;
					}
				}
				g_array_append_val(m->nicknames, n);
				if (s.error)
					return FALSE;
			}
			break;
	DECODE_END(r)
	}
}

/* ======================================================================
 * CFriendMessages_SendMessage_Request / Response
 * ====================================================================== */

void
steam_msg_friend_messages_send_message_request_init(SteamMsgFriendMessagesSendMessageRequest *m)
{
	memset(m, 0, sizeof(*m));
}

void
steam_msg_friend_messages_send_message_request_clear(SteamMsgFriendMessagesSendMessageRequest *m)
{
	FREE_STR(m->message);
	FREE_STR(m->client_message_id);
}

void
steam_msg_friend_messages_send_message_request_encode(const SteamMsgFriendMessagesSendMessageRequest *m, GByteArray *o)
{
	W_FIXED64(o, 1, m, steamid);
	W_I32(o, 2, m, chat_entry_type);
	W_STR(o, 3, m, message);
	W_BOOL(o, 4, m, contains_bbcode);
	W_BOOL(o, 5, m, echo_to_sender);
	W_BOOL(o, 6, m, low_priority);
	W_STR(o, 8, m, client_message_id);
}

gboolean
steam_msg_friend_messages_send_message_request_decode(SteamMsgFriendMessagesSendMessageRequest *m, const guint8 *data, gsize len)
{
	steam_msg_friend_messages_send_message_request_clear(m);
	steam_msg_friend_messages_send_message_request_init(m);
	{
	DECODE_BEGIN(r, data, len)
		case 1: R_FIXED64(&r, m, steamid); break;
		case 2: R_I32(&r, m, chat_entry_type); break;
		case 3: rd_string(&r, &m->message); break;
		case 4: R_BOOL(&r, m, contains_bbcode); break;
		case 5: R_BOOL(&r, m, echo_to_sender); break;
		case 6: R_BOOL(&r, m, low_priority); break;
		case 8: rd_string(&r, &m->client_message_id); break;
	DECODE_END(r)
	}
}

void
steam_msg_friend_messages_send_message_response_init(SteamMsgFriendMessagesSendMessageResponse *m)
{
	memset(m, 0, sizeof(*m));
}

void
steam_msg_friend_messages_send_message_response_clear(SteamMsgFriendMessagesSendMessageResponse *m)
{
	FREE_STR(m->modified_message);
	FREE_STR(m->message_without_bb_code);
}

void
steam_msg_friend_messages_send_message_response_encode(const SteamMsgFriendMessagesSendMessageResponse *m, GByteArray *o)
{
	W_STR(o, 1, m, modified_message);
	W_U32(o, 2, m, server_timestamp);
	W_U32(o, 3, m, ordinal);
	W_STR(o, 4, m, message_without_bb_code);
}

gboolean
steam_msg_friend_messages_send_message_response_decode(SteamMsgFriendMessagesSendMessageResponse *m, const guint8 *data, gsize len)
{
	steam_msg_friend_messages_send_message_response_clear(m);
	steam_msg_friend_messages_send_message_response_init(m);
	{
	DECODE_BEGIN(r, data, len)
		case 1: rd_string(&r, &m->modified_message); break;
		case 2: R_U32(&r, m, server_timestamp); break;
		case 3: R_U32(&r, m, ordinal); break;
		case 4: rd_string(&r, &m->message_without_bb_code); break;
	DECODE_END(r)
	}
}

/* ======================================================================
 * CFriendMessages_IncomingMessage_Notification
 * ====================================================================== */

void
steam_msg_friend_messages_incoming_message_init(SteamMsgFriendMessagesIncomingMessage *m)
{
	memset(m, 0, sizeof(*m));
}

void
steam_msg_friend_messages_incoming_message_clear(SteamMsgFriendMessagesIncomingMessage *m)
{
	FREE_STR(m->message);
	FREE_STR(m->message_no_bbcode);
}

void
steam_msg_friend_messages_incoming_message_encode(const SteamMsgFriendMessagesIncomingMessage *m, GByteArray *o)
{
	W_FIXED64(o, 1, m, steamid_friend);
	W_I32(o, 2, m, chat_entry_type);
	W_BOOL(o, 3, m, from_limited_account);
	W_STR(o, 4, m, message);
	W_FIXED32(o, 5, m, rtime32_server_timestamp);
	W_U32(o, 6, m, ordinal);
	W_BOOL(o, 7, m, local_echo);
	W_STR(o, 8, m, message_no_bbcode);
	W_BOOL(o, 9, m, low_priority);
}

gboolean
steam_msg_friend_messages_incoming_message_decode(SteamMsgFriendMessagesIncomingMessage *m, const guint8 *data, gsize len)
{
	steam_msg_friend_messages_incoming_message_clear(m);
	steam_msg_friend_messages_incoming_message_init(m);
	{
	DECODE_BEGIN(r, data, len)
		case 1: R_FIXED64(&r, m, steamid_friend); break;
		case 2: R_I32(&r, m, chat_entry_type); break;
		case 3: R_BOOL(&r, m, from_limited_account); break;
		case 4: rd_string(&r, &m->message); break;
		case 5: R_FIXED32(&r, m, rtime32_server_timestamp); break;
		case 6: R_U32(&r, m, ordinal); break;
		case 7: R_BOOL(&r, m, local_echo); break;
		case 8: rd_string(&r, &m->message_no_bbcode); break;
		case 9: R_BOOL(&r, m, low_priority); break;
	DECODE_END(r)
	}
}

/* ======================================================================
 * CFriendMessages_GetRecentMessages_Request / Response
 * ====================================================================== */

void
steam_msg_friend_messages_get_recent_messages_request_init(SteamMsgFriendMessagesGetRecentMessagesRequest *m)
{
	memset(m, 0, sizeof(*m));
}

void
steam_msg_friend_messages_get_recent_messages_request_clear(SteamMsgFriendMessagesGetRecentMessagesRequest *m)
{
}

void
steam_msg_friend_messages_get_recent_messages_request_encode(const SteamMsgFriendMessagesGetRecentMessagesRequest *m, GByteArray *o)
{
	W_FIXED64(o, 1, m, steamid1);
	W_FIXED64(o, 2, m, steamid2);
	W_U32(o, 3, m, count);
	W_BOOL(o, 4, m, most_recent_conversation);
	W_FIXED32(o, 5, m, rtime32_start_time);
	W_BOOL(o, 6, m, bbcode_format);
	W_U32(o, 7, m, start_ordinal);
	W_U32(o, 8, m, time_last);
	W_U32(o, 9, m, ordinal_last);
}

gboolean
steam_msg_friend_messages_get_recent_messages_request_decode(SteamMsgFriendMessagesGetRecentMessagesRequest *m, const guint8 *data, gsize len)
{
	steam_msg_friend_messages_get_recent_messages_request_init(m);
	{
	DECODE_BEGIN(r, data, len)
		case 1: R_FIXED64(&r, m, steamid1); break;
		case 2: R_FIXED64(&r, m, steamid2); break;
		case 3: R_U32(&r, m, count); break;
		case 4: R_BOOL(&r, m, most_recent_conversation); break;
		case 5: R_FIXED32(&r, m, rtime32_start_time); break;
		case 6: R_BOOL(&r, m, bbcode_format); break;
		case 7: R_U32(&r, m, start_ordinal); break;
		case 8: R_U32(&r, m, time_last); break;
		case 9: R_U32(&r, m, ordinal_last); break;
	DECODE_END(r)
	}
}

static void
friend_message_reaction_clear(gpointer p)
{
	SteamMsgFriendMessageReaction *mr = p;

	FREE_STR(mr->reaction);
	FREE_ARRAY(mr->reactors);
}

static void
friend_message_clear(gpointer p)
{
	SteamMsgFriendMessage *fm = p;

	FREE_STR(fm->message);
	FREE_ARRAY(fm->reactions);
}

SteamMsgFriendMessageReaction *
steam_msg_friend_message_add_reaction(SteamMsgFriendMessage *fm)
{
	SteamMsgFriendMessageReaction mr;

	if (fm->reactions == NULL) {
		fm->reactions = g_array_new(FALSE, TRUE, sizeof(SteamMsgFriendMessageReaction));
		g_array_set_clear_func(fm->reactions, friend_message_reaction_clear);
	}
	memset(&mr, 0, sizeof(mr));
	mr.reactors = g_array_new(FALSE, TRUE, sizeof(guint32));
	g_array_append_val(fm->reactions, mr);
	return &g_array_index(fm->reactions, SteamMsgFriendMessageReaction, fm->reactions->len - 1);
}

static void
friend_message_reaction_encode(const SteamMsgFriendMessageReaction *mr, GByteArray *o)
{
	guint i;

	W_I32(o, 1, mr, reaction_type);
	W_STR(o, 2, mr, reaction);
	/* proto2 repeated without [packed=true]: one tag per element */
	for (i = 0; mr->reactors && i < mr->reactors->len; i++)
		steam_proto_put_varint(o, 3, g_array_index(mr->reactors, guint32, i));
}

/* Appends varint uint32 values of field `r` (either encoding) to `out` */
static gboolean
rd_repeated_u32(const SteamProtoReader *r, GArray *out)
{
	if (r->wt == STEAM_PROTO_WT_VARINT) {
		guint32 v = (guint32) r->varint;

		g_array_append_val(out, v);
	} else if (r->wt == STEAM_PROTO_WT_LEN) {
		/* packed encoding: parsers must accept both */
		gsize pos = 0;

		while (pos < r->bytes_len) {
			guint64 v = 0;
			guint shift = 0;
			guint32 v32;

			do {
				if (pos >= r->bytes_len || shift > 63)
					return FALSE;
				v |= (guint64) (r->bytes[pos] & 0x7f) << shift;
				shift += 7;
			} while (r->bytes[pos++] & 0x80);
			v32 = (guint32) v;
			g_array_append_val(out, v32);
		}
	}
	return TRUE;
}

static gboolean
friend_message_reaction_decode(SteamMsgFriendMessage *fm, const SteamProtoReader *r)
{
	SteamMsgFriendMessageReaction *mr;
	SteamProtoReader s;

	if (r->wt != STEAM_PROTO_WT_LEN)
		return TRUE;
	mr = steam_msg_friend_message_add_reaction(fm);
	steam_proto_sub_reader(r, &s);
	while (steam_proto_next(&s)) {
		switch (s.field) {
		case 1: R_I32(&s, mr, reaction_type); break;
		case 2: rd_string(&s, &mr->reaction); break;
		case 3:
			if (!rd_repeated_u32(&s, mr->reactors))
				return FALSE;
			break;
		default: break;
		}
	}
	return !s.error;
}

static void
friend_message_encode(const SteamMsgFriendMessage *fm, GByteArray *o)
{
	guint i;

	W_U32(o, 1, fm, accountid);
	W_U32(o, 2, fm, timestamp);
	W_STR(o, 3, fm, message);
	W_U32(o, 4, fm, ordinal);
	for (i = 0; fm->reactions && i < fm->reactions->len; i++)
		W_SUB(o, 5, friend_message_reaction_encode,
		      &g_array_index(fm->reactions, SteamMsgFriendMessageReaction, i));
}

void
steam_msg_friend_messages_get_recent_messages_response_init(SteamMsgFriendMessagesGetRecentMessagesResponse *m)
{
	memset(m, 0, sizeof(*m));
	m->messages = g_array_new(FALSE, TRUE, sizeof(SteamMsgFriendMessage));
	g_array_set_clear_func(m->messages, friend_message_clear);
}

void
steam_msg_friend_messages_get_recent_messages_response_clear(SteamMsgFriendMessagesGetRecentMessagesResponse *m)
{
	FREE_ARRAY(m->messages);
}

void
steam_msg_friend_messages_get_recent_messages_response_encode(const SteamMsgFriendMessagesGetRecentMessagesResponse *m, GByteArray *o)
{
	guint i;

	for (i = 0; i < m->messages->len; i++)
		W_SUB(o, 1, friend_message_encode,
		      &g_array_index(m->messages, SteamMsgFriendMessage, i));
	W_BOOL(o, 4, m, more_available);
}

gboolean
steam_msg_friend_messages_get_recent_messages_response_decode(SteamMsgFriendMessagesGetRecentMessagesResponse *m, const guint8 *data, gsize len)
{
	steam_msg_friend_messages_get_recent_messages_response_clear(m);
	steam_msg_friend_messages_get_recent_messages_response_init(m);
	{
	DECODE_BEGIN(r, data, len)
		case 1:
			if (r.wt == STEAM_PROTO_WT_LEN) {
				SteamMsgFriendMessage fm;
				SteamProtoReader s;
				memset(&fm, 0, sizeof(fm));
				steam_proto_sub_reader(&r, &s);
				while (steam_proto_next(&s)) {
					switch (s.field) {
					case 1: R_U32(&s, &fm, accountid); break;
					case 2: R_U32(&s, &fm, timestamp); break;
					case 3: rd_string(&s, &fm.message); break;
					case 4: R_U32(&s, &fm, ordinal); break;
					case 5:
						if (!friend_message_reaction_decode(&fm, &s)) {
							g_array_append_val(m->messages, fm);
							return FALSE;
						}
						break;
					default: break;
					}
				}
				g_array_append_val(m->messages, fm);
				if (s.error)
					return FALSE;
			}
			break;
		case 4: R_BOOL(&r, m, more_available); break;
	DECODE_END(r)
	}
}

/* ======================================================================
 * CFriendsMessages_GetActiveMessageSessions_Request / Response
 * ====================================================================== */

void
steam_msg_friend_messages_get_active_message_sessions_request_init(SteamMsgFriendMessagesGetActiveMessageSessionsRequest *m)
{
	memset(m, 0, sizeof(*m));
}

void
steam_msg_friend_messages_get_active_message_sessions_request_clear(SteamMsgFriendMessagesGetActiveMessageSessionsRequest *m)
{
}

void
steam_msg_friend_messages_get_active_message_sessions_request_encode(const SteamMsgFriendMessagesGetActiveMessageSessionsRequest *m, GByteArray *o)
{
	W_U32(o, 1, m, lastmessage_since);
	W_BOOL(o, 2, m, only_sessions_with_messages);
}

gboolean
steam_msg_friend_messages_get_active_message_sessions_request_decode(SteamMsgFriendMessagesGetActiveMessageSessionsRequest *m, const guint8 *data, gsize len)
{
	steam_msg_friend_messages_get_active_message_sessions_request_init(m);
	{
	DECODE_BEGIN(r, data, len)
		case 1: R_U32(&r, m, lastmessage_since); break;
		case 2: R_BOOL(&r, m, only_sessions_with_messages); break;
	DECODE_END(r)
	}
}

static void
message_session_encode(const SteamMsgFriendMessageSession *s, GByteArray *o)
{
	W_U32(o, 1, s, accountid_friend);
	W_U32(o, 2, s, last_message);
	W_U32(o, 3, s, last_view);
	W_U32(o, 4, s, unread_message_count);
}

void
steam_msg_friend_messages_get_active_message_sessions_response_init(SteamMsgFriendMessagesGetActiveMessageSessionsResponse *m)
{
	memset(m, 0, sizeof(*m));
	m->message_sessions = g_array_new(FALSE, TRUE, sizeof(SteamMsgFriendMessageSession));
}

void
steam_msg_friend_messages_get_active_message_sessions_response_clear(SteamMsgFriendMessagesGetActiveMessageSessionsResponse *m)
{
	FREE_ARRAY(m->message_sessions);
}

void
steam_msg_friend_messages_get_active_message_sessions_response_encode(const SteamMsgFriendMessagesGetActiveMessageSessionsResponse *m, GByteArray *o)
{
	guint i;

	for (i = 0; i < m->message_sessions->len; i++)
		W_SUB(o, 1, message_session_encode,
		      &g_array_index(m->message_sessions, SteamMsgFriendMessageSession, i));
	W_U32(o, 2, m, timestamp);
}

gboolean
steam_msg_friend_messages_get_active_message_sessions_response_decode(SteamMsgFriendMessagesGetActiveMessageSessionsResponse *m, const guint8 *data, gsize len)
{
	steam_msg_friend_messages_get_active_message_sessions_response_clear(m);
	steam_msg_friend_messages_get_active_message_sessions_response_init(m);
	{
	DECODE_BEGIN(r, data, len)
		case 1:
			if (r.wt == STEAM_PROTO_WT_LEN) {
				SteamMsgFriendMessageSession ms;
				SteamProtoReader s;
				memset(&ms, 0, sizeof(ms));
				steam_proto_sub_reader(&r, &s);
				while (steam_proto_next(&s)) {
					switch (s.field) {
					case 1: R_U32(&s, &ms, accountid_friend); break;
					case 2: R_U32(&s, &ms, last_message); break;
					case 3: R_U32(&s, &ms, last_view); break;
					case 4: R_U32(&s, &ms, unread_message_count); break;
					default: break;
					}
				}
				if (s.error)
					return FALSE;
				g_array_append_val(m->message_sessions, ms);
			}
			break;
		case 2: R_U32(&r, m, timestamp); break;
	DECODE_END(r)
	}
}

/* ======================================================================
 * CFriendMessages_AckMessage_Notification
 * ====================================================================== */

void
steam_msg_friend_messages_ack_message_init(SteamMsgFriendMessagesAckMessage *m)
{
	memset(m, 0, sizeof(*m));
}

void
steam_msg_friend_messages_ack_message_clear(SteamMsgFriendMessagesAckMessage *m)
{
}

void
steam_msg_friend_messages_ack_message_encode(const SteamMsgFriendMessagesAckMessage *m, GByteArray *o)
{
	W_FIXED64(o, 1, m, steamid_partner);
	W_U32(o, 2, m, timestamp);
}

gboolean
steam_msg_friend_messages_ack_message_decode(SteamMsgFriendMessagesAckMessage *m, const guint8 *data, gsize len)
{
	steam_msg_friend_messages_ack_message_init(m);
	{
	DECODE_BEGIN(r, data, len)
		case 1: R_FIXED64(&r, m, steamid_partner); break;
		case 2: R_U32(&r, m, timestamp); break;
	DECODE_END(r)
	}
}

/* ======================================================================
 * CFriendMessages_UpdateMessageReaction_Request / Response
 * ====================================================================== */

void
steam_msg_friend_messages_update_message_reaction_request_init(SteamMsgFriendMessagesUpdateMessageReactionRequest *m)
{
	memset(m, 0, sizeof(*m));
}

void
steam_msg_friend_messages_update_message_reaction_request_clear(SteamMsgFriendMessagesUpdateMessageReactionRequest *m)
{
	FREE_STR(m->reaction);
}

void
steam_msg_friend_messages_update_message_reaction_request_encode(const SteamMsgFriendMessagesUpdateMessageReactionRequest *m, GByteArray *o)
{
	W_FIXED64(o, 1, m, steamid);
	W_U32(o, 2, m, server_timestamp);
	W_U32(o, 3, m, ordinal);
	W_I32(o, 4, m, reaction_type);
	W_STR(o, 5, m, reaction);
	W_BOOL(o, 6, m, is_add);
}

gboolean
steam_msg_friend_messages_update_message_reaction_request_decode(SteamMsgFriendMessagesUpdateMessageReactionRequest *m, const guint8 *data, gsize len)
{
	steam_msg_friend_messages_update_message_reaction_request_clear(m);
	steam_msg_friend_messages_update_message_reaction_request_init(m);
	{
	DECODE_BEGIN(r, data, len)
		case 1: R_FIXED64(&r, m, steamid); break;
		case 2: R_U32(&r, m, server_timestamp); break;
		case 3: R_U32(&r, m, ordinal); break;
		case 4: R_I32(&r, m, reaction_type); break;
		case 5: rd_string(&r, &m->reaction); break;
		case 6: R_BOOL(&r, m, is_add); break;
	DECODE_END(r)
	}
}

void
steam_msg_friend_messages_update_message_reaction_response_init(SteamMsgFriendMessagesUpdateMessageReactionResponse *m)
{
	memset(m, 0, sizeof(*m));
	m->reactors = g_array_new(FALSE, TRUE, sizeof(guint32));
}

void
steam_msg_friend_messages_update_message_reaction_response_clear(SteamMsgFriendMessagesUpdateMessageReactionResponse *m)
{
	FREE_ARRAY(m->reactors);
}

void
steam_msg_friend_messages_update_message_reaction_response_encode(const SteamMsgFriendMessagesUpdateMessageReactionResponse *m, GByteArray *o)
{
	guint i;

	for (i = 0; i < m->reactors->len; i++)
		steam_proto_put_varint(o, 1, g_array_index(m->reactors, guint32, i));
}

gboolean
steam_msg_friend_messages_update_message_reaction_response_decode(SteamMsgFriendMessagesUpdateMessageReactionResponse *m, const guint8 *data, gsize len)
{
	steam_msg_friend_messages_update_message_reaction_response_clear(m);
	steam_msg_friend_messages_update_message_reaction_response_init(m);
	{
	DECODE_BEGIN(r, data, len)
		case 1:
			if (!rd_repeated_u32(&r, m->reactors))
				return FALSE;
			break;
	DECODE_END(r)
	}
}

/* ======================================================================
 * CFriendMessages_MessageReaction_Notification
 * ====================================================================== */

void
steam_msg_friend_messages_message_reaction_init(SteamMsgFriendMessagesMessageReaction *m)
{
	memset(m, 0, sizeof(*m));
}

void
steam_msg_friend_messages_message_reaction_clear(SteamMsgFriendMessagesMessageReaction *m)
{
	FREE_STR(m->reaction);
}

void
steam_msg_friend_messages_message_reaction_encode(const SteamMsgFriendMessagesMessageReaction *m, GByteArray *o)
{
	W_FIXED64(o, 1, m, steamid_friend);
	W_U32(o, 2, m, server_timestamp);
	W_U32(o, 3, m, ordinal);
	W_FIXED64(o, 4, m, reactor);
	W_I32(o, 5, m, reaction_type);
	W_STR(o, 6, m, reaction);
	W_BOOL(o, 7, m, is_add);
}

gboolean
steam_msg_friend_messages_message_reaction_decode(SteamMsgFriendMessagesMessageReaction *m, const guint8 *data, gsize len)
{
	steam_msg_friend_messages_message_reaction_clear(m);
	steam_msg_friend_messages_message_reaction_init(m);
	{
	DECODE_BEGIN(r, data, len)
		case 1: R_FIXED64(&r, m, steamid_friend); break;
		case 2: R_U32(&r, m, server_timestamp); break;
		case 3: R_U32(&r, m, ordinal); break;
		case 4: R_FIXED64(&r, m, reactor); break;
		case 5: R_I32(&r, m, reaction_type); break;
		case 6: rd_string(&r, &m->reaction); break;
		case 7: R_BOOL(&r, m, is_add); break;
	DECODE_END(r)
	}
}

/* ======================================================================
 * CPlayer_GetNicknameList_Request / Response
 * ====================================================================== */

void
steam_msg_player_get_nickname_list_request_init(SteamMsgPlayerGetNicknameListRequest *m)
{
	memset(m, 0, sizeof(*m));
}

void
steam_msg_player_get_nickname_list_request_clear(SteamMsgPlayerGetNicknameListRequest *m)
{
}

void
steam_msg_player_get_nickname_list_request_encode(const SteamMsgPlayerGetNicknameListRequest *m, GByteArray *o)
{
}

gboolean
steam_msg_player_get_nickname_list_request_decode(SteamMsgPlayerGetNicknameListRequest *m, const guint8 *data, gsize len)
{
	steam_msg_player_get_nickname_list_request_init(m);
	{
	DECODE_BEGIN(r, data, len)
	DECODE_END(r)
	}
}

static void
player_nickname_clear(gpointer p)
{
	SteamMsgPlayerNickname *n = p;

	FREE_STR(n->nickname);
}

static void
player_nickname_encode(const SteamMsgPlayerNickname *n, GByteArray *o)
{
	W_FIXED32(o, 1, n, accountid);
	W_STR(o, 2, n, nickname);
}

void
steam_msg_player_get_nickname_list_response_init(SteamMsgPlayerGetNicknameListResponse *m)
{
	memset(m, 0, sizeof(*m));
	m->nicknames = g_array_new(FALSE, TRUE, sizeof(SteamMsgPlayerNickname));
	g_array_set_clear_func(m->nicknames, player_nickname_clear);
}

void
steam_msg_player_get_nickname_list_response_clear(SteamMsgPlayerGetNicknameListResponse *m)
{
	FREE_ARRAY(m->nicknames);
}

void
steam_msg_player_get_nickname_list_response_encode(const SteamMsgPlayerGetNicknameListResponse *m, GByteArray *o)
{
	guint i;

	for (i = 0; i < m->nicknames->len; i++)
		W_SUB(o, 1, player_nickname_encode,
		      &g_array_index(m->nicknames, SteamMsgPlayerNickname, i));
}

gboolean
steam_msg_player_get_nickname_list_response_decode(SteamMsgPlayerGetNicknameListResponse *m, const guint8 *data, gsize len)
{
	steam_msg_player_get_nickname_list_response_clear(m);
	steam_msg_player_get_nickname_list_response_init(m);
	{
	DECODE_BEGIN(r, data, len)
		case 1:
			if (r.wt == STEAM_PROTO_WT_LEN) {
				SteamMsgPlayerNickname n;
				SteamProtoReader s;
				memset(&n, 0, sizeof(n));
				steam_proto_sub_reader(&r, &s);
				while (steam_proto_next(&s)) {
					switch (s.field) {
					case 1: R_FIXED32(&s, &n, accountid); break;
					case 2: rd_string(&s, &n.nickname); break;
					default: break;
					}
				}
				g_array_append_val(m->nicknames, n);
				if (s.error)
					return FALSE;
			}
			break;
	DECODE_END(r)
	}
}

/* ======================================================================
 * Names
 * ====================================================================== */

const char *
steam_emsg_to_string(guint32 emsg)
{
	switch (emsg) {
	case STEAM_EMSG_MULTI: return "Multi";
	case STEAM_EMSG_DEST_JOB_FAILED: return "DestJobFailed";
	case STEAM_EMSG_SERVICE_METHOD: return "ServiceMethod";
	case STEAM_EMSG_SERVICE_METHOD_RESPONSE: return "ServiceMethodResponse";
	case STEAM_EMSG_SERVICE_METHOD_CALL_FROM_CLIENT: return "ServiceMethodCallFromClient";
	case STEAM_EMSG_CLIENT_HEART_BEAT: return "ClientHeartBeat";
	case STEAM_EMSG_CLIENT_LOG_OFF: return "ClientLogOff";
	case STEAM_EMSG_CLIENT_REMOVE_FRIEND: return "ClientRemoveFriend";
	case STEAM_EMSG_CLIENT_CHANGE_STATUS: return "ClientChangeStatus";
	case STEAM_EMSG_CLIENT_GAMES_PLAYED: return "ClientGamesPlayed";
	case STEAM_EMSG_CLIENT_LOG_ON_RESPONSE: return "ClientLogOnResponse";
	case STEAM_EMSG_CLIENT_LOGGED_OFF: return "ClientLoggedOff";
	case STEAM_EMSG_CLIENT_PERSONA_STATE: return "ClientPersonaState";
	case STEAM_EMSG_CLIENT_FRIENDS_LIST: return "ClientFriendsList";
	case STEAM_EMSG_CLIENT_ACCOUNT_INFO: return "ClientAccountInfo";
	case STEAM_EMSG_CLIENT_GAME_CONNECT_TOKENS: return "ClientGameConnectTokens";
	case STEAM_EMSG_CLIENT_LICENSE_LIST: return "ClientLicenseList";
	case STEAM_EMSG_CLIENT_VAC_BAN_STATUS: return "ClientVACBanStatus";
	case STEAM_EMSG_CLIENT_CM_LIST: return "ClientCMList";
	case STEAM_EMSG_CLIENT_ADD_FRIEND: return "ClientAddFriend";
	case STEAM_EMSG_CLIENT_ADD_FRIEND_RESPONSE: return "ClientAddFriendResponse";
	case STEAM_EMSG_CLIENT_UPDATE_GUEST_PASSES_LIST: return "ClientUpdateGuestPassesList";
	case STEAM_EMSG_CLIENT_REQUEST_FRIEND_DATA: return "ClientRequestFriendData";
	case STEAM_EMSG_CLIENT_CLAN_STATE: return "ClientClanState";
	case STEAM_EMSG_CLIENT_SESSION_TOKEN: return "ClientSessionToken";
	case STEAM_EMSG_CLIENT_SERVER_LIST: return "ClientServerList";
	case STEAM_EMSG_CLIENT_IS_LIMITED_ACCOUNT: return "ClientIsLimitedAccount";
	case STEAM_EMSG_CLIENT_EMAIL_ADDR_INFO: return "ClientEmailAddrInfo";
	case STEAM_EMSG_CLIENT_SERVER_UNAVAILABLE: return "ClientServerUnavailable";
	case STEAM_EMSG_CLIENT_SERVERS_AVAILABLE: return "ClientServersAvailable";
	case STEAM_EMSG_CLIENT_LOGON: return "ClientLogon";
	case STEAM_EMSG_CLIENT_WALLET_INFO_UPDATE: return "ClientWalletInfoUpdate";
	case STEAM_EMSG_CLIENT_FRIENDS_GROUPS_LIST: return "ClientFriendsGroupsList";
	case STEAM_EMSG_CLIENT_PLAYER_NICKNAME_LIST: return "ClientPlayerNicknameList";
	case STEAM_EMSG_CLIENT_USER_NOTIFICATIONS: return "ClientUserNotifications";
	case STEAM_EMSG_CLIENT_RICH_PRESENCE_INFO: return "ClientRichPresenceInfo";
	case STEAM_EMSG_CLIENT_CHAT_OFFLINE_MESSAGE_NOTIFICATION: return "ClientChatOfflineMessageNotification";
	case STEAM_EMSG_CLIENT_PICS_CHANGES_SINCE_RESPONSE: return "ClientPICSChangesSinceResponse";
	case STEAM_EMSG_CLIENT_PICS_PRODUCT_INFO_RESPONSE: return "ClientPICSProductInfoResponse";
	case STEAM_EMSG_CLIENT_PICS_ACCESS_TOKEN_RESPONSE: return "ClientPICSAccessTokenResponse";
	case STEAM_EMSG_CLIENT_PLAYING_SESSION_STATE: return "ClientPlayingSessionState";
	case STEAM_EMSG_SERVICE_METHOD_CALL_FROM_CLIENT_NON_AUTHED: return "ServiceMethodCallFromClientNonAuthed";
	case STEAM_EMSG_CLIENT_HELLO: return "ClientHello";
	default: return NULL;
	}
}

/* EResult numbers follow SteamKit2 Resources/SteamLanguage/eresult.steamd. */
static const char * const eresult_strings[] = {
	/*   0 */ "Invalid result",
	/*   1 */ "Success",
	/*   2 */ "Generic failure",
	/*   3 */ "No connection to Steam",
	/*   4 */ NULL,
	/*   5 */ "Invalid password",
	/*   6 */ "Logged in elsewhere",
	/*   7 */ "Protocol version mismatch",
	/*   8 */ "Invalid parameter",
	/*   9 */ "File not found",
	/*  10 */ "Server busy",
	/*  11 */ "Invalid state",
	/*  12 */ "Invalid name",
	/*  13 */ "Invalid email address",
	/*  14 */ "Name already in use",
	/*  15 */ "Access denied",
	/*  16 */ "Operation timed out",
	/*  17 */ "Account banned",
	/*  18 */ "Account not found",
	/*  19 */ "Invalid SteamID",
	/*  20 */ "Steam service unavailable",
	/*  21 */ "Not logged on",
	/*  22 */ "Request pending",
	/*  23 */ "Encryption failure",
	/*  24 */ "Insufficient privilege",
	/*  25 */ "Limit exceeded",
	/*  26 */ "Access revoked",
	/*  27 */ "Expired",
	/*  28 */ "Already redeemed",
	/*  29 */ "Duplicate request",
	/*  30 */ "Already owned",
	/*  31 */ "IP address not found",
	/*  32 */ "Failed to save changes",
	/*  33 */ "Locking failed",
	/*  34 */ "Logon session replaced by another login",
	/*  35 */ "Connection failed",
	/*  36 */ "Handshake failed",
	/*  37 */ "I/O failure",
	/*  38 */ "Remote server disconnected",
	/*  39 */ "Shopping cart not found",
	/*  40 */ "Blocked",
	/*  41 */ "Ignored",
	/*  42 */ "No match",
	/*  43 */ "Account disabled",
	/*  44 */ "Service is read-only",
	/*  45 */ "Account not featured",
	/*  46 */ "Administrator OK",
	/*  47 */ "Content version mismatch",
	/*  48 */ "Try another server",
	/*  49 */ "Password required to kick session",
	/*  50 */ "Already logged in elsewhere",
	/*  51 */ "Suspended",
	/*  52 */ "Cancelled",
	/*  53 */ "Data corruption",
	/*  54 */ "Disk full",
	/*  55 */ "Remote call failed",
	/*  56 */ "Password not set",
	/*  57 */ "External account unlinked",
	/*  58 */ "PSN ticket invalid",
	/*  59 */ "External account already linked",
	/*  60 */ "Remote file conflict",
	/*  61 */ "Illegal password",
	/*  62 */ "Same as previous value",
	/*  63 */ "Logon denied: Steam Guard code required (check your email)",
	/*  64 */ "Cannot use old password",
	/*  65 */ "Invalid Steam Guard code",
	/*  66 */ "Logon denied: Steam Guard code required, no email sent",
	/*  67 */ "Hardware not capable of IPT",
	/*  68 */ "IPT initialisation error",
	/*  69 */ "Restricted by parental controls",
	/*  70 */ "Facebook query error",
	/*  71 */ "Steam Guard code expired",
	/*  72 */ "IP login restriction failed",
	/*  73 */ "Account locked down",
	/*  74 */ "Logon denied: verified email required",
	/*  75 */ "No matching URL",
	/*  76 */ "Bad response",
	/*  77 */ "Password re-entry required",
	/*  78 */ "Value out of range",
	/*  79 */ "Unexpected error",
	/*  80 */ "Disabled",
	/*  81 */ "Invalid CEG submission",
	/*  82 */ "Restricted device",
	/*  83 */ "Region locked",
	/*  84 */ "Rate limit exceeded, try again later",
	/*  85 */ "Logon denied: two-factor code required",
	/*  86 */ "Item deleted",
	/*  87 */ "Too many login attempts, try again later",
	/*  88 */ "Two-factor code mismatch",
	/*  89 */ "Two-factor activation code mismatch",
	/*  90 */ "Account associated with multiple partners",
	/*  91 */ "Not modified",
	/*  92 */ "No mobile device",
	/*  93 */ "Time not synced",
	/*  94 */ "SMS code failed",
	/*  95 */ "Account limit exceeded",
	/*  96 */ "Account activity limit exceeded",
	/*  97 */ "Phone activity limit exceeded",
	/*  98 */ "Refund to wallet",
	/*  99 */ "Email send failure",
	/* 100 */ "Not settled",
	/* 101 */ "Captcha required",
	/* 102 */ "Game server login token denied",
	/* 103 */ "Game server owner denied",
	/* 104 */ "Invalid item type",
	/* 105 */ "IP address banned",
	/* 106 */ "Game server login token expired",
	/* 107 */ "Insufficient funds",
	/* 108 */ "Too many pending",
	/* 109 */ "No site licenses found",
	/* 110 */ "Network send limit exceeded",
	/* 111 */ "Not friends",
	/* 112 */ "Limited user account",
	/* 113 */ "Cannot remove item",
	/* 114 */ "Account deleted",
	/* 115 */ "Existing user cancelled license",
	/* 116 */ "Community cooldown",
	/* 117 */ "No launcher specified",
	/* 118 */ "Must agree to the Steam Subscriber Agreement",
	/* 119 */ "Launcher migrated",
	/* 120 */ "Steam realm mismatch",
	/* 121 */ "Invalid signature",
	/* 122 */ "Parse failure",
	/* 123 */ "No verified phone number",
	/* 124 */ "Insufficient battery",
	/* 125 */ "Charger required",
	/* 126 */ "Cached credential invalid",
	/* 127 */ "Phone number is VOIP",
	/* 128 */ "Not supported",
	/* 129 */ "Family size limit exceeded",
	/* 130 */ "Offline app cache invalid",
	/* 131 */ "Try again later"
};

const char *
steam_eresult_to_string(int eresult)
{
	const char *s = NULL;
	gchar *tmp;

	if (eresult >= 0 && eresult < (int)G_N_ELEMENTS(eresult_strings))
		s = eresult_strings[eresult];
	if (s)
		return s;

	/* Interned so the returned pointer stays valid, like the static ones */
	tmp = g_strdup_printf("Unknown error (%d)", eresult);
	s = g_intern_string(tmp);
	g_free(tmp);
	return s;
}
