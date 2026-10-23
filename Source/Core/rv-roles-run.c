/*
 * Changes and restores the passwords of database roles, through a secure tunnel.
 *
 * Copyright © 2026, Wexford House <https://wexfordhouse.co.za/>
 * Copyright © 2026, Ross Cameron <ross@wexfordhouse.co.za>
 * Licensed under the GNU General Public License, version 3.
 */
#include "rv-private.h"

#include <string.h>

#define ROLES_COLUMNS 12
#define LOCAL_HOST "127.0.0.1"

/* One row of roles.csv. The strings belong to the parsed CSV. */
typedef struct {
    const gchar *project_id, *environment_id, *db_service_id;
    const gchar *project, *environment, *db_service;
    const gchar *role, *database, *host;
    guint        port;
    const gchar *old_password, *new_password;
} RoleRow;

static void
role_row(gchar **r, RoleRow *out)
{
    out->project_id = r[0];
    out->environment_id = r[1];
    out->db_service_id = r[2];
    out->project = r[3];
    out->environment = r[4];
    out->db_service = r[5];
    out->role = r[6];
    out->database = r[7];
    out->host = r[8];
    out->port = (guint)g_ascii_strtoull(r[9], NULL, 10);
    out->old_password = r[10];
    out->new_password = r[11];
}

static gchar *
role_key(const RoleRow *r)
{
    return g_strdup_printf("%s|%s|%s|%s", r->project_id, r->environment_id, r->db_service_id, r->role);
}

/* The roles of a run, or NULL if there are none (an older run, or none were planned). */
static GPtrArray *
load_roles(const gchar *run_dir, GError **error)
{
    gchar *path = rv_build_path(run_dir, RV_FILE_ROLES), *text = NULL;
    GPtrArray *rows = NULL;

    if (!g_file_test(path, G_FILE_TEST_EXISTS)) {
        g_free(path);
        return NULL;
    }
    if (rv_read_file(path, &text, error)) {
        rows = rv_csv_parse(text, error);
        rw_secure_free(text);
    }
    g_free(path);
    for (guint i = 0; rows != NULL && i < rows->len; i++) {
        if (g_strv_length(rows->pdata[i]) < ROLES_COLUMNS) {
            g_set_error(error, RV_ERROR, RV_ERROR_CSV, "roles.csv has a row with too few columns (row %u).", i + 1);
            g_ptr_array_unref(rows);
            return NULL;
        }
    }
    if (rows != NULL && rows->len <= 1) {
        g_ptr_array_unref(rows);
        return NULL;
    }
    return rows;
}

/* ---- the login of the database service, and its tunnel ---------------------------- */

typedef struct {
    gchar *key;
    gchar *admin_user, *admin_password, *admin_database;
    guint  port;
    gpointer handle;
} Session;

typedef struct {
    const RvRunOptions *o;
    const RvDbOps      *db;
    const RvTunnelOps  *tunnels;
    GPtrArray          *sessions;
} Ctx;

static void
session_free(Ctx *ctx, Session *s)
{
    if (s->handle != NULL)
        ctx->tunnels->close(ctx->o->tunnel_data, s->handle);
    g_free(s->key);
    g_free(s->admin_user);
    rw_secure_free(s->admin_password);
    g_free(s->admin_database);
    g_free(s);
}

static void
ctx_init(Ctx *ctx, const RvRunOptions *o)
{
    ctx->o = o;
    ctx->db = o->db != NULL ? o->db : rv_db_ops_libpq();
    ctx->tunnels = o->tunnels != NULL ? o->tunnels : rv_tunnel_ops_cli();
    ctx->sessions = g_ptr_array_new();
}

static void
ctx_clear(Ctx *ctx)
{
    for (guint i = 0; i < ctx->sessions->len; i++)
        session_free(ctx, ctx->sessions->pdata[i]);
    g_ptr_array_unref(ctx->sessions);
}

static const gchar *
first_of(GHashTable *vars, const gchar *a, const gchar *b)
{
    const gchar *v = g_hash_table_lookup(vars, a);

    if (v == NULL || *v == '\0')
        v = g_hash_table_lookup(vars, b);
    return v != NULL && *v != '\0' ? v : NULL;
}

