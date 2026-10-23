/*
 * railway-variable-auditor-scan: a read-only inventory.
 *
 * Signs in through the Railway CLI, reads every variable in every workspace,
 * classifies them, and writes not-changed.csv and run-report.csv. It never
 * writes to Railway and never prints or saves a variable value.
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
    g_print("Usage: railway-variable-auditor-scan [--out FOLDER] [--policy FILE]\n\n"
            "Reads every variable in every Railway workspace you can access and writes\n"
            "not-changed.csv and run-report.csv to a new run folder. Read only. No value\n"
            "is printed or saved. Sign in first with: railway login\n\n"
            "  --out FOLDER    where to create the run folder (default: current folder)\n"
            "  --policy FILE   policy file (default: the user config folder)\n"
            "  --help          show this help\n\n"
            "Exit status: 0 complete, 1 some workspaces or projects were skipped or failed,\n"
            "2 the scan could not start.\n");
}

int
main(int argc, char **argv)
{
    const gchar *out_base = ".";
    gchar *policy_path = NULL;
    GError *error = NULL;
    gchar *version, *name = NULL, *run_dir, *path;
    RwClient *client;
    RvPolicy *policy;
    RvTopology *topology;
    RvInventory *inventory;
    RvScanOptions options;
    RvPlan *plan;
    guint counts[RV_CAT_UNSHARED + 1] = { 0 };
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

    run_dir = rv_run_dir_new(out_base, &error);
    if (run_dir == NULL) {
        g_printerr("%s\n", error->message);
        return EXIT_CANNOT_START;
    }
    path = g_build_filename(run_dir, "not-changed.csv", NULL);
    if (!rv_write_not_changed(inventory, path, TRUE, &error)) {
        g_printerr("%s\n", error->message);
        return EXIT_CANNOT_START;
    }
    g_free(path);
    path = g_build_filename(run_dir, "run-report.csv", NULL);
    if (!rv_write_run_report(inventory, path, &error)) {
        g_printerr("%s\n", error->message);
        return EXIT_CANNOT_START;
    }
    g_free(path);

    for (guint i = 0; i < inventory->variables->len; i++)
        counts[((RvVariable *)inventory->variables->pdata[i])->category]++;

    g_print("\nRead %u variables in %u projects (%u environments, %u services).\n\n",
            inventory->variables->len, inventory->projects, inventory->environments,
            inventory->services);
    for (guint c = RV_CAT_ROTATE; c <= RV_CAT_UNSHARED; c++) {
        g_print("  %-22s %u\n", rv_category_name(c), counts[c]);
    }

    plan = rv_plan_build(inventory, TRUE, &error);
    if (plan != NULL) {
        g_print("\nShared secrets the tool could rotate: %u groups (names only, no values)\n",
                plan->groups->len);
        for (guint i = 0; i < plan->groups->len; i++)
            g_print("  %s\n", ((RvPlanGroup *)plan->groups->pdata[i])->label);
        rv_plan_free(plan);
    }

    if (inventory->roles->len > 0) {
        g_print("\nDatabase roles the tool could rotate, through a secure tunnel (names only, no passwords):\n");
        for (guint i = 0; i < inventory->roles->len; i++) {
            const RvRole *r = inventory->roles->pdata[i];

            g_print("  %s / %s: role %s on %s, %u variable%s hold its password\n", r->project,
                    r->environment, r->role, r->db_service, r->consumers->len,
                    r->consumers->len == 1 ? "" : "s");
        }
    }

    if (!inventory->complete) {
        g_print("\nThe scan was NOT complete. See run-report.csv.\n");
        status = EXIT_INCOMPLETE;
    }
    g_print("\nWrote not-changed.csv and run-report.csv to %s\n", run_dir);
    g_print("not-changed.csv is your checklist of what to rotate by hand. It holds no values.\n");

    rv_inventory_free(inventory);
    rv_policy_free(policy);
    rw_client_free(client);
    g_free(run_dir);
    g_free(policy_path);
    g_free(name);
    g_free(version);
    return status;
}
