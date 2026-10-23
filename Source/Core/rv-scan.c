/*
 * Copyright © 2026, Wexford House <https://wexfordhouse.co.za/>
 * Copyright © 2026, Ross Cameron <ross@wexfordhouse.co.za>
 * Licensed under the GNU General Public License, version 3.
 */
#include "rv-private.h"

#include <string.h>

static void
project_free(RvProject *p)
{
    if (p == NULL)
        return;
    g_free(p->workspace_id);
    g_free(p->workspace);
    g_free(p->id);
    g_free(p->name);
    if (p->environments != NULL)
        g_ptr_array_unref(p->environments);
    if (p->services != NULL)
        g_ptr_array_unref(p->services);
    g_free(p);
}

static void
report_free(RvReportRow *row)
{
    g_free(row->workspace);
    g_free(row->project);
    g_free(row->status);
    g_free(row->detail);
    g_free(row);
}

static void
topology_report(RvTopology *t, const gchar *workspace, const gchar *project,
                const gchar *status, const gchar *detail)
{
    RvReportRow *row = g_new0(RvReportRow, 1);

    row->workspace = g_strdup(workspace);
    row->project = g_strdup(project);
    row->status = g_strdup(status);
    row->detail = g_strdup(detail != NULL ? detail : "");
    g_ptr_array_add(t->report, row);
}

void
rv_topology_free(RvTopology *t)
{
    if (t == NULL)
        return;
    g_ptr_array_unref(t->projects);
    g_ptr_array_unref(t->report);
    g_free(t);
}

/* Errors that stop the whole scan. Everything else is isolated. */
static gboolean
is_fatal(const GError *error)
{
    return error->domain == RW_ERROR
           && (error->code == RW_ERROR_RATE_LIMITED || error->code == RW_ERROR_CLI
               || error->code == RW_ERROR_NETWORK);
}

RvTopology *
rv_discover(const RvScanOptions *o, GError **error)
{
    RvTopology *t = g_new0(RvTopology, 1);
    GPtrArray *workspaces;

    t->projects = g_ptr_array_new_with_free_func((GDestroyNotify)project_free);
    t->report = g_ptr_array_new_with_free_func((GDestroyNotify)report_free);

    rv_progress(o->progress, o->progress_data, -1, "Listing your workspaces");
    workspaces = rw_list_workspaces(o->client, error);
    if (workspaces == NULL) {
        rv_topology_free(t);
        return NULL;
    }
    t->workspaces = workspaces->len;

    for (guint w = 0; w < workspaces->len; w++) {
        RwEntity *ws = workspaces->pdata[w];
        GPtrArray *projects;
        GError *local = NULL;

        if (rv_check_cancelled(o->cancel, error))
            goto cancelled;

        if (rv_policy_workspace_skipped(o->policy, ws->id)) {
            topology_report(t, ws->name, NULL, "skipped", "excluded by policy");
            continue;
        }

        rv_progress(o->progress, o->progress_data, (gdouble)w / workspaces->len,
                    "Listing projects in %s", ws->name);
        projects = rw_list_projects(o->client, ws->id, &local);
        if (projects == NULL) {
            if (is_fatal(local)) {
                g_propagate_error(error, local);
                goto fail;
            }
            topology_report(t, ws->name, NULL,
                            local->code == RW_ERROR_AUTH ? "skipped" : "failed",
                            local->code == RW_ERROR_AUTH ? "insufficient role" : local->message);
            g_error_free(local);
            continue;
        }
        topology_report(t, ws->name, NULL, "done", "");

        for (guint p = 0; p < projects->len; p++) {
            RwEntity *pr = projects->pdata[p];
            GPtrArray *envs, *svcs;

            if (rv_check_cancelled(o->cancel, error)) {
                g_ptr_array_unref(projects);
                goto cancelled;
            }
            envs = rw_list_environments(o->client, pr->id, &local);
            svcs = envs != NULL ? rw_list_services(o->client, pr->id, &local) : NULL;
            if (envs == NULL || svcs == NULL) {
                if (envs != NULL)
                    g_ptr_array_unref(envs);
                if (is_fatal(local)) {
                    g_propagate_error(error, local);
                    g_ptr_array_unref(projects);
                    goto fail;
                }
                topology_report(t, ws->name, pr->name, "failed", local->message);
                g_error_free(local);
                local = NULL;
                continue;
            }

            {
                RvProject *rp = g_new0(RvProject, 1);

                rp->workspace_id = g_strdup(ws->id);
                rp->workspace = g_strdup(ws->name);
                rp->id = g_strdup(pr->id);
                rp->name = g_strdup(pr->name);
                rp->environments = envs;
                rp->services = svcs;
                g_ptr_array_add(t->projects, rp);
            }
        }
        g_ptr_array_unref(projects);
    }
    g_ptr_array_unref(workspaces);
    return t;

cancelled:
fail:
    g_ptr_array_unref(workspaces);
    rv_topology_free(t);
    return NULL;
}

