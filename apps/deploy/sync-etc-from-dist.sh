#!/usr/bin/env bash
# Seed env/dist/etc/*.conf from repo *.conf.dist when missing, so Docker
# bind-mounts have files on first deploy. Does NOT overwrite existing .conf
# unless SYNC_ETC_FORCE=1 (avoids resetting SOAP, rates, etc. every deploy).
#
# Database and paths: docker-compose sets AC_* env vars; those override file
# values at runtime where supported.
#
# Intentional full reset from templates:
#   SYNC_ETC_FORCE=1 bash apps/deploy/sync-etc-from-dist.sh .

set -euo pipefail

repo_root="${1:-.}"
cd "$repo_root"

world_src="src/server/apps/worldserver/worldserver.conf.dist"
auth_src="src/server/apps/authserver/authserver.conf.dist"
etc_dir="env/dist/etc"

force=0
if [[ "${SYNC_ETC_FORCE:-}" == "1" || "${SYNC_ETC_FORCE:-}" == "true" ]]; then
  force=1
fi

if [[ ! -f "$world_src" ]]; then
  echo "sync-etc-from-dist: missing $world_src (wrong directory?)" >&2
  exit 1
fi

mkdir -p "$etc_dir"

world_dst="$etc_dir/worldserver.conf"
if [[ "$force" -eq 1 ]] || [[ ! -f "$world_dst" ]]; then
  cp -f "$world_src" "$world_dst"
  echo "sync-etc-from-dist: wrote $world_dst"
else
  echo "sync-etc-from-dist: skip $world_dst (exists; SYNC_ETC_FORCE=1 to replace)"
fi

auth_dst="$etc_dir/authserver.conf"
if [[ -f "$auth_src" ]]; then
  if [[ "$force" -eq 1 ]] || [[ ! -f "$auth_dst" ]]; then
    cp -f "$auth_src" "$auth_dst"
    echo "sync-etc-from-dist: wrote $auth_dst"
  else
    echo "sync-etc-from-dist: skip $auth_dst (exists; SYNC_ETC_FORCE=1 to replace)"
  fi
fi
