/*
 * Finds the application roles of Postgres services and every variable that holds
 * their passwords.
 *
 * Copyright © 2026, Wexford House <https://wexfordhouse.co.za/>
 * Copyright © 2026, Ross Cameron <ross@wexfordhouse.co.za>
 * Licensed under the GNU General Public License, version 3.
 */
#include "rv-private.h"

#include <string.h>

#define MIN_PASSWORD 12     /* shorter passwords are not searched for: too many false hits */

typedef struct {
    const RvVariable *anchor;   /* the POSTGRES_PASSWORD variable: project, environment, service */
    gchar *host_name;           /* the private DNS name without the domain, normalised */
    gchar *superuser;
} DbService;

typedef struct {
    DbService *db;
    gchar *role, *database, *host;
    guint port;
    gchar *password;
    gboolean conflicted;
    gchar *why;
    GPtrArray *consumers;       /* RvVariable*, borrowed */
} Candidate;

static void
db_service_free(DbService *d)
{
    g_free(d->host_name);
    g_free(d->superuser);
    g_free(d);
}

static void
candidate_free(Candidate *c)
{
    g_free(c->role);
    g_free(c->database);
    g_free(c->host);
    rw_secure_free(c->password);
    g_free(c->why);
    g_ptr_array_unref(c->consumers);
    g_free(c);
}

/* "My Postgres" and "my_postgres" both become "my-postgres", as in a private domain. */
static gchar *
normalise(const gchar *name)
{
    gchar *s = g_ascii_strdown(name, -1);

    for (gchar *p = s; *p != '\0'; p++) {
        if (*p == ' ' || *p == '_')
            *p = '-';
    }
    return s;
}

static gchar *
host_to_name(const gchar *host)
{
    gchar *n = normalise(host);

    if (g_str_has_suffix(n, ".railway.internal"))
        n[strlen(n) - strlen(".railway.internal")] = '\0';
    return n;
}

static gboolean
same_place(const RvVariable *a, const RvVariable *b)
{
    return g_strcmp0(a->project_id, b->project_id) == 0
           && g_strcmp0(a->environment_id, b->environment_id) == 0;
}

static gboolean
holds_password(const gchar *value, const gchar *password)
{
    gchar *encoded;
    gboolean found = strstr(value, password) != NULL;

    if (!found) {
        encoded = g_uri_escape_string(password, NULL, FALSE);
        found = strcmp(encoded, password) != 0 && strstr(value, encoded) != NULL;
        g_free(encoded);
    }
    return found;
}

static const gchar *
literal_in_service(const RvInventory *inv, const RvVariable *anchor, const gchar *name)
{
    for (guint i = 0; i < inv->variables->len; i++) {
        const RvVariable *v = inv->variables->pdata[i];

        if (same_place(v, anchor) && g_strcmp0(v->service_id, anchor->service_id) == 0
            && strcmp(v->name, name) == 0 && v->value[0] != '\0' && strstr(v->value, "${{") == NULL)
            return v->value;
    }
    return NULL;
}

static void
fail(Candidate *c, const gchar *why)
{
    if (!c->conflicted) {
        c->conflicted = TRUE;
        c->why = g_strdup(why);
    }
}

static void
demote(RvVariable *v, const gchar *reason)
{
    g_free(v->reason);
    v->reason = g_strdup(reason);
    v->category = RV_CAT_DATABASE;
}

