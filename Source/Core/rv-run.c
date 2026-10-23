/*
 * Copyright © 2026, Wexford House <https://wexfordhouse.co.za/>
 * Copyright © 2026, Ross Cameron <ross@wexfordhouse.co.za>
 * Licensed under the GNU General Public License, version 3.
 */
#include "rv-private.h"

#include <glib/gstdio.h>
#include <string.h>

#define CHANGES_COLUMNS 12

gchar *
rv_run_dir_new(const gchar *base, GError **error)
{
    GDateTime *now = g_date_time_new_now_utc();
    gchar *stamp = g_date_time_format(now, "run-%Y%m%d-%H%M%S");
    gchar *path = NULL;

    g_date_time_unref(now);
    for (guint n = 0; n < 100; n++) {
        gchar *name = n == 0 ? g_strdup(stamp) : g_strdup_printf("%s-%u", stamp, n);

        path = g_build_filename(base, name, NULL);
        g_free(name);
        if (!g_file_test(path, G_FILE_TEST_EXISTS))
            break;
        g_clear_pointer(&path, g_free);
    }
    g_free(stamp);

    if (path == NULL || g_mkdir_with_parents(path, 0700) != 0) {
        g_set_error(error, RV_ERROR, RV_ERROR_IO, "Could not create a run folder in %s.", base);
        g_free(path);
        return NULL;
    }
    return path;
}

/* ---- prepare -------------------------------------------------------------- */

static gboolean
write_changes(const RvPlan *plan, const gchar *dir, GError **error)
{
    static const gchar *const header[] = {
        "workspace", "project", "environment", "service", "variable", "old_value",
        "new_value", "timestamp", "workspace_id", "project_id", "environment_id",
        "service_id", NULL
    };
    GPtrArray *changes = rv_plan_changes(plan);
    GString *out = g_string_new(NULL);
    gchar *stamp = rv_now_iso(), *path;
    gboolean ok;

    rv_csv_append_row(out, header);
    for (guint i = 0; i < changes->len; i++) {
        const RvChange *c = changes->pdata[i];
        const RvVariable *v = c->variable;
        const gchar *row[] = {
            v->workspace, v->project, v->environment, v->service, v->name, v->value,
            c->new_value, stamp, v->workspace_id, v->project_id, v->environment_id,
            v->service_id != NULL ? v->service_id : "", NULL
        };

        rv_csv_append_row(out, row);
    }
    path = rv_build_path(dir, RV_FILE_CHANGES);
    ok = rv_write_file(path, out->str, out->len, TRUE, error);
    g_free(path);
    g_free(stamp);
    rv_gstring_wipe_free(out);
    g_ptr_array_unref(changes);
    return ok;
}

static gboolean
write_roles(const RvPlan *plan, const gchar *dir, GError **error)
{
    static const gchar *const header[] = {
        "project_id", "environment_id", "db_service_id", "project", "environment", "db_service",
        "role", "database", "host", "port", "old_password", "new_password", NULL
    };
    GString *out = g_string_new(NULL);
    gchar *path;
    gboolean ok;

    rv_csv_append_row(out, header);
    for (guint k = 0; k < plan->roles->len; k++) {
        const RvPlanRole *pr = plan->roles->pdata[k];
        const RvRole *r = pr->role;
        gchar *port;

        if (!pr->enabled)
            continue;
        port = g_strdup_printf("%u", r->port);
        {
            const gchar *row[] = { r->project_id, r->environment_id, r->db_service_id, r->project,
                                   r->environment, r->db_service, r->role,
                                   r->database != NULL ? r->database : "", r->host, port,
                                   r->old_password, pr->new_password, NULL };

            rv_csv_append_row(out, row);
        }
        g_free(port);
    }
    path = rv_build_path(dir, RV_FILE_ROLES);
    ok = rv_write_file(path, out->str, out->len, TRUE, error);
    g_free(path);
    rv_gstring_wipe_free(out);
    return ok;
}

static gboolean
write_deploys(const RvPlan *plan, const gchar *dir, GError **error)
{
    static const gchar *const header[] = {
        "project_id", "environment_id", "service_id", "project", "environment",
        "service", NULL
    };
    GPtrArray *targets = rv_plan_deploy_targets(plan);
    GString *out = g_string_new(NULL);
    gchar *path;
    gboolean ok;

    rv_csv_append_row(out, header);
    for (guint i = 0; i < targets->len; i++) {
        const RvDeployTarget *t = targets->pdata[i];
        const gchar *row[] = { t->project_id, t->environment_id, t->service_id,
                               t->project, t->environment, t->service, NULL };

        rv_csv_append_row(out, row);
    }
    path = rv_build_path(dir, RV_FILE_DEPLOYS);
    ok = rv_write_file(path, out->str, out->len, FALSE, error);
    g_free(path);
    g_string_free(out, TRUE);
    g_ptr_array_unref(targets);
    return ok;
}

