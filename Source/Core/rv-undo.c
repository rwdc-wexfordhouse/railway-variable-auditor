/*
 * Copyright © 2026, Wexford House <https://wexfordhouse.co.za/>
 * Copyright © 2026, Ross Cameron <ross@wexfordhouse.co.za>
 * Licensed under the GNU General Public License, version 3.
 */
#include "rv-private.h"

#include <string.h>

#define CHANGES_COLUMNS 12

typedef struct {
    gchar      **row;         /* the changes.csv row, borrowed */
    const gchar *status;
    const gchar *detail;
} Outcome;

static void
set_outcome(Outcome *o, const gchar *status, const gchar *detail)
{
    o->status = status;
    o->detail = detail;
}

/* Redeploy the services of each environment that now holds the old values. */
static void
redeploy_restored(const gchar *run_dir, const RvRunOptions *o, GHashTable *environments,
                  RvUndoResult *result)
{
    gchar *path = rv_build_path(run_dir, RV_FILE_DEPLOYS), *text = NULL;
    GPtrArray *deploys = NULL;
    GHashTable *done = rv_progress_done(run_dir, "undo-deploy", NULL);

    if (rv_read_file(path, &text, NULL)) {
        deploys = rv_csv_parse(text, NULL);
        g_free(text);
    }
    g_free(path);

    for (guint i = 1; deploys != NULL && i < deploys->len; i++) {
        gchar **r = deploys->pdata[i];
        gchar *env_key, *service_key;
        GError *local = NULL;

        if (g_strv_length(r) < 6)
            continue;
        env_key = g_strdup_printf("%s|%s", r[0], r[1]);
        service_key = g_strdup_printf("%s|%s|%s", r[0], r[1], r[2]);
        if (g_hash_table_contains(environments, env_key)
            && !g_hash_table_contains(done, service_key)) {
            rv_progress(o->progress, o->progress_data, -1, "Redeploying %s / %s / %s",
                        r[3], r[4], r[5]);
            if (rw_redeploy_service(o->client, r[2], r[1], &local)) {
                rv_progress_note(run_dir, "undo-deploy", service_key, "done", NULL);
            } else {
                rv_progress_note(run_dir, "undo-deploy", service_key, "failed", local->message);
                result->redeploy_failed++;
                g_error_free(local);
            }
        }
        g_free(service_key);
        g_free(env_key);
    }
    if (deploys != NULL)
        g_ptr_array_unref(deploys);
    g_hash_table_unref(done);
}

