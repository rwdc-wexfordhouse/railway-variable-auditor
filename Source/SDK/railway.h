/*
 * Railway SDK for C11
 *
 * Copyright © 2026, Wexford House <https://wexfordhouse.co.za/>
 * Copyright © 2026, Ross Cameron <ross@wexfordhouse.co.za>
 * Licensed under the GNU General Public License, version 3.
 *
 * A small client for the Railway public GraphQL API
 * (https://docs.railway.com/integrations/api).
 *
 * By default requests run through the Railway CLI (`railway api`), so the
 * CLI must be installed and signed in (`railway login`). The application
 * never handles the token. Variables, which carry secret values, are sent to
 * the CLI on stdin and never appear in a process listing.
 *
 * Calls block, so run them off the GUI thread. One RwClient must not be
 * used from two threads at once.
 *
 * Status of each call (checked 2026-10-22):
 *   documented by Railway    rw_list_projects, rw_get_variables, rw_upsert_variables
 *   confirmed live           rw_list_workspaces, and paging with first and after
 *                            on projects, services and environments
 *   confirmed in the schema  rw_redeploy_service (serviceInstanceRedeploy exists,
 *                            it has not been called yet)
 */
#ifndef RAILWAY_H
#define RAILWAY_H

#include <glib.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

#define RW_ERROR (rw_error_quark())
GQuark rw_error_quark(void);

typedef enum {
    RW_ERROR_INVALID,       /* bad arguments */
    RW_ERROR_NETWORK,       /* could not reach Railway */
    RW_ERROR_HTTP,          /* unexpected HTTP status */
    RW_ERROR_AUTH,          /* token rejected or lacks permission */
    RW_ERROR_RATE_LIMITED,  /* quota used up, see rw_client_get_retry_after() */
    RW_ERROR_GRAPHQL,       /* the API returned errors */
    RW_ERROR_PARSE,         /* response was not what we expected */
    RW_ERROR_CLI            /* the Railway CLI is missing or failed to run */
} RwErrorCode;

/* Transport: one HTTP POST. Replaceable so tests need no network. */
typedef struct {
    gint   status;          /* HTTP status */
    gchar *body;            /* response body, owned by the response */
    gint   retry_after;     /* Retry-After seconds, -1 if absent */
    gint   rate_remaining;  /* X-RateLimit-Remaining, -1 if absent */
    gint   rate_reset;      /* X-RateLimit-Reset, -1 if absent */
} RwHttpResponse;

typedef gboolean (*RwTransportFunc)(gpointer user_data, const gchar *url,
                                    const gchar *token, const gchar *body,
                                    RwHttpResponse *response, GError **error);
typedef void (*RwSleepFunc)(gpointer user_data, guint seconds);

typedef struct _RwClient RwClient;

/* An id and a name: a workspace, project, service or environment. */
typedef struct {
    gchar *id;
    gchar *name;
} RwEntity;

RwEntity *rw_entity_new(const gchar *id, const gchar *name);
void      rw_entity_free(RwEntity *entity);

/* Overwrite a secret with zeros, then free it. Safe on NULL. */
void rw_secure_free(gchar *secret);

/* Variable name to value. Values are wiped when the table is destroyed. */
GHashTable *rw_variables_new(void);

/*
 * Pass NULL to use the Railway CLI login, which reaches every workspace the
 * signed-in user can access. A token, if given, is copied and handed to the
 * CLI through its environment only (RAILWAY_API_TOKEN).
 */
RwClient *rw_client_new(const gchar *token);
void      rw_client_free(RwClient *client);

/* Only used by transports that make HTTP calls. The CLI transport ignores it. */
void rw_client_set_endpoint(RwClient *client, const gchar *url);
void rw_client_set_transport(RwClient *client, RwTransportFunc func,
                             gpointer user_data, GDestroyNotify destroy);
void rw_client_set_sleep(RwClient *client, RwSleepFunc func, gpointer user_data);
void rw_client_set_max_retries(RwClient *client, guint retries);    /* default 5 */
void rw_client_set_max_wait(RwClient *client, guint seconds);       /* default 120 */

guint64 rw_client_get_request_count(const RwClient *client);
gint    rw_client_get_rate_remaining(const RwClient *client);       /* -1 unknown */
gint    rw_client_get_retry_after(const RwClient *client);          /* -1 unknown */

/*
 * Run one GraphQL operation. `variables` is borrowed and may be NULL.
 * Returns the "data" member (free with json_node_unref) or NULL on error.
 * 429 and 5xx responses are retried. A 429 that needs a longer wait than
 * the maximum fails with RW_ERROR_RATE_LIMITED so the caller can checkpoint.
 */
JsonNode *rw_client_execute(RwClient *client, const gchar *query,
                            JsonNode *variables, GError **error);

/* Arrays of RwEntity. Free with g_ptr_array_unref(). */
GPtrArray *rw_list_workspaces(RwClient *client, GError **error);
/* Projects are listed one workspace at a time. Without a workspace Railway lists nothing useful. */
GPtrArray *rw_list_projects(RwClient *client, const gchar *workspace_id, GError **error);
GPtrArray *rw_list_services(RwClient *client, const gchar *project_id, GError **error);
GPtrArray *rw_list_environments(RwClient *client, const gchar *project_id, GError **error);

/*
 * service_id NULL means the shared variables of the environment.
 * `unrendered` keeps references such as ${{Postgres.DATABASE_URL}} intact.
 */
GHashTable *rw_get_variables(RwClient *client, const gchar *project_id,
                             const gchar *environment_id, const gchar *service_id,
                             gboolean unrendered, GError **error);

/*
 * Create or update variables in one request. Never replaces: variables
 * missing from `variables` are left alone. `skip_deploys` stops Railway
 * redeploying on each write.
 */
gboolean rw_upsert_variables(RwClient *client, const gchar *project_id,
                             const gchar *environment_id, const gchar *service_id,
                             GHashTable *variables, gboolean skip_deploys,
                             GError **error);

gboolean rw_redeploy_service(RwClient *client, const gchar *service_id,
                           const gchar *environment_id, GError **error);

/*
 * The Railway CLI. rw_cli_find() returns the path or NULL (free with g_free).
 * rw_cli_set_program() overrides the path, for tests or a custom install.
 * rw_cli_version() returns text such as "railway 5.52.1".
 * rw_cli_whoami() fails with RW_ERROR_AUTH when nobody is signed in.
 * rw_cli_login() opens the browser sign-in and blocks until it ends.
 */
gchar   *rw_cli_find(void);
void     rw_cli_set_program(const gchar *path);
gchar   *rw_cli_version(GError **error);
gboolean rw_cli_whoami(gchar **name, gchar **email, GError **error);
gboolean rw_cli_login(GError **error);
gboolean rw_cli_logout(GError **error);

G_END_DECLS

#endif /* RAILWAY_H */
