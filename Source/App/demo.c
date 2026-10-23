/*
 * Copyright © 2026, Wexford House <https://wexfordhouse.co.za/>
 * Copyright © 2026, Ross Cameron <ross@wexfordhouse.co.za>
 * Licensed under the GNU General Public License, version 3.
 */
#include "demo.h"

#include "rvcore.h"

#include <json-glib/json-glib.h>
#include <string.h>

/* Every name, id and value here is invented. */

typedef struct {
    GMutex      lock;
    GHashTable *store;           /* "project|env|service" -> (name -> value) */
    gchar      *failing_project;
    GHashTable *verifiers;       /* the fake Postgres: role -> SCRAM verifier */
} Demo;

static GHashTable *
bucket(Demo *d, const gchar *key)
{
    GHashTable *t = g_hash_table_lookup(d->store, key);

    if (t == NULL) {
        t = rw_variables_new();
        g_hash_table_insert(d->store, g_strdup(key), t);
    }
    return t;
}

static void
seed(Demo *d, const gchar *key, const gchar *name, const gchar *value)
{
    g_hash_table_replace(bucket(d, key), g_strdup(name), g_strdup(value));
}

#define SESSION "3b9e0c7a41d85f26e7a1c4b8d09f5236"
#define CSRF    "Xt4Lm9Qp2Wr7Zk3Nb8Vh5Jd6Fg1Sc0Ya"
#define SALT    "Qm7Zr3Tb9Lx2Wv5Kn8Hd4Jf6Gc1Sa0Pe"
#define DBPASS  "demo-db-pass-48151623"
#define DEMO_ADMIN_PW "demo-postgres-admin-pw-7391"
#define DEMO_ROLE_PW  "LedgerDemoPw4Xk9Lm2Qp7Zr"
#define XERO    "[xero]\nclient_id=DEMO0000\nclient_secret=demo-not-a-real-secret\nrefresh_token=demo"

