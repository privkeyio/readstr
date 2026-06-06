#!/bin/sh
set -e

attempts=0
max_attempts=30
until npx prisma migrate deploy; do
  attempts=$((attempts + 1))
  if [ "$attempts" -ge "$max_attempts" ]; then
    echo "prisma migrate deploy failed after $max_attempts attempts" >&2
    exit 1
  fi
  echo "migrate deploy failed, retrying ($attempts/$max_attempts)..." >&2
  sleep 2
done

exec node server.js
