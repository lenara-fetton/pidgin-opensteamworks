/*
 * Headless libpurple harness for standalone test programs.
 *
 * Initialises the libpurple core with a GLib main loop and no UI, so that
 * purple_ssl_connect(), purple_proxy_connect(), timeouts and input watchers
 * work from a plain command-line program.
 *
 * Usage:
 *   SteamAccount *sa = purple_harness_init("steam-test");   // once
 *   ... start async work that uses sa ...
 *   purple_harness_run();       // runs the main loop until purple_harness_quit()
 *   purple_harness_shutdown();
 */

#ifndef PURPLE_HARNESS_H
#define PURPLE_HARNESS_H

#include "libsteam.h"

/* Creates a throwaway PurpleAccount + PurpleConnection for the steam prpl
 * id and wraps them in a SteamAccount with all lists/tables allocated.
 * Debug output goes to stderr when the STEAM_DEBUG env var is set. */
SteamAccount *purple_harness_init(const gchar *username);

void purple_harness_run(void);
void purple_harness_quit(int exit_code);
/* Returns the exit code passed to purple_harness_quit(). */
int purple_harness_shutdown(void);

#endif /* PURPLE_HARNESS_H */
