# Security

## Reporting a vulnerability

Please do not open a public issue for a security problem. Email ross@wexfordhouse.co.za with what you found and how to reproduce it. Do not include real tokens or variable values.

## What to know

- The tool reads every variable you can reach in Railway and writes the values to `audit.csv`. That file holds live secrets. It is created owner only (0600). Keep it safe and delete it when you have finished.
- The tool never sees your Railway token. The Railway CLI holds it. The tool never writes to Railway.
- Supported version: the latest release only.
