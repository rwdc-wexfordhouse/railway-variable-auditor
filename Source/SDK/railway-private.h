/*
 * Copyright © 2026, Wexford House <https://wexfordhouse.co.za/>
 * Copyright © 2026, Ross Cameron <ross@wexfordhouse.co.za>
 * Licensed under the GNU General Public License, version 3.
 */
#ifndef RAILWAY_PRIVATE_H
#define RAILWAY_PRIVATE_H

#include "railway.h"

G_BEGIN_DECLS

/* Default transport: runs `railway api`. The user data is not used. */
gboolean rw_cli_transport_send(gpointer user_data, const gchar *url,
                               const gchar *token, const gchar *body,
                               RwHttpResponse *response, GError **error);

/* Exposed for the unit tests. */
gboolean rw_cli_split_request(const gchar *body, gchar **query,
                              gchar **variables_json, GError **error);
gint     rw_cli_status_from_failure(const gchar *stderr_text);

G_END_DECLS

#endif /* RAILWAY_PRIVATE_H */