guint
rv_topology_estimate(const RvTopology *t)
{
    guint requests = 0;

    for (guint i = 0; i < t->projects->len; i++) {
        const RvProject *p = t->projects->pdata[i];

        requests += p->environments->len * (1 + p->services->len);
    }
    return requests;
}

/* Read one environment into `out`. */
static gboolean
read_one(RwClient *client, const RvProject *p, const RwEntity *env,
         const RwEntity *service, GPtrArray *out, GError **error)
{
    GHashTable *table = rw_get_variables(client, p->id, env->id,
                                         service != NULL ? service->id : NULL,
                                         TRUE, error);
    GHashTableIter iter;
    gpointer name, value;

    if (table == NULL)
        return FALSE;
    g_hash_table_iter_init(&iter, table);
    while (g_hash_table_iter_next(&iter, &name, &value)) {
        g_ptr_array_add(out, rv_variable_new(p->workspace_id, p->workspace, p->id, p->name,
                                             env->id, env->name,
                                             service != NULL ? service->id : NULL,
                                             service != NULL ? service->name : "shared",
                                             name, value));
    }
    g_hash_table_unref(table);
    return TRUE;
}

static gint
compare_variables(gconstpointer a, gconstpointer b)
{
    const RvVariable *x = *(RvVariable *const *)a, *y = *(RvVariable *const *)b;
    gint c;

    if ((c = g_strcmp0(x->project, y->project)) != 0) return c;
    if ((c = g_strcmp0(x->environment, y->environment)) != 0) return c;
    if ((c = g_strcmp0(x->service, y->service)) != 0) return c;
    return g_strcmp0(x->name, y->name);
}

RvInventory *
rv_read_variables(RvTopology *t, const RvScanOptions *o, GError **error)
{
    RvInventory *inv = rv_inventory_new();
    guint total = rv_topology_estimate(t), done = 0;

    inv->workspaces = t->workspaces;
    for (guint i = 0; i < t->report->len; i++) {
        RvReportRow *src = t->report->pdata[i];

        rv_inventory_add_report(inv, src->workspace, src->project, src->status, src->detail);
    }

    for (guint i = 0; i < t->projects->len; i++) {
        const RvProject *p = t->projects->pdata[i];
        GPtrArray *found = g_ptr_array_new_with_free_func((GDestroyNotify)rv_variable_free);
        GError *local = NULL;
        gboolean ok = TRUE;

        for (guint e = 0; e < p->environments->len && ok; e++) {
            const RwEntity *env = p->environments->pdata[e];

            if (rv_check_cancelled(o->cancel, error)) {
                g_ptr_array_unref(found);
                goto cancelled;
            }
            rv_progress(o->progress, o->progress_data,
                        total > 0 ? (gdouble)done / total : 1.0,
                        "Reading %s, %s", p->name, env->name);
            ok = read_one(o->client, p, env, NULL, found, &local);
            done++;
            for (guint s = 0; s < p->services->len && ok; s++) {
                ok = read_one(o->client, p, env, p->services->pdata[s], found, &local);
                done++;
            }
        }

        if (!ok) {
            if (is_fatal(local)) {
                g_propagate_error(error, local);
                g_ptr_array_unref(found);
                goto fail;
            }
            /* Keep none of this project, so a half read project cannot mislead the plan. */
            rv_inventory_add_report(inv, p->workspace, p->name, "failed", local->message);
            g_error_free(local);
            g_ptr_array_unref(found);
            continue;
        }

        rv_inventory_add_report(inv, p->workspace, p->name, "done", "");
        inv->projects++;
        inv->environments += p->environments->len;
        inv->services += p->services->len;
        g_ptr_array_extend_and_steal(inv->variables, found);
    }

    g_ptr_array_sort(inv->variables, compare_variables);
    rv_progress(o->progress, o->progress_data, 1.0, "Read %u variables", inv->variables->len);
    return inv;

cancelled:
fail:
    rv_inventory_free(inv);
    return NULL;
}
