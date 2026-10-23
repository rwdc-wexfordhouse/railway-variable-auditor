/*
 * Copyright © 2026, Wexford House <https://wexfordhouse.co.za/>
 * Copyright © 2026, Ross Cameron <ross@wexfordhouse.co.za>
 * Licensed under the GNU General Public License, version 3.
 */
#include "rv-private.h"

#include <string.h>

static void
set_category(RvVariable *v, RvCategory category, const gchar *reason)
{
    v->category = category;
    g_free(v->reason);
    v->reason = g_strdup(reason);
}

/* ---- Individual tests ---------------------------------------------------- */

static gboolean
is_ini(const gchar *value)
{
    gchar **lines;
    gboolean section = FALSE;
    guint pairs = 0;

    if (strchr(value, '\n') == NULL)
        return FALSE;

    lines = g_strsplit(value, "\n", -1);
    for (guint i = 0; lines[i] != NULL; i++) {
        gchar *line = g_strstrip(lines[i]);
        gsize length = strlen(line);

        if (length > 2 && line[0] == '[' && line[length - 1] == ']') {
            section = TRUE;
        } else {
            const gchar *eq = strchr(line, '=');

            if (eq != NULL && eq != line) {
                gboolean key = TRUE;

                for (const gchar *c = line; c < eq; c++) {
                    if (!(g_ascii_isalnum(*c) || *c == '_' || *c == '.' || *c == '-'
                          || *c == ' '))
                        key = FALSE;
                }
                if (key)
                    pairs++;
            }
        }
    }
    g_strfreev(lines);
    return section || pairs >= 2;
}

static gboolean
is_database_name(const gchar *name)
{
    static const gchar *const db_words[] = {
        "PG", "POSTGRES", "POSTGRESQL", "MYSQL", "MARIADB", "REDIS", "MONGO", "MONGODB",
        "CLICKHOUSE", "RABBITMQ", "AMQP", "MSSQL", "SQLSERVER", "DATABASE", "DB",
        "COCKROACH", "ORACLE", "CASSANDRA", NULL
    };
    static const gchar *const credential_words[] = {
        "PASSWORD", "PASS", "PWD", "URL", "URI", "DSN", NULL
    };
    gchar *upper = g_ascii_strup(name, -1);
    gboolean found = FALSE;
    GString *current = g_string_new(NULL);
    gboolean has_db = FALSE, has_credential = FALSE;

    /* Words, and names that run the two together, such as PGPASSWORD or DBPASS. */
    for (const gchar *p = upper; ; p++) {
        if (*p != '\0' && g_ascii_isalnum(*p)) {
            g_string_append_c(current, *p);
        } else {
            for (guint i = 0; db_words[i] != NULL; i++) {
                if (strcmp(current->str, db_words[i]) == 0)
                    has_db = TRUE;
                else if (g_str_has_prefix(current->str, db_words[i])) {
                    for (guint j = 0; credential_words[j] != NULL; j++) {
                        if (g_str_has_suffix(current->str, credential_words[j]))
                            found = TRUE;
                    }
                }
            }
            for (guint j = 0; credential_words[j] != NULL; j++) {
                if (strcmp(current->str, credential_words[j]) == 0)
                    has_credential = TRUE;
            }
            g_string_truncate(current, 0);
            if (*p == '\0')
                break;
        }
    }
    found = found || (has_db && has_credential);
    g_string_free(current, TRUE);
    g_free(upper);
    return found;
}

/* scheme://user:password@host */
static gboolean
is_credential_url(const gchar *value)
{
    const gchar *scheme_end = strstr(value, "://");
    const gchar *at, *colon, *slash;

    if (scheme_end == NULL || scheme_end == value)
        return FALSE;
    for (const gchar *c = value; c < scheme_end; c++) {
        if (!(g_ascii_isalnum(*c) || *c == '+' || *c == '.' || *c == '-'))
            return FALSE;
    }
    slash = strchr(scheme_end + 3, '/');
    at = strchr(scheme_end + 3, '@');
    if (at == NULL || (slash != NULL && slash < at))
        return FALSE;
    colon = memchr(scheme_end + 3, ':', (gsize)(at - (scheme_end + 3)));
    return colon != NULL && colon + 1 < at && colon > scheme_end + 3;
}