void
rv_roles_discover(RvInventory *inv, const RvPolicy *policy)
{
    GPtrArray *services = g_ptr_array_new_with_free_func((GDestroyNotify)db_service_free);
    GPtrArray *candidates = g_ptr_array_new_with_free_func((GDestroyNotify)candidate_free);

    /* 1. Postgres services: they define POSTGRES_PASSWORD. */
    for (guint i = 0; i < inv->variables->len; i++) {
        const RvVariable *v = inv->variables->pdata[i];
        DbService *d;
        const gchar *user;

        if (v->service_id == NULL || strcmp(v->name, "POSTGRES_PASSWORD") != 0)
            continue;
        d = g_new0(DbService, 1);
        d->anchor = v;
        d->host_name = normalise(v->service);
        user = literal_in_service(inv, v, "POSTGRES_USER");
        d->superuser = g_strdup(user != NULL ? user : "postgres");
        g_ptr_array_add(services, d);
    }

    /* 2. Connection URLs on the private network that name a role other than the superuser. */
    for (guint i = 0; i < inv->variables->len; i++) {
        RvVariable *v = inv->variables->pdata[i];
        RvPgUrl *url;
        DbService *db = NULL;
        gchar *name;
        Candidate *c = NULL;

        if (v->category != RV_CAT_DATABASE)
            continue;
        url = rv_pgurl_parse(v->value);
        if (url == NULL)
            continue;
        name = host_to_name(url->host);
        for (guint s = 0; s < services->len && db == NULL; s++) {
            DbService *d = services->pdata[s];

            if (same_place(d->anchor, v) && strcmp(d->host_name, name) == 0)
                db = d;
        }
        g_free(name);
        if (db == NULL || strcmp(url->user, db->superuser) == 0 || strlen(url->password) < MIN_PASSWORD) {
            rv_pgurl_free(url);
            continue;
        }

        for (guint k = 0; k < candidates->len && c == NULL; k++) {
            Candidate *x = candidates->pdata[k];

            if (x->db == db && strcmp(x->role, url->user) == 0)
                c = x;
        }
        if (c == NULL) {
            c = g_new0(Candidate, 1);
            c->db = db;
            c->role = g_strdup(url->user);
            c->database = g_strdup(url->database);
            c->host = g_strdup(url->host);
            c->port = url->port;
            c->password = g_strdup(url->password);
            c->consumers = g_ptr_array_new();
            g_ptr_array_add(candidates, c);
        } else if (strcmp(c->password, url->password) != 0) {
            fail(c, "its connection URLs hold different passwords");
        } else if (g_strcmp0(c->database, url->database) != 0) {
            fail(c, "its connection URLs name different databases");
        }
        rv_pgurl_free(url);
    }

    /* 3. Everything that holds the password. All of it must be understood and in reach. */
    for (guint k = 0; k < candidates->len; k++) {
        Candidate *c = candidates->pdata[k];

        for (guint i = 0; i < inv->variables->len; i++) {
            RvVariable *v = inv->variables->pdata[i];

            if (v->value[0] == '\0' || !holds_password(v->value, c->password))
                continue;
            if (!same_place(v, c->db->anchor)) {
                fail(c, "the password is also used in another project or environment, which may be another server");
            } else if (v->category == RV_CAT_INI || v->category == RV_CAT_ROTATE
                       || v->category == RV_CAT_SHARED_MANUAL || v->category == RV_CAT_REFERENCE) {
                fail(c, "the password is inside a file or a shared value that this tool does not edit");
            } else if (rv_policy_is_pinned(policy, v)) {
                fail(c, "a variable that holds it is excluded by the policy");
            } else if (strchr(v->value, '\n') != NULL) {
                fail(c, "the password is inside a multi-line value");
            } else {
                g_ptr_array_add(c->consumers, v);
            }
        }
        if (c->consumers->len == 0)
            fail(c, "no variable could be rewritten");
    }

    /* 4. Rotatable roles go in the inventory. The others stay database credentials, with the reason. */
    for (guint k = 0; k < candidates->len; k++) {
        Candidate *c = candidates->pdata[k];
        gchar *reason;

        if (c->conflicted) {
            reason = g_strdup_printf("password of database role %s on %s, not rotated: %s", c->role,
                                     c->db->anchor->service, c->why);
            for (guint i = 0; i < inv->variables->len; i++) {
                RvVariable *v = inv->variables->pdata[i];

                if (v->category == RV_CAT_DATABASE && holds_password(v->value, c->password))
                    demote(v, reason);
            }
            g_free(reason);
            continue;
        }
        {
            RvRole *r = g_new0(RvRole, 1);
            const RvVariable *a = c->db->anchor;

            r->project_id = g_strdup(a->project_id);
            r->project = g_strdup(a->project);
            r->environment_id = g_strdup(a->environment_id);
            r->environment = g_strdup(a->environment);
            r->db_service_id = g_strdup(a->service_id);
            r->db_service = g_strdup(a->service);
            r->role = g_strdup(c->role);
            r->database = g_strdup(c->database);
            r->host = g_strdup(c->host);
            r->port = c->port;
            r->old_password = g_strdup(c->password);
            r->consumers = g_ptr_array_new();
            reason = g_strdup_printf("password of database role %s on %s, rotated with the role",
                                     c->role, a->service);
            for (guint i = 0; i < c->consumers->len; i++) {
                RvVariable *v = c->consumers->pdata[i];

                g_ptr_array_add(r->consumers, v);
                v->category = RV_CAT_DB_ROLE;
                g_free(v->reason);
                v->reason = g_strdup(reason);
            }
            g_free(reason);
            g_ptr_array_add(inv->roles, r);
        }
    }

    g_ptr_array_unref(candidates);
    g_ptr_array_unref(services);
}
