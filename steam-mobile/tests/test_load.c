/* Loads the built libsteam.so through libpurple's plugin loader and checks
 * the prpl registers. Catches plugin_load crashes and missing symbols. */
#include "purple_harness.h"
#include <stdio.h>
#include <stdlib.h>
#include "plugin.h"
#include "prpl.h"

int main(int argc, char **argv)
{
	PurplePlugin *plugin;
	PurplePluginProtocolInfo *prpl;
	GList *l;
	gchar *path;
	int fails = 0;

	purple_harness_init("loadtest");

	path = g_build_filename(g_get_current_dir(), "..", NULL);
	purple_plugins_add_search_path(path);
	purple_plugins_probe(G_MODULE_SUFFIX);
	plugin = purple_plugins_find_with_id(STEAM_PLUGIN_ID);
	if (!plugin) {
		printf("FAIL: plugin %s not found after probing %s\n", STEAM_PLUGIN_ID, path);
		return 1;
	}
	if (plugin->error) {
		printf("FAIL: probe error: %s\n", plugin->error);
		return 1;
	}
	if (!purple_plugin_load(plugin)) {
		printf("FAIL: purple_plugin_load: %s\n", plugin->error ? plugin->error : "?");
		return 1;
	}
	printf("loaded: %s %s (%s)\n", plugin->info->name, plugin->info->version, plugin->info->id);

	if (!purple_find_prpl(STEAM_PLUGIN_ID)) { printf("FAIL: prpl not registered\n"); fails++; }
	prpl = PURPLE_PLUGIN_PROTOCOL_INFO(plugin);
	if (!prpl->login || !prpl->close || !prpl->send_im || !prpl->status_types) { printf("FAIL: prpl ops missing\n"); fails++; }
	l = prpl->status_types(NULL);
	printf("status types: %d\n", g_list_length(l));
	if (g_list_length(l) < 5) fails++;
	for (l = prpl->protocol_options; l; l = l->next)
		printf("option: %s\n", purple_account_option_get_setting(l->data));

	if (!purple_plugin_unload(plugin)) { printf("FAIL: unload\n"); fails++; }
	printf("%s\n", fails ? "FAILED" : "PASSED");
	purple_harness_shutdown();
	return fails ? 1 : 0;
}