/* One tunnel and one admin login for each database service, opened when first needed. */
static Session *
session_for(Ctx *ctx, const RoleRow *r, GError **error)
{
    gchar *key = g_strdup_printf("%s|%s|%s", r->project_id, r->environment_id, r->db_service_id);
    Session *s;
    GHashTable *vars;
    const gchar *user, *password, *database;

    for (guint i = 0; i < ctx->sessions->len; i++) {
        s = ctx->sessions->pdata[i];
        if (strcmp(s->key, key) == 0) {
            g_free(key);
            return s;
        }
    }

    /* The admin login is the database service's own, read in memory and never shown. */
    vars = rw_get_variables(ctx->o->client, r->project_id, r->environment_id, r->db_service_id,
                            FALSE, error);
    if (vars == NULL) {
        g_free(key);
        return NULL;
    }
    user = first_of(vars, "PGUSER", "POSTGRES_USER");
    password = first_of(vars, "PGPASSWORD", "POSTGRES_PASSWORD");
    database = first_of(vars, "PGDATABASE", "POSTGRES_DB");
    if (password == NULL) {
        g_set_error(error, RV_ERROR, RV_ERROR_DB,
                    "The login of database service %s could not be read from its variables.", r->db_service);
        g_hash_table_unref(vars);
        g_free(key);
        return NULL;
    }
    s = g_new0(Session, 1);
    s->key = key;
    s->admin_user = g_strdup(user != NULL ? user : "postgres");
    s->admin_password = g_strdup(password);
    s->admin_database = g_strdup(database != NULL ? database : "postgres");
    g_hash_table_unref(vars);

    if (!ctx->tunnels->open(ctx->o->tunnel_data, r->project_id, r->environment_id, r->db_service,
                            &s->port, &s->handle, error)) {
        session_free(ctx, s);
        return NULL;
    }
    g_ptr_array_add(ctx->sessions, s);
    return s;
}

static const gchar *
login_database(const RoleRow *r)
{
    /* A role with no database in its URL connects to the database named after it. */
    return r->database != NULL && *r->database != '\0' ? r->database : r->role;
}

static gboolean
is_auth_error(const GError *e)
{
    return e != NULL && e->domain == RV_ERROR && e->code == RV_ERROR_DB_AUTH;
}

/* ---- change the passwords ---------------------------------------------------------- */