gboolean
rv_run_prepare(const RvPlan *plan, const gchar *run_dir, GError **error)
{
    gchar *path;
    gboolean ok;

    if (!write_changes(plan, run_dir, error))
        return FALSE;
    if (!write_roles(plan, run_dir, error))
        return FALSE;
    if (!write_deploys(plan, run_dir, error))
        return FALSE;

    /* A group the user switched off is rotated by hand, so it belongs on the checklist. */
    for (guint g = 0; g < plan->groups->len; g++) {
        const RvPlanGroup *group = plan->groups->pdata[g];

        if (group->enabled)
            continue;
        for (guint i = 0; i < group->members->len; i++) {
            RvVariable *v = group->members->pdata[i];

            v->category = RV_CAT_SHARED_MANUAL;
            g_free(v->reason);
            v->reason = g_strdup("you excluded this group on the review screen, rotate it by hand");
        }
    }

    /* A database role the user switched off is rotated by hand too. */
    for (guint k = 0; k < plan->roles->len; k++) {
        const RvPlanRole *pr = plan->roles->pdata[k];

        if (pr->enabled)
            continue;
        for (guint i = 0; i < pr->role->consumers->len; i++) {
            RvVariable *v = pr->role->consumers->pdata[i];

            v->category = RV_CAT_DATABASE;
            g_free(v->reason);
            v->reason = g_strdup_printf("password of database role %s on %s, you excluded this role on the review screen, rotate it by hand",
                                        pr->role->role, pr->role->db_service);
        }
    }

    path = rv_build_path(run_dir, RV_FILE_NOT_CHANGED);
    ok = rv_write_not_changed(plan->inventory, path, FALSE, error);
    g_free(path);
    if (!ok)
        return FALSE;

    path = rv_build_path(run_dir, RV_FILE_RUN_REPORT);
    ok = rv_write_run_report(plan->inventory, path, error);
    g_free(path);
    if (!ok)
        return FALSE;

    path = rv_build_path(run_dir, RV_FILE_PROGRESS);
    ok = rv_write_file(path, "phase,key,status,detail,timestamp\r\n", -1, FALSE, error);
    g_free(path);
    return ok;
}

/* ---- progress file -------------------------------------------------------- */

void
rv_progress_note(const gchar *dir, const gchar *phase, const gchar *key,
              const gchar *status, const gchar *detail)
{
    GString *line = g_string_new(NULL);
    gchar *stamp = rv_now_iso(), *path = rv_build_path(dir, RV_FILE_PROGRESS);
    const gchar *row[] = { phase, key, status, detail != NULL ? detail : "", stamp, NULL };

    rv_csv_append_row(line, row);
    g_string_truncate(line, line->len - 2);   /* rv_append_line adds the line end */
    rv_append_line(path, line->str, NULL);
    g_string_free(line, TRUE);
    g_free(stamp);
    g_free(path);
}

GHashTable *
rv_progress_done(const gchar *dir, const gchar *phase, GHashTable *failed_out)
{
    GHashTable *done = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    gchar *path = rv_build_path(dir, RV_FILE_PROGRESS), *text = NULL;
    GPtrArray *rows;

    if (rv_read_file(path, &text, NULL)) {
        rows = rv_csv_parse(text, NULL);
        for (guint i = 1; rows != NULL && i < rows->len; i++) {
            gchar **r = rows->pdata[i];

            if (g_strv_length(r) < 3 || strcmp(r[0], phase) != 0)
                continue;
            if (strcmp(r[2], "done") == 0) {
                g_hash_table_add(done, g_strdup(r[1]));
                if (failed_out != NULL)
                    g_hash_table_remove(failed_out, r[1]);
            } else if (failed_out != NULL && !g_hash_table_contains(done, r[1])) {
                g_hash_table_add(failed_out, g_strdup(r[1]));
            }
        }
        if (rows != NULL)
            g_ptr_array_unref(rows);
        g_free(text);
    }
    g_free(path);
    return done;
}

/* ---- reading the run folder ----------------------------------------------- */

