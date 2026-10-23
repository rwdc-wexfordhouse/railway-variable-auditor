/*
 * railway-variable-audit: write every variable, with its value, to one CSV.
 *
 * Signs in through the Railway CLI, finds every workspace you can access,
 * reads every variable of every service and environment, and writes audit.csv.
 * It never writes to Railway and never prints a variable value.
 *
 * Copyright © 2026, Wexford House <https://wexfordhouse.co.za/>
 * Copyright © 2026, Ross Cameron <ross@wexfordhouse.co.za>
 * Licensed under the GNU General Public License, version 3.
 */
#include "rvcore.h"

#include <stdio.h>
#include <string.h>

enum { EXIT_OK = 0, EXIT_INCOMPLETE = 1, EXIT_CANNOT_START = 2 };

static void
show_progress(gpointer data, const gchar *message, gdouble fraction)
{
    (void)data;
    if (fraction >= 0)
        g_printerr("\r\033[K[%3d%%] %s", (int)(fraction * 100), message);
    else
        g_printerr("\r\033[K%s", message);
}

static void
usage(void)
{
    g_print("Usage: railway-variable-audit [--out FOLDER] [--policy FILE]\n\n"
            "Reads every variable in every Railway workspace you can access and writes\n"
            "audit.csv (with the current values) to a new run folder on your Desktop.\n"
            "Anything that could not be read is listed on the screen. Read only.\n"
            "Sign in first with: railway login\n\n"
            "audit.csv holds live secrets. It is readable by you only. Keep it safe and\n"
            "delete it when you have finished.\n\n"
            "  --out FOLDER    where to create the run folder (default: your Desktop)\n"
            "  --policy FILE   policy file (default: the user config folder)\n"
            "  --help          show this help\n\n"
            "Exit status: 0 complete, 1 some workspaces or projects were skipped or failed,\n"
            "2 the audit could not start.\n");
}

static void
list_problems(const RvInventory *inventory)
{
    g_print("\nThe audit is NOT complete. These were skipped or failed:\n");
    for (guint i = 0; i < inventory->report->len; i++) {
        const RvReportRow *r = inventory->report->pdata[i];

        if (g_strcmp0(r->status, "done") == 0)
            continue;
        g_print("  %s%s%s: %s (%s)\n", r->workspace, r->project != NULL && *r->project ? " / " : "",
                r->project != NULL ? r->project : "", r->status, r->detail);
    }
}

int
main(int argc, char **argv)
{
#ifdef G_OS_WIN32
    /* The C runtime hands over arguments in the console's code page. Ask Windows for UTF-8. */
    gchar **utf8_argv = g_win32_get_command_line();

    argv = utf8_argv;
    argc = (int)g_strv_length(utf8_argv);
#endif
    const gchar *out_base = NULL;
    gchar *default_base = NULL, *policy_path = NULL;
    GError *error = NULL;
    gchar *version, *name = NULL, *run_dir;
    RwClient *client;
    RvPolicy *policy;
    RvTopology *topology;
    RvInventory *inventory;
    RvScanOptions options;
    int status = EXIT_OK;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            usage();
            return EXIT_OK;
        } else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            out_base = argv[++i];
        } else if (strcmp(argv[i], "--policy") == 0 && i + 1 < argc) {
            policy_path = g_strdup(argv[++i]);
        } else {
            g_printerr("Unknown option: %s\n", argv[i]);
            usage();
            return EXIT_CANNOT_START;
        }
    }

    if (out_base == NULL) {
        default_base = rv_audit_default_folder();
        out_base = default_base;
    }

    version = rw_cli_version(&error);
    if (version == NULL) {
        g_printerr("%s\n", error->message);
        return EXIT_CANNOT_START;
    }
    if (!rw_cli_whoami(&name, NULL, &error)) {
        g_printerr("%s\n", error->message);
        return EXIT_CANNOT_START;
    }
    g_print("%s. Signed in as %s.\n", version, name);

    if (policy_path == NULL)
        policy_path = rv_policy_default_path();
    policy = rv_policy_load(policy_path, &error);
    if (policy == NULL) {
        g_printerr("%s\n", error->message);
        return EXIT_CANNOT_START;
    }

    client = rw_client_new(NULL);
    options = (RvScanOptions){ client, policy, show_progress, NULL, NULL };

    topology = rv_discover(&options, &error);
    if (topology == NULL) {
        g_printerr("\n%s\n", error->message);
        return EXIT_CANNOT_START;
    }
    g_print("\n%u workspaces, %u projects. Reading variables will need about %u requests.\n",
            topology->workspaces, topology->projects->len, rv_topology_estimate(topology));

    inventory = rv_read_variables(topology, &options, &error);
    rv_topology_free(topology);
    if (inventory == NULL) {
        g_printerr("\n%s\n", error->message);
        return EXIT_CANNOT_START;
    }
    g_print("\n");
    rv_classify(inventory, policy);

    run_dir = rv_audit_save(inventory, out_base, &error);
    if (run_dir == NULL) {
        g_printerr("%s\n", error->message);
        return EXIT_CANNOT_START;
    }

    g_print("Read %u variables in %u projects (%u environments, %u services).\n",
            inventory->variables->len, inventory->projects, inventory->environments,
            inventory->services);
    if (!inventory->complete) {
        list_problems(inventory);
        status = EXIT_INCOMPLETE;
    }
    g_print("\nWrote audit.csv to %s\n", run_dir);
    g_print("audit.csv holds live secrets. Only you can read it. Delete it when you have finished.\n");

    rv_inventory_free(inventory);
    rv_policy_free(policy);
    rw_client_free(client);
    g_free(run_dir);
    g_free(policy_path);
    g_free(default_base);
    g_free(name);
    g_free(version);
#ifdef G_OS_WIN32
    g_strfreev(utf8_argv);
#endif
    return status;
}
