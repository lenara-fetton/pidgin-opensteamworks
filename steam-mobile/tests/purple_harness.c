/*
 * Headless libpurple harness for standalone test programs. See purple_harness.h.
 * Modelled on libpurple's nullclient example.
 */

#include "purple_harness.h"

#include <stdio.h>
#include <stdlib.h>

#include "eventloop.h"
#include "debug.h"

static GMainLoop *main_loop = NULL;
static int exit_code = 0;

/* ---- GLib event loop ops (copied from nullclient) ---- */

#define PURPLE_GLIB_READ_COND  (G_IO_IN | G_IO_HUP | G_IO_ERR)
#define PURPLE_GLIB_WRITE_COND (G_IO_OUT | G_IO_HUP | G_IO_ERR | G_IO_NVAL)

typedef struct {
	PurpleInputFunction function;
	guint result;
	gpointer data;
} PurpleGLibIOClosure;

static void
purple_glib_io_destroy(gpointer data)
{
	g_free(data);
}

static gboolean
purple_glib_io_invoke(GIOChannel *source, GIOCondition condition, gpointer data)
{
	PurpleGLibIOClosure *closure = data;
	PurpleInputCondition purple_cond = 0;

	if (condition & PURPLE_GLIB_READ_COND)
		purple_cond |= PURPLE_INPUT_READ;
	if (condition & PURPLE_GLIB_WRITE_COND)
		purple_cond |= PURPLE_INPUT_WRITE;

	closure->function(closure->data, g_io_channel_unix_get_fd(source), purple_cond);

	return TRUE;
}

static guint
glib_input_add(gint fd, PurpleInputCondition condition, PurpleInputFunction function,
               gpointer data)
{
	PurpleGLibIOClosure *closure = g_new0(PurpleGLibIOClosure, 1);
	GIOChannel *channel;
	GIOCondition cond = 0;

	closure->function = function;
	closure->data = data;

	if (condition & PURPLE_INPUT_READ)
		cond |= PURPLE_GLIB_READ_COND;
	if (condition & PURPLE_INPUT_WRITE)
		cond |= PURPLE_GLIB_WRITE_COND;

	channel = g_io_channel_unix_new(fd);
	closure->result = g_io_add_watch_full(channel, G_PRIORITY_DEFAULT, cond,
	                                      purple_glib_io_invoke, closure,
	                                      purple_glib_io_destroy);

	g_io_channel_unref(channel);
	return closure->result;
}

static PurpleEventLoopUiOps glib_eventloops = {
	g_timeout_add,
	g_source_remove,
	glib_input_add,
	g_source_remove,
	NULL,
	g_timeout_add_seconds,
	NULL, NULL, NULL
};

/* ---- debug to stderr ---- */

static void
harness_debug_print(PurpleDebugLevel level, const char *category, const char *arg_s)
{
	const char *lvl;
	switch (level) {
	case PURPLE_DEBUG_ERROR:   lvl = "error"; break;
	case PURPLE_DEBUG_WARNING: lvl = "warn "; break;
	case PURPLE_DEBUG_FATAL:   lvl = "fatal"; break;
	default:                   lvl = "info "; break;
	}
	fprintf(stderr, "[%s] %s: %s", lvl, category ? category : "", arg_s);
}

static gboolean
harness_debug_enabled(PurpleDebugLevel level, const char *category)
{
	return getenv("STEAM_DEBUG") != NULL;
}

static PurpleDebugUiOps harness_debug_ops = {
	harness_debug_print,
	harness_debug_enabled,
	NULL, NULL, NULL, NULL
};

/* ---- public ---- */

SteamAccount *
purple_harness_init(const gchar *username)
{
	gchar *dir;
	PurpleAccount *account;
	PurpleConnection *pc;
	SteamAccount *sa;

	dir = g_build_filename(g_get_tmp_dir(), "steam-test-purple", NULL);
	purple_util_set_user_dir(dir);
	g_free(dir);

	purple_debug_set_enabled(getenv("STEAM_DEBUG") != NULL);
	purple_debug_set_ui_ops(&harness_debug_ops);
	purple_eventloop_set_ui_ops(&glib_eventloops);

	if (!purple_core_init("steam-test")) {
		fprintf(stderr, "libpurple initialization failed\n");
		exit(2);
	}

	purple_set_blist(purple_blist_new());

	account = purple_account_new(username, STEAM_PLUGIN_ID);

	/* A PurpleConnection without a loaded prpl: enough for
	 * purple_ssl_connect() (which only needs the account) and for code
	 * that stores its SteamAccount in pc->proto_data. */
	pc = g_new0(PurpleConnection, 1);
	pc->account = account;
	pc->state = PURPLE_CONNECTING;

	sa = g_new0(SteamAccount, 1);
	sa->account = account;
	sa->pc = pc;
	sa->cookie_table = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	sa->hostname_ip_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	sa->sent_messages_hash = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	sa->waiting_conns = g_queue_new();
	pc->proto_data = sa;

	main_loop = g_main_loop_new(NULL, FALSE);
	return sa;
}

void
purple_harness_run(void)
{
	g_main_loop_run(main_loop);
}

void
purple_harness_quit(int code)
{
	exit_code = code;
	if (main_loop && g_main_loop_is_running(main_loop))
		g_main_loop_quit(main_loop);
}

int
purple_harness_shutdown(void)
{
	purple_core_quit();
	return exit_code;
}