gboolean
rv_undo_run(const gchar *run_dir, const RvRunOptions *o, const RvUndoOptions *u,
            RvUndoResult *result, GError **error)
{
    static const gchar *const header[] = {
        "workspace", "project", "environment", "service", "variable", "status",
        "detail", "timestamp", NULL
    };
    gchar *path = rv_build_path(run_dir, RV_FILE_CHANGES), *text = NULL;
    GPtrArray *rows, *batches;
    GArray *outcomes;
    GHashTable *holding_old;
    GPtrArray *role_outcomes = NULL;
    GString *report = g_string_new(NULL);
    GError *stop = NULL;      /* cancelled, or a redeploy failed: reported after the report is written */
    gboolean ok;

    memset(result, 0, sizeof *result);
    if (!rv_read_file(path, &text, error)) {
        g_free(path);
        g_string_free(report, TRUE);
        return FALSE;
    }
    g_free(path);
    rows = rv_csv_parse(text, error);
    rw_secure_free(text);
    if (rows == NULL) {
        g_string_free(report, TRUE);
        return FALSE;
    }
    for (guint i = 0; i < rows->len; i++) {
        if (g_strv_length(rows->pdata[i]) < CHANGES_COLUMNS) {
            g_set_error(error, RV_ERROR, RV_ERROR_CSV,
                        "changes.csv has a row with too few columns (row %u).", i + 1);
            g_ptr_array_unref(rows);
            g_string_free(report, TRUE);
            return FALSE;
        }
    }

    batches = rv_batches_from_changes(rows);
    outcomes = g_array_new(FALSE, TRUE, sizeof(Outcome));
    holding_old = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    for (guint b = 0; b < batches->len; b++) {
        RvBatch *batch = batches->pdata[b];
        GHashTable *current = NULL, *restore;
        GError *local = NULL;
        guint first = outcomes->len;
        gboolean wanted = u->only_projects == NULL
                          || g_hash_table_contains(u->only_projects, batch->project_id);

        if (rv_check_cancelled(o->cancel, &stop))
            break;

        for (guint j = 0; j < batch->rows->len; j++) {
            Outcome out = { batch->rows->pdata[j], NULL, NULL };

            g_array_append_val(outcomes, out);
        }
        if (!wanted) {
            for (guint j = first; j < outcomes->len; j++)
                set_outcome(&g_array_index(outcomes, Outcome, j), "skipped",
                            "project not selected");
            continue;
        }

        rv_progress(o->progress, o->progress_data, (gdouble)b / batches->len,
                    "Checking %s", batch->label);
        current = rw_get_variables(o->client, batch->project_id, batch->environment_id,
                                   batch->service_id, TRUE, &local);
        if (current == NULL) {
            for (guint j = first; j < outcomes->len; j++)
                set_outcome(&g_array_index(outcomes, Outcome, j), "failed",
                            "could not read the current values");
            g_clear_error(&local);
            continue;
        }

        restore = rw_variables_new();
        for (guint j = first; j < outcomes->len; j++) {
            Outcome *out = &g_array_index(outcomes, Outcome, j);
            const gchar *now = g_hash_table_lookup(current, out->row[4]);

            if (now == NULL && u->overwrite_conflicts)
                set_outcome(out, "restored", "recreated a variable that was removed");
            else if (now == NULL)
                set_outcome(out, "conflict", "the variable no longer exists");
            else if (strcmp(now, out->row[6]) == 0)
                set_outcome(out, "restored", "");
            else if (strcmp(now, out->row[5]) == 0)
                set_outcome(out, "already-original", "");
            else if (u->overwrite_conflicts)
                set_outcome(out, "restored", "overwrote a value that changed since the run");
            else
                set_outcome(out, "conflict", "value changed since the run, left alone");

            if (g_strcmp0(out->status, "restored") == 0)
                g_hash_table_replace(restore, g_strdup(out->row[4]), g_strdup(out->row[5]));
        }

        if (!u->dry_run && g_hash_table_size(restore) > 0
            && !rw_upsert_variables(o->client, batch->project_id, batch->environment_id,
                                    batch->service_id, restore, TRUE, &local)) {
            for (guint j = first; j < outcomes->len; j++) {
                Outcome *out = &g_array_index(outcomes, Outcome, j);

                if (g_strcmp0(out->status, "restored") == 0)
                    set_outcome(out, "failed", "could not write the old value");
            }
            g_clear_error(&local);
        }
        g_hash_table_unref(restore);
        g_hash_table_unref(current);
    }

    /* Then the database roles, so they match the variables that were just restored. */
    if (stop == NULL)
        role_outcomes = rv_roles_undo(run_dir, o, u, &stop);

    /* The report is always written, even after a cancel, because some values may
     * already have been restored. */
    {
        gchar *stamp = rv_now_iso(), *out_path;

        rv_csv_append_row(report, header);
        for (guint i = 0; i < outcomes->len; i++) {
            const Outcome *out = &g_array_index(outcomes, Outcome, i);
            const gchar *line[] = { out->row[0], out->row[1], out->row[2], out->row[3],
                                    out->row[4], out->status, out->detail, stamp, NULL };

            rv_csv_append_row(report, line);
            if (g_strcmp0(out->status, "restored") == 0) result->restored++;
            else if (g_strcmp0(out->status, "conflict") == 0) result->conflicts++;
            else if (g_strcmp0(out->status, "already-original") == 0) result->already_original++;
            else if (g_strcmp0(out->status, "skipped") == 0) result->skipped++;
            else result->failed++;

            /* An environment holds the old values if anything was restored or already was. */
            if (g_strcmp0(out->status, "restored") == 0
                || g_strcmp0(out->status, "already-original") == 0)
                g_hash_table_add(holding_old,
                                 g_strdup_printf("%s|%s", out->row[9], out->row[10]));
        }
        for (guint i = 0; role_outcomes != NULL && i < role_outcomes->len; i++) {
            const RvRoleOutcome *ro = role_outcomes->pdata[i];
            gchar *what = g_strdup_printf("role:%s", ro->role);
            const gchar *line[] = { "", ro->project, ro->environment, ro->service, what, ro->status,
                                    ro->detail, stamp, NULL };

            rv_csv_append_row(report, line);
            if (g_strcmp0(ro->status, "restored") == 0) result->restored++;
            else if (g_strcmp0(ro->status, "conflict") == 0) result->conflicts++;
            else if (g_strcmp0(ro->status, "already-original") == 0) result->already_original++;
            else if (g_strcmp0(ro->status, "skipped") == 0) result->skipped++;
            else result->failed++;
            if (g_strcmp0(ro->status, "restored") == 0 || g_strcmp0(ro->status, "already-original") == 0)
                g_hash_table_add(holding_old, g_strdup_printf("%s|%s", ro->project_id, ro->environment_id));
            g_free(what);
        }
        out_path = rv_build_path(run_dir, u->dry_run ? RV_FILE_UNDO_PREVIEW : RV_FILE_UNDO_REPORT);
        ok = rv_write_file(out_path, report->str, report->len, FALSE, error);
        g_free(out_path);
        g_free(stamp);
    }

    /* Redeploy where the old values are now in place. Services that an earlier undo already
     * redeployed are skipped, and a service that failed is tried again. */
    if (ok && stop == NULL && u->deploy && !u->dry_run)
        redeploy_restored(run_dir, o, holding_old, result);

    if (ok && stop != NULL) {
        g_propagate_error(error, stop);
        stop = NULL;
        ok = FALSE;
    } else if (ok && result->redeploy_failed > 0) {
        g_set_error(error, RV_ERROR, RV_ERROR_STOPPED,
                    "The old values are back, but %u services could not be redeployed. "
                    "Run the undo again later to redeploy them.", result->redeploy_failed);
        ok = FALSE;
    }
    g_clear_error(&stop);

    if (role_outcomes != NULL)
        g_ptr_array_unref(role_outcomes);
    g_hash_table_unref(holding_old);
    g_array_free(outcomes, TRUE);
    g_ptr_array_unref(batches);
    g_ptr_array_unref(rows);
    g_string_free(report, TRUE);
    return ok;
}