static gboolean
is_hostname(const gchar *value)
{
    return strstr(value, ".railway.internal") != NULL
           || strstr(value, "RAILWAY_PRIVATE_DOMAIN") != NULL;
}

/* The whole value is one ${{ ... }} reference. */
static gboolean
is_pure_reference(const gchar *value)
{
    gsize length = strlen(value);
    const gchar *close;

    if (length < 5 || !g_str_has_prefix(value, "${{") || !g_str_has_suffix(value, "}}"))
        return FALSE;
    close = strstr(value + 3, "}}");
    return close == value + length - 2;
}

/* ---- Grouping ------------------------------------------------------------- */

static gchar *
location_key(const RvVariable *v)
{
    return g_strdup_printf("%s|%s", v->environment_id,
                           v->service_id != NULL ? v->service_id : "(shared)");
}

static guint
count_locations(GPtrArray *members)
{
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    guint n;

    for (guint i = 0; i < members->len; i++)
        g_hash_table_add(seen, location_key(members->pdata[i]));
    n = g_hash_table_size(seen);
    g_hash_table_unref(seen);
    return n;
}

static gboolean
any_member(GPtrArray *members, gboolean (*test)(const RvVariable *, gpointer), gpointer data)
{
    for (guint i = 0; i < members->len; i++) {
        if (test(members->pdata[i], data))
            return TRUE;
    }
    return FALSE;
}

static gboolean test_shared_level(const RvVariable *v, gpointer d) { (void)d; return v->service_id == NULL; }
static gboolean test_pinned(const RvVariable *v, gpointer d) { return rv_policy_is_pinned(d, v); }
static gboolean test_secretish(const RvVariable *v, gpointer d) { (void)d; return rv_looks_like_secret(v->name, v->value); }
static gboolean test_provider(const RvVariable *v, gpointer d) { (void)d; return rv_name_is_provider(v->name) || rv_value_has_provider_prefix(v->value); }

static gboolean
test_internal(const RvVariable *v, gpointer policy)
{
    return rv_name_is_internal_secret(v->name) || rv_policy_is_internal_name(policy, v->name);
}

/* Every member must pass, so one secret looking name cannot carry along a
 * variable with another purpose. */
static gboolean
all_members(GPtrArray *members, gboolean (*test)(const RvVariable *, gpointer), gpointer data)
{
    for (guint i = 0; i < members->len; i++) {
        if (!test(members->pdata[i], data))
            return FALSE;
    }
    return TRUE;
}

static void
decide_group(RvInventory *inv, GPtrArray *members, const RvPolicy *policy)
{
    RvVariable *first = members->pdata[0];
    guint locations = count_locations(members);
    RvCategory category;
    const gchar *reason;
    gchar *text = NULL;

    if (locations < 2 && !any_member(members, test_shared_level, NULL)) {
        for (guint i = 0; i < members->len; i++)
            set_category(members->pdata[i], RV_CAT_UNSHARED, "used by one service only");
        return;
    }

    if (any_member(members, test_pinned, (gpointer)policy)) {
        category = RV_CAT_SHARED_MANUAL;
        reason = "excluded by policy, so the whole shared group is left alone";
    } else if (!any_member(members, test_secretish, NULL)) {
        category = RV_CAT_SHARED_CONFIG;
        reason = "shared, but does not look like a secret";
    } else if (any_member(members, test_provider, NULL)) {
        category = RV_CAT_SHARED_MANUAL;
        reason = "looks like a key from another provider, rotate it at the provider";
    } else if (!all_members(members, test_internal, (gpointer)policy)) {
        category = RV_CAT_SHARED_MANUAL;
        reason = "not every name marks an internal secret, check it and rotate it by hand";
    } else if (!rv_format_supported(first->value)) {
        category = RV_CAT_SHARED_MANUAL;
        reason = "format cannot be regenerated safely, rotate it by hand";
    } else {
        category = RV_CAT_ROTATE;
        text = g_strdup_printf("shared secret, same value in %u places", locations);
        reason = text;
    }

    for (guint i = 0; i < members->len; i++) {
        RvVariable *v = members->pdata[i];

        set_category(v, category, reason);
        v->group = category == RV_CAT_ROTATE ? (gint)inv->rotation_groups : -1;
    }
    if (category == RV_CAT_ROTATE)
        inv->rotation_groups++;
    g_free(text);
}

