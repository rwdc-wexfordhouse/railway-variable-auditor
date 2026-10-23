/*
 * PostgreSQL through libpq, loaded when needed.
 *
 * libpq is opened at run time (GModule), with the few calls we use declared here, so the
 * program builds without libpq-devel and works the same on Linux, macOS and Windows. If
 * libpq is not installed, only the database step is unavailable.
 *
 * Copyright © 2026, Wexford House <https://wexfordhouse.co.za/>
 * Copyright © 2026, Ross Cameron <ross@wexfordhouse.co.za>
 * Licensed under the GNU General Public License, version 3.
 */
#include "rv-private.h"

#include <gmodule.h>
#include <string.h>

/* The parts of libpq's stable interface that we use. */
typedef struct pg_conn PGconn;
typedef struct pg_result PGresult;
#define PG_CONNECTION_OK   0
#define PG_RES_COMMAND_OK  1

typedef struct {
    GModule *module;
    PGconn *(*connect_params)(const char *const *keywords, const char *const *values, int expand);
    int (*status)(const PGconn *);
    char *(*error_message)(const PGconn *);
    void (*finish)(PGconn *);
    PGresult *(*exec)(PGconn *, const char *);
    int (*result_status)(const PGresult *);
    char *(*result_error_message)(const PGresult *);
    void (*clear)(PGresult *);
    char *(*escape_identifier)(PGconn *, const char *, size_t);
    char *(*escape_literal)(PGconn *, const char *, size_t);
    void (*free_mem)(void *);
} Pq;

static Pq pq;
static gsize pq_state;       /* 0 not tried, 1 loaded, 2 failed */
static gchar *pq_problem;

static void
load_libpq(void)
{
    const gchar *names[] = {
        g_getenv("RVC_LIBPQ"),
#if defined(G_OS_WIN32)
        "libpq-5.dll", "libpq.dll",
#elif defined(__APPLE__)
        "libpq.5.dylib", "/opt/homebrew/opt/libpq/lib/libpq.5.dylib", "/usr/local/opt/libpq/lib/libpq.5.dylib",
#else
        "libpq.so.5",
#endif
        NULL
    };

    for (guint i = 0; i < G_N_ELEMENTS(names) && pq.module == NULL; i++) {
        if (names[i] != NULL)
            pq.module = g_module_open(names[i], G_MODULE_BIND_LAZY | G_MODULE_BIND_LOCAL);
    }
    if (pq.module == NULL) {
        pq_problem = g_strdup("libpq, the PostgreSQL client library, was not found. Install it (for example the libpq package) "
                              "and try again.");
        g_once_init_leave(&pq_state, 2);
        return;
    }
#define SYM(field, name) \
    if (!g_module_symbol(pq.module, name, (gpointer *)&pq.field)) { \
        pq_problem = g_strdup_printf("libpq does not provide %s, so it is not usable.", name); \
        g_once_init_leave(&pq_state, 2); \
        return; \
    }
    SYM(connect_params, "PQconnectdbParams")
    SYM(status, "PQstatus")
    SYM(error_message, "PQerrorMessage")
    SYM(finish, "PQfinish")
    SYM(exec, "PQexec")
    SYM(result_status, "PQresultStatus")
    SYM(result_error_message, "PQresultErrorMessage")
    SYM(clear, "PQclear")
    SYM(escape_identifier, "PQescapeIdentifier")
    SYM(escape_literal, "PQescapeLiteral")
    SYM(free_mem, "PQfreemem")
#undef SYM
    g_once_init_leave(&pq_state, 1);
}

gboolean
rv_libpq_available(GError **error)
{
    if (g_once_init_enter(&pq_state))
        load_libpq();
    if (pq_state == 1)
        return TRUE;
    g_set_error_literal(error, RV_ERROR, RV_ERROR_DB, pq_problem != NULL ? pq_problem : "libpq is not available.");
    return FALSE;
}

