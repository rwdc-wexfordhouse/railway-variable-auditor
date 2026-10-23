/*
 * The secure tunnel to a database: `railway connect <service> --tunnel-only`.
 *
 * The CLI opens an SSH tunnel to a private database, with no public TCP proxy, and holds
 * it open. We start it on a free local port, wait for the port, and stop it afterwards.
 * Its output may print connection details, including the password, so it is thrown away.
 *
 * Copyright © 2026, Wexford House <https://wexfordhouse.co.za/>
 * Copyright © 2026, Ross Cameron <ross@wexfordhouse.co.za>
 * Licensed under the GNU General Public License, version 3.
 */
#include "rv-private.h"

#include <gio/gio.h>
#include <string.h>

#ifndef G_OS_WIN32
#include <signal.h>
#endif

#define WAIT_MS 45000
#define STEP_MS 250

typedef struct {
    GSubprocess *process;
} Tunnel;

static guint
free_port(GError **error)
{
    GSocketListener *listener = g_socket_listener_new();
    guint16 port = g_socket_listener_add_any_inet_port(listener, NULL, error);

    g_socket_listener_close(listener);
    g_object_unref(listener);
    return port;
}

/* Is the local end of the tunnel accepting connections? */
static gboolean
port_open(guint port)
{
    GSocketClient *client = g_socket_client_new();
    GSocketConnection *connection;

    g_socket_client_set_timeout(client, 2);
    connection = g_socket_client_connect_to_host(client, "127.0.0.1", (guint16)port, NULL, NULL);
    g_object_unref(client);
    if (connection == NULL)
        return FALSE;
    g_object_unref(connection);
    return TRUE;
}

static gboolean
has_exited(GSubprocess *process)
{
    return g_subprocess_get_identifier(process) == NULL;   /* NULL once it has exited */
}

static void
tunnel_close(gpointer data, gpointer handle)
{
    Tunnel *t = handle;

    (void)data;
    if (t == NULL)
        return;
    if (!has_exited(t->process)) {
#ifndef G_OS_WIN32
        /* Ctrl+C lets the CLI end its own SSH process. */
        g_subprocess_send_signal(t->process, SIGINT);
        for (guint waited = 0; waited < 5000 && !has_exited(t->process); waited += 100)
            g_usleep(100 * 1000);
#endif
        if (!has_exited(t->process))
            g_subprocess_force_exit(t->process);
    }
    g_object_unref(t->process);
    g_free(t);
}

static gboolean
tunnel_open(gpointer data, const gchar *project_id, const gchar *environment_id,
            const gchar *service, guint *port_out, gpointer *handle, GError **error)
{
    gchar *program = rw_cli_find();
    gchar *port_text;
    guint port;
    GSubprocessLauncher *launcher;
    GSubprocess *process;
    GError *local = NULL;
    Tunnel *t;

    (void)data;
    if (program == NULL) {
        g_set_error_literal(error, RV_ERROR, RV_ERROR_TUNNEL,
                            "The Railway CLI was not found, and it opens the secure tunnel.");
        return FALSE;
    }
    port = free_port(&local);
    if (port == 0) {
        g_set_error(error, RV_ERROR, RV_ERROR_TUNNEL, "No free local port for the tunnel: %s", local->message);
        g_error_free(local);
        g_free(program);
        return FALSE;
    }
    port_text = g_strdup_printf("%u", port);

    launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_SILENCE
                                         | G_SUBPROCESS_FLAGS_STDERR_SILENCE);
    process = g_subprocess_launcher_spawn(launcher, &local, program, "connect", service, "-p", project_id,
                                          "-e", environment_id, "--tunnel-only", "-P", port_text, NULL);
    g_object_unref(launcher);
    g_free(program);
    g_free(port_text);
    if (process == NULL) {
        g_set_error(error, RV_ERROR, RV_ERROR_TUNNEL, "Could not start the tunnel: %s", local->message);
        g_error_free(local);
        return FALSE;
    }

    for (guint waited = 0; waited < WAIT_MS; waited += STEP_MS) {
        if (has_exited(process)) {
            g_set_error(error, RV_ERROR, RV_ERROR_TUNNEL,
                        "The tunnel to %s did not open. In a terminal, run: railway connect %s --tunnel-only. "
                        "Railway SSH keys must be registered once (railway ssh keys) and the database must be running.",
                        service, service);
            g_object_unref(process);
            return FALSE;
        }
        if (port_open(port))
            break;
        g_usleep(STEP_MS * 1000);
        if (waited + STEP_MS >= WAIT_MS) {
            g_set_error(error, RV_ERROR, RV_ERROR_TUNNEL, "The tunnel to %s did not open within %d seconds.",
                        service, WAIT_MS / 1000);
            tunnel_close(NULL, (t = g_new0(Tunnel, 1), t->process = process, t));
            return FALSE;
        }
    }
    t = g_new0(Tunnel, 1);
    t->process = process;
    *handle = t;
    *port_out = port;
    return TRUE;
}

static const RvTunnelOps cli_ops = { tunnel_open, tunnel_close };

const RvTunnelOps *
rv_tunnel_ops_cli(void)
{
    return &cli_ops;
}