static GPtrArray *
load_csv(const gchar *dir, const gchar *file, guint min_columns, GError **error)
{
    gchar *path = rv_build_path(dir, file), *text = NULL;
    GPtrArray *rows;

    if (!rv_read_file(path, &text, error)) {
        g_free(path);
        return NULL;
    }
    rows = rv_csv_parse(text, error);
    rw_secure_free(text);
    if (rows != NULL) {
        for (guint i = 0; i < rows->len; i++) {
            if (g_strv_length(rows->pdata[i]) < min_columns) {
                g_set_error(error, RV_ERROR, RV_ERROR_CSV,
                            "%s has a row with too few columns (row %u).", path, i + 1);
                g_ptr_array_unref(rows);
                rows = NULL;
                break;
            }
        }
    }
    g_free(path);
    return rows;
}

static void
batch_free(RvBatch *b)
{
    g_free(b->key);
    g_free(b->project_id);
    g_free(b->environment_id);
    g_free(b->service_id);
    g_free(b->label);
    g_ptr_array_unref(b->rows);
    g_free(b);
}

GPtrArray *
rv_batches_from_changes(GPtrArray *rows)
{
    GPtrArray *batches = g_ptr_array_new_with_free_func((GDestroyNotify)batch_free);
    GHashTable *index = g_hash_table_new(g_str_hash, g_str_equal);

    for (guint i = 1; i < rows->len; i++) {
        gchar **r = rows->pdata[i];
        gchar *key = g_strdup_printf("%s|%s|%s", r[9], r[10], r[11][0] != '\0' ? r[11] : "shared");
        RvBatch *b = g_hash_table_lookup(index, key);

        if (b == NULL) {
            b = g_new0(RvBatch, 1);
            b->key = key;
            b->project_id = g_strdup(r[9]);
            b->environment_id = g_strdup(r[10]);
            b->service_id = r[11][0] != '\0' ? g_strdup(r[11]) : NULL;
            b->label = g_strdup_printf("%s / %s / %s", r[1], r[2], r[3]);
            b->rows = g_ptr_array_new();
            g_ptr_array_add(batches, b);
            g_hash_table_insert(index, b->key, b);
        } else {
            g_free(key);
        }
        g_ptr_array_add(b->rows, r);
    }
    g_hash_table_unref(index);
    return batches;
}

/* ---- stage ---------------------------------------------------------------- */

gboolean
rv_run_stage(const gchar *run_dir, const RvRunOptions *o, GError **error)
{
    GPtrArray *rows = load_csv(run_dir, RV_FILE_CHANGES, CHANGES_COLUMNS, error);
    GPtrArray *batches;
    GHashTable *done;
    guint staged = 0;
    gboolean ok = TRUE;

    if (rows == NULL)
        return FALSE;
    batches = rv_batches_from_changes(rows);
    done = rv_progress_done(run_dir, "stage", NULL);

    for (guint i = 0; i < batches->len && ok; i++) {
        RvBatch *b = batches->pdata[i];
        GHashTable *table;
        GError *local = NULL;

        if (g_hash_table_contains(done, b->key)) {
            staged++;
            continue;
        }
        if (rv_check_cancelled(o->cancel, error)) {
            ok = FALSE;
            break;
        }
        rv_progress(o->progress, o->progress_data, (gdouble)i / batches->len,
                    "Staging %s", b->label);

        /* Never overwrite a change someone made after the scan. */
        {
            GHashTable *current = rw_get_variables(o->client, b->project_id, b->environment_id,
                                                   b->service_id, TRUE, &local);
            const gchar *moved = NULL;

            table = rw_variables_new();
            if (current == NULL) {
                rv_progress_note(run_dir, "stage", b->key, "failed", local->message);
                g_set_error(error, RV_ERROR, RV_ERROR_STOPPED,
                            "Staging stopped at %s: could not check the current values: %s. "
                            "%u of %u groups of variables were staged. Resume, or undo what was staged.",
                            b->label, local->message, staged, batches->len);
                g_error_free(local);
                g_hash_table_unref(table);
                ok = FALSE;
                break;
            }
            for (guint j = 0; j < b->rows->len && moved == NULL; j++) {
                gchar **r = b->rows->pdata[j];
                const gchar *now = g_hash_table_lookup(current, r[4]);

                if (now != NULL && strcmp(now, r[6]) == 0)
                    continue;                               /* already holds the new value */
                if (now != NULL && strcmp(now, r[5]) == 0)
                    g_hash_table_replace(table, g_strdup(r[4]), g_strdup(r[6]));
                else
                    moved = r[4];
            }
            g_hash_table_unref(current);
            if (moved != NULL) {
                rv_progress_note(run_dir, "stage", b->key, "failed", "changed since the scan");
                g_set_error(error, RV_ERROR, RV_ERROR_STOPPED,
                            "Staging stopped at %s: %s was changed by someone since the scan, "
                            "so it was not overwritten. Scan again. %u of %u groups of variables "
                            "were staged.", b->label, moved, staged, batches->len);
                g_hash_table_unref(table);
                ok = FALSE;
                break;
            }
            if (g_hash_table_size(table) == 0) {
                rv_progress_note(run_dir, "stage", b->key, "done", "already staged");
                staged++;
                g_hash_table_unref(table);
                continue;
            }
        }
        if (rw_upsert_variables(o->client, b->project_id, b->environment_id,
                                b->service_id, table, TRUE, &local)) {
            rv_progress_note(run_dir, "stage", b->key, "done", NULL);
            staged++;
        } else {
            rv_progress_note(run_dir, "stage", b->key, "failed", local->message);
            g_set_error(error, RV_ERROR, RV_ERROR_STOPPED,
                        "Staging stopped at %s: %s. %u of %u groups of variables were staged. "
                        "Resume, or undo what was staged.",
                        b->label, local->message, staged, batches->len);
            g_error_free(local);
            ok = FALSE;
        }
        g_hash_table_unref(table);
    }
    if (ok)
        rv_progress(o->progress, o->progress_data, 1.0, "Staged %u groups of variables", staged);

    g_hash_table_unref(done);
    g_ptr_array_unref(batches);
    g_ptr_array_unref(rows);
    return ok;
}