/* libpq ends its messages with a line break. */
static gchar *
clean(const gchar *message)
{
    gchar *m = g_strdup(message != NULL ? message : "unknown error");

    return g_strstrip(m);
}

static gboolean
is_login_refusal(const gchar *m)
{
    return strstr(m, "authentication failed") != NULL
           || (strstr(m, "role \"") != NULL && strstr(m, "does not exist") != NULL)
           || strstr(m, "no pg_hba.conf entry") != NULL;
}

static PGconn *
connect_as(const gchar *host, guint port, const gchar *database, const gchar *user,
           const gchar *password, GError **error)
{
    gchar *port_text = g_strdup_printf("%u", port);
    const char *keys[] = { "host", "port", "dbname", "user", "password", "connect_timeout",
                           "sslmode", "application_name", NULL };
    const char *values[] = { host, port_text, database, user, password, "15", "prefer",
                             "railway-variable-auditor", NULL };
    PGconn *conn;

    if (!rv_libpq_available(error)) {
        g_free(port_text);
        return NULL;
    }
    conn = pq.connect_params(keys, values, 0);
    g_free(port_text);
    if (conn == NULL) {
        g_set_error_literal(error, RV_ERROR, RV_ERROR_DB, "libpq could not allocate a connection.");
        return NULL;
    }
    if (pq.status(conn) != PG_CONNECTION_OK) {
        gchar *message = clean(pq.error_message(conn));

        g_set_error(error, RV_ERROR, is_login_refusal(message) ? RV_ERROR_DB_AUTH : RV_ERROR_DB,
                    "The database refused the connection: %s", message);
        g_free(message);
        pq.finish(conn);
        return NULL;
    }
    return conn;
}

static gboolean
libpq_login_ok(gpointer data, const gchar *host, guint port, const gchar *database,
               const gchar *user, const gchar *password, GError **error)
{
    PGconn *conn = connect_as(host, port, database, user, password, error);

    (void)data;
    if (conn == NULL)
        return FALSE;
    pq.finish(conn);
    return TRUE;
}

static gboolean
libpq_set_password(gpointer data, const gchar *host, guint port, const gchar *database,
                   const gchar *admin_user, const gchar *admin_password, const gchar *role,
                   const gchar *verifier, GError **error)
{
    PGconn *conn = connect_as(host, port, database, admin_user, admin_password, error);
    char *ident, *literal;
    gchar *sql;
    PGresult *result;
    gboolean ok;

    (void)data;
    if (conn == NULL)
        return FALSE;
    ident = pq.escape_identifier(conn, role, strlen(role));
    literal = pq.escape_literal(conn, verifier, strlen(verifier));
    if (ident == NULL || literal == NULL) {
        g_set_error_literal(error, RV_ERROR, RV_ERROR_DB, "libpq could not quote the role name or the password hash.");
        if (ident != NULL)
            pq.free_mem(ident);
        if (literal != NULL)
            pq.free_mem(literal);
        pq.finish(conn);
        return FALSE;
    }
    /* The statement carries a SCRAM-SHA-256 hash, never the password itself. */
    sql = g_strdup_printf("ALTER ROLE %s PASSWORD %s", ident, literal);
    pq.free_mem(ident);
    pq.free_mem(literal);
    result = pq.exec(conn, sql);
    g_free(sql);
    ok = result != NULL && pq.result_status(result) == PG_RES_COMMAND_OK;
    if (!ok) {
        gchar *message = clean(result != NULL ? pq.result_error_message(result) : pq.error_message(conn));

        g_set_error(error, RV_ERROR, RV_ERROR_DB, "The database refused to change the password of %s: %s", role, message);
        g_free(message);
    }
    if (result != NULL)
        pq.clear(result);
    pq.finish(conn);
    return ok;
}

static const RvDbOps libpq_ops = { libpq_login_ok, libpq_set_password };

const RvDbOps *
rv_db_ops_libpq(void)
{
    return &libpq_ops;
}