gpointer
demo_new(void)
{
    Demo *d = g_new0(Demo, 1);

    g_mutex_init(&d->lock);
    d->store = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                     (GDestroyNotify)g_hash_table_unref);
    d->verifiers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    g_hash_table_insert(d->verifiers, g_strdup("ledger"), rv_scram_verifier(DEMO_ROLE_PW, NULL));

    /* shop-api: api (s1), worker (s2), web (s3) */
    seed(d, "p1|e1|shared", "APP_SALT", SALT);
    seed(d, "p1|e1|s1", "SESSION_SECRET", SESSION);
    seed(d, "p1|e1|s1", "CSRF_SECRET", CSRF);
    seed(d, "p1|e1|s1", "DATABASE_URL", "postgres://shop:" DBPASS "@postgres.railway.internal:5432/shop");
    seed(d, "p1|e1|s1", "PGPASSWORD", DBPASS);
    seed(d, "p1|e1|s1", "OPENAI_API_KEY", "sk-demo0000000000000000000000000000");
    seed(d, "p1|e1|s1", "NODE_ENV", "production");
    seed(d, "p1|e1|s1", "PORT", "8080");
    seed(d, "p1|e1|s1", "LOG_LEVEL", "info");
    seed(d, "p1|e1|s1", "SALT_REF", "${{shared.APP_SALT}}");
    seed(d, "p1|e1|s1", "DB_REF", "${{Postgres.DATABASE_URL}}");
    seed(d, "p1|e1|s1", "WORKER_URL", "http://worker.railway.internal:3000");
    seed(d, "p1|e1|s2", "SESSION_SECRET", SESSION);
    seed(d, "p1|e1|s2", "NODE_ENV", "production");
    seed(d, "p1|e1|s2", "OPENAI_API_KEY", "sk-demo0000000000000000000000000000");
    seed(d, "p1|e1|s2", "SHOP_DB_PASS", DBPASS);
    seed(d, "p1|e1|s3", "CSRF_SECRET", CSRF);
    seed(d, "p1|e1|s3", "NODE_ENV", "production");
    seed(d, "p1|e1|s3", "PUBLIC_URL", "https://shop.example.test");
    seed(d, "p1|e1|s3", "SALT_USE", "${{ shared.APP_SALT }}");

    /* billing also has a Postgres service (s9) and a role used two ways: a URL and a password. */
    seed(d, "p2|e2|s9", "POSTGRES_PASSWORD", DEMO_ADMIN_PW);
    seed(d, "p2|e2|s9", "POSTGRES_USER", "postgres");
    seed(d, "p2|e2|s9", "PGUSER", "postgres");
    seed(d, "p2|e2|s9", "PGPASSWORD", DEMO_ADMIN_PW);
    seed(d, "p2|e2|s9", "PGDATABASE", "railway");
    seed(d, "p2|e2|s4", "LEDGER_URL", "postgres://ledger:" DEMO_ROLE_PW "@postgres.railway.internal:5432/ledger?sslmode=require");
    seed(d, "p2|e2|s5", "LEDGER_PASSWORD", DEMO_ROLE_PW);
    seed(d, "p2|e2|s5", "LEDGER_USER", "ledger");

    /* billing: invoicing (s4), xero-sync (s5). The same session secret as shop-api. */
    bucket(d, "p2|e2|shared");
    seed(d, "p2|e2|s4", "SESSION_SECRET", SESSION);
    seed(d, "p2|e2|s4", "XERO_CONFIG", XERO);
    seed(d, "p2|e2|s4", "NODE_ENV", "production");
    seed(d, "p2|e2|s4", "STRIPE_SECRET_KEY", "sk_test_demo000000000000000000");
    seed(d, "p2|e2|s4", "MYSQL_URL", "mysql://billing:demo-mysql-pw@mysql.railway.internal:3306/billing");
    seed(d, "p2|e2|s5", "XERO_CONFIG", XERO);
    seed(d, "p2|e2|s5", "NODE_ENV", "production");
    seed(d, "p2|e2|s5", "SYNC_TOKEN", "SYNCtokenAbCdEf0123456789xyz");

    /* marketing-site: site (s6) */
    seed(d, "p3|e3|s6", "NODE_ENV", "production");
    seed(d, "p3|e3|s6", "GA_ID", "G-DEMO12345");
    seed(d, "p3|e3|s6", "CONTACT_EMAIL", "hello@example.test");
    bucket(d, "p3|e3|shared");

    /* reporting (second workspace): dashboard (s7), loader (s8) */
    seed(d, "p4|e4|s7", "NODE_ENV", "production");
    seed(d, "p4|e4|s7", "REDIS_URL", "redis://default:demo-redis-pw@redis.railway.internal:6379");
    seed(d, "p4|e4|s8", "NODE_ENV", "production");
    seed(d, "p4|e4|s8", "MARIADB_PASSWORD", "demo-mariadb-pw-2468");
    bucket(d, "p4|e4|shared");
    return d;
}

void
demo_free(gpointer demo)
{
    Demo *d = demo;

    if (d == NULL)
        return;
    g_hash_table_unref(d->store);
    g_hash_table_unref(d->verifiers);
    g_free(d->failing_project);
    g_mutex_clear(&d->lock);
    g_free(d);
}

void
demo_set_failing_project(gpointer demo, const gchar *project_id)
{
    Demo *d = demo;

    g_mutex_lock(&d->lock);
    g_free(d->failing_project);
    d->failing_project = g_strdup(project_id);
    g_mutex_unlock(&d->lock);
}

/* JSON helpers */

static void
add_entity(JsonBuilder *b, const gchar *id, const gchar *name)
{
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "id");
    json_builder_add_string_value(b, id);
    json_builder_set_member_name(b, "name");
    json_builder_add_string_value(b, name);
    json_builder_end_object(b);
}

static void
connection(JsonBuilder *b, const gchar *const *pairs)
{
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "edges");
    json_builder_begin_array(b);
    for (guint i = 0; pairs[i] != NULL; i += 2) {
        json_builder_begin_object(b);
        json_builder_set_member_name(b, "node");
        add_entity(b, pairs[i], pairs[i + 1]);
        json_builder_end_object(b);
    }
    json_builder_end_array(b);
    json_builder_set_member_name(b, "pageInfo");
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "hasNextPage");
    json_builder_add_boolean_value(b, FALSE);
    json_builder_end_object(b);
    json_builder_end_object(b);
}