/* ---- deploy --------------------------------------------------------------- */

gboolean
rv_run_deploy(const gchar *run_dir, const RvRunOptions *o, GError **error)
{
    /* Services must not run on the new connection settings before the database knows them. */
    if (rv_roles_pending(run_dir)) {
        g_set_error_literal(error, RV_ERROR, RV_ERROR_STATE,
                            "The database passwords have not all been changed yet, so nothing was "
                            "deployed. Change them first.");
        return FALSE;
    }
    GPtrArray *rows = load_csv(run_dir, RV_FILE_DEPLOYS, 6, error);
    GHashTable *done;
    guint failed = 0, total;

    if (rows == NULL)
        return FALSE;
    done = rv_progress_done(run_dir, "deploy", NULL);
    total = rows->len > 0 ? rows->len - 1 : 0;

    for (guint i = 1; i < rows->len; i++) {
        gchar **r = rows->pdata[i];
        gchar *key = g_strdup_printf("%s|%s|%s", r[0], r[1], r[2]);
        GError *local = NULL;

        if (g_hash_table_contains(done, key)) {
            g_free(key);
            continue;
        }
        if (rv_check_cancelled(o->cancel, error)) {
            g_free(key);
            g_hash_table_unref(done);
            g_ptr_array_unref(rows);
            return FALSE;
        }
        rv_progress(o->progress, o->progress_data, (gdouble)(i - 1) / total,
                    "Redeploying %s / %s / %s", r[3], r[4], r[5]);
        if (rw_redeploy_service(o->client, r[2], r[1], &local)) {
            rv_progress_note(run_dir, "deploy", key, "done", NULL);
        } else {
            rv_progress_note(run_dir, "deploy", key, "failed", local->message);
            failed++;
            g_error_free(local);
        }
        g_free(key);
    }
    g_hash_table_unref(done);
    g_ptr_array_unref(rows);

    if (failed > 0) {
        g_set_error(error, RV_ERROR, RV_ERROR_STOPPED,
                    "%u of %u services could not be redeployed. The new values are staged. "
                    "Run the deploy step again later.", failed, total);
        return FALSE;
    }
    rv_progress(o->progress, o->progress_data, 1.0, "Redeployed %u services", total);
    return TRUE;
}

