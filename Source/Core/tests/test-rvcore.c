/*
 * Unit tests for the engine. A fake, in-memory Railway replaces the network,
 * so scan, classify, plan, stage, deploy, resume and undo all run for real
 * without a Railway account.
 *
 * Copyright © 2026, Wexford House <https://wexfordhouse.co.za/>
 * Copyright © 2026, Ross Cameron <ross@wexfordhouse.co.za>
 * Licensed under the GNU General Public License, version 3.
 */
#include "rvcore.h"
#include "rv-private.h"

#include <glib/gstdio.h>
#include <string.h>

#define HEX "9f2c4e1ab7d34c5e8a1f0b6d2c3e4f57"
#define SALT "Zk3Pq9Lm2Xv8Rt5Wy1Nb7Hd4Jf6Gc0Sa"

/* ---- A fake Railway ------------------------------------------------------- */

typedef struct {
    GHashTable *store;          /* "project|env|service" -> (name -> value) */
    GPtrArray  *upserts;        /* "key" of each upsert call, in order */
    GPtrArray  *upsert_bodies;  /* the JSON of each upsert, for checks */
    GPtrArray  *redeploys;      /* service ids */
    guint       upsert_count;
    guint       fail_upsert_at; /* 1-based, 0 for never */
    const gchar *fail_read_project;
    const gchar *auth_fail_workspace;
    gboolean     with_db;       /* project beta also has a Postgres service and two consumers */
} Fake;

static GHashTable *
bucket(Fake *f, const gchar *key)
{
    GHashTable *t = g_hash_table_lookup(f->store, key);

    if (t == NULL) {
        t = rw_variables_new();
        g_hash_table_insert(f->store, g_strdup(key), t);
    }
    return t;
}

static void
seed(Fake *f, const gchar *key, const gchar *name, const gchar *value)
{
    g_hash_table_replace(bucket(f, key), g_strdup(name), g_strdup(value));
}

static const gchar *
stored(Fake *f, const gchar *key, const gchar *name)
{
    return g_hash_table_lookup(bucket(f, key), name);
}

static Fake *
fake_new(void)
{
    Fake *f = g_new0(Fake, 1);

    f->store = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                     (GDestroyNotify)g_hash_table_unref);
    f->upserts = g_ptr_array_new_with_free_func(g_free);
    f->upsert_bodies = g_ptr_array_new_with_free_func(g_free);
    f->redeploys = g_ptr_array_new_with_free_func(g_free);

    /* alpha / production: api, worker, cron.  beta: web.  gamma: demo. */
    seed(f, "p1|e1|s1", "SESSION_SECRET", HEX);
    seed(f, "p1|e1|s1", "DATABASE_URL", "postgres://u:pw@db.railway.internal:5432/app");
    seed(f, "p1|e1|s1", "PGPASSWORD", "hunter2hunter2hunter2");
    seed(f, "p1|e1|s1", "NODE_ENV", "production");
    seed(f, "p1|e1|s1", "OPENAI_API_KEY", "sk-abcdefghijklmnopqrstuvwxyz012345");
    seed(f, "p1|e1|s1", "XERO_CONFIG", "[xero]\nclient_id=a\nclient_secret=b");
    seed(f, "p1|e1|s1", "PORT", "3000");
    seed(f, "p1|e1|s1", "DB_REF", "${{Postgres.DATABASE_URL}}");
    seed(f, "p1|e1|s1", "SALT_REF", "${{shared.APP_SALT}}");
    seed(f, "p1|e1|s1", "INTERNAL_URL", "http://worker.railway.internal:8080");
    seed(f, "p1|e1|s2", "SESSION_SECRET", HEX);
    seed(f, "p1|e1|s2", "NODE_ENV", "production");
    seed(f, "p1|e1|s2", "OPENAI_API_KEY", "sk-abcdefghijklmnopqrstuvwxyz012345");
    seed(f, "p1|e1|s2", "PASSWORD_COPY", "hunter2hunter2hunter2");
    seed(f, "p1|e1|s5", "SALT", "${{ shared.APP_SALT }}");
    seed(f, "p1|e1|shared", "APP_SALT", SALT);
    seed(f, "p2|e2|s3", "COOKIE_SECRET", HEX);
    seed(f, "p2|e2|s3", "LOG_LEVEL", "debug");
    bucket(f, "p2|e2|shared");
    bucket(f, "p3|e3|s4");
    bucket(f, "p3|e3|shared");
    bucket(f, "p1|e1|s5");
    return f;
}

static void
fake_free(Fake *f)
{
    g_hash_table_unref(f->store);
    g_ptr_array_unref(f->upserts);
    g_ptr_array_unref(f->upsert_bodies);
    g_ptr_array_unref(f->redeploys);
    g_free(f);
}

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