static gchar *
to_text(JsonBuilder *b)
{
    JsonGenerator *g = json_generator_new();
    JsonNode *root = json_builder_get_root(b);
    gchar *text;

    json_generator_set_root(g, root);
    text = json_generator_to_data(g, NULL);
    json_node_unref(root);
    g_object_unref(g);
    return text;
}

static const gchar *
member(JsonObject *o, const gchar *name)
{
    return o != NULL && json_object_has_member(o, name) ? json_object_get_string_member(o, name) : NULL;
}

gboolean
demo_send(gpointer demo, const gchar *url, const gchar *token, const gchar *body,
          RwHttpResponse *response, GError **error)
{
    Demo *d = demo;
    JsonParser *parser = json_parser_new();
    JsonObject *request, *vars = NULL;
    const gchar *query;
    JsonBuilder *b = json_builder_new();
    gchar *reply = NULL;

    (void)url;
    (void)token;
    (void)error;
    g_usleep(110 * 1000);          /* so the working screen has something to show */
    g_mutex_lock(&d->lock);

    json_parser_load_from_data(parser, body, -1, NULL);
    request = json_node_get_object(json_parser_get_root(parser));
    query = json_object_get_string_member(request, "query");
    if (json_object_has_member(request, "variables"))
        vars = json_object_get_object_member(request, "variables");

    response->status = 200;
    response->retry_after = -1;
    response->rate_remaining = -1;
    response->rate_reset = -1;

    json_builder_begin_object(b);
    json_builder_set_member_name(b, "data");
    json_builder_begin_object(b);

    if (strstr(query, "me { workspaces")) {
        json_builder_set_member_name(b, "me");
        json_builder_begin_object(b);
        json_builder_set_member_name(b, "workspaces");
        json_builder_begin_array(b);
        add_entity(b, "w1", "Acme Trading");
        add_entity(b, "w2", "Acme Labs");
        json_builder_end_array(b);
        json_builder_end_object(b);
    } else if (strstr(query, "projects(workspaceId")) {
        const gchar *w1[] = { "p1", "shop-api", "p2", "billing", "p3", "marketing-site", NULL };
        const gchar *w2[] = { "p4", "reporting", NULL };

        json_builder_set_member_name(b, "projects");
        connection(b, g_strcmp0(member(vars, "workspaceId"), "w1") == 0 ? w1 : w2);
    } else if (strstr(query, "services(first")) {
        const gchar *p = member(vars, "projectId");
        const gchar *p1[] = { "s1", "api", "s2", "worker", "s3", "web", NULL };
        const gchar *p2[] = { "s4", "invoicing", "s5", "xero-sync", "s9", "Postgres", NULL };
        const gchar *p3[] = { "s6", "site", NULL };
        const gchar *p4[] = { "s7", "dashboard", "s8", "loader", NULL };

        json_builder_set_member_name(b, "project");
        json_builder_begin_object(b);
        json_builder_set_member_name(b, "services");
        connection(b, g_strcmp0(p, "p1") == 0 ? p1 : g_strcmp0(p, "p2") == 0 ? p2
                      : g_strcmp0(p, "p3") == 0 ? p3 : p4);
        json_builder_end_object(b);
    } else if (strstr(query, "environments(first")) {
        const gchar *p = member(vars, "projectId");
        const gchar *e1[] = { "e1", "production", NULL };
        const gchar *e2[] = { "e2", "production", NULL };
        const gchar *e3[] = { "e3", "production", NULL };
        const gchar *e4[] = { "e4", "production", NULL };

        json_builder_set_member_name(b, "project");
        json_builder_begin_object(b);
        json_builder_set_member_name(b, "environments");
        connection(b, g_strcmp0(p, "p1") == 0 ? e1 : g_strcmp0(p, "p2") == 0 ? e2
                      : g_strcmp0(p, "p3") == 0 ? e3 : e4);
        json_builder_end_object(b);
    } else if (strstr(query, "variables(projectId")) {
        const gchar *p = member(vars, "projectId");
        gchar *key = g_strdup_printf("%s|%s|%s", p, member(vars, "environmentId"),
                                     member(vars, "serviceId") != NULL ? member(vars, "serviceId") : "shared");
        GHashTable *t = bucket(d, key);
        GHashTableIter iter;
        gpointer k, v;

        g_free(key);
        if (d->failing_project != NULL && g_strcmp0(p, d->failing_project) == 0) {
            reply = g_strdup("{\"errors\":[{\"message\":\"Problem processing request\"}]}");
            goto finish;
        }
        json_builder_set_member_name(b, "variables");
        json_builder_begin_object(b);
        g_hash_table_iter_init(&iter, t);
        while (g_hash_table_iter_next(&iter, &k, &v)) {
            json_builder_set_member_name(b, k);
            json_builder_add_string_value(b, v);
        }
        json_builder_end_object(b);
    } else if (strstr(query, "variableCollectionUpsert")) {
        JsonObject *in = json_object_get_object_member(vars, "input");
        gchar *key = g_strdup_printf("%s|%s|%s", member(in, "projectId"), member(in, "environmentId"),
                                     member(in, "serviceId") != NULL ? member(in, "serviceId") : "shared");
        JsonObject *values = json_object_get_object_member(in, "variables");
        GList *names = json_object_get_members(values);

        for (GList *l = names; l != NULL; l = l->next)
            seed(d, key, l->data, json_object_get_string_member(values, l->data));
        g_list_free(names);
        g_free(key);
        json_builder_set_member_name(b, "variableCollectionUpsert");
        json_builder_add_boolean_value(b, TRUE);
    } else if (strstr(query, "serviceInstanceRedeploy")) {
        json_builder_set_member_name(b, "serviceInstanceRedeploy");
        json_builder_add_boolean_value(b, TRUE);
    }

    json_builder_end_object(b);
    json_builder_end_object(b);
    reply = to_text(b);

finish:
    response->body = reply;
    g_mutex_unlock(&d->lock);
    g_object_unref(b);
    g_object_unref(parser);
    return TRUE;
}

