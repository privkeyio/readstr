# Project Instructions for AI Agents

This file provides instructions and context for AI coding agents working on this project.

<!-- BEGIN BEADS INTEGRATION v:1 profile:minimal hash:7510c1e2 -->
## Beads Issue Tracker

This project uses **bd (beads)** for issue tracking. Run `bd prime` to see full workflow context and commands.

### Quick Reference

```bash
bd ready              # Find available work
bd show <id>          # View issue details
bd update <id> --claim  # Claim work
bd close <id>         # Complete work
```

### Rules

- Use `bd` for ALL task tracking — do NOT use TodoWrite, TaskCreate, or markdown TODO lists
- Run `bd prime` for detailed command reference and session close protocol
- Use `bd remember` for persistent knowledge — do NOT use MEMORY.md files

**Architecture in one line:** issues live in a local Dolt DB; sync uses `refs/dolt/data` on your git remote; `.beads/issues.jsonl` is a passive export. See https://github.com/gastownhall/beads/blob/main/docs/SYNC_CONCEPTS.md for details and anti-patterns.

## Session Completion

**When ending a work session**, you MUST complete ALL steps below. Work is NOT complete until `git push` succeeds.

**MANDATORY WORKFLOW:**

1. **File issues for remaining work** - Create issues for anything that needs follow-up
2. **Run quality gates** (if code changed) - Tests, linters, builds
3. **Update issue status** - Close finished work, update in-progress items
4. **PUSH TO REMOTE** - This is MANDATORY:
   ```bash
   git pull --rebase
   bd dolt push
   git push
   git status  # MUST show "up to date with origin"
   ```
5. **Clean up** - Clear stashes, prune remote branches
6. **Verify** - All changes committed AND pushed
7. **Hand off** - Provide context for next session

**CRITICAL RULES:**
- Work is NOT complete until `git push` succeeds
- NEVER stop before pushing - that leaves work stranded locally
- NEVER say "ready to push when you are" - YOU must push
- If push fails, resolve and retry until it succeeds
<!-- END BEADS INTEGRATION -->

## Machine Roles: Dev vs Server

This repo is checked out on two kinds of machines. **Figure out which one you are on before following the Beads "Session Completion" workflow above** — that workflow is written for the dev machine and does NOT apply on the server.

| | **Dev machine** | **Server / deploy host** |
|---|---|---|
| Purpose | Write code, track work | Pull latest code, build, serve |
| Beads | Source of truth. Dolt backend → syncs to `refs/dolt/data` on origin | Stealth mode (`.beads/` is in `.git/info/exclude`), SQLite backend — does NOT sync anywhere |
| `bd dolt push` / Session Completion beads steps | Apply | Do **not** apply — there is no path for beads data to reach origin |
| Hosts readstr.privkey.io | No | Yes (host Caddy → app container) |

**How to tell where you are:** run `bd vc status`. If it errors with *"requires Dolt backend"*, you are on the **server** — do all issue tracking on the dev machine instead, and skip every beads/`bd dolt push` step when ending a session here. (Note: on the dev box, `bd dolt push` is the correct sync command in bd 1.0.4 — it pushes the Dolt data to `refs/dolt/data` on origin. There is no `bd daemon` in 1.0.4; `bd vc status` is just for inspecting local branch/commit state.)

**Server session completion** is just code, no beads:
```bash
git pull --rebase
git push        # only if you committed code changes here
```

## Build & Test

```bash
npm install
npm run build       # next build --webpack
npx tsc --noEmit    # typecheck (no dedicated script)
npm run lint        # next lint
npm run dev         # local dev server
```

## Deployment (server host only)

The app runs via Docker Compose: a `postgres` service and an `app` service built from `Dockerfile`. The bundled `caddy` service is disabled (`docker-compose.override.yml`); the **host** Caddy owns ports 80/443 and reverse-proxies to the app on `127.0.0.1:3100`.

To ship repo updates on the server:
```bash
git pull --rebase
docker compose up -d --build app   # rebuild + restart only the app container
docker compose ps                  # confirm it's up
docker compose logs -f app         # watch startup
```
Postgres keeps its named volume across rebuilds. Note: the host Caddy bind-mounts its Caddyfile as a single file — edits that replace the file's inode won't be seen until the container is restarted.

## Architecture Overview

Next.js 16 (App Router) + tRPC + Prisma/Postgres RSS & Nostr feed reader (PWA). Nostr auth is keyless via NIP-07 (`src/contexts/NostrAuthContext.tsx`); cross-device sync of subscriptions (kind 30404) and read status (kind 30405) goes through Nostr relays (`src/lib/nostr-sync.ts`) — the app never handles a private key.

## Conventions & Patterns

_Add your project-specific conventions here_
