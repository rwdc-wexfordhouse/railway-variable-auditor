/*
 * Demo Railway: an in-memory stand-in with invented company data, used to
 * take screenshots for the documentation. Built only with -Dautomation=true.
 *
 * Copyright © 2026, Wexford House <https://wexfordhouse.co.za/>
 * Copyright © 2026, Ross Cameron <ross@wexfordhouse.co.za>
 * Licensed under the GNU General Public License, version 3.
 */
#ifndef DEMO_H
#define DEMO_H

#include "railway.h"
#include "rvcore.h"

G_BEGIN_DECLS

gpointer demo_new(void);
/* A fake Postgres and a fake tunnel for the database step. Their data is the demo. */
const RvDbOps     *demo_db_ops(void);
const RvTunnelOps *demo_tunnel_ops(void);
void     demo_free(gpointer demo);
/* Reads in this project fail, to show an incomplete scan. NULL clears it. */
void     demo_set_failing_project(gpointer demo, const gchar *project_id);
gboolean demo_send(gpointer demo, const gchar *url, const gchar *token, const gchar *body,
                   RwHttpResponse *response, GError **error);

G_END_DECLS

#endif /* DEMO_H */
