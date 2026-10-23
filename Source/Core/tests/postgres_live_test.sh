#!/bin/sh
# Copyright (c) 2026, Wexford House <https://wexfordhouse.co.za/>
# Copyright (c) 2026, Ross Cameron <ross@wexfordhouse.co.za>
# Licensed under the GNU General Public License, version 3.
#
# Usage: postgres_live_test.sh <test-pg-live> [postgres image]
# Starts a throwaway PostgreSQL in a container, bound to 127.0.0.1 only, makes a role,
# and runs the live test of the real libpq interface against it. Exit 77 skips.
set -eu
exe=$1
image=${2:-postgres:16}
command -v docker >/dev/null || { echo "docker not found, skipping"; exit 77; }
docker image inspect "$image" >/dev/null 2>&1 || docker pull -q "$image" >/dev/null 2>&1 || { echo "cannot get $image, skipping"; exit 77; }
admin="AdminPw$(head -c 12 /dev/urandom | od -An -tx1 | tr -d ' \n')"
name="rvc-pg-live-$$"
cleanup() { docker rm -f "$name" >/dev/null 2>&1 || true; }
trap cleanup EXIT
docker run -d --name "$name" -e POSTGRES_PASSWORD="$admin" -p 127.0.0.1::5432 "$image" >/dev/null
i=0
until docker exec "$name" pg_isready -U postgres -h 127.0.0.1 >/dev/null 2>&1 && docker exec "$name" psql -U postgres -tAc "select 1" >/dev/null 2>&1; do
    i=$((i + 1)); [ $i -gt 60 ] && { echo "the server did not start"; exit 1; }; sleep 1
done
port=$(docker port "$name" 5432/tcp | head -1 | sed 's/.*://')
docker exec "$name" psql -U postgres -q \
    -c "CREATE ROLE ledger LOGIN PASSWORD 'OldPassword0123456789'" \
    -c 'CREATE DATABASE ledgerdb OWNER ledger' \
    -c "CREATE ROLE \"Odd Role\"\"x\" LOGIN PASSWORD 'OriginalOddPassword0123'" >/dev/null
echo "server: $image on 127.0.0.1:$port, password_encryption=$(docker exec "$name" psql -U postgres -tAc 'show password_encryption')"
RVC_PG_PORT=$port RVC_PG_ADMIN_PASSWORD=$admin "$exe"
