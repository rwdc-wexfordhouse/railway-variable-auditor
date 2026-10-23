/*
 * Copyright © 2026, Wexford House <https://wexfordhouse.co.za/>
 * Copyright © 2026, Ross Cameron <ross@wexfordhouse.co.za>
 * Licensed under the GNU General Public License, version 3.
 */
#include "rv-private.h"

#include <string.h>

#define POLICY_GROUP "policy"

struct _RvPolicy {
    GHashTable *skip_workspaces;     /* ids */
    GHashTable *never_rotate;        /* variable keys */
    GHashTable *credential_names;    /* upper case names */
    GHashTable *internal_names;      /* upper case names */
};

static GHashTable *
new_set(void)
{
    return g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
}

RvPolicy *
rv_policy_new(void)
{
    RvPolicy *policy = g_new0(RvPolicy, 1);

    policy->skip_workspaces = new_set();
    policy->never_rotate = new_set();
    policy->credential_names = new_set();
    policy->internal_names = new_set();
    return policy;
}

void
rv_policy_free(RvPolicy *policy)
{
    if (policy == NULL)
        return;
    g_hash_table_unref(policy->skip_workspaces);
    g_hash_table_unref(policy->never_rotate);
    g_hash_table_unref(policy->credential_names);
    g_hash_table_unref(policy->internal_names);
    g_free(policy);
}

/* The program was called railway-variable-cycler before it became the auditor. A policy
 * file left in the old folder is still used until a new one exists. */
gchar *
rv_policy_default_path(void)
{
    gchar *path = g_build_filename(g_get_user_config_dir(), "railway-variable-auditor",
                                   "policy.ini", NULL);
    gchar *old = g_build_filename(g_get_user_config_dir(), "railway-variable-cycler",
                                  "policy.ini", NULL);

    if (!g_file_test(path, G_FILE_TEST_EXISTS) && g_file_test(old, G_FILE_TEST_EXISTS)) {
        g_free(path);
        path = old;
    } else {
        g_free(old);
    }
    return path;
}

static void
load_list(GKeyFile *file, const gchar *key, GHashTable *set, gboolean upper)
{
    gsize n = 0;
    gchar **items = g_key_file_get_string_list(file, POLICY_GROUP, key, &n, NULL);

    for (gsize i = 0; items != NULL && i < n; i++) {
        if (items[i][0] != '\0')
            g_hash_table_add(set, upper ? g_ascii_strup(items[i], -1) : g_strdup(items[i]));
    }
    g_strfreev(items);
}

RvPolicy *
rv_policy_load(const gchar *path, GError **error)
{
    RvPolicy *policy = rv_policy_new();
    GKeyFile *file = g_key_file_new();
    GError *local = NULL;

    if (!g_key_file_load_from_file(file, path, G_KEY_FILE_NONE, &local)) {
        if (g_error_matches(local, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
            g_error_free(local);
            g_key_file_free(file);
            return policy;
        }
        g_set_error(error, RV_ERROR, RV_ERROR_IO, "Could not read the policy %s: %s",
                    path, local->message);
        g_error_free(local);
        g_key_file_free(file);
        rv_policy_free(policy);
        return NULL;
    }
    load_list(file, "skip_workspaces", policy->skip_workspaces, FALSE);
    load_list(file, "never_rotate", policy->never_rotate, FALSE);
    load_list(file, "credential_names", policy->credential_names, TRUE);
    load_list(file, "internal_names", policy->internal_names, TRUE);
    g_key_file_free(file);
    return policy;
}

static void
store_list(GKeyFile *file, const gchar *key, GHashTable *set)
{
    GPtrArray *items = g_ptr_array_new();
    GHashTableIter iter;
    gpointer k;

    g_hash_table_iter_init(&iter, set);
    while (g_hash_table_iter_next(&iter, &k, NULL))
        g_ptr_array_add(items, k);
    g_ptr_array_sort(items, (GCompareFunc)g_strcmp0);
    g_ptr_array_add(items, NULL);
    g_key_file_set_string_list(file, POLICY_GROUP, key,
                               (const gchar *const *)items->pdata, items->len - 1);
    g_ptr_array_free(items, TRUE);
}

gboolean
rv_policy_save(const RvPolicy *policy, const gchar *path, GError **error)
{
    GKeyFile *file = g_key_file_new();
    gchar *data, *dir;
    gsize length;
    gboolean ok;

    store_list(file, "skip_workspaces", policy->skip_workspaces);
    store_list(file, "never_rotate", policy->never_rotate);
    store_list(file, "credential_names", policy->credential_names);
    store_list(file, "internal_names", policy->internal_names);
    data = g_key_file_to_data(file, &length, NULL);
    g_key_file_free(file);

    dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0700);
    g_free(dir);
    ok = rv_write_file(path, data, length, TRUE, error);
    g_free(data);
    return ok;
}

gboolean
rv_policy_workspace_skipped(const RvPolicy *policy, const gchar *workspace_id)
{
    return policy != NULL && workspace_id != NULL
           && g_hash_table_contains(policy->skip_workspaces, workspace_id);
}

void
rv_policy_skip_workspace(RvPolicy *policy, const gchar *workspace_id, gboolean skip)
{
    if (skip)
        g_hash_table_add(policy->skip_workspaces, g_strdup(workspace_id));
    else
        g_hash_table_remove(policy->skip_workspaces, workspace_id);
}

gchar *
rv_policy_variable_key(const RvVariable *variable)
{
    return g_strdup_printf("%s/%s/%s/%s", variable->project, variable->environment,
                           variable->service, variable->name);
}

gboolean
rv_policy_is_pinned(const RvPolicy *policy, const RvVariable *variable)
{
    gchar *key;
    gboolean pinned;

    if (policy == NULL)
        return FALSE;
    key = rv_policy_variable_key(variable);
    pinned = g_hash_table_contains(policy->never_rotate, key);
    g_free(key);
    return pinned;
}

void
rv_policy_pin(RvPolicy *policy, const RvVariable *variable, gboolean pin)
{
    gchar *key = rv_policy_variable_key(variable);

    if (pin)
        g_hash_table_add(policy->never_rotate, key);
    else {
        g_hash_table_remove(policy->never_rotate, key);
        g_free(key);
    }
}

void
rv_policy_add_credential_name(RvPolicy *policy, const gchar *name)
{
    g_hash_table_add(policy->credential_names, g_ascii_strup(name, -1));
}

void
rv_policy_add_internal_name(RvPolicy *policy, const gchar *name)
{
    g_hash_table_add(policy->internal_names, g_ascii_strup(name, -1));
}

static gboolean
has_name(GHashTable *set, const gchar *name)
{
    gchar *upper = g_ascii_strup(name, -1);
    gboolean found = g_hash_table_contains(set, upper);

    g_free(upper);
    return found;
}

gboolean
rv_policy_is_credential_name(const RvPolicy *policy, const gchar *name)
{
    return policy != NULL && has_name(policy->credential_names, name);
}

gboolean
rv_policy_is_internal_name(const RvPolicy *policy, const gchar *name)
{
    return policy != NULL && has_name(policy->internal_names, name);
}