void
rv_classify(RvInventory *inv, const RvPolicy *policy)
{
    GHashTable *database_values, *groups;

    inv->rotation_groups = 0;
    g_ptr_array_set_size(inv->roles, 0);

    /* Pass 1: the tests that look at one variable. */
    for (guint i = 0; i < inv->variables->len; i++) {
        RvVariable *v = inv->variables->pdata[i];

        v->group = -1;
        if (v->value[0] == '\0')
            set_category(v, RV_CAT_UNSHARED, "empty or unreadable value, it may be sealed");
        else if (is_ini(v->value))
            set_category(v, RV_CAT_INI,
                         "INI file content, may hold tokens for other systems, change by hand");
        else if (is_database_name(v->name) || rv_policy_is_credential_name(policy, v->name)
                 || is_credential_url(v->value))
            set_category(v, RV_CAT_DATABASE,
                         "database credential, use Regenerate on the database service");
        else if (is_hostname(v->value))
            set_category(v, RV_CAT_HOSTNAME, "names another container on the private network");
        else if (is_pure_reference(v->value))
            set_category(v, RV_CAT_REFERENCE, "a reference, the source variable owns the value");
        else
            set_category(v, RV_CAT_UNCLASSIFIED, NULL);
    }

    /* Pass 1b: passwords of application roles of Postgres services. They are rotated
     * with the role, so they leave the database-credential group. */
    rv_roles_discover(inv, policy);

    /* Pass 2: a pasted copy of a database credential is a database credential. */
    database_values = g_hash_table_new(g_str_hash, g_str_equal);
    for (guint i = 0; i < inv->variables->len; i++) {
        RvVariable *v = inv->variables->pdata[i];

        if (v->category == RV_CAT_DATABASE)
            g_hash_table_add(database_values, v->value);
    }
    for (guint i = 0; i < inv->variables->len; i++) {
        RvVariable *v = inv->variables->pdata[i];

        if (v->category == RV_CAT_UNCLASSIFIED && g_hash_table_contains(database_values, v->value))
            set_category(v, RV_CAT_DATABASE,
                         "pasted copy of a database credential, replace it with a reference");
    }
    g_hash_table_unref(database_values);

    /* Pass 3: group by value, anywhere in the scan. */
    groups = g_hash_table_new_full(g_str_hash, g_str_equal, NULL,
                                   (GDestroyNotify)g_ptr_array_unref);
    for (guint i = 0; i < inv->variables->len; i++) {
        RvVariable *v = inv->variables->pdata[i];
        GPtrArray *members;

        if (v->category != RV_CAT_UNCLASSIFIED)
            continue;
        members = g_hash_table_lookup(groups, v->value);
        if (members == NULL) {
            members = g_ptr_array_new();
            g_hash_table_insert(groups, v->value, members);
        }
        g_ptr_array_add(members, v);
    }

    /* Decide in a stable order, so group numbers do not change between runs. */
    {
        GPtrArray *ordered = g_ptr_array_new();

        for (guint i = 0; i < inv->variables->len; i++) {
            RvVariable *v = inv->variables->pdata[i];
            GPtrArray *members;

            if (v->category != RV_CAT_UNCLASSIFIED)
                continue;
            members = g_hash_table_lookup(groups, v->value);
            if (members != NULL && members->pdata[0] == v)
                g_ptr_array_add(ordered, members);
        }
        for (guint i = 0; i < ordered->len; i++)
            decide_group(inv, ordered->pdata[i], policy);
        g_ptr_array_free(ordered, TRUE);
    }

    g_hash_table_unref(groups);
}