gboolean
rv_run_roles(const gchar *run_dir, const RvRunOptions *o, GError **error)
{
    GPtrArray *rows = load_roles(run_dir, error);
    GHashTable *done;
    Ctx ctx;
    guint changed = 0, total;
    gboolean ok = TRUE;

    if (rows == NULL)
        return error == NULL || *error == NULL;
    total = rows->len - 1;
    done = rv_progress_done(run_dir, "role", NULL);
    ctx_init(&ctx, o);

    for (guint i = 1; i < rows->len && ok; i++) {
        RoleRow r;
        gchar *key;
        Session *s;
        GError *local = NULL;
        gchar *verifier = NULL;
        const gchar *failure = NULL;

        role_row(rows->pdata[i], &r);
        key = role_key(&r);
        if (g_hash_table_contains(done, key)) {
            changed++;
            g_free(key);
            continue;
        }
        if (rv_check_cancelled(o->cancel, error)) {
            ok = FALSE;
            g_free(key);
            break;
        }
        rv_progress(o->progress, o->progress_data, (gdouble)(i - 1) / total,
                    "Changing the password of role %s on %s", r.role, r.db_service);

        s = session_for(&ctx, &r, &local);
        if (s == NULL) {
            gchar *message = g_strdup(local->message);

            rv_progress_note(run_dir, "role", key, "failed", message);
            g_set_error(error, RV_ERROR, RV_ERROR_STOPPED,
                        "Changing the password of role %s on %s stopped: %s. Nothing was deployed. "
                        "%u of %u roles were changed. Try again, or undo.", r.role, r.db_service,
                        message, changed, total);
            g_free(message);
            g_error_free(local);
            g_free(key);
            ok = FALSE;
            break;
        }

        /* A repeat after a crash: the new password may already be in place. */
        if (ctx.db->login_ok(o->db_data, LOCAL_HOST, s->port, login_database(&r), r.role, r.new_password, &local)) {
            rv_progress_note(run_dir, "role", key, "done", "already changed");
            changed++;
            g_free(key);
            continue;
        }
        if (!is_auth_error(local)) {
            failure = local != NULL ? local->message : "the database did not answer";
        }
        if (failure == NULL) {
            g_clear_error(&local);
            /* The current password must work, or the host or the database is not what we think. */
            if (!ctx.db->login_ok(o->db_data, LOCAL_HOST, s->port, login_database(&r), r.role, r.old_password, &local))
                failure = is_auth_error(local)
                    ? "the role did not accept its current password, so the host or database is not what was expected. Nothing was changed"
                    : (local != NULL ? local->message : "the database did not answer");
        }
        if (failure == NULL) {
            g_clear_error(&local);
            verifier = rv_scram_verifier(r.new_password, &local);
            if (verifier == NULL) {
                failure = local->message;
            } else if (!ctx.db->set_password(o->db_data, LOCAL_HOST, s->port, s->admin_database, s->admin_user,
                                             s->admin_password, r.role, verifier, &local)) {
                failure = local != NULL ? local->message : "the statement failed";
            }
        }
        if (failure == NULL) {
            /* Prove it: log in with the new password. If that fails, put the old one back. */
            g_clear_error(&local);
            if (!ctx.db->login_ok(o->db_data, LOCAL_HOST, s->port, login_database(&r), r.role, r.new_password, &local)) {
                gchar *old_verifier = rv_scram_verifier(r.old_password, NULL);
                gboolean back = old_verifier != NULL
                    && ctx.db->set_password(o->db_data, LOCAL_HOST, s->port, s->admin_database, s->admin_user,
                                            s->admin_password, r.role, old_verifier, NULL);

                g_free(old_verifier);
                failure = back ? "the new password did not work, so the old one was put back"
                               : "the new password did not work, and the old one could NOT be put back. Check this role by hand";
            }
        }

        if (failure != NULL) {
            gchar *message = g_strdup(failure);

            rv_progress_note(run_dir, "role", key, "failed", message);
            g_set_error(error, RV_ERROR, RV_ERROR_STOPPED,
                        "Changing the password of role %s on %s stopped: %s. Nothing was deployed. "
                        "%u of %u roles were changed. Try again, or undo.", r.role, r.db_service,
                        message, changed, total);
            g_free(message);
            ok = FALSE;
        } else {
            rv_progress_note(run_dir, "role", key, "done", NULL);
            changed++;
        }
        g_clear_error(&local);
        g_free(verifier);
        g_free(key);
    }

    if (ok)
        rv_progress(o->progress, o->progress_data, 1.0, "Changed %u database role passwords", changed);
    ctx_clear(&ctx);
    g_hash_table_unref(done);
    g_ptr_array_unref(rows);
    return ok;
}

/* ---- state, for the status and for deploying ------------------------------------------ */

void
rv_roles_status(const gchar *run_dir, guint *total, guint *done_count, guint *failed_count)
{
    GPtrArray *rows = load_roles(run_dir, NULL);
    GHashTable *failed = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    GHashTable *done = rv_progress_done(run_dir, "role", failed);

    *total = *done_count = *failed_count = 0;
    for (guint i = 1; rows != NULL && i < rows->len; i++) {
        RoleRow r;
        gchar *key;

        role_row(rows->pdata[i], &r);
        key = role_key(&r);
        (*total)++;
        if (g_hash_table_contains(done, key))
            (*done_count)++;
        else if (g_hash_table_contains(failed, key))
            (*failed_count)++;
        g_free(key);
    }
    g_hash_table_unref(done);
    g_hash_table_unref(failed);
    if (rows != NULL)
        g_ptr_array_unref(rows);
}

gboolean
rv_roles_pending(const gchar *run_dir)
{
    guint total, done, failed;

    rv_roles_status(run_dir, &total, &done, &failed);
    return done < total;
}

/* ---- undo --------------------------------------------------------------------------------- */

void
rv_role_outcome_free(RvRoleOutcome *o)
{
    g_free(o->project_id);
    g_free(o->environment_id);
    g_free(o->project);
    g_free(o->environment);
    g_free(o->service);
    g_free(o->role);
    g_free(o->detail);
    g_free(o);
}

