#!/usr/bin/env python3
# A fake Railway CLI with invented data, for testing railway-variable-audit.
# Copyright © 2026, Wexford House <https://wexfordhouse.co.za/>
# Licensed under the GNU General Public License, version 3.
import json
import sys

args = sys.argv[1:]
if args[:1] == ["--version"]:
    print("railway 9.9.9")
elif args[:1] == ["whoami"]:
    print(json.dumps({"name": "Test Admin", "email": "admin@example.invalid"}))
elif args[:1] == ["api"]:
    query = args[1]
    variables = json.loads(sys.stdin.read() or "{}")
    page = {"pageInfo": {"hasNextPage": False, "endCursor": None}}
    if "me {" in query:
        data = {"me": {"workspaces": [{"id": "w1", "name": "Alpha"}, {"id": "w2", "name": "Beta"}]}}
    elif "projects(" in query:
        n = variables["workspaceId"]
        data = {"projects": dict(edges=[{"node": {"id": "p-" + n, "name": "Proj " + n}}], **page)}
    elif "services(" in query:
        data = {"project": {"services": dict(edges=[{"node": {"id": "s1", "name": "web"}}], **page)}}
    elif "environments(" in query:
        data = {"project": {"environments": dict(edges=[{"node": {"id": "e1", "name": "production"}}], **page)}}
    elif "variables(" in query:
        if variables.get("serviceId"):
            vals = {"API_TOKEN": "tok,with\"quote-" + variables["projectId"]}
        else:
            vals = {"SESSION_SECRET": "0123456789abcdef0123456789abcdef"}
        data = {"variables": vals}
    else:
        data = {}
    print(json.dumps({"data": data}))
else:
    sys.exit(1)