/* { "<field>": { edges: [...], pageInfo } } wrapped in the given path. */
static void
connection(JsonBuilder *b, const gchar *const *ids_names)
{
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "edges");
    json_builder_begin_array(b);
    for (guint i = 0; ids_names[i] != NULL; i += 2) {
        json_builder_begin_object(b);
        json_builder_set_member_name(b, "node");
        add_entity(b, ids_names[i], ids_names[i + 1]);
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

static gboolean
fake_send(gpointer user_data, const gchar *url, const gchar *token, const gchar *body,
          RwHttpResponse *response, GError **error)
{
    Fake *f = user_data;
    JsonParser *parser = json_parser_new();
    JsonObject *request, *vars = NULL;
    const gchar *query;
    JsonBuilder *b = json_builder_new();
    gchar *data;
    GString *reply = g_string_new(NULL);

    (void)url;
    (void)token;
    (void)error;
    g_assert_true(json_parser_load_from_data(parser, body, -1, NULL));
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
        add_entity(b, "w1", "Work");
        add_entity(b, "w2", "Personal");
        json_builder_end_array(b);
        json_builder_end_object(b);
    } else if (strstr(query, "projects(workspaceId")) {
        const gchar *w = json_object_get_string_member(vars, "workspaceId");
        const gchar *w1[] = { "p1", "alpha", "p2", "beta", NULL };
        const gchar *w2[] = { "p3", "gamma", NULL };

        if (f->auth_fail_workspace != NULL && g_strcmp0(w, f->auth_fail_workspace) == 0) {
            g_string_assign(reply, "{\"errors\":[{\"message\":\"Not Authorized\"}]}");
            goto done_reply;
        }
        json_builder_set_member_name(b, "projects");
        connection(b, strcmp(w, "w1") == 0 ? w1 : w2);
    } else if (strstr(query, "services(first")) {
        const gchar *p = json_object_get_string_member(vars, "projectId");
        const gchar *p1[] = { "s1", "api", "s2", "worker", "s5", "cron", NULL };
        const gchar *p2[] = { "s3", "web", NULL };
        const gchar *p2db[] = { "s3", "web", "s9", "Postgres", "s10", "jobs", NULL };
        const gchar *p3[] = { "s4", "demo", NULL };

        json_builder_set_member_name(b, "project");
        json_builder_begin_object(b);
        json_builder_set_member_name(b, "services");
        connection(b, strcmp(p, "p1") == 0 ? p1 : strcmp(p, "p2") == 0 ? (f->with_db ? p2db : p2) : p3);
        json_builder_end_object(b);
    } else if (strstr(query, "environments(first")) {
        const gchar *p = json_object_get_string_member(vars, "projectId");
        const gchar *e1[] = { "e1", "production", NULL };
        const gchar *e2[] = { "e2", "production", NULL };
        const gchar *e3[] = { "e3", "production", NULL };

        json_builder_set_member_name(b, "project");
        json_builder_begin_object(b);
        json_builder_set_member_name(b, "environments");
        connection(b, strcmp(p, "p1") == 0 ? e1 : strcmp(p, "p2") == 0 ? e2 : e3);
        json_builder_end_object(b);
    } else if (strstr(query, "variables(projectId")) {
        const gchar *p = json_object_get_string_member(vars, "projectId");
        const gchar *e = json_object_get_string_member(vars, "environmentId");
        const gchar *s = json_object_has_member(vars, "serviceId")
                             ? json_object_get_string_member(vars, "serviceId") : "shared";
        gchar *key = g_strdup_printf("%s|%s|%s", p, e, s);
        GHashTable *t = bucket(f, key);
        GHashTableIter iter;
        gpointer k, v;

        g_free(key);
        if (f->fail_read_project != NULL && strcmp(p, f->fail_read_project) == 0) {
            g_string_assign(reply, "{\"errors\":[{\"message\":\"Problem processing request\"}]}");
            goto done_reply;
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
        const gchar *p = json_object_get_string_member(in, "projectId");
        const gchar *e = json_object_get_string_member(in, "environmentId");
        const gchar *s = json_object_has_member(in, "serviceId")
                             ? json_object_get_string_member(in, "serviceId") : "shared";
        gchar *key = g_strdup_printf("%s|%s|%s", p, e, s);
        JsonObject *values = json_object_get_object_member(in, "variables");
        GList *names;

        f->upsert_count++;
        g_ptr_array_add(f->upserts, g_strdup(key));
        g_ptr_array_add(f->upsert_bodies, g_strdup(body));
        if (f->fail_upsert_at != 0 && f->upsert_count == f->fail_upsert_at) {
            g_free(key);
            g_string_assign(reply, "{\"errors\":[{\"message\":\"Problem processing request\"}]}");
            goto done_reply;
        }
        names = json_object_get_members(values);
        for (GList *l = names; l != NULL; l = l->next)
            seed(f, key, l->data, json_object_get_string_member(values, l->data));
        g_list_free(names);
        g_free(key);
        json_builder_set_member_name(b, "variableCollectionUpsert");
        json_builder_add_boolean_value(b, TRUE);
    } else if (strstr(query, "serviceInstanceRedeploy")) {
        g_ptr_array_add(f->redeploys, g_strdup(json_object_get_string_member(vars, "serviceId")));
        json_builder_set_member_name(b, "serviceInstanceRedeploy");
        json_builder_add_boolean_value(b, TRUE);
    } else {
        g_error("fake Railway: unexpected query: %s", query);
    }

    json_builder_end_object(b);
    json_builder_end_object(b);
    data = to_text(b);
    g_string_assign(reply, data);
    g_free(data);

done_reply:
    response->body = g_strdup(reply->str);
    g_string_free(reply, TRUE);
    g_object_unref(b);
    g_object_unref(parser);
    return TRUE;
}

static RwClient *
make_client(Fake *f)
{
    RwClient *c = rw_client_new("test-token");

    rw_client_set_transport(c, fake_send, f, NULL);
    return c;
}

/* ---- Helpers -------------------------------------------------------------- */

static RvVariable *
find(RvInventory *inv, const gchar *project, const gchar *service, const gchar *name)
{
    for (guint i = 0; i < inv->variables->len; i++) {
        RvVariable *v = inv->variables->pdata[i];

        if (strcmp(v->project, project) == 0 && strcmp(v->service, service) == 0
            && strcmp(v->name, name) == 0)
            return v;
    }
    return NULL;
}

static RvInventory *
scan(RwClient *c, const RvPolicy *policy, GError **error)
{
    RvScanOptions o = { c, policy, NULL, NULL, NULL };
    RvTopology *t = rv_discover(&o, error);
    RvInventory *inv;

    if (t == NULL)
        return NULL;
    inv = rv_read_variables(t, &o, error);
    rv_topology_free(t);
    if (inv != NULL)
        rv_classify(inv, policy);
    return inv;
}

static void
rm_rf(const gchar *dir)
{
    GDir *d = g_dir_open(dir, 0, NULL);
    const gchar *name;

    while (d != NULL && (name = g_dir_read_name(d)) != NULL) {
        gchar *p = g_build_filename(dir, name, NULL);

        g_remove(p);
        g_free(p);
    }
    if (d != NULL)
        g_dir_close(d);
    g_rmdir(dir);
}

static gchar *
temp_dir(void)
{
    return g_dir_make_tmp("rvcore-test-XXXXXX", NULL);
}

static gboolean
is_hex32_lower(const gchar *s)
{
    if (s == NULL || strlen(s) != 32)
        return FALSE;
    for (; *s != '\0'; s++) {
        if (!(g_ascii_isdigit(*s) || (*s >= 'a' && *s <= 'f')))
            return FALSE;
    }
    return TRUE;
}

/* ---- Random and generation ------------------------------------------------ */

static void
test_random_string(void)
{
    gchar *a = rv_random_string("abc", 64, NULL);
    gchar *b = rv_random_string("0123456789abcdef", 32, NULL);
    gchar *c = rv_random_string("0123456789abcdef", 32, NULL);

    g_assert_cmpuint(strlen(a), ==, 64);
    g_assert_cmpuint(strspn(a, "abc"), ==, 64);
    g_assert_cmpstr(b, !=, c);
    rw_secure_free(a);
    rw_secure_free(b);
    rw_secure_free(c);
}

static void
test_random_is_unbiased(void)
{
    /* 3 does not divide 256, so a careless modulo would favour 'a'. */
    gchar *s = rv_random_string("abc", 30000, NULL);
    guint counts[3] = { 0, 0, 0 };

    for (const gchar *p = s; *p != '\0'; p++)
        counts[*p - 'a']++;
    for (guint i = 0; i < 3; i++) {
        g_assert_cmpuint(counts[i], >, 9400);
        g_assert_cmpuint(counts[i], <, 10600);
    }
    rw_secure_free(s);
}

static void
check_same_shape(const gchar *old_value)
{
    GError *error = NULL;
    gchar *fresh = rv_generate_like(old_value, &error);

    g_assert_no_error(error);
    g_assert_nonnull(fresh);
    g_assert_cmpuint(strlen(fresh), ==, strlen(old_value));
    g_assert_cmpstr(fresh, !=, old_value);
    for (gsize i = 0; i < strlen(old_value); i++) {
        if (!g_ascii_isalnum(old_value[i]))
            g_assert_cmpint(fresh[i], ==, old_value[i]);
    }
    rw_secure_free(fresh);
}

static void
test_generate_keeps_format(void)
{
    gchar *hex = rv_generate_like(HEX, NULL);
    gchar *upper = rv_generate_like("9F2C4E1AB7D34C5E8A1F0B6D2C3E4F57", NULL);
    gchar *digits = rv_generate_like("1234567890123456789", NULL);
    gchar *b64 = rv_generate_like("aGVsbG8gd29ybGQhIGhlbGxvIHdvcmxkIQ==", NULL);
    gchar *url = rv_generate_like("abcDEF123-_abcDEF123-_abcDEF", NULL);
    gchar *uuid = rv_generate_like("3f2504e0-4f89-41d3-9a0c-0305e82c3301", NULL);

    g_assert_true(is_hex32_lower(hex));
    {
        gchar *shouted = g_ascii_strup(upper, -1);

        g_assert_cmpstr(upper, ==, shouted);
        g_free(shouted);
    }
    g_assert_cmpuint(strspn(digits, "0123456789"), ==, 19);
    g_assert_true(g_str_has_suffix(b64, "=="));
    g_assert_cmpuint(strlen(b64), ==, 36);
    g_assert_cmpuint(strspn(url, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"), ==, strlen(url));
    g_assert_cmpuint(strlen(uuid), ==, 36);
    g_assert_cmpint(uuid[8], ==, '-');
    g_assert_cmpint(uuid[14], ==, '4');
    check_same_shape(SALT);
    rw_secure_free(hex);
    rw_secure_free(upper);
    rw_secure_free(digits);
    rw_secure_free(b64);
    rw_secure_free(url);
    rw_secure_free(uuid);
}

static void
test_generate_refuses_unknown_formats(void)
{
    const gchar *bad[] = {
        "short", "production", "has some spaces in it as a passphrase",
        "postgres://user:pass@host/db", "!!@@##$$%%^^&&**((a1b2c3d4",
        "sk-abcdefghijklmnopqrstuvwxyz012345", NULL
    };

    for (guint i = 0; bad[i] != NULL; i++) {
        GError *error = NULL;

        g_assert_false(rv_format_supported(bad[i]) && g_str_has_prefix(bad[i], "post"));
        if (!rv_format_supported(bad[i])) {
            g_assert_null(rv_generate_like(bad[i], &error));
            g_assert_error(error, RV_ERROR, RV_ERROR_INVALID);
            g_error_free(error);
        }
    }
    g_assert_false(rv_format_supported("short"));
    g_assert_false(rv_format_supported("postgres://user:pass@host/db"));
}

static void
test_looks_like_secret(void)
{
    g_assert_true(rv_looks_like_secret("SESSION_SECRET", "abcd1234efgh"));
    g_assert_true(rv_looks_like_secret("FOO", HEX));
    g_assert_false(rv_looks_like_secret("NODE_ENV", "production"));
    g_assert_false(rv_looks_like_secret("PORT", "3000"));
    g_assert_false(rv_looks_like_secret("BASE_URL", "https://example.test/some/long/path/here"));
    g_assert_false(rv_looks_like_secret("FOO", ""));
    g_assert_true(rv_name_is_provider("OPENAI_API_KEY"));
    g_assert_true(rv_name_is_provider("STRIPE_SECRET"));
    g_assert_false(rv_name_is_provider("SESSION_SECRET"));
    g_assert_true(rv_name_is_internal_secret("JWT_SIGNING_KEY"));
    g_assert_true(rv_value_has_provider_prefix("ghp_abcdef"));
    g_assert_false(rv_value_has_provider_prefix(HEX));
}

/* ---- CSV and files -------------------------------------------------------- */

static void
test_csv_round_trip(void)
{
    GString *out = g_string_new(NULL);
    const gchar *a[] = { "plain", "has,comma", "has \"quote\"", "line\nbreak", "", NULL };
    const gchar *b[] = { "x", "", "", "", "end", NULL };
    GPtrArray *rows;
    GError *error = NULL;

    rv_csv_append_row(out, a);
    rv_csv_append_row(out, b);
    rows = rv_csv_parse(out->str, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(rows->len, ==, 2);
    for (guint i = 0; a[i] != NULL; i++)
        g_assert_cmpstr(((gchar **)rows->pdata[0])[i], ==, a[i]);
    g_assert_cmpuint(g_strv_length(rows->pdata[1]), ==, 5);
    g_assert_cmpstr(((gchar **)rows->pdata[1])[4], ==, "end");
    g_ptr_array_unref(rows);
    g_string_free(out, TRUE);
}

static void
test_csv_parse_edge_cases(void)
{
    GError *error = NULL;
    GPtrArray *rows = rv_csv_parse("a,b\r\n\r\nc,d", &error);

    g_assert_no_error(error);
    g_assert_cmpuint(rows->len, ==, 2);
    g_ptr_array_unref(rows);

    g_assert_null(rv_csv_parse("a,\"unterminated", &error));
    g_assert_error(error, RV_ERROR, RV_ERROR_CSV);
    g_error_free(error);
}

#ifndef G_OS_WIN32
static void
test_private_file_is_owner_only(void)
{
    gchar *dir = temp_dir(), *path = g_build_filename(dir, "secret.csv", NULL);
    GStatBuf st;

    g_assert_true(rv_write_file(path, "a,b\n", -1, TRUE, NULL));
    g_assert_cmpint(g_stat(path, &st), ==, 0);
    g_assert_cmpint(st.st_mode & 0777, ==, 0600);
    g_free(path);
    rm_rf(dir);
    g_free(dir);
}
#endif

static void
test_policy_round_trip(void)
{
    gchar *dir = temp_dir(), *path = g_build_filename(dir, "sub", "policy.ini", NULL);
    RvPolicy *p = rv_policy_new(), *q;
    RvVariable *v = rv_variable_new("w1", "Work", "p1", "alpha", "e1", "production",
                                    "s1", "api", "SESSION_SECRET", HEX);
    GError *error = NULL;
    gchar *missing = g_build_filename(dir, "nothing.ini", NULL);

    rv_policy_skip_workspace(p, "w2", TRUE);
    rv_policy_pin(p, v, TRUE);
    rv_policy_add_credential_name(p, "my_db_pw");
    rv_policy_add_internal_name(p, "Magic_Token");
    g_assert_true(rv_policy_save(p, path, &error));
    g_assert_no_error(error);

    q = rv_policy_load(path, &error);
    g_assert_no_error(error);
    g_assert_true(rv_policy_workspace_skipped(q, "w2"));
    g_assert_false(rv_policy_workspace_skipped(q, "w1"));
    g_assert_true(rv_policy_is_pinned(q, v));
    g_assert_true(rv_policy_is_credential_name(q, "MY_DB_PW"));
    g_assert_true(rv_policy_is_internal_name(q, "magic_token"));
    rv_policy_pin(q, v, FALSE);
    g_assert_false(rv_policy_is_pinned(q, v));

    rv_policy_free(q);
    q = rv_policy_load(missing, &error);
    g_assert_no_error(error);
    g_assert_false(rv_policy_workspace_skipped(q, "w2"));
    rv_policy_free(q);
    rv_policy_free(p);
    rv_variable_free(v);
    g_free(missing);
    {
        gchar *sub = g_build_filename(dir, "sub", NULL);

        rm_rf(sub);
        g_free(sub);
    }
    g_free(path);
    rm_rf(dir);
    g_free(dir);
}

/* ---- Scan and classify ---------------------------------------------------- */

static void
test_scan_counts_and_estimate(void)
{
    Fake *f = fake_new();
    RwClient *c = make_client(f);
    RvScanOptions o = { c, NULL, NULL, NULL, NULL };
    GError *error = NULL;
    RvTopology *t = rv_discover(&o, &error);
    RvInventory *inv;

    g_assert_no_error(error);
    g_assert_cmpuint(t->workspaces, ==, 2);
    g_assert_cmpuint(t->projects->len, ==, 3);
    g_assert_cmpuint(rv_topology_estimate(t), ==, 8);
    inv = rv_read_variables(t, &o, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(inv->projects, ==, 3);
    g_assert_cmpuint(inv->environments, ==, 3);
    g_assert_cmpuint(inv->services, ==, 5);
    g_assert_cmpuint(inv->variables->len, ==, 18);
    g_assert_true(inv->complete);
    rv_inventory_free(inv);
    rv_topology_free(t);
    rw_client_free(c);
    fake_free(f);
}

static void
test_classify_groups(void)
{
    Fake *f = fake_new();
    RwClient *c = make_client(f);
    GError *error = NULL;
    RvInventory *inv = scan(c, NULL, &error);

    g_assert_no_error(error);

    g_assert_cmpint(find(inv, "alpha", "api", "SESSION_SECRET")->category, ==, RV_CAT_ROTATE);
    g_assert_cmpint(find(inv, "alpha", "worker", "SESSION_SECRET")->category, ==, RV_CAT_ROTATE);
    g_assert_cmpint(find(inv, "beta", "web", "COOKIE_SECRET")->category, ==, RV_CAT_ROTATE);
    g_assert_cmpint(find(inv, "alpha", "shared", "APP_SALT")->category, ==, RV_CAT_ROTATE);
    g_assert_cmpint(find(inv, "alpha", "api", "SESSION_SECRET")->group, ==,
                    find(inv, "beta", "web", "COOKIE_SECRET")->group);
    g_assert_cmpint(find(inv, "alpha", "api", "SESSION_SECRET")->group, !=,
                    find(inv, "alpha", "shared", "APP_SALT")->group);
    g_assert_cmpuint(inv->rotation_groups, ==, 2);

    g_assert_cmpint(find(inv, "alpha", "api", "DATABASE_URL")->category, ==, RV_CAT_DATABASE);
    g_assert_cmpint(find(inv, "alpha", "api", "PGPASSWORD")->category, ==, RV_CAT_DATABASE);
    g_assert_cmpint(find(inv, "alpha", "worker", "PASSWORD_COPY")->category, ==, RV_CAT_DATABASE);
    g_assert_nonnull(strstr(find(inv, "alpha", "worker", "PASSWORD_COPY")->reason, "pasted copy"));
    g_assert_cmpint(find(inv, "alpha", "api", "NODE_ENV")->category, ==, RV_CAT_SHARED_CONFIG);
    g_assert_cmpint(find(inv, "alpha", "api", "OPENAI_API_KEY")->category, ==, RV_CAT_SHARED_MANUAL);
    g_assert_cmpint(find(inv, "alpha", "worker", "OPENAI_API_KEY")->category, ==, RV_CAT_SHARED_MANUAL);
    g_assert_cmpint(find(inv, "alpha", "api", "XERO_CONFIG")->category, ==, RV_CAT_INI);
    g_assert_cmpint(find(inv, "alpha", "api", "INTERNAL_URL")->category, ==, RV_CAT_HOSTNAME);
    g_assert_cmpint(find(inv, "alpha", "api", "DB_REF")->category, ==, RV_CAT_REFERENCE);
    g_assert_cmpint(find(inv, "alpha", "api", "SALT_REF")->category, ==, RV_CAT_REFERENCE);
    g_assert_cmpint(find(inv, "alpha", "cron", "SALT")->category, ==, RV_CAT_REFERENCE);
    g_assert_cmpint(find(inv, "alpha", "api", "PORT")->category, ==, RV_CAT_UNSHARED);
    g_assert_cmpint(find(inv, "beta", "web", "LOG_LEVEL")->category, ==, RV_CAT_UNSHARED);

    rv_inventory_free(inv);
    rw_client_free(c);
    fake_free(f);
}

static void
test_classify_ini_rules(void)
{
    RvInventory *inv = rv_inventory_new();

    g_ptr_array_add(inv->variables, rv_variable_new("w", "W", "p", "P", "e", "E", "s", "S", "A",
                                                    "[section]\nanything"));
    g_ptr_array_add(inv->variables, rv_variable_new("w", "W", "p", "P", "e", "E", "s", "S", "B",
                                                    "one=1\ntwo=2"));
    g_ptr_array_add(inv->variables, rv_variable_new("w", "W", "p", "P", "e", "E", "s", "S", "C",
                                                    "one=1"));
    g_ptr_array_add(inv->variables, rv_variable_new("w", "W", "p", "P", "e", "E", "s", "S", "D",
                                                    "line one\nline two"));
    g_ptr_array_add(inv->variables, rv_variable_new("w", "W", "p", "P", "e", "E", "s", "S", "E", ""));
    rv_classify(inv, NULL);
    g_assert_cmpint(((RvVariable *)inv->variables->pdata[0])->category, ==, RV_CAT_INI);
    g_assert_cmpint(((RvVariable *)inv->variables->pdata[1])->category, ==, RV_CAT_INI);
    g_assert_cmpint(((RvVariable *)inv->variables->pdata[2])->category, ==, RV_CAT_UNSHARED);
    g_assert_cmpint(((RvVariable *)inv->variables->pdata[3])->category, ==, RV_CAT_UNSHARED);
    g_assert_cmpint(((RvVariable *)inv->variables->pdata[4])->category, ==, RV_CAT_UNSHARED);
    g_assert_nonnull(strstr(((RvVariable *)inv->variables->pdata[4])->reason, "sealed"));
    rv_inventory_free(inv);
}

static void
test_ini_wins_over_sharing(void)
{
    RvInventory *inv = rv_inventory_new();
    const gchar *ini = "[xero]\nclient_id=abc\nclient_secret=def";

    g_ptr_array_add(inv->variables, rv_variable_new("w", "W", "p", "P", "e", "E", "s1", "A", "X", ini));
    g_ptr_array_add(inv->variables, rv_variable_new("w", "W", "p", "P", "e", "E", "s2", "B", "X", ini));
    rv_classify(inv, NULL);
    g_assert_cmpint(((RvVariable *)inv->variables->pdata[0])->category, ==, RV_CAT_INI);
    g_assert_cmpint(((RvVariable *)inv->variables->pdata[1])->category, ==, RV_CAT_INI);
    g_assert_cmpuint(inv->rotation_groups, ==, 0);
    rv_inventory_free(inv);
}

static void
test_policy_pin_blocks_whole_group(void)
{
    Fake *f = fake_new();
    RwClient *c = make_client(f);
    RvPolicy *policy = rv_policy_new();
    RvInventory *first = scan(c, NULL, NULL);
    GError *error = NULL;
    RvInventory *inv;

    rv_policy_pin(policy, find(first, "beta", "web", "COOKIE_SECRET"), TRUE);
    rv_inventory_free(first);
    inv = scan(c, policy, &error);
    g_assert_no_error(error);
    g_assert_cmpint(find(inv, "alpha", "api", "SESSION_SECRET")->category, ==, RV_CAT_SHARED_MANUAL);
    g_assert_cmpint(find(inv, "alpha", "worker", "SESSION_SECRET")->category, ==, RV_CAT_SHARED_MANUAL);
    g_assert_cmpint(find(inv, "beta", "web", "COOKIE_SECRET")->category, ==, RV_CAT_SHARED_MANUAL);
    g_assert_cmpuint(inv->rotation_groups, ==, 1);
    rv_inventory_free(inv);
    rv_policy_free(policy);
    rw_client_free(c);
    fake_free(f);
}

static void
test_custom_credential_name(void)
{
    RvPolicy *policy = rv_policy_new();
    RvInventory *inv = rv_inventory_new();

    rv_policy_add_credential_name(policy, "LEDGER_LOGIN");
    g_ptr_array_add(inv->variables, rv_variable_new("w", "W", "p", "P", "e", "E", "s", "S",
                                                    "LEDGER_LOGIN", "something-else-entirely-1234"));
    rv_classify(inv, policy);
    g_assert_cmpint(((RvVariable *)inv->variables->pdata[0])->category, ==, RV_CAT_DATABASE);
    rv_inventory_free(inv);
    rv_policy_free(policy);
}

static void
test_incomplete_scan_is_isolated_and_blocks_plan(void)
{
    Fake *f = fake_new();
    RwClient *c = make_client(f);
    GError *error = NULL;
    RvInventory *inv;
    RvPlan *plan;

    f->fail_read_project = "p3";
    inv = scan(c, NULL, &error);
    g_assert_no_error(error);
    g_assert_false(inv->complete);
    g_assert_cmpuint(inv->projects, ==, 2);
    {
        gboolean found_failed = FALSE;

        for (guint i = 0; i < inv->report->len; i++) {
            RvReportRow *r = inv->report->pdata[i];

            if (r->project != NULL && strcmp(r->project, "gamma") == 0
                && strcmp(r->status, "failed") == 0)
                found_failed = TRUE;
        }
        g_assert_true(found_failed);
    }
    g_assert_null(rv_plan_build(inv, FALSE, &error));
    g_assert_error(error, RV_ERROR, RV_ERROR_STATE);
    g_clear_error(&error);
    plan = rv_plan_build(inv, TRUE, &error);
    g_assert_no_error(error);
    g_assert_nonnull(plan);
    rv_plan_free(plan);
    rv_inventory_free(inv);
    rw_client_free(c);
    fake_free(f);
}

static void
test_workspace_skipped_by_policy_and_by_role(void)
{
    Fake *f = fake_new();
    RwClient *c = make_client(f);
    RvPolicy *policy = rv_policy_new();
    GError *error = NULL;
    RvInventory *inv;

    rv_policy_skip_workspace(policy, "w2", TRUE);
    inv = scan(c, policy, &error);
    g_assert_no_error(error);
    g_assert_false(inv->complete);
    g_assert_cmpuint(inv->projects, ==, 2);
    rv_inventory_free(inv);
    rv_policy_free(policy);

    f->auth_fail_workspace = "w1";
    inv = scan(c, NULL, &error);
    g_assert_no_error(error);
    g_assert_false(inv->complete);
    g_assert_cmpuint(inv->projects, ==, 1);
    {
        gboolean skipped = FALSE;

        for (guint i = 0; i < inv->report->len; i++) {
            RvReportRow *r = inv->report->pdata[i];

            if (r->project == NULL && strcmp(r->workspace, "Work") == 0
                && strcmp(r->status, "skipped") == 0)
                skipped = TRUE;
        }
        g_assert_true(skipped);
    }
    rv_inventory_free(inv);
    rw_client_free(c);
    fake_free(f);
}

static void
test_scan_can_be_cancelled(void)
{
    Fake *f = fake_new();
    RwClient *c = make_client(f);
    GCancellable *cancel = g_cancellable_new();
    RvScanOptions o = { c, NULL, NULL, NULL, cancel };
    GError *error = NULL;

    g_cancellable_cancel(cancel);
    g_assert_null(rv_discover(&o, &error));
    g_assert_error(error, RV_ERROR, RV_ERROR_CANCELLED);
    g_error_free(error);
    g_object_unref(cancel);
    rw_client_free(c);
    fake_free(f);
}

/* ---- Plan, stage, deploy, undo -------------------------------------------- */

static void
test_plan_one_value_per_group(void)
{
    Fake *f = fake_new();
    RwClient *c = make_client(f);
    RvInventory *inv = scan(c, NULL, NULL);
    GError *error = NULL;
    RvPlan *plan = rv_plan_build(inv, FALSE, &error);

    g_assert_no_error(error);
    g_assert_cmpuint(plan->groups->len, ==, 2);
    {
        RvPlanGroup *hex = plan->groups->pdata[0];

        g_assert_cmpuint(hex->members->len, ==, 3);
        g_assert_true(is_hex32_lower(hex->new_value));
        g_assert_cmpstr(hex->new_value, !=, HEX);
    }
    /* a read and a write for each of four batches, then four redeploys */
    g_assert_cmpuint(rv_plan_request_estimate(plan), ==, 12);
    ((RvPlanGroup *)plan->groups->pdata[1])->enabled = FALSE;
    g_assert_cmpuint(rv_plan_request_estimate(plan), ==, 9);
    rv_plan_free(plan);
    rv_inventory_free(inv);
    rw_client_free(c);
    fake_free(f);
}

static RvPlan *
make_plan(Fake *f, RwClient **client, RvInventory **inventory)
{
    *client = make_client(f);
    *inventory = scan(*client, NULL, NULL);
    return rv_plan_build(*inventory, FALSE, NULL);
}

static void
test_stage_deploy_and_status(void)
{
    Fake *f = fake_new();
    RwClient *c;
    RvInventory *inv;
    RvPlan *plan = make_plan(f, &c, &inv);
    gchar *dir = temp_dir(), *run = rv_run_dir_new(dir, NULL);
    RvRunOptions o = { .client = c };
    GError *error = NULL;
    RvRunStatus st;
    gchar *text = NULL, *path;
    GPtrArray *rows;

    g_assert_nonnull(run);
    g_assert_true(rv_run_prepare(plan, run, &error));
    g_assert_no_error(error);
    g_assert_cmpuint(f->upsert_count, ==, 0);   /* prepare sends nothing */

    path = g_build_filename(run, "changes.csv", NULL);
    g_assert_true(g_file_get_contents(path, &text, NULL, NULL));
    rows = rv_csv_parse(text, NULL);
    g_assert_cmpuint(rows->len, ==, 5);          /* header and four changes */
    g_assert_cmpuint(g_strv_length(rows->pdata[0]), ==, 12);
    g_ptr_array_unref(rows);
    g_free(text);
#ifndef G_OS_WIN32
    {
        GStatBuf stb;

        g_assert_cmpint(g_stat(path, &stb), ==, 0);
        g_assert_cmpint(stb.st_mode & 0777, ==, 0600);
    }
#endif
    g_free(path);

    g_assert_true(rv_run_stage(run, &o, &error));
    g_assert_no_error(error);
    g_assert_cmpuint(f->upsert_count, ==, 4);
    g_assert_cmpuint(f->redeploys->len, ==, 0);  /* stage never redeploys */

    /* every member of the shared group now holds the same new value */
    {
        const gchar *a = stored(f, "p1|e1|s1", "SESSION_SECRET");
        const gchar *b = stored(f, "p1|e1|s2", "SESSION_SECRET");
        const gchar *w = stored(f, "p2|e2|s3", "COOKIE_SECRET");

        g_assert_cmpstr(a, ==, b);
        g_assert_cmpstr(a, ==, w);
        g_assert_cmpstr(a, !=, HEX);
        g_assert_true(is_hex32_lower(a));
        g_assert_cmpstr(stored(f, "p1|e1|shared", "APP_SALT"), !=, SALT);
    }
    /* untouched variables stay untouched */
    g_assert_cmpstr(stored(f, "p1|e1|s1", "PGPASSWORD"), ==, "hunter2hunter2hunter2");
    g_assert_cmpstr(stored(f, "p1|e1|s1", "OPENAI_API_KEY"), ==, "sk-abcdefghijklmnopqrstuvwxyz012345");

    /* each write suppressed redeploys and never replaced */
    for (guint i = 0; i < f->upsert_bodies->len; i++) {
        JsonParser *p = json_parser_new();
        JsonObject *in;

        json_parser_load_from_data(p, f->upsert_bodies->pdata[i], -1, NULL);
        in = json_object_get_object_member(json_object_get_object_member(
                json_node_get_object(json_parser_get_root(p)), "variables"), "input");
        g_assert_true(json_object_get_boolean_member(in, "skipDeploys"));
        g_assert_false(json_object_get_boolean_member(in, "replace"));
        g_object_unref(p);
    }

    g_assert_true(rv_run_status(run, &st, &error));
    g_assert_cmpuint(st.batches_total, ==, 4);
    g_assert_cmpuint(st.batches_done, ==, 4);
    g_assert_false(st.stage_failed);
    g_assert_cmpuint(st.deploys_total, ==, 4);
    g_assert_cmpuint(st.deploys_done, ==, 0);

    g_assert_true(rv_run_deploy(run, &o, &error));
    g_assert_no_error(error);
    g_assert_cmpuint(f->redeploys->len, ==, 4);
    {
        GHashTable *set = g_hash_table_new(g_str_hash, g_str_equal);

        for (guint i = 0; i < f->redeploys->len; i++)
            g_hash_table_add(set, f->redeploys->pdata[i]);
        g_assert_true(g_hash_table_contains(set, "s1"));
        g_assert_true(g_hash_table_contains(set, "s2"));
        g_assert_true(g_hash_table_contains(set, "s3"));
        g_assert_true(g_hash_table_contains(set, "s5"));   /* by reference only */
        g_hash_table_unref(set);
    }
    g_assert_true(rv_run_deploy(run, &o, &error));        /* idempotent */
    g_assert_cmpuint(f->redeploys->len, ==, 4);
    g_assert_true(rv_run_status(run, &st, &error));
    g_assert_cmpuint(st.deploys_done, ==, 4);

    rv_plan_free(plan);
    rv_inventory_free(inv);
    rw_client_free(c);
    fake_free(f);
    rm_rf(run);
    rm_rf(dir);
    g_free(run);
    g_free(dir);
}

static void
test_stage_stops_and_resumes(void)
{
    Fake *f = fake_new();
    RwClient *c;
    RvInventory *inv;
    RvPlan *plan = make_plan(f, &c, &inv);
    gchar *dir = temp_dir(), *run = rv_run_dir_new(dir, NULL);
    RvRunOptions o = { .client = c };
    GError *error = NULL;
    RvRunStatus st;

    g_assert_true(rv_run_prepare(plan, run, &error));
    f->fail_upsert_at = 3;
    rw_client_set_max_retries(c, 0);

    g_assert_false(rv_run_stage(run, &o, &error));
    g_assert_error(error, RV_ERROR, RV_ERROR_STOPPED);
    g_clear_error(&error);
    g_assert_cmpuint(f->upsert_count, ==, 3);        /* it stopped, it did not carry on */
    g_assert_true(rv_run_status(run, &st, NULL));
    g_assert_cmpuint(st.batches_done, ==, 2);
    g_assert_true(st.stage_failed);

    f->fail_upsert_at = 0;
    g_assert_true(rv_run_stage(run, &o, &error));
    g_assert_no_error(error);
    g_assert_cmpuint(f->upsert_count, ==, 5);        /* only the last two were sent again */
    g_assert_cmpuint(f->upserts->len, ==, 5);
    g_assert_cmpstr(f->upserts->pdata[0], !=, f->upserts->pdata[3]);
    g_assert_cmpstr(f->upserts->pdata[1], !=, f->upserts->pdata[3]);
    g_assert_cmpstr(f->upserts->pdata[2], ==, f->upserts->pdata[3]);   /* the failed one, retried */
    g_assert_true(rv_run_status(run, &st, NULL));
    g_assert_cmpuint(st.batches_done, ==, 4);
    g_assert_false(st.stage_failed);

    rv_plan_free(plan);
    rv_inventory_free(inv);
    rw_client_free(c);
    fake_free(f);
    rm_rf(run);
    rm_rf(dir);
    g_free(run);
    g_free(dir);
}

static void
test_undo_restores_and_respects_conflicts(void)
{
    Fake *f = fake_new();
    RwClient *c;
    RvInventory *inv;
    RvPlan *plan = make_plan(f, &c, &inv);
    gchar *dir = temp_dir(), *run = rv_run_dir_new(dir, NULL);
    RvRunOptions o = { .client = c };
    RvUndoOptions dry = { TRUE, FALSE, FALSE, NULL };
    RvUndoOptions real = { FALSE, FALSE, TRUE, NULL };
    RvUndoResult r;
    GError *error = NULL;
    gchar *report_path, *report = NULL;

    g_assert_true(rv_run_prepare(plan, run, &error));
    g_assert_true(rv_run_stage(run, &o, &error));
    g_assert_true(rv_run_deploy(run, &o, &error));
    g_assert_cmpuint(f->redeploys->len, ==, 4);

    /* a dry run changes nothing */
    f->upsert_count = 0;
    g_assert_true(rv_undo_run(run, &o, &dry, &r, &error));
    g_assert_no_error(error);
    g_assert_cmpuint(r.restored, ==, 4);
    g_assert_cmpuint(f->upsert_count, ==, 0);
    g_assert_cmpstr(stored(f, "p1|e1|s1", "SESSION_SECRET"), !=, HEX);

    /* someone changes one value after the run */
    seed(f, "p1|e1|s2", "SESSION_SECRET", "changed-by-someone-else-0000000000");

    g_assert_true(rv_undo_run(run, &o, &real, &r, &error));
    g_assert_no_error(error);
    g_assert_cmpuint(r.restored, ==, 3);
    g_assert_cmpuint(r.conflicts, ==, 1);
    g_assert_cmpstr(stored(f, "p1|e1|s1", "SESSION_SECRET"), ==, HEX);
    g_assert_cmpstr(stored(f, "p2|e2|s3", "COOKIE_SECRET"), ==, HEX);
    g_assert_cmpstr(stored(f, "p1|e1|shared", "APP_SALT"), ==, SALT);
    g_assert_cmpstr(stored(f, "p1|e1|s2", "SESSION_SECRET"), ==, "changed-by-someone-else-0000000000");
    g_assert_cmpuint(f->redeploys->len, >, 4);           /* restored services redeployed */

    report_path = g_build_filename(run, "undo-report.csv", NULL);
    g_assert_true(g_file_get_contents(report_path, &report, NULL, NULL));
    g_assert_nonnull(strstr(report, "conflict"));
    g_assert_null(strstr(report, HEX));                  /* no values in the report */
    g_free(report);
    g_free(report_path);

    /* repeating it is safe */
    g_assert_true(rv_undo_run(run, &o, &real, &r, &error));
    g_assert_cmpuint(r.restored, ==, 0);
    g_assert_cmpuint(r.already_original, ==, 3);
    g_assert_cmpuint(r.conflicts, ==, 1);

    /* and the conflict can be overwritten on request */
    real.overwrite_conflicts = TRUE;
    g_assert_true(rv_undo_run(run, &o, &real, &r, &error));
    g_assert_cmpuint(r.restored, ==, 1);
    g_assert_cmpstr(stored(f, "p1|e1|s2", "SESSION_SECRET"), ==, HEX);

    rv_plan_free(plan);
    rv_inventory_free(inv);
    rw_client_free(c);
    fake_free(f);
    rm_rf(run);
    rm_rf(dir);
    g_free(run);
    g_free(dir);
}

static void
test_undo_can_be_limited_to_projects(void)
{
    Fake *f = fake_new();
    RwClient *c;
    RvInventory *inv;
    RvPlan *plan = make_plan(f, &c, &inv);
    gchar *dir = temp_dir(), *run = rv_run_dir_new(dir, NULL);
    RvRunOptions o = { .client = c };
    GHashTable *only = g_hash_table_new(g_str_hash, g_str_equal);
    RvUndoOptions u = { FALSE, FALSE, FALSE, only };
    RvUndoResult r;

    g_hash_table_add(only, (gpointer)"p2");
    g_assert_true(rv_run_prepare(plan, run, NULL));
    g_assert_true(rv_run_stage(run, &o, NULL));
    g_assert_true(rv_undo_run(run, &o, &u, &r, NULL));
    g_assert_cmpuint(r.restored, ==, 1);
    g_assert_cmpuint(r.skipped, ==, 3);
    g_assert_cmpstr(stored(f, "p2|e2|s3", "COOKIE_SECRET"), ==, HEX);
    g_assert_cmpstr(stored(f, "p1|e1|s1", "SESSION_SECRET"), !=, HEX);

    g_hash_table_unref(only);
    rv_plan_free(plan);
    rv_inventory_free(inv);
    rw_client_free(c);
    fake_free(f);
    rm_rf(run);
    rm_rf(dir);
    g_free(run);
    g_free(dir);
}

static void
test_reports(void)
{
    Fake *f = fake_new();
    RwClient *c = make_client(f);
    RvInventory *inv = scan(c, NULL, NULL);
    gchar *dir = temp_dir();
    gchar *nc = g_build_filename(dir, "nc.csv", NULL), *rr = g_build_filename(dir, "rr.csv", NULL);
    gchar *text = NULL;
    GPtrArray *rows;

    g_assert_true(rv_write_not_changed(inv, nc, FALSE, NULL));
    g_assert_true(rv_write_run_report(inv, rr, NULL));

    g_assert_true(g_file_get_contents(nc, &text, NULL, NULL));
    g_assert_null(strstr(text, HEX));                       /* no secret values */
    g_assert_null(strstr(text, "hunter2"));
    g_assert_null(strstr(text, "sk-abc"));
    rows = rv_csv_parse(text, NULL);
    g_assert_cmpuint(rows->len, ==, 1 + 18 - 4);           /* everything except the four rotated */
    g_ptr_array_unref(rows);
    g_free(text);

    /* the scan command also lists what the tool could rotate, so the checklist is complete */
    g_assert_true(rv_write_not_changed(inv, nc, TRUE, NULL));
    g_assert_true(g_file_get_contents(nc, &text, NULL, NULL));
    rows = rv_csv_parse(text, NULL);
    g_assert_cmpuint(rows->len, ==, 1 + 18);
    g_assert_nonnull(strstr(text, "shared secret, same value in"));
    g_assert_null(strstr(text, HEX));
    g_ptr_array_unref(rows);
    g_free(text);

    g_assert_true(g_file_get_contents(rr, &text, NULL, NULL));
    rows = rv_csv_parse(text, NULL);
    g_assert_cmpuint(rows->len, >, 3);
    g_ptr_array_unref(rows);
    g_free(text);

    rv_inventory_free(inv);
    rw_client_free(c);
    fake_free(f);
    g_remove(nc);
    g_remove(rr);
    g_rmdir(dir);
    g_free(nc);
    g_free(rr);
    g_free(dir);
}

/* ---- SCRAM-SHA-256 ----------------------------------------------------------- */

static void
hmac_sha256(const guint8 *key, gsize key_len, const gchar *data, guint8 out[32])
{
    GHmac *h = g_hmac_new(G_CHECKSUM_SHA256, key, key_len);
    gsize len = 32;

    g_hmac_update(h, (const guchar *)data, -1);
    g_hmac_get_digest(h, out, &len);
    g_hmac_unref(h);
}

/* The worked example in RFC 7677: user "user", password "pencil", 4096 iterations. */
static void
test_scram_matches_rfc_7677(void)
{
    gsize salt_len = 0, n = 0;
    guchar *salt = g_base64_decode("W22ZaJ0SNY7soEsUEjb6gQ==", &salt_len);
    guint8 stored[32], server[32], signature[32], key_check[32];
    const gchar *auth_message =
        "n=user,r=rOprNGfwEbeRWgbNEkqO,"
        "r=rOprNGfwEbeRWgbNEkqO%hvYDpWUa2RaTCAfuxFIlj)hNlF$k0,"
        "s=W22ZaJ0SNY7soEsUEjb6gQ==,i=4096,c=biws,"
        "r=rOprNGfwEbeRWgbNEkqO%hvYDpWUa2RaTCAfuxFIlj)hNlF$k0";
    guchar *proof = g_base64_decode("dHzbZapWIk4jUhN+Ute9ytag9zjfMHgsqmmiz7AndVQ=", &n);
    gchar *server_signature, *sum_text;
    guint8 client_key[32];
    GChecksum *sum;
    gsize len = 32;

    rv_scram_derive("pencil", salt, salt_len, 4096, stored, server);

    /* ClientProof = ClientKey XOR ClientSignature, so the client key can be recovered from the
     * published proof, and it must hash to the StoredKey we computed. */
    hmac_sha256(stored, 32, auth_message, signature);
    for (guint i = 0; i < 32; i++)
        client_key[i] = proof[i] ^ signature[i];
    sum = g_checksum_new(G_CHECKSUM_SHA256);
    g_checksum_update(sum, client_key, 32);
    g_checksum_get_digest(sum, key_check, &len);
    g_checksum_free(sum);
    sum_text = g_base64_encode(key_check, 32);
    {
        gchar *stored_text = g_base64_encode(stored, 32);

        g_assert_cmpstr(sum_text, ==, stored_text);
        g_free(stored_text);
    }

    /* ServerSignature = HMAC(ServerKey, AuthMessage) */
    hmac_sha256(server, 32, auth_message, signature);
    server_signature = g_base64_encode(signature, 32);
    g_assert_cmpstr(server_signature, ==, "6rriTRBi23WpRR/wtup+mMhUZUn/dB5nLTJRsjl95G4=");

    g_free(server_signature);
    g_free(sum_text);
    g_free(proof);
    g_free(salt);
}

static void
test_scram_verifier_round_trip(void)
{
    GError *error = NULL;
    gchar *a = rv_scram_verifier("Kq8Zr2Lm9Xv4Tb7Wn3Hd5Jf6Gc1Sa0Pe", &error);
    gchar *b = rv_scram_verifier("Kq8Zr2Lm9Xv4Tb7Wn3Hd5Jf6Gc1Sa0Pe", &error);

    g_assert_no_error(error);
    g_assert_true(g_str_has_prefix(a, "SCRAM-SHA-256$4096:"));
    g_assert_cmpstr(a, !=, b);                                     /* a fresh salt each time */
    g_assert_null(strstr(a, "Kq8Zr2Lm9Xv4Tb7Wn3Hd5Jf6Gc1Sa0Pe"));   /* no plaintext in it */
    g_assert_true(rv_scram_verify("Kq8Zr2Lm9Xv4Tb7Wn3Hd5Jf6Gc1Sa0Pe", a));
    g_assert_true(rv_scram_verify("Kq8Zr2Lm9Xv4Tb7Wn3Hd5Jf6Gc1Sa0Pe", b));
    g_assert_false(rv_scram_verify("Kq8Zr2Lm9Xv4Tb7Wn3Hd5Jf6Gc1Sa0Pf", a));
    g_assert_false(rv_scram_verify("anything", "not a verifier"));
    g_assert_false(rv_scram_verify("anything", "SCRAM-SHA-256$0:AAAA$BBBB:CCCC"));
    g_assert_null(rv_scram_verifier("", &error));
    g_assert_error(error, RV_ERROR, RV_ERROR_INVALID);
    g_clear_error(&error);
    g_assert_null(rv_scram_verifier("p\xc3\xa4ssword", &error));
    g_assert_error(error, RV_ERROR, RV_ERROR_INVALID);
    g_error_free(error);
    g_free(a);
    g_free(b);
}

/* ---- PostgreSQL connection URLs ------------------------------------------------ */

static void
test_pgurl_parse(void)
{
    RvPgUrl *u = rv_pgurl_parse("postgres://shop:s3cretPw@db.railway.internal:5433/shopdb?sslmode=require");

    g_assert_nonnull(u);
    g_assert_cmpstr(u->user, ==, "shop");
    g_assert_cmpstr(u->password, ==, "s3cretPw");
    g_assert_cmpstr(u->host, ==, "db.railway.internal");
    g_assert_cmpuint(u->port, ==, 5433);
    g_assert_cmpstr(u->database, ==, "shopdb");
    rv_pgurl_free(u);

    u = rv_pgurl_parse("POSTGRESQL://app:a%40b%3Ac@Postgres.railway.internal/db");
    g_assert_nonnull(u);
    g_assert_cmpstr(u->password, ==, "a@b:c");                 /* percent-decoded */
    g_assert_cmpuint(u->port, ==, 5432);                       /* the default */
    g_assert_cmpstr(u->host, ==, "Postgres.railway.internal");
    rv_pgurl_free(u);

    u = rv_pgurl_parse("postgres://app:pw@host");
    g_assert_nonnull(u);
    g_assert_null(u->database);
    rv_pgurl_free(u);

    g_assert_null(rv_pgurl_parse("postgres://host/db"));                 /* no user */
    g_assert_null(rv_pgurl_parse("postgres://user@host/db"));            /* no password */
    g_assert_null(rv_pgurl_parse("postgres://user:@host/db"));           /* empty password */
    g_assert_null(rv_pgurl_parse("mysql://user:pw@host/db"));            /* not Postgres */
    g_assert_null(rv_pgurl_parse("not a url"));
    g_assert_null(rv_pgurl_parse(NULL));
}

static void
test_pgurl_replace_password(void)
{
    gchar *a = rv_pgurl_replace_password("postgres://shop:OldPassw0rd12@db:5432/shop?sslmode=require",
                                         "OldPassw0rd12", "NewPassw0rd34");
    gchar *b = rv_pgurl_replace_password("OldPassw0rd12", "OldPassw0rd12", "NewPassw0rd34");
    gchar *c = rv_pgurl_replace_password("postgres://app:p%40ss%3Aword%21x@db/app", "p@ss:word!x", "NewPassw0rd34");
    gchar *d = rv_pgurl_replace_password("jdbc:postgresql://db/app?user=app&password=OldPassw0rd12&ssl=1",
                                         "OldPassw0rd12", "NewPassw0rd34");
    gchar *e = rv_pgurl_replace_password("twice OldPassw0rd12 and OldPassw0rd12", "OldPassw0rd12", "N");

    g_assert_cmpstr(a, ==, "postgres://shop:NewPassw0rd34@db:5432/shop?sslmode=require");
    g_assert_cmpstr(b, ==, "NewPassw0rd34");
    g_assert_cmpstr(c, ==, "postgres://app:NewPassw0rd34@db/app");       /* found, though it was encoded */
    g_assert_cmpstr(d, ==, "jdbc:postgresql://db/app?user=app&password=NewPassw0rd34&ssl=1");
    g_assert_cmpstr(e, ==, "twice N and N");
    g_assert_null(rv_pgurl_replace_password("postgres://shop:other@db/shop", "OldPassw0rd12", "x"));
    g_assert_null(rv_pgurl_replace_password("anything", "", "x"));
    g_assert_null(rv_pgurl_replace_password(NULL, "OldPassw0rd12", "x"));
    g_free(a); g_free(b); g_free(c); g_free(d); g_free(e);
}

/* ---- Database roles ------------------------------------------------------------- */

#define SUPER_PW  "SuperUserPw0123456789abcd"
#define ROLE_PW   "LedgerPw9Xk2Lm7Qp4Zr"

static RvVariable *
rvar(RvInventory *inv, const gchar *project, const gchar *env, const gchar *svc_id,
     const gchar *svc, const gchar *name, const gchar *value)
{
    RvVariable *v = rv_variable_new("w", "W", project, project, env, env, svc_id, svc, name, value);

    g_ptr_array_add(inv->variables, v);
    return v;
}

/* A Postgres service, its template login, and an application role used two ways. */
static RvInventory *
role_inventory(void)
{
    RvInventory *inv = rv_inventory_new();

    rvar(inv, "P", "E", "sdb", "Postgres", "POSTGRES_PASSWORD", SUPER_PW);
    rvar(inv, "P", "E", "sdb", "Postgres", "POSTGRES_USER", "postgres");
    rvar(inv, "P", "E", "sapi", "api", "DATABASE_URL", "postgres://postgres:" SUPER_PW "@postgres.railway.internal:5432/railway");
    rvar(inv, "P", "E", "sapi", "api", "LEDGER_URL", "postgres://ledger:" ROLE_PW "@postgres.railway.internal:5432/ledgerdb?sslmode=require");
    rvar(inv, "P", "E", "swk", "worker", "LEDGER_PASSWORD", ROLE_PW);
    rvar(inv, "P", "E", "swk", "worker", "LEDGER_USER", "ledger");
    return inv;
}

static RvVariable *
named(RvInventory *inv, const gchar *name)
{
    for (guint i = 0; i < inv->variables->len; i++) {
        RvVariable *v = inv->variables->pdata[i];

        if (strcmp(v->name, name) == 0)
            return v;
    }
    return NULL;
}

static void
test_roles_are_discovered(void)
{
    RvInventory *inv = role_inventory();
    const RvRole *r;

    rv_classify(inv, NULL);
    g_assert_cmpuint(inv->roles->len, ==, 1);
    r = inv->roles->pdata[0];
    g_assert_cmpstr(r->role, ==, "ledger");
    g_assert_cmpstr(r->db_service, ==, "Postgres");
    g_assert_cmpstr(r->database, ==, "ledgerdb");
    g_assert_cmpuint(r->port, ==, 5432);
    g_assert_cmpstr(r->old_password, ==, ROLE_PW);
    g_assert_cmpuint(r->consumers->len, ==, 2);                  /* the URL, and the pasted password */

    g_assert_cmpint(named(inv, "LEDGER_URL")->category, ==, RV_CAT_DB_ROLE);
    g_assert_cmpint(named(inv, "LEDGER_PASSWORD")->category, ==, RV_CAT_DB_ROLE);
    /* the template login is never touched, and the role name alone is not a secret */
    g_assert_cmpint(named(inv, "DATABASE_URL")->category, ==, RV_CAT_DATABASE);
    g_assert_cmpint(named(inv, "POSTGRES_PASSWORD")->category, ==, RV_CAT_DATABASE);
    g_assert_cmpint(named(inv, "LEDGER_USER")->category, ==, RV_CAT_UNSHARED);
    rv_inventory_free(inv);
}

static void
check_not_rotated(RvInventory *inv, const gchar *why)
{
    rv_classify(inv, NULL);
    g_assert_cmpuint(inv->roles->len, ==, 0);
    g_assert_cmpint(named(inv, "LEDGER_URL")->category, ==, RV_CAT_DATABASE);
    g_assert_nonnull(strstr(named(inv, "LEDGER_URL")->reason, why));
    rv_inventory_free(inv);
}

static void
test_roles_refuse_unsafe_cases(void)
{
    RvInventory *inv;

    /* the same password in another environment may be another server */
    inv = role_inventory();
    rvar(inv, "P", "OTHER", "sx", "api", "LEDGER_PASSWORD", ROLE_PW);
    check_not_rotated(inv, "another project or environment");

    /* ... and in another project */
    inv = role_inventory();
    rvar(inv, "Q", "E", "sx", "api", "COPY", "postgres://ledger:" ROLE_PW "@elsewhere/db");
    check_not_rotated(inv, "another project or environment");

    /* two URLs, two passwords */
    inv = role_inventory();
    rvar(inv, "P", "E", "swk", "worker", "LEDGER_URL_2", "postgres://ledger:DifferentPw99887766@postgres.railway.internal/ledgerdb");
    check_not_rotated(inv, "different passwords");

    /* the password sits inside a file this tool does not edit */
    inv = role_inventory();
    rvar(inv, "P", "E", "swk", "worker", "CONFIG", "[db]\nuser=ledger\npassword=" ROLE_PW);
    check_not_rotated(inv, "does not edit");

    /* ... or a multi-line value that is not an INI file */
    inv = role_inventory();
    rvar(inv, "P", "E", "swk", "worker", "NOTES", "line one\nthe password is " ROLE_PW "\nline three");
    check_not_rotated(inv, "multi-line");

    /* a variable that the policy says never to touch */
    {
        RvPolicy *policy = rv_policy_new();

        inv = role_inventory();
        rv_policy_pin(policy, named(inv, "LEDGER_PASSWORD"), TRUE);
        rv_classify(inv, policy);
        g_assert_cmpuint(inv->roles->len, ==, 0);
        g_assert_cmpint(named(inv, "LEDGER_URL")->category, ==, RV_CAT_DATABASE);
        g_assert_nonnull(strstr(named(inv, "LEDGER_URL")->reason, "excluded by the policy"));
        rv_inventory_free(inv);
        rv_policy_free(policy);
    }
}

static void
test_roles_ignore_what_they_cannot_place(void)
{
    RvInventory *inv;

    /* a host that is not a Postgres service in this environment */
    inv = rv_inventory_new();
    rvar(inv, "P", "E", "sdb", "Postgres", "POSTGRES_PASSWORD", SUPER_PW);
    rvar(inv, "P", "E", "sapi", "api", "URL", "postgres://ledger:" ROLE_PW "@some-other-host.railway.internal/db");
    rv_classify(inv, NULL);
    g_assert_cmpuint(inv->roles->len, ==, 0);
    g_assert_cmpint(named(inv, "URL")->category, ==, RV_CAT_DATABASE);
    rv_inventory_free(inv);

    /* a password too short to search for safely */
    inv = rv_inventory_new();
    rvar(inv, "P", "E", "sdb", "Postgres", "POSTGRES_PASSWORD", SUPER_PW);
    rvar(inv, "P", "E", "sapi", "api", "URL", "postgres://ledger:short1@postgres.railway.internal/db");
    rv_classify(inv, NULL);
    g_assert_cmpuint(inv->roles->len, ==, 0);
    rv_inventory_free(inv);

    /* the template's own login, written out in full */
    inv = rv_inventory_new();
    rvar(inv, "P", "E", "sdb", "Postgres", "POSTGRES_PASSWORD", SUPER_PW);
    rvar(inv, "P", "E", "sapi", "api", "URL", "postgres://postgres:" SUPER_PW "@postgres.railway.internal/db");
    rv_classify(inv, NULL);
    g_assert_cmpuint(inv->roles->len, ==, 0);
    rv_inventory_free(inv);

    /* a service name with a space matches its private domain */
    inv = rv_inventory_new();
    rvar(inv, "P", "E", "sdb", "My Postgres", "POSTGRES_PASSWORD", SUPER_PW);
    rvar(inv, "P", "E", "sapi", "api", "URL", "postgres://ledger:" ROLE_PW "@my-postgres.railway.internal/db");
    rv_classify(inv, NULL);
    g_assert_cmpuint(inv->roles->len, ==, 1);
    rv_inventory_free(inv);

    /* a password with characters that are percent-encoded in the URL is still found as plain text */
    inv = rv_inventory_new();
    rvar(inv, "P", "E", "sdb", "Postgres", "POSTGRES_PASSWORD", SUPER_PW);
    rvar(inv, "P", "E", "sapi", "api", "URL", "postgres://ledger:p%40ss%3Aword%21Xyz99@postgres.railway.internal/db");
    rvar(inv, "P", "E", "swk", "worker", "PW", "p@ss:word!Xyz99");
    rv_classify(inv, NULL);
    g_assert_cmpuint(inv->roles->len, ==, 1);
    g_assert_cmpuint(((RvRole *)inv->roles->pdata[0])->consumers->len, ==, 2);
    rv_inventory_free(inv);
}

static void
test_role_plan_rewrites_every_consumer(void)
{
    RvInventory *inv = role_inventory();
    RvPlan *plan;
    GError *error = NULL;
    RvPlanRole *pr;
    GPtrArray *changes, *targets;

    rv_classify(inv, NULL);
    plan = rv_plan_build(inv, FALSE, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(plan->roles->len, ==, 1);
    pr = plan->roles->pdata[0];
    g_assert_cmpuint(strlen(pr->new_password), ==, 32);
    g_assert_cmpstr(pr->new_password, !=, ROLE_PW);
    g_assert_cmpuint(pr->new_values->len, ==, 2);

    /* every value keeps its shape, with only the password swapped */
    {
        gchar *want_url = g_strdup_printf("postgres://ledger:%s@postgres.railway.internal:5432/ledgerdb?sslmode=require",
                                          pr->new_password);

        g_assert_cmpstr(pr->new_values->pdata[0], ==, want_url);
        g_assert_cmpstr(pr->new_values->pdata[1], ==, pr->new_password);
        g_free(want_url);
    }
    g_assert_null(strstr(pr->new_values->pdata[0], ROLE_PW));

    changes = rv_plan_changes(plan);
    g_assert_cmpuint(changes->len, ==, 2);
    targets = rv_plan_deploy_targets(plan);
    g_assert_cmpuint(targets->len, ==, 2);                         /* api and worker, not the database */
    /* the database service, one read of its login, plus 2 batches (read and write each) and 2 redeploys */
    g_assert_cmpuint(rv_plan_request_estimate(plan), ==, 1 + 2 * 2 + 2);

    pr->enabled = FALSE;
    g_ptr_array_unref(changes);
    changes = rv_plan_changes(plan);
    g_assert_cmpuint(changes->len, ==, 0);

    g_ptr_array_unref(changes);
    g_ptr_array_unref(targets);
    rv_plan_free(plan);
    rv_inventory_free(inv);
}

static void
test_role_files_and_checklist(void)
{
    RvInventory *inv = role_inventory();
    RvPlan *plan;
    gchar *dir = temp_dir(), *run = rv_run_dir_new(dir, NULL);
    gchar *path, *text = NULL;
    GPtrArray *rows;

    rv_classify(inv, NULL);
    plan = rv_plan_build(inv, FALSE, NULL);
    g_assert_true(rv_run_prepare(plan, run, NULL));

    path = g_build_filename(run, "roles.csv", NULL);
    g_assert_true(g_file_get_contents(path, &text, NULL, NULL));
    rows = rv_csv_parse(text, NULL);
    g_assert_cmpuint(rows->len, ==, 2);                            /* header and one role */
    g_assert_cmpuint(g_strv_length(rows->pdata[0]), ==, 12);
    g_assert_cmpstr(((gchar **)rows->pdata[1])[6], ==, "ledger");
    g_assert_cmpstr(((gchar **)rows->pdata[1])[10], ==, ROLE_PW);
    g_assert_cmpstr(((gchar **)rows->pdata[1])[11], ==, ((RvPlanRole *)plan->roles->pdata[0])->new_password);
#ifndef G_OS_WIN32
    {
        GStatBuf st;

        g_assert_cmpint(g_stat(path, &st), ==, 0);
        g_assert_cmpint(st.st_mode & 0777, ==, 0600);              /* it holds passwords */
    }
#endif
    g_ptr_array_unref(rows);
    rw_secure_free(text);
    g_free(path);

    /* the changed variables are in changes.csv, and not on the checklist */
    path = g_build_filename(run, "changes.csv", NULL);
    g_assert_true(g_file_get_contents(path, &text, NULL, NULL));
    g_assert_nonnull(strstr(text, "LEDGER_URL"));
    g_assert_nonnull(strstr(text, "LEDGER_PASSWORD"));
    rw_secure_free(text);
    g_free(path);
    path = g_build_filename(run, "not-changed.csv", NULL);
    g_assert_true(g_file_get_contents(path, &text, NULL, NULL));
    g_assert_null(strstr(text, "LEDGER_URL"));
    g_assert_nonnull(strstr(text, "DATABASE_URL"));               /* the template login stays on it */
    g_assert_null(strstr(text, ROLE_PW));
    g_free(text);
    g_free(path);

    rv_plan_free(plan);
    rv_inventory_free(inv);
    rm_rf(run);
    rm_rf(dir);
    g_free(run);
    g_free(dir);
}

static void
test_switched_off_role_stays_on_the_checklist(void)
{
    RvInventory *inv = role_inventory();
    RvPlan *plan;
    gchar *dir = temp_dir(), *run = rv_run_dir_new(dir, NULL);
    gchar *path, *text = NULL;

    rv_classify(inv, NULL);
    plan = rv_plan_build(inv, FALSE, NULL);
    ((RvPlanRole *)plan->roles->pdata[0])->enabled = FALSE;
    g_assert_true(rv_run_prepare(plan, run, NULL));

    path = g_build_filename(run, "not-changed.csv", NULL);
    g_assert_true(g_file_get_contents(path, &text, NULL, NULL));
    g_assert_nonnull(strstr(text, "LEDGER_URL"));
    g_assert_nonnull(strstr(text, "you excluded this role"));
    g_assert_null(strstr(text, ROLE_PW));
    g_free(text);
    g_free(path);
    path = g_build_filename(run, "roles.csv", NULL);
    g_assert_true(g_file_get_contents(path, &text, NULL, NULL));
    g_assert_null(strstr(text, "ledger"));                         /* nothing to do for it */
    rw_secure_free(text);
    g_free(path);

    rv_plan_free(plan);
    rv_inventory_free(inv);
    rm_rf(run);
    rm_rf(dir);
    g_free(run);
    g_free(dir);
}

static void
test_policy_old_folder(void)
{
    gchar *home = temp_dir(), *old_dir = g_build_filename(home, "railway-variable-cycler", NULL);
    gchar *old = g_build_filename(old_dir, "policy.ini", NULL), *path;
    gchar *new_dir = g_build_filename(home, "railway-variable-auditor", NULL);

    g_setenv("XDG_CONFIG_HOME", home, TRUE);
    g_reload_user_special_dirs_cache();

    path = rv_policy_default_path();               /* nothing exists: the new folder */
    g_assert_true(g_str_has_suffix(path, "railway-variable-auditor/policy.ini"));
    g_free(path);

    g_assert_cmpint(g_mkdir_with_parents(old_dir, 0700), ==, 0);
    g_assert_true(g_file_set_contents(old, "[policy]\n", -1, NULL));
    path = rv_policy_default_path();               /* only the old file exists: use it */
    g_assert_true(g_str_has_suffix(path, "railway-variable-cycler/policy.ini"));
    g_free(path);

    g_assert_cmpint(g_mkdir_with_parents(new_dir, 0700), ==, 0);
    {
        gchar *fresh = g_build_filename(new_dir, "policy.ini", NULL);

        g_assert_true(g_file_set_contents(fresh, "[policy]\n", -1, NULL));
        g_free(fresh);
    }
    path = rv_policy_default_path();               /* both exist: the new one wins */
    g_assert_true(g_str_has_suffix(path, "railway-variable-auditor/policy.ini"));
    g_free(path);

    g_unsetenv("XDG_CONFIG_HOME");
    g_reload_user_special_dirs_cache();
    rm_rf(home);
    g_free(new_dir);
    g_free(old);
    g_free(old_dir);
    g_free(home);
}

static void
test_audit_default_folder(void)
{
    gchar *home = temp_dir(), *desktop = g_build_filename(home, "Desktop", NULL), *folder;
    gchar *config = g_build_filename(home, ".config", NULL);

    g_setenv("HOME", home, TRUE);
    g_setenv("XDG_CONFIG_HOME", config, TRUE);
    g_unsetenv("XDG_DESKTOP_DIR");
    g_reload_user_special_dirs_cache();
    folder = rv_audit_default_folder();                        /* no Desktop folder: home */
    g_assert_cmpstr(folder, ==, home);
    g_free(folder);

    g_assert_cmpint(g_mkdir_with_parents(desktop, 0700), ==, 0);
    g_reload_user_special_dirs_cache();
    folder = rv_audit_default_folder();
    g_assert_cmpstr(folder, ==, desktop);                      /* Desktop exists: use it */
    g_free(folder);

    g_reload_user_special_dirs_cache();
    rm_rf(home);
    g_free(config);
    g_free(desktop);
    g_free(home);
}

static void
test_audit_save(void)
{
    RvInventory *inv = rv_inventory_new();
    gchar *base = temp_dir(), *dir, *audit, *report;
    GError *error = NULL;
    GStatBuf st;

    rvar(inv, "P", "E", "s1", "api", "TOKEN", "abc");
    rv_inventory_add_report(inv, "W", "P", "done", "");
    rv_classify(inv, NULL);
    dir = rv_audit_save(inv, base, &error);
    g_assert_no_error(error);
    g_assert_nonnull(dir);
    audit = g_build_filename(dir, "audit.csv", NULL);
    report = g_build_filename(dir, "run-report.csv", NULL);
    g_assert_true(g_file_test(audit, G_FILE_TEST_IS_REGULAR));
    g_assert_false(g_file_test(report, G_FILE_TEST_EXISTS));     /* the audit is one file */
    g_assert_cmpint(g_stat(audit, &st), ==, 0);
    g_assert_cmpuint(st.st_mode & 0777, ==, 0600);
    rm_rf(base);
    g_free(audit);
    g_free(report);
    g_free(dir);
    g_free(base);
    rv_inventory_free(inv);

    /* a folder that cannot be made is an error, not a crash */
    inv = rv_inventory_new();
    g_assert_null(rv_audit_save(inv, "/proc/nope/never", &error));
    g_assert_error(error, RV_ERROR, RV_ERROR_IO);
    g_clear_error(&error);
    rv_inventory_free(inv);
}

static void
test_audit_file(void)
{
    RvInventory *inv = rv_inventory_new();
    gchar *dir = temp_dir(), *path = g_build_filename(dir, "audit.csv", NULL);
    gchar *text = NULL;
    GPtrArray *rows;
    GError *error = NULL;

    /* a value with a comma, a quote and a line break must come back exactly */
    rvar(inv, "P", "E", "s1", "api", "TRICKY", "a,b \"quoted\"\nsecond line");
    rvar(inv, "P", "E", "s1", "api", "SESSION_SECRET", HEX);
    rvar(inv, "P", "E", "s2", "worker", "SESSION_SECRET", HEX);
    rvar(inv, "P", "E", "s1", "api", "REF", "${{Postgres.DATABASE_URL}}");
    rvar(inv, "P", "E", "s1", "api", "EMPTY", "");
    rv_classify(inv, NULL);

    g_assert_true(rv_write_audit(inv, path, &error));
    g_assert_no_error(error);
    g_assert_true(g_file_get_contents(path, &text, NULL, NULL));
    rows = rv_csv_parse(text, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(rows->len, ==, 1 + 5);                       /* the header, and every variable */
    g_assert_cmpuint(g_strv_length(rows->pdata[0]), ==, 8);
    g_assert_cmpstr(((gchar **)rows->pdata[0])[5], ==, "value");
    g_assert_nonnull(strstr(text, HEX));                          /* this file is meant to hold values */
    {
        gboolean tricky = FALSE, reference = FALSE, shared = FALSE;

        for (guint i = 1; i < rows->len; i++) {
            gchar **r = rows->pdata[i];

            if (strcmp(r[4], "TRICKY") == 0)
                tricky = strcmp(r[5], "a,b \"quoted\"\nsecond line") == 0;
            if (strcmp(r[4], "REF") == 0)
                reference = strcmp(r[5], "${{Postgres.DATABASE_URL}}") == 0 && strcmp(r[6], "reference") == 0;
            if (strcmp(r[4], "SESSION_SECRET") == 0)
                shared = strcmp(r[6], "shared") == 0;
        }
        g_assert_true(tricky);
        g_assert_true(reference);                                  /* shown as written, not resolved */
        g_assert_true(shared);                                     /* and the group is there to help */
    }
#ifndef G_OS_WIN32
    {
        GStatBuf st;

        g_assert_cmpint(g_stat(path, &st), ==, 0);
        g_assert_cmpint(st.st_mode & 0777, ==, 0600);              /* owner only */
    }
#endif
    g_ptr_array_unref(rows);
    rw_secure_free(text);
    g_free(path);
    rm_rf(dir);
    g_free(dir);
    rv_inventory_free(inv);
}

/* ---- Database roles, end to end, with a fake tunnel and a fake Postgres ---------------- */

#define DB_SUPER_PW  "SuperUserPw0123456789abcd"
#define DB_ROLE_PW   "LedgerPw9Xk2Lm7Qp4Zr"

typedef struct {
    GHashTable *verifiers;      /* role -> SCRAM verifier */
    gchar      *admin_password;
    guint       sets;           /* ALTER ROLE statements received */
    guint       logins;
    guint       opens, closes;
    gboolean    tunnel_fails;
    gboolean    break_next_set; /* the next password change stores something that does not match */
    gchar      *last_verifier;
    GString    *everything;     /* every string the fake saw as a password hash, to look for plaintext */
} FakeDb;

static gboolean
fdb_login(gpointer data, const gchar *host, guint port, const gchar *database, const gchar *user,
          const gchar *password, GError **error)
{
    FakeDb *d = data;
    const gchar *verifier;

    (void)database;
    g_assert_cmpstr(host, ==, "127.0.0.1");            /* only ever through the tunnel */
    g_assert_cmpuint(port, ==, 55432);
    d->logins++;
    if (strcmp(user, "postgres") == 0) {
        if (strcmp(password, d->admin_password) == 0)
            return TRUE;
    } else if ((verifier = g_hash_table_lookup(d->verifiers, user)) != NULL
               && rv_scram_verify(password, verifier)) {
        return TRUE;
    }
    g_set_error(error, RV_ERROR, RV_ERROR_DB_AUTH, "password authentication failed for user \"%s\"", user);
    return FALSE;
}

static gboolean
fdb_set(gpointer data, const gchar *host, guint port, const gchar *database, const gchar *admin_user,
        const gchar *admin_password, const gchar *role, const gchar *verifier, GError **error)
{
    FakeDb *d = data;

    (void)database;
    g_assert_cmpstr(host, ==, "127.0.0.1");
    g_assert_cmpuint(port, ==, 55432);
    g_assert_true(g_str_has_prefix(verifier, "SCRAM-SHA-256$"));          /* a hash, never plaintext */
    g_assert_cmpstr(verifier, !=, DB_ROLE_PW);
    g_assert_cmpstr(admin_user, ==, "postgres");
    d->sets++;
    g_string_append(d->everything, verifier);
    g_free(d->last_verifier);
    d->last_verifier = g_strdup(verifier);
    if (strcmp(admin_password, d->admin_password) != 0) {
        g_set_error(error, RV_ERROR, RV_ERROR_DB_AUTH, "admin login refused");
        return FALSE;
    }
    if (d->break_next_set) {
        gchar *other = rv_scram_verifier("a-completely-different-password", NULL);

        d->break_next_set = FALSE;
        g_hash_table_replace(d->verifiers, g_strdup(role), other);
        return TRUE;
    }
    g_hash_table_replace(d->verifiers, g_strdup(role), g_strdup(verifier));
    return TRUE;
}

static gboolean
ftun_open(gpointer data, const gchar *project_id, const gchar *environment_id, const gchar *service,
          guint *port, gpointer *handle, GError **error)
{
    FakeDb *d = data;

    g_assert_cmpstr(project_id, ==, "p2");
    g_assert_cmpstr(environment_id, ==, "e2");
    g_assert_cmpstr(service, ==, "Postgres");
    if (d->tunnel_fails) {
        g_set_error_literal(error, RV_ERROR, RV_ERROR_TUNNEL, "The tunnel to Postgres did not open.");
        return FALSE;
    }
    d->opens++;
    *port = 55432;
    *handle = d;
    return TRUE;
}

static void
ftun_close(gpointer data, gpointer handle)
{
    (void)handle;
    ((FakeDb *)data)->closes++;
}

static const RvDbOps fake_db_ops = { fdb_login, fdb_set };
static const RvTunnelOps fake_tunnel_ops = { ftun_open, ftun_close };

static FakeDb *
fdb_new(void)
{
    FakeDb *d = g_new0(FakeDb, 1);

    d->verifiers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    d->admin_password = g_strdup(DB_SUPER_PW);
    d->everything = g_string_new(NULL);
    g_hash_table_insert(d->verifiers, g_strdup("ledger"), rv_scram_verifier(DB_ROLE_PW, NULL));
    return d;
}

static void
fdb_free(FakeDb *d)
{
    g_hash_table_unref(d->verifiers);
    g_free(d->admin_password);
    g_free(d->last_verifier);
    g_string_free(d->everything, TRUE);
    g_free(d);
}

static RvRunOptions
role_options(RwClient *c, FakeDb *d)
{
    RvRunOptions o = { .client = c, .db = &fake_db_ops, .db_data = d, .tunnels = &fake_tunnel_ops, .tunnel_data = d };

    return o;
}

typedef struct {
    Fake *f;
    RwClient *c;
    RvInventory *inv;
    RvPlan *plan;
    FakeDb *d;
    gchar *dir, *run;
} Rig;

/* Scan, plan and prepare a run on a fake Railway where project beta has a Postgres service. */
static void
rig_setup(Rig *g)
{
    memset(g, 0, sizeof *g);
    g->f = fake_new();
    g->f->with_db = TRUE;
    seed(g->f, "p2|e2|s9", "POSTGRES_PASSWORD", DB_SUPER_PW);
    seed(g->f, "p2|e2|s9", "POSTGRES_USER", "postgres");
    seed(g->f, "p2|e2|s9", "PGUSER", "postgres");
    seed(g->f, "p2|e2|s9", "PGPASSWORD", DB_SUPER_PW);
    seed(g->f, "p2|e2|s9", "PGDATABASE", "railway");
    seed(g->f, "p2|e2|s3", "LEDGER_URL", "postgres://ledger:" DB_ROLE_PW "@postgres.railway.internal:5432/ledgerdb?sslmode=require");
    seed(g->f, "p2|e2|s10", "LEDGER_PASSWORD", DB_ROLE_PW);
    seed(g->f, "p2|e2|s10", "LEDGER_USER", "ledger");
    g->c = make_client(g->f);
    g->inv = scan(g->c, NULL, NULL);
    g->plan = rv_plan_build(g->inv, FALSE, NULL);
    g->d = fdb_new();
    g->dir = temp_dir();
    g->run = rv_run_dir_new(g->dir, NULL);
    g_assert_true(rv_run_prepare(g->plan, g->run, NULL));
}

static void
rig_teardown(Rig *g)
{
    rv_plan_free(g->plan);
    rv_inventory_free(g->inv);
    rw_client_free(g->c);
    fake_free(g->f);
    fdb_free(g->d);
    rm_rf(g->run);
    rm_rf(g->dir);
    g_free(g->run);
    g_free(g->dir);
}

static void
test_roles_change_password_then_deploy(void)
{
    Rig g;
    RvRunOptions o;
    GError *error = NULL;
    RvRunStatus st;

    rig_setup(&g);
    o = role_options(g.c, g.d);
    g_assert_cmpuint(g.plan->roles->len, ==, 1);

    g_assert_true(rv_run_stage(g.run, &o, &error));
    g_assert_no_error(error);
    /* the variables now hold the new password, and the database still has the old one */
    {
        const gchar *new_pw = ((RvPlanRole *)g.plan->roles->pdata[0])->new_password;
        gchar *want = g_strdup_printf("postgres://ledger:%s@postgres.railway.internal:5432/ledgerdb?sslmode=require", new_pw);

        g_assert_cmpstr(stored(g.f, "p2|e2|s3", "LEDGER_URL"), ==, want);
        g_assert_cmpstr(stored(g.f, "p2|e2|s10", "LEDGER_PASSWORD"), ==, new_pw);
        g_free(want);
    }
    g_assert_true(rv_scram_verify(DB_ROLE_PW, g_hash_table_lookup(g.d->verifiers, "ledger")));
    g_assert_cmpuint(g.d->sets, ==, 0);

    /* services must not be deployed on settings that the database does not know yet */
    g_assert_false(rv_run_deploy(g.run, &o, &error));
    g_assert_error(error, RV_ERROR, RV_ERROR_STATE);
    g_clear_error(&error);
    g_assert_cmpuint(g.f->redeploys->len, ==, 0);
    g_assert_true(rv_run_status(g.run, &st, NULL));
    g_assert_cmpuint(st.roles_total, ==, 1);
    g_assert_cmpuint(st.roles_done, ==, 0);

    g_assert_true(rv_run_roles(g.run, &o, &error));
    g_assert_no_error(error);
    {
        const gchar *new_pw = ((RvPlanRole *)g.plan->roles->pdata[0])->new_password;
        const gchar *verifier = g_hash_table_lookup(g.d->verifiers, "ledger");

        g_assert_true(rv_scram_verify(new_pw, verifier));        /* the new password works */
        g_assert_false(rv_scram_verify(DB_ROLE_PW, verifier));   /* the old one no longer does */
        g_assert_null(strstr(g.d->everything->str, new_pw));     /* only a hash ever reached the database */
        g_assert_null(strstr(g.d->everything->str, DB_ROLE_PW));
    }
    g_assert_cmpuint(g.d->sets, ==, 1);
    g_assert_cmpuint(g.d->opens, ==, 1);
    g_assert_cmpuint(g.d->closes, ==, 1);                        /* the tunnel was closed */

    g_assert_true(rv_run_status(g.run, &st, NULL));
    g_assert_cmpuint(st.roles_done, ==, 1);
    g_assert_true(rv_run_deploy(g.run, &o, &error));
    g_assert_no_error(error);
    {
        GHashTable *set = g_hash_table_new(g_str_hash, g_str_equal);

        for (guint i = 0; i < g.f->redeploys->len; i++)
            g_hash_table_add(set, g.f->redeploys->pdata[i]);
        g_assert_true(g_hash_table_contains(set, "s3"));         /* the consumers */
        g_assert_true(g_hash_table_contains(set, "s10"));
        g_assert_false(g_hash_table_contains(set, "s9"));        /* not the database itself */
        g_hash_table_unref(set);
    }

    /* repeating it changes nothing */
    g_assert_true(rv_run_roles(g.run, &o, &error));
    g_assert_cmpuint(g.d->sets, ==, 1);
    rig_teardown(&g);
}

static void
test_roles_stop_when_the_mapping_is_wrong(void)
{
    Rig g;
    RvRunOptions o;
    GError *error = NULL;
    RvRunStatus st;
    const gchar *new_pw;

    rig_setup(&g);
    o = role_options(g.c, g.d);
    new_pw = ((RvPlanRole *)g.plan->roles->pdata[0])->new_password;
    /* the database does not know this password: the URL points at some other server */
    g_hash_table_replace(g.d->verifiers, g_strdup("ledger"), rv_scram_verifier("not-the-password-in-the-variables", NULL));

    g_assert_true(rv_run_stage(g.run, &o, NULL));
    g_assert_false(rv_run_roles(g.run, &o, &error));
    g_assert_error(error, RV_ERROR, RV_ERROR_STOPPED);
    g_assert_nonnull(strstr(error->message, "did not accept its current password"));
    g_assert_nonnull(strstr(error->message, "Nothing was changed"));
    g_assert_null(strstr(error->message, DB_ROLE_PW));          /* no password in the message */
    g_assert_null(strstr(error->message, new_pw));
    g_clear_error(&error);
    g_assert_cmpuint(g.d->sets, ==, 0);                          /* the database was never touched */
    g_assert_cmpuint(g.d->closes, ==, g.d->opens);

    g_assert_true(rv_run_status(g.run, &st, NULL));
    g_assert_cmpuint(st.roles_failed, ==, 1);
    g_assert_false(rv_run_deploy(g.run, &o, &error));            /* and nothing is deployed */
    g_clear_error(&error);
    g_assert_cmpuint(g.f->redeploys->len, ==, 0);
    rig_teardown(&g);
}

static void
test_roles_put_the_old_password_back_when_the_new_one_fails(void)
{
    Rig g;
    RvRunOptions o;
    GError *error = NULL;

    rig_setup(&g);
    o = role_options(g.c, g.d);
    g.d->break_next_set = TRUE;

    g_assert_true(rv_run_stage(g.run, &o, NULL));
    g_assert_false(rv_run_roles(g.run, &o, &error));
    g_assert_error(error, RV_ERROR, RV_ERROR_STOPPED);
    g_assert_nonnull(strstr(error->message, "the old one was put back"));
    g_clear_error(&error);
    g_assert_cmpuint(g.d->sets, ==, 2);                          /* the change, then the rollback */
    g_assert_true(rv_scram_verify(DB_ROLE_PW, g_hash_table_lookup(g.d->verifiers, "ledger")));
    g_assert_cmpuint(g.f->redeploys->len, ==, 0);

    /* a retry then works */
    g_assert_true(rv_run_roles(g.run, &o, &error));
    g_assert_no_error(error);
    g_assert_true(rv_scram_verify(((RvPlanRole *)g.plan->roles->pdata[0])->new_password,
                                  g_hash_table_lookup(g.d->verifiers, "ledger")));
    rig_teardown(&g);
}

static void
test_roles_accept_a_password_that_is_already_new(void)
{
    Rig g;
    RvRunOptions o;
    const gchar *new_pw;

    rig_setup(&g);
    o = role_options(g.c, g.d);
    new_pw = ((RvPlanRole *)g.plan->roles->pdata[0])->new_password;
    /* a crash after the database changed, before the run recorded it */
    g_hash_table_replace(g.d->verifiers, g_strdup("ledger"), rv_scram_verifier(new_pw, NULL));

    g_assert_true(rv_run_roles(g.run, &o, NULL));
    g_assert_cmpuint(g.d->sets, ==, 0);                          /* it did not change it again */
    g_assert_false(rv_roles_pending(g.run));
    rig_teardown(&g);
}

static void
test_roles_tunnel_and_login_failures(void)
{
    Rig g;
    RvRunOptions o;
    GError *error = NULL;

    rig_setup(&g);
    o = role_options(g.c, g.d);
    g.d->tunnel_fails = TRUE;
    g_assert_false(rv_run_roles(g.run, &o, &error));
    g_assert_error(error, RV_ERROR, RV_ERROR_STOPPED);
    g_assert_nonnull(strstr(error->message, "did not open"));
    g_clear_error(&error);
    g_assert_cmpuint(g.d->logins, ==, 0);

    /* the database service has no readable login */
    g.d->tunnel_fails = FALSE;
    g_hash_table_remove(bucket(g.f, "p2|e2|s9"), "POSTGRES_PASSWORD");
    g_hash_table_remove(bucket(g.f, "p2|e2|s9"), "PGPASSWORD");
    g_assert_false(rv_run_roles(g.run, &o, &error));
    g_assert_nonnull(strstr(error->message, "could not be read"));
    g_clear_error(&error);
    g_assert_cmpuint(g.d->sets, ==, 0);
    rig_teardown(&g);
}

static void
test_roles_undo_restores_the_password(void)
{
    Rig g;
    RvRunOptions o;
    RvUndoOptions real = { FALSE, FALSE, TRUE, NULL };
    RvUndoOptions dry = { TRUE, FALSE, FALSE, NULL };
    RvUndoResult r;
    GError *error = NULL;
    gchar *path, *report = NULL;
    const gchar *new_pw;

    rig_setup(&g);
    o = role_options(g.c, g.d);
    new_pw = ((RvPlanRole *)g.plan->roles->pdata[0])->new_password;

    /* before the password was changed there is nothing to restore, and no tunnel is opened */
    g_assert_true(rv_run_stage(g.run, &o, NULL));
    g_assert_true(rv_undo_run(g.run, &o, &dry, &r, NULL));
    g_assert_cmpuint(g.d->opens, ==, 0);
    g_assert_cmpuint(r.skipped, >=, 1);

    g_assert_true(rv_run_roles(g.run, &o, NULL));
    g_assert_true(rv_run_deploy(g.run, &o, NULL));
    g.d->sets = 0;

    /* a preview changes nothing */
    g_assert_true(rv_undo_run(g.run, &o, &dry, &r, &error));
    g_assert_no_error(error);
    g_assert_cmpuint(g.d->sets, ==, 0);
    g_assert_true(rv_scram_verify(new_pw, g_hash_table_lookup(g.d->verifiers, "ledger")));

    g_assert_true(rv_undo_run(g.run, &o, &real, &r, &error));
    g_assert_no_error(error);
    g_assert_true(rv_scram_verify(DB_ROLE_PW, g_hash_table_lookup(g.d->verifiers, "ledger")));   /* back */
    g_assert_false(rv_scram_verify(new_pw, g_hash_table_lookup(g.d->verifiers, "ledger")));
    g_assert_cmpstr(stored(g.f, "p2|e2|s10", "LEDGER_PASSWORD"), ==, DB_ROLE_PW);               /* and the variables */
    g_assert_cmpuint(g.d->sets, ==, 1);

    path = g_build_filename(g.run, "undo-report.csv", NULL);
    g_assert_true(g_file_get_contents(path, &report, NULL, NULL));
    g_assert_nonnull(strstr(report, "role:ledger"));
    g_assert_null(strstr(report, DB_ROLE_PW));                   /* the report holds no passwords */
    g_assert_null(strstr(report, new_pw));
    g_free(report);
    g_free(path);

    /* repeating is safe */
    g_assert_true(rv_undo_run(g.run, &o, &real, &r, &error));
    g_assert_cmpuint(g.d->sets, ==, 1);
    rig_teardown(&g);
}

static void
test_roles_undo_respects_a_password_changed_since(void)
{
    Rig g;
    RvRunOptions o;
    RvUndoOptions leave = { FALSE, FALSE, FALSE, NULL };
    RvUndoOptions force = { FALSE, TRUE, FALSE, NULL };
    RvUndoResult r;

    rig_setup(&g);
    o = role_options(g.c, g.d);
    g_assert_true(rv_run_stage(g.run, &o, NULL));
    g_assert_true(rv_run_roles(g.run, &o, NULL));
    /* someone sets another password afterwards */
    g_hash_table_replace(g.d->verifiers, g_strdup("ledger"), rv_scram_verifier("someone-elses-new-password", NULL));
    g.d->sets = 0;

    g_assert_true(rv_undo_run(g.run, &o, &leave, &r, NULL));
    g_assert_cmpuint(r.conflicts, >=, 1);
    g_assert_cmpuint(g.d->sets, ==, 0);                          /* left alone */
    g_assert_true(rv_scram_verify("someone-elses-new-password", g_hash_table_lookup(g.d->verifiers, "ledger")));

    g_assert_true(rv_undo_run(g.run, &o, &force, &r, NULL));
    g_assert_cmpuint(g.d->sets, ==, 1);                          /* overwritten on request */
    g_assert_true(rv_scram_verify(DB_ROLE_PW, g_hash_table_lookup(g.d->verifiers, "ledger")));
    rig_teardown(&g);
}

/* ---- Findings from the independent review -------------------------------- */

static void
test_review_one_name_cannot_carry_a_group(void)
{
    const gchar *same = "Kq8Zr2Lm9Xv4Tb7Wn3Hd5Jf6G";   /* 26 letters and digits */
    RvInventory *inv = rv_inventory_new();

    /* A secret name next to a database password the classifier does not know by name. */
    g_ptr_array_add(inv->variables, rv_variable_new("w", "W", "p", "P", "e", "E", "s1", "A", "SESSION_SECRET", same));
    g_ptr_array_add(inv->variables, rv_variable_new("w", "W", "p", "P", "e", "E", "s2", "B", "APP_LEDGER_PASS", same));
    /* two members that both mark a secret still rotate */
    g_ptr_array_add(inv->variables, rv_variable_new("w", "W", "p", "P", "e", "E", "s1", "A", "COOKIE_SECRET", HEX));
    g_ptr_array_add(inv->variables, rv_variable_new("w", "W", "p", "P", "e", "E", "s2", "B", "SESSION_SECRET", HEX));
    rv_classify(inv, NULL);
    g_assert_cmpint(((RvVariable *)inv->variables->pdata[0])->category, ==, RV_CAT_SHARED_MANUAL);
    g_assert_cmpint(((RvVariable *)inv->variables->pdata[1])->category, ==, RV_CAT_SHARED_MANUAL);
    g_assert_nonnull(strstr(((RvVariable *)inv->variables->pdata[0])->reason, "not every name"));
    g_assert_cmpint(((RvVariable *)inv->variables->pdata[2])->category, ==, RV_CAT_ROTATE);
    g_assert_cmpint(((RvVariable *)inv->variables->pdata[3])->category, ==, RV_CAT_ROTATE);
    rv_inventory_free(inv);
}

static void
test_review_more_database_names(void)
{
    const gchar *names[] = {
        "DATABASE_PASSWORD", "MARIADB_PASSWORD", "CLICKHOUSE_PASSWORD", "RABBITMQ_URL",
        "AMQP_URL", "DB_PASSWORD", "APP_DB_PASS", "MSSQL_PWD", "DBPASS", "PGPASSWORD", NULL
    };
    const gchar *not_database[] = { "DATABASE_NAME", "DB_HOST", "REDIS_PORT", "PASSWORD_MIN_LENGTH_RULE", NULL };

    for (guint i = 0; names[i] != NULL; i++) {
        RvInventory *inv = rv_inventory_new();

        g_ptr_array_add(inv->variables, rv_variable_new("w", "W", "p", "P", "e", "E", "s", "S",
                                                        names[i], "plain-value-1234567890"));
        rv_classify(inv, NULL);
        g_assert_cmpint(((RvVariable *)inv->variables->pdata[0])->category, ==, RV_CAT_DATABASE);
        rv_inventory_free(inv);
    }
    for (guint i = 0; not_database[i] != NULL; i++) {
        RvInventory *inv = rv_inventory_new();

        g_ptr_array_add(inv->variables, rv_variable_new("w", "W", "p", "P", "e", "E", "s", "S",
                                                        not_database[i], "plain-value-1234567890"));
        rv_classify(inv, NULL);
        g_assert_cmpint(((RvVariable *)inv->variables->pdata[0])->category, !=, RV_CAT_DATABASE);
        rv_inventory_free(inv);
    }
}

static void
test_review_names_match_whole_words(void)
{
    RvInventory *inv = rv_inventory_new();

    g_assert_false(rv_name_is_secretish("JWT_AUDIENCE"));
    g_assert_false(rv_name_is_secretish("JWT_ISSUER"));
    g_assert_false(rv_name_is_secretish("BYPASS_MODE"));
    g_assert_false(rv_name_is_secretish("DESIGN_MODE"));
    g_assert_false(rv_name_is_secretish("AUTHOR_NAME"));
    g_assert_false(rv_name_is_internal_secret("JWT_AUDIENCE"));
    g_assert_true(rv_name_is_secretish("SESSION_SECRET"));
    g_assert_true(rv_name_is_secretish("jwt.signing.key"));
    g_assert_true(rv_name_is_internal_secret("NEXTAUTH_SECRET"));
    g_assert_true(rv_name_is_provider("AWS_SECRET_ACCESS_KEY"));
    g_assert_false(rv_name_is_provider("LAWSUIT_SECRET"));
    /* a secret word does not make a URL or a sentence a secret */
    g_assert_false(rv_looks_like_secret("SECRET_URL", "https://example.test/a/long/path/here"));
    g_assert_false(rv_looks_like_secret("SECRET_NOTE", "this is a long note with spaces"));

    /* the same config value in two services is not rotated */
    g_ptr_array_add(inv->variables, rv_variable_new("w", "W", "p", "P", "e", "E", "s1", "A", "JWT_AUDIENCE", "wexford-clients-api"));
    g_ptr_array_add(inv->variables, rv_variable_new("w", "W", "p", "P", "e", "E", "s2", "B", "JWT_AUDIENCE", "wexford-clients-api"));
    rv_classify(inv, NULL);
    g_assert_cmpint(((RvVariable *)inv->variables->pdata[0])->category, ==, RV_CAT_SHARED_CONFIG);
    g_assert_cmpuint(inv->rotation_groups, ==, 0);
    rv_inventory_free(inv);
}

static void
test_review_base64_padding_is_canonical(void)
{
    guint8 bytes[32] = { 0 };
    gchar *one = g_base64_encode(bytes, 32);     /* ends with one = */
    gchar *two = g_base64_encode(bytes, 31);     /* ends with == */
    const gchar *templates[] = { one, two };

    g_assert_true(g_str_has_suffix(one, "=") && !g_str_has_suffix(one, "=="));
    g_assert_true(g_str_has_suffix(two, "=="));
    for (guint t = 0; t < 2; t++) {
        for (guint i = 0; i < 200; i++) {
            gchar *fresh = rv_generate_like(templates[t], NULL);
            gsize n = 0;
            guchar *decoded = g_base64_decode(fresh, &n);
            gchar *again = g_base64_encode(decoded, n);

            g_assert_cmpstr(again, ==, fresh);       /* a strict decoder accepts it */
            g_assert_cmpuint(strlen(fresh), ==, strlen(templates[t]));
            g_free(again);
            g_free(decoded);
            rw_secure_free(fresh);
        }
    }
    g_free(one);
    g_free(two);
}

static void
test_review_stage_never_overwrites_a_changed_value(void)
{
    Fake *f = fake_new();
    RwClient *c;
    RvInventory *inv;
    RvPlan *plan = make_plan(f, &c, &inv);
    gchar *dir = temp_dir(), *run = rv_run_dir_new(dir, NULL);
    RvRunOptions o = { .client = c };
    GError *error = NULL;
    RvRunStatus st;

    g_assert_true(rv_run_prepare(plan, run, NULL));
    /* someone edits the variable after the scan, before staging */
    seed(f, "p1|e1|s1", "SESSION_SECRET", "edited-by-a-colleague-after-the-scan");
    g_assert_false(rv_run_stage(run, &o, &error));
    g_assert_error(error, RV_ERROR, RV_ERROR_STOPPED);
    g_assert_nonnull(strstr(error->message, "was changed by someone since the scan"));
    g_assert_null(strstr(error->message, "edited-by-a-colleague"));      /* no value in the message */
    g_clear_error(&error);
    g_assert_cmpstr(stored(f, "p1|e1|s1", "SESSION_SECRET"), ==, "edited-by-a-colleague-after-the-scan");
    g_assert_cmpuint(f->upsert_count, ==, 0);
    g_assert_true(rv_run_status(run, &st, NULL));
    g_assert_true(st.stage_failed);

    rv_plan_free(plan);
    rv_inventory_free(inv);
    rw_client_free(c);
    fake_free(f);
    rm_rf(run);
    rm_rf(dir);
    g_free(run);
    g_free(dir);
}

static void
test_review_stage_accepts_a_value_already_staged(void)
{
    Fake *f = fake_new();
    RwClient *c;
    RvInventory *inv;
    RvPlan *plan = make_plan(f, &c, &inv);
    gchar *dir = temp_dir(), *run = rv_run_dir_new(dir, NULL);
    RvRunOptions o = { .client = c };
    RvPlanGroup *hex = plan->groups->pdata[0];

    g_assert_true(rv_run_prepare(plan, run, NULL));
    /* a crash after Railway accepted the write but before progress.csv was updated */
    seed(f, "p1|e1|s1", "SESSION_SECRET", hex->new_value);
    g_assert_true(rv_run_stage(run, &o, NULL));
    g_assert_cmpuint(f->upsert_count, ==, 3);          /* that batch was not written again */
    g_assert_cmpstr(stored(f, "p1|e1|s1", "SESSION_SECRET"), ==, hex->new_value);

    rv_plan_free(plan);
    rv_inventory_free(inv);
    rw_client_free(c);
    fake_free(f);
    rm_rf(run);
    rm_rf(dir);
    g_free(run);
    g_free(dir);
}

static void
test_review_unticked_group_stays_on_the_checklist(void)
{
    Fake *f = fake_new();
    RwClient *c;
    RvInventory *inv;
    RvPlan *plan = make_plan(f, &c, &inv);
    gchar *dir = temp_dir(), *run = rv_run_dir_new(dir, NULL);
    gchar *text = NULL, *path;

    ((RvPlanGroup *)plan->groups->pdata[1])->enabled = FALSE;      /* APP_SALT */
    g_assert_true(rv_run_prepare(plan, run, NULL));

    path = g_build_filename(run, "not-changed.csv", NULL);
    g_assert_true(g_file_get_contents(path, &text, NULL, NULL));
    g_assert_nonnull(strstr(text, "APP_SALT"));
    g_assert_nonnull(strstr(text, "you excluded this group"));
    g_assert_cmpstr(strstr(text, "APP_SALT"), !=, NULL);
    g_free(text);
    g_free(path);

    path = g_build_filename(run, "changes.csv", NULL);
    g_assert_true(g_file_get_contents(path, &text, NULL, NULL));
    g_assert_null(strstr(text, "APP_SALT"));                       /* and it is not changed */
    rw_secure_free(text);
    g_free(path);

    rv_plan_free(plan);
    rv_inventory_free(inv);
    rw_client_free(c);
    fake_free(f);
    rm_rf(run);
    rm_rf(dir);
    g_free(run);
    g_free(dir);
}

typedef struct {
    GCancellable *cancel;
    guint         seen;
} CancelAfter;

static void
cancel_on_second_check(gpointer data, const gchar *message, gdouble fraction)
{
    CancelAfter *c = data;

    (void)fraction;
    if (g_str_has_prefix(message, "Checking") && ++c->seen == 2)
        g_cancellable_cancel(c->cancel);
}

static void
test_review_cancelled_undo_still_reports(void)
{
    Fake *f = fake_new();
    RwClient *c;
    RvInventory *inv;
    RvPlan *plan = make_plan(f, &c, &inv);
    gchar *dir = temp_dir(), *run = rv_run_dir_new(dir, NULL);
    CancelAfter ca = { g_cancellable_new(), 0 };
    RvRunOptions o = { .client = c };
    RvRunOptions cancelling = { .client = c, .progress = cancel_on_second_check, .progress_data = &ca, .cancel = ca.cancel };
    RvUndoOptions u = { FALSE, FALSE, TRUE, NULL };
    RvUndoResult r;
    GError *error = NULL;
    gchar *path, *text = NULL;

    g_assert_true(rv_run_prepare(plan, run, NULL));
    g_assert_true(rv_run_stage(run, &o, NULL));

    g_assert_false(rv_undo_run(run, &cancelling, &u, &r, &error));
    g_assert_error(error, RV_ERROR, RV_ERROR_CANCELLED);
    g_clear_error(&error);
    g_assert_cmpuint(r.restored, ==, 2);                  /* two batches finished before the safe point */
    path = g_build_filename(run, "undo-report.csv", NULL);
    g_assert_true(g_file_get_contents(path, &text, NULL, NULL));   /* so the report must exist */
    g_assert_nonnull(strstr(text, "restored"));
    g_free(text);
    g_free(path);
    g_assert_cmpuint(f->redeploys->len, ==, 0);           /* a cancelled undo does not redeploy */

    /* finishing it later restores the rest and redeploys everything once */
    g_assert_true(rv_undo_run(run, &o, &u, &r, &error));
    g_assert_no_error(error);
    g_assert_cmpuint(r.restored, ==, 2);
    g_assert_cmpuint(r.already_original, ==, 2);
    g_assert_cmpuint(f->redeploys->len, ==, 4);

    g_object_unref(ca.cancel);
    rv_plan_free(plan);
    rv_inventory_free(inv);
    rw_client_free(c);
    fake_free(f);
    rm_rf(run);
    rm_rf(dir);
    g_free(run);
    g_free(dir);
}

static void
test_review_undo_redeploys_once_even_on_a_later_run(void)
{
    Fake *f = fake_new();
    RwClient *c;
    RvInventory *inv;
    RvPlan *plan = make_plan(f, &c, &inv);
    gchar *dir = temp_dir(), *run = rv_run_dir_new(dir, NULL);
    RvRunOptions o = { .client = c };
    RvUndoOptions no_deploy = { FALSE, FALSE, FALSE, NULL };
    RvUndoOptions with_deploy = { FALSE, FALSE, TRUE, NULL };
    RvUndoResult r;

    g_assert_true(rv_run_prepare(plan, run, NULL));
    g_assert_true(rv_run_stage(run, &o, NULL));
    g_assert_true(rv_undo_run(run, &o, &no_deploy, &r, NULL));
    g_assert_cmpuint(r.restored, ==, 4);
    g_assert_cmpuint(f->redeploys->len, ==, 0);

    /* the services still run on the new values, so a later undo must redeploy them */
    g_assert_true(rv_undo_run(run, &o, &with_deploy, &r, NULL));
    g_assert_cmpuint(r.restored, ==, 0);
    g_assert_cmpuint(r.already_original, ==, 4);
    g_assert_cmpuint(f->redeploys->len, ==, 4);

    /* and not again after that */
    g_assert_true(rv_undo_run(run, &o, &with_deploy, &r, NULL));
    g_assert_cmpuint(f->redeploys->len, ==, 4);

    rv_plan_free(plan);
    rv_inventory_free(inv);
    rw_client_free(c);
    fake_free(f);
    rm_rf(run);
    rm_rf(dir);
    g_free(run);
    g_free(dir);
}


int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/random/string", test_random_string);
    g_test_add_func("/random/unbiased", test_random_is_unbiased);
    g_test_add_func("/generate/keeps-format", test_generate_keeps_format);
    g_test_add_func("/generate/refuses-unknown-formats", test_generate_refuses_unknown_formats);
    g_test_add_func("/generate/looks-like-secret", test_looks_like_secret);
    g_test_add_func("/scram/matches-rfc-7677", test_scram_matches_rfc_7677);
    g_test_add_func("/scram/verifier-round-trip", test_scram_verifier_round_trip);
    g_test_add_func("/pgurl/parse", test_pgurl_parse);
    g_test_add_func("/pgurl/replace-password", test_pgurl_replace_password);
    g_test_add_func("/roles/discovered", test_roles_are_discovered);
    g_test_add_func("/roles/refuse-unsafe-cases", test_roles_refuse_unsafe_cases);
    g_test_add_func("/roles/ignore-what-they-cannot-place", test_roles_ignore_what_they_cannot_place);
    g_test_add_func("/roles/plan-rewrites-every-consumer", test_role_plan_rewrites_every_consumer);
    g_test_add_func("/roles/files-and-checklist", test_role_files_and_checklist);
    g_test_add_func("/roles/switched-off-stays-on-checklist", test_switched_off_role_stays_on_the_checklist);
    g_test_add_func("/roles-run/change-then-deploy", test_roles_change_password_then_deploy);
    g_test_add_func("/roles-run/stop-when-mapping-wrong", test_roles_stop_when_the_mapping_is_wrong);
    g_test_add_func("/roles-run/roll-back-when-new-fails", test_roles_put_the_old_password_back_when_the_new_one_fails);
    g_test_add_func("/roles-run/already-new", test_roles_accept_a_password_that_is_already_new);
    g_test_add_func("/roles-run/tunnel-and-login-failures", test_roles_tunnel_and_login_failures);
    g_test_add_func("/roles-run/undo-restores", test_roles_undo_restores_the_password);
    g_test_add_func("/roles-run/undo-respects-changes", test_roles_undo_respects_a_password_changed_since);
    g_test_add_func("/audit/file", test_audit_file);
    g_test_add_func("/audit/save", test_audit_save);
    g_test_add_func("/audit/default-folder", test_audit_default_folder);
    g_test_add_func("/policy/old-folder", test_policy_old_folder);
    g_test_add_func("/csv/round-trip", test_csv_round_trip);
    g_test_add_func("/csv/edge-cases", test_csv_parse_edge_cases);
#ifndef G_OS_WIN32
    g_test_add_func("/files/private-is-owner-only", test_private_file_is_owner_only);
#endif
    g_test_add_func("/policy/round-trip", test_policy_round_trip);
    g_test_add_func("/scan/counts-and-estimate", test_scan_counts_and_estimate);
    g_test_add_func("/scan/cancel", test_scan_can_be_cancelled);
    g_test_add_func("/scan/incomplete-blocks-plan", test_incomplete_scan_is_isolated_and_blocks_plan);
    g_test_add_func("/scan/skipped-workspaces", test_workspace_skipped_by_policy_and_by_role);
    g_test_add_func("/classify/groups", test_classify_groups);
    g_test_add_func("/classify/ini-rules", test_classify_ini_rules);
    g_test_add_func("/classify/ini-wins-over-sharing", test_ini_wins_over_sharing);
    g_test_add_func("/classify/pin-blocks-group", test_policy_pin_blocks_whole_group);
    g_test_add_func("/classify/custom-credential-name", test_custom_credential_name);
    g_test_add_func("/plan/one-value-per-group", test_plan_one_value_per_group);
    g_test_add_func("/run/stage-deploy-status", test_stage_deploy_and_status);
    g_test_add_func("/run/stops-and-resumes", test_stage_stops_and_resumes);
    g_test_add_func("/undo/restores-and-respects-conflicts", test_undo_restores_and_respects_conflicts);
    g_test_add_func("/undo/limited-to-projects", test_undo_can_be_limited_to_projects);
    g_test_add_func("/reports/no-secrets", test_reports);
    g_test_add_func("/review/one-name-cannot-carry-a-group", test_review_one_name_cannot_carry_a_group);
    g_test_add_func("/review/more-database-names", test_review_more_database_names);
    g_test_add_func("/review/names-match-whole-words", test_review_names_match_whole_words);
    g_test_add_func("/review/base64-padding-is-canonical", test_review_base64_padding_is_canonical);
    g_test_add_func("/review/stage-never-overwrites", test_review_stage_never_overwrites_a_changed_value);
    g_test_add_func("/review/stage-accepts-already-staged", test_review_stage_accepts_a_value_already_staged);
    g_test_add_func("/review/unticked-group-on-checklist", test_review_unticked_group_stays_on_the_checklist);
    g_test_add_func("/review/cancelled-undo-still-reports", test_review_cancelled_undo_still_reports);
    g_test_add_func("/review/undo-redeploys-once", test_review_undo_redeploys_once_even_on_a_later_run);

    return g_test_run();
}
