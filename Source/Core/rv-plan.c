/*
 * Copyright © 2026, Wexford House <https://wexfordhouse.co.za/>
 * Copyright © 2026, Ross Cameron <ross@wexfordhouse.co.za>
 * Licensed under the GNU General Public License, version 3.
 */
#include "rv-private.h"

#include <string.h>

static void
plan_group_free(RvPlanGroup *g)
{
    g_free(g->label);
    g_ptr_array_unref(g->members);
    rw_secure_free(g->new_value);
    g_free(g);
}

static void
plan_role_free(RvPlanRole *r)
{
    g_free(r->label);
    rw_secure_free(r->new_password);
    g_ptr_array_unref(r->new_values);
    g_free(r);
}

void
rv_plan_free(RvPlan *plan)
{
    if (plan == NULL)
        return;
    g_ptr_array_unref(plan->groups);
    g_ptr_array_unref(plan->roles);
    g_free(plan);
}

RvPlan *
rv_plan_build(RvInventory *inv, gboolean accept_incomplete, GError **error)
{
    RvPlan *plan;

    if (!inv->complete && !accept_incomplete) {
        g_set_error_literal(error, RV_ERROR, RV_ERROR_STATE,
                            "The scan was incomplete. A secret shared with a place that "
                            "could not be read would be changed on one side only. Fix the "
                            "problem and scan again, or accept the risk.");
        return NULL;
    }

    plan = g_new0(RvPlan, 1);
    plan->inventory = inv;
    plan->groups = g_ptr_array_new_with_free_func((GDestroyNotify)plan_group_free);
    plan->roles = g_ptr_array_new_with_free_func((GDestroyNotify)plan_role_free);

    for (guint g = 0; g < inv->rotation_groups; g++) {
        RvPlanGroup *group = g_new0(RvPlanGroup, 1);
        GError *local = NULL;

        group->members = g_ptr_array_new();
        group->enabled = TRUE;
        for (guint i = 0; i < inv->variables->len; i++) {
            RvVariable *v = inv->variables->pdata[i];

            if (v->group == (gint)g)
                g_ptr_array_add(group->members, v);
        }
        if (group->members->len == 0) {
            plan_group_free(group);
            continue;
        }

        {
            RvVariable *first = group->members->pdata[0];

            if (group->members->len > 1)
                group->label = g_strdup_printf("%s / %s: %s.%s (and %u more)",
                                               first->project, first->environment,
                                               first->service, first->name,
                                               group->members->len - 1);
            else
                group->label = g_strdup_printf("%s / %s: %s.%s", first->project,
                                               first->environment, first->service,
                                               first->name);
            group->new_value = rv_generate_like(first->value, &local);
        }
        if (group->new_value == NULL) {
            g_propagate_prefixed_error(error, local, "Could not make a new value for %s: ",
                                       group->label);
            plan_group_free(group);
            rv_plan_free(plan);
            return NULL;
        }
        g_ptr_array_add(plan->groups, group);
    }

    for (guint k = 0; k < inv->roles->len; k++) {
        const RvRole *role = inv->roles->pdata[k];
        RvPlanRole *pr = g_new0(RvPlanRole, 1);
        GError *local = NULL;

        pr->role = role;
        pr->enabled = TRUE;
        pr->label = g_strdup_printf("%s / %s: password of role %s (%u variable%s)", role->project,
                                    role->environment, role->role, role->consumers->len,
                                    role->consumers->len == 1 ? "" : "s");
        pr->new_values = g_ptr_array_new_with_free_func((GDestroyNotify)rw_secure_free);
        pr->new_password = rv_random_string(
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789", 32, &local);
        for (guint i = 0; pr->new_password != NULL && i < role->consumers->len; i++) {
            const RvVariable *v = role->consumers->pdata[i];
            gchar *rewritten = rv_pgurl_replace_password(v->value, role->old_password, pr->new_password);

            if (rewritten == NULL) {
                g_set_error(&local, RV_ERROR, RV_ERROR_INVALID,
                            "The password of role %s could not be found in %s.", role->role, v->name);
                break;
            }
            g_ptr_array_add(pr->new_values, rewritten);
        }
        if (local != NULL || pr->new_values->len != role->consumers->len) {
            g_propagate_prefixed_error(error, local, "Could not plan role %s: ", role->role);
            plan_role_free(pr);
            rv_plan_free(plan);
            return NULL;
        }
        g_ptr_array_add(plan->roles, pr);
    }
    return plan;
}

GPtrArray *
rv_plan_changes(const RvPlan *plan)
{
    GPtrArray *changes = g_ptr_array_new_with_free_func(g_free);

    for (guint g = 0; g < plan->groups->len; g++) {
        const RvPlanGroup *group = plan->groups->pdata[g];

        if (!group->enabled)
            continue;
        for (guint i = 0; i < group->members->len; i++) {
            RvChange *c = g_new0(RvChange, 1);

            c->variable = group->members->pdata[i];
            c->new_value = group->new_value;
            g_ptr_array_add(changes, c);
        }
    }
    for (guint k = 0; k < plan->roles->len; k++) {
        const RvPlanRole *pr = plan->roles->pdata[k];

        if (!pr->enabled)
            continue;
        for (guint i = 0; i < pr->role->consumers->len; i++) {
            RvChange *c = g_new0(RvChange, 1);

            c->variable = pr->role->consumers->pdata[i];
            c->new_value = pr->new_values->pdata[i];
            g_ptr_array_add(changes, c);
        }
    }
    return changes;
}

void
rv_deploy_target_free(RvDeployTarget *t)
{
    if (t == NULL)
        return;
    g_free(t->project_id);
    g_free(t->project);
    g_free(t->environment_id);
    g_free(t->environment);
    g_free(t->service_id);
    g_free(t->service);
    g_free(t);
}

static void
add_target(GPtrArray *targets, GHashTable *seen, const RvVariable *v)
{
    gchar *key;
    RvDeployTarget *t;

    if (v->service_id == NULL)
        return;
    key = g_strdup_printf("%s|%s|%s", v->project_id, v->environment_id, v->service_id);
    if (g_hash_table_contains(seen, key)) {
        g_free(key);
        return;
    }
    g_hash_table_add(seen, key);

    t = g_new0(RvDeployTarget, 1);
    t->project_id = g_strdup(v->project_id);
    t->project = g_strdup(v->project);
    t->environment_id = g_strdup(v->environment_id);
    t->environment = g_strdup(v->environment);
    t->service_id = g_strdup(v->service_id);
    t->service = g_strdup(v->service);
    g_ptr_array_add(targets, t);
}

/* Services that use a changed variable, either as their own or by reference. */
GPtrArray *
rv_plan_deploy_targets(const RvPlan *plan)
{
    GPtrArray *targets = g_ptr_array_new_with_free_func((GDestroyNotify)rv_deploy_target_free);
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    GPtrArray *changes = rv_plan_changes(plan);
    const GPtrArray *all = plan->inventory->variables;

    for (guint i = 0; i < changes->len; i++) {
        const RvChange *c = changes->pdata[i];
        const RvVariable *changed = c->variable;
        gchar *escaped_service = g_regex_escape_string(changed->service, -1);
        gchar *escaped_name = g_regex_escape_string(changed->name, -1);
        gchar *pattern = g_strdup_printf("\\$\\{\\{\\s*%s\\.%s\\s*\\}\\}",
                                         escaped_service, escaped_name);
        GRegex *regex = g_regex_new(pattern, 0, 0, NULL);

        add_target(targets, seen, changed);
        for (guint j = 0; j < all->len && regex != NULL; j++) {
            const RvVariable *other = all->pdata[j];

            if (g_strcmp0(other->project_id, changed->project_id) == 0
                && g_strcmp0(other->environment_id, changed->environment_id) == 0
                && g_regex_match(regex, other->value, 0, NULL))
                add_target(targets, seen, other);
        }
        if (regex != NULL)
            g_regex_unref(regex);
        g_free(pattern);
        g_free(escaped_name);
        g_free(escaped_service);
    }
    g_ptr_array_unref(changes);
    g_hash_table_unref(seen);
    return targets;
}

guint
rv_plan_request_estimate(const RvPlan *plan)
{
    GPtrArray *changes = rv_plan_changes(plan);
    GPtrArray *targets = rv_plan_deploy_targets(plan);
    GHashTable *batches = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    guint n;

    for (guint i = 0; i < changes->len; i++) {
        const RvVariable *v = ((RvChange *)changes->pdata[i])->variable;

        g_hash_table_add(batches, g_strdup_printf("%s|%s|%s", v->project_id,
                                                  v->environment_id,
                                                  v->service_id != NULL ? v->service_id : "shared"));
    }
    /* a read and a write for each batch, then one redeploy per service */
    n = g_hash_table_size(batches) * 2 + targets->len;
    /* each database service: one read of its own login, then the tunnel and the SQL (no requests) */
    {
        GHashTable *dbs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

        for (guint k = 0; k < plan->roles->len; k++) {
            const RvPlanRole *pr = plan->roles->pdata[k];

            if (pr->enabled)
                g_hash_table_add(dbs, g_strdup_printf("%s|%s|%s", pr->role->project_id,
                                                      pr->role->environment_id, pr->role->db_service_id));
        }
        n += g_hash_table_size(dbs);
        g_hash_table_unref(dbs);
    }
    g_hash_table_unref(batches);
    g_ptr_array_unref(targets);
    g_ptr_array_unref(changes);
    return n;
}
