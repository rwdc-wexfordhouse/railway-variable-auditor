/*
 * Copyright © 2026, Wexford House <https://wexfordhouse.co.za/>
 * Copyright © 2026, Ross Cameron <ross@wexfordhouse.co.za>
 * Licensed under the GNU General Public License, version 3.
 */
#include "rv-private.h"

RvVariable *
rv_variable_new(const gchar *workspace_id, const gchar *workspace,
                const gchar *project_id, const gchar *project,
                const gchar *environment_id, const gchar *environment,
                const gchar *service_id, const gchar *service,
                const gchar *name, const gchar *value)
{
    RvVariable *v = g_new0(RvVariable, 1);

    v->workspace_id = g_strdup(workspace_id);
    v->workspace = g_strdup(workspace);
    v->project_id = g_strdup(project_id);
    v->project = g_strdup(project);
    v->environment_id = g_strdup(environment_id);
    v->environment = g_strdup(environment);
    v->service_id = g_strdup(service_id);
    v->service = g_strdup(service != NULL ? service : "shared");
    v->name = g_strdup(name);
    v->value = g_strdup(value != NULL ? value : "");
    v->category = RV_CAT_UNCLASSIFIED;
    v->group = -1;
    return v;
}

void
rv_variable_free(RvVariable *v)
{
    if (v == NULL)
        return;
    g_free(v->workspace_id);
    g_free(v->workspace);
    g_free(v->project_id);
    g_free(v->project);
    g_free(v->environment_id);
    g_free(v->environment);
    g_free(v->service_id);
    g_free(v->service);
    g_free(v->name);
    rw_secure_free(v->value);
    g_free(v->reason);
    g_free(v);
}

void
rv_role_free(RvRole *r)
{
    if (r == NULL)
        return;
    g_free(r->project_id);
    g_free(r->project);
    g_free(r->environment_id);
    g_free(r->environment);
    g_free(r->db_service_id);
    g_free(r->db_service);
    g_free(r->role);
    g_free(r->database);
    g_free(r->host);
    rw_secure_free(r->old_password);
    g_ptr_array_unref(r->consumers);
    g_free(r);
}

static void
report_row_free(RvReportRow *row)
{
    g_free(row->workspace);
    g_free(row->project);
    g_free(row->status);
    g_free(row->detail);
    g_free(row);
}

RvInventory *
rv_inventory_new(void)
{
    RvInventory *inv = g_new0(RvInventory, 1);

    inv->variables = g_ptr_array_new_with_free_func((GDestroyNotify)rv_variable_free);
    inv->report = g_ptr_array_new_with_free_func((GDestroyNotify)report_row_free);
    inv->roles = g_ptr_array_new_with_free_func((GDestroyNotify)rv_role_free);
    inv->complete = TRUE;
    return inv;
}

void
rv_inventory_free(RvInventory *inv)
{
    if (inv == NULL)
        return;
    g_ptr_array_unref(inv->variables);
    g_ptr_array_unref(inv->report);
    g_ptr_array_unref(inv->roles);
    g_free(inv);
}

void
rv_inventory_add_report(RvInventory *inv, const gchar *workspace, const gchar *project,
                        const gchar *status, const gchar *detail)
{
    RvReportRow *row = g_new0(RvReportRow, 1);

    row->workspace = g_strdup(workspace);
    row->project = g_strdup(project);
    row->status = g_strdup(status);
    row->detail = g_strdup(detail != NULL ? detail : "");
    g_ptr_array_add(inv->report, row);
    if (g_strcmp0(status, "done") != 0)
        inv->complete = FALSE;
}