gboolean
rv_run_status(const gchar *run_dir, RvRunStatus *status, GError **error)
{
    GPtrArray *changes = load_csv(run_dir, RV_FILE_CHANGES, CHANGES_COLUMNS, error);
    GPtrArray *deploys, *batches;
    GHashTable *stage_done, *stage_failed, *deploy_done, *deploy_failed;

    if (changes == NULL)
        return FALSE;
    deploys = load_csv(run_dir, RV_FILE_DEPLOYS, 6, error);
    if (deploys == NULL) {
        g_ptr_array_unref(changes);
        return FALSE;
    }
    memset(status, 0, sizeof *status);
    rv_roles_status(run_dir, &status->roles_total, &status->roles_done, &status->roles_failed);

    batches = rv_batches_from_changes(changes);
    stage_failed = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    stage_done = rv_progress_done(run_dir, "stage", stage_failed);
    status->batches_total = batches->len;
    for (guint i = 0; i < batches->len; i++) {
        if (g_hash_table_contains(stage_done, ((RvBatch *)batches->pdata[i])->key))
            status->batches_done++;
    }
    status->stage_failed = g_hash_table_size(stage_failed) > 0;
    g_hash_table_unref(stage_failed);

    deploy_failed = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    deploy_done = rv_progress_done(run_dir, "deploy", deploy_failed);
    status->deploys_total = deploys->len > 0 ? deploys->len - 1 : 0;
    status->deploys_done = g_hash_table_size(deploy_done);
    status->deploys_failed = g_hash_table_size(deploy_failed);

    g_hash_table_unref(deploy_failed);
    g_hash_table_unref(deploy_done);
    g_hash_table_unref(stage_done);
    g_ptr_array_unref(batches);
    g_ptr_array_unref(deploys);
    g_ptr_array_unref(changes);
    return TRUE;
}

/* ---- reports -------------------------------------------------------------- */

gboolean
rv_write_not_changed(const RvInventory *inv, const gchar *path, gboolean include_rotatable,
                     GError **error)
{
    static const gchar *const header[] = {
        "workspace", "project", "environment", "service", "variable", "category",
        "reason", NULL
    };
    GString *out = g_string_new(NULL);
    gboolean ok;

    rv_csv_append_row(out, header);
    for (guint i = 0; i < inv->variables->len; i++) {
        const RvVariable *v = inv->variables->pdata[i];
        const gchar *row[] = { v->workspace, v->project, v->environment, v->service,
                               v->name, rv_category_name(v->category),
                               v->reason != NULL ? v->reason : "", NULL };

        if (v->category == RV_CAT_ROTATE || v->category == RV_CAT_DB_ROLE) {
            const gchar *rotatable[] = { v->workspace, v->project, v->environment, v->service,
                                         v->name, rv_category_name(v->category),
                                         v->reason != NULL ? v->reason : "", NULL };

            if (include_rotatable)
                rv_csv_append_row(out, rotatable);
            continue;
        }
        rv_csv_append_row(out, row);
    }
    ok = rv_write_file(path, out->str, out->len, FALSE, error);
    g_string_free(out, TRUE);
    return ok;
}

gboolean
rv_write_audit(const RvInventory *inv, const gchar *path, GError **error)
{
    static const gchar *const header[] = {
        "workspace", "project", "environment", "service", "variable", "value", "category",
        "reason", NULL
    };
    GString *out = g_string_new(NULL);
    gboolean ok;

    rv_csv_append_row(out, header);
    for (guint i = 0; i < inv->variables->len; i++) {
        const RvVariable *v = inv->variables->pdata[i];
        const gchar *row[] = { v->workspace, v->project, v->environment, v->service, v->name,
                               v->value, rv_category_name(v->category),
                               v->reason != NULL ? v->reason : "", NULL };

        rv_csv_append_row(out, row);
    }
    ok = rv_write_file(path, out->str, out->len, TRUE, error);
    rv_gstring_wipe_free(out);
    return ok;
}

gchar *
rv_audit_default_folder(void)
{
    const gchar *desktop = g_get_user_special_dir(G_USER_DIRECTORY_DESKTOP);

    if (desktop != NULL && g_file_test(desktop, G_FILE_TEST_IS_DIR))
        return g_strdup(desktop);
    return g_strdup(g_get_home_dir());
}

gchar *
rv_audit_save(const RvInventory *inv, const gchar *base, GError **error)
{
    gchar *dir = rv_run_dir_new(base, error), *path;
    gboolean ok;

    if (dir == NULL)
        return NULL;
    path = g_build_filename(dir, "audit.csv", NULL);
    ok = rv_write_audit(inv, path, error);
    g_free(path);
    if (!ok)
        g_clear_pointer(&dir, g_free);
    return dir;
}

gboolean
rv_write_run_report(const RvInventory *inv, const gchar *path, GError **error)
{
    static const gchar *const header[] = { "workspace", "project", "status", "detail", NULL };
    GString *out = g_string_new(NULL);
    gboolean ok;

    rv_csv_append_row(out, header);
    for (guint i = 0; i < inv->report->len; i++) {
        const RvReportRow *r = inv->report->pdata[i];
        const gchar *row[] = { r->workspace, r->project != NULL ? r->project : "",
                               r->status, r->detail, NULL };

        rv_csv_append_row(out, row);
    }
    ok = rv_write_file(path, out->str, out->len, FALSE, error);
    g_string_free(out, TRUE);
    return ok;
}