static RvRoleOutcome *
outcome(const RoleRow *r, const gchar *status, const gchar *detail)
{
    RvRoleOutcome *o = g_new0(RvRoleOutcome, 1);

    o->project_id = g_strdup(r->project_id);
    o->environment_id = g_strdup(r->environment_id);
    o->project = g_strdup(r->project);
    o->environment = g_strdup(r->environment);
    o->service = g_strdup(r->db_service);
    o->role = g_strdup(r->role);
    o->status = status;
    o->detail = g_strdup(detail != NULL ? detail : "");
    return o;
}

GPtrArray *
rv_roles_undo(const gchar *run_dir, const RvRunOptions *o, const RvUndoOptions *u, GError **error)
{
    GPtrArray *rows = load_roles(run_dir, error);
    GPtrArray *outcomes = g_ptr_array_new_with_free_func((GDestroyNotify)rv_role_outcome_free);
    GHashTable *done;
    Ctx ctx;

    if (rows == NULL)
        return outcomes;
    done = rv_progress_done(run_dir, "role", NULL);
    ctx_init(&ctx, o);

    for (guint i = 1; i < rows->len; i++) {
        RoleRow r;
        gchar *key;
        Session *s;
        GError *local = NULL;
        RvRoleOutcome *out;

        role_row(rows->pdata[i], &r);
        key = role_key(&r);
        if (rv_check_cancelled(o->cancel, error)) {
            g_free(key);
            break;
        }
        if (u->only_projects != NULL && !g_hash_table_contains(u->only_projects, r.project_id)) {
            g_ptr_array_add(outcomes, outcome(&r, "skipped", "project not selected"));
        } else if (!g_hash_table_contains(done, key)) {
            g_ptr_array_add(outcomes, outcome(&r, "skipped", "the password was never changed"));
        } else if ((s = session_for(&ctx, &r, &local)) == NULL) {
            g_ptr_array_add(outcomes, outcome(&r, "failed", local->message));
            g_error_free(local);
        } else {
            gboolean new_works = ctx.db->login_ok(o->db_data, LOCAL_HOST, s->port, login_database(&r), r.role, r.new_password, &local);

            if (!new_works && !is_auth_error(local)) {
                g_ptr_array_add(outcomes, outcome(&r, "failed", local != NULL ? local->message : "the database did not answer"));
            } else {
                gboolean restore = new_works;
                const gchar *detail = "";

                g_clear_error(&local);
                if (!new_works) {
                    if (ctx.db->login_ok(o->db_data, LOCAL_HOST, s->port, login_database(&r), r.role, r.old_password, NULL)) {
                        g_ptr_array_add(outcomes, outcome(&r, "already-original", ""));
                        g_free(key);
                        continue;
                    }
                    if (u->overwrite_conflicts) {
                        restore = TRUE;
                        detail = "overwrote a password that changed since the run";
                    } else {
                        g_ptr_array_add(outcomes, outcome(&r, "conflict", "the role accepts neither password, so it was changed since the run and left alone"));
                        g_free(key);
                        continue;
                    }
                }
                if (restore && u->dry_run) {
                    g_ptr_array_add(outcomes, outcome(&r, "restored", detail));
                } else if (restore) {
                    gchar *verifier = rv_scram_verifier(r.old_password, &local);

                    if (verifier == NULL) {
                        out = outcome(&r, "failed", local->message);
                    } else if (!ctx.db->set_password(o->db_data, LOCAL_HOST, s->port, s->admin_database, s->admin_user,
                                                     s->admin_password, r.role, verifier, &local)) {
                        out = outcome(&r, "failed", local != NULL ? local->message : "the statement failed");
                    } else if (!ctx.db->login_ok(o->db_data, LOCAL_HOST, s->port, login_database(&r), r.role, r.old_password, NULL)) {
                        out = outcome(&r, "failed", "the old password was set but does not work. Check this role by hand");
                    } else {
                        out = outcome(&r, "restored", detail);
                    }
                    g_ptr_array_add(outcomes, out);
                    g_free(verifier);
                    g_clear_error(&local);
                }
            }
            g_clear_error(&local);
        }
        g_free(key);
    }

    ctx_clear(&ctx);
    g_hash_table_unref(done);
    g_ptr_array_unref(rows);
    return outcomes;
}