/* ---- A fake Postgres and a fake tunnel, so the database step can be shown ---------------- */

static gboolean
demo_login_ok(gpointer data, const gchar *host, guint port, const gchar *database,
              const gchar *user, const gchar *password, GError **error)
{
    Demo *d = data;
    const gchar *verifier;

    (void)host;
    (void)port;
    (void)database;
    g_usleep(150 * 1000);
    if (strcmp(user, "postgres") == 0 && strcmp(password, DEMO_ADMIN_PW) == 0)
        return TRUE;
    verifier = g_hash_table_lookup(d->verifiers, user);
    if (verifier != NULL && rv_scram_verify(password, verifier))
        return TRUE;
    g_set_error(error, RV_ERROR, RV_ERROR_DB_AUTH, "password authentication failed for user \"%s\"", user);
    return FALSE;
}

static gboolean
demo_set_password(gpointer data, const gchar *host, guint port, const gchar *database,
                  const gchar *admin_user, const gchar *admin_password, const gchar *role,
                  const gchar *verifier, GError **error)
{
    Demo *d = data;

    (void)host;
    (void)port;
    (void)database;
    g_usleep(300 * 1000);
    if (strcmp(admin_user, "postgres") != 0 || strcmp(admin_password, DEMO_ADMIN_PW) != 0) {
        g_set_error_literal(error, RV_ERROR, RV_ERROR_DB_AUTH, "the admin login was refused");
        return FALSE;
    }
    g_hash_table_replace(d->verifiers, g_strdup(role), g_strdup(verifier));
    return TRUE;
}

static gboolean
demo_tunnel_open(gpointer data, const gchar *project_id, const gchar *environment_id,
                 const gchar *service, guint *port, gpointer *handle, GError **error)
{
    (void)project_id;
    (void)environment_id;
    (void)service;
    (void)error;
    g_usleep(400 * 1000);
    *port = 54320;
    *handle = data;
    return TRUE;
}

static void
demo_tunnel_close(gpointer data, gpointer handle)
{
    (void)data;
    (void)handle;
}

static const RvDbOps demo_db = { demo_login_ok, demo_set_password };
static const RvTunnelOps demo_tunnel = { demo_tunnel_open, demo_tunnel_close };

const RvDbOps *demo_db_ops(void) { return &demo_db; }
const RvTunnelOps *demo_tunnel_ops(void) { return &demo_tunnel; }
