# Host the ranked service

This runs the standalone matchmaking and provisional-rating service on a Linux
server. It uses Docker Compose, stores SQLite data in a persistent Docker volume,
and puts Caddy in front to provide HTTPS. The service container itself is only
reachable on the private Compose network.

This does **not** connect the game to the service. The existing game runtime does
not yet call these endpoints, and peer-reported match results are not cheat-proof.
Treat this as a community test service, not an authoritative competitive ladder.

## Requirements

- A Linux VPS with a public IPv4 address (and optionally IPv6), Docker Engine and
  the Docker Compose plugin.
- A domain or subdomain whose DNS A record points to that VPS. Set an AAAA record
  only if the server is reachable over IPv6 too.
- Inbound TCP ports 80 and 443 open in the VPS firewall and provider firewall.
  UDP 443 is optional for HTTP/3. Keep port 8765 closed to the public Internet.

## Deploy

Copy this project folder to the VPS, then from its root directory:

```sh
cp deploy/.env.example .env
```

Edit `.env` and set `RANKED_DOMAIN` to the DNS name you configured. Start the
stack:

```sh
docker compose --env-file .env -f deploy/compose.yml up -d --build
docker compose --env-file .env -f deploy/compose.yml ps
```

Caddy obtains and renews a public TLS certificate automatically when DNS points
to the server and ports 80/443 are reachable. Check service logs with:

```sh
docker compose --env-file .env -f deploy/compose.yml logs -f ranked proxy
```

The health endpoint is `https://YOUR_DOMAIN/healthz`; it should return
`{"status":"ok"}`. The matchmaking endpoints are under `/v1/` and require a
player bearer token.

## Create player accounts

Provision each unique player ID from the VPS. The command prints that player's
secret once; deliver it privately and do not paste it into chat, source control,
or public logs:

```sh
docker compose --env-file .env -f deploy/compose.yml run --rm ranked provision alice
docker compose --env-file .env -f deploy/compose.yml run --rm ranked provision bob
```

The returned credentials are long-lived in this prototype and there is no
self-service password reset or token rotation. To add another account, use a new
ID. Protect SSH access to the host and keep `.env` private.

## Operate and stop

Restart after an update with `docker compose ... up -d --build`; stop with
`docker compose ... down`. Do not use `down -v` unless you intend to permanently
delete the ranked database and Caddy certificate state. Back up the Docker
volumes `ranked-data` and `caddy-data` regularly, and test restoring a backup.

For a quick local deployment check, Docker Compose can run on a development
machine too. Set `RANKED_DOMAIN=localhost` in `.env`; public certificate issuance
is not available for localhost, so use the service's Python unit tests for local
verification rather than expecting public HTTPS.

## Current service limits

The HTTP server uses Python's standard library and SQLite. It has no account
registration, administrator web console, distributed rate limiting, multi-region
queue, server-verified gameplay, or public relay. Do not treat client-reported
results as trustworthy tournament outcomes. For a public competitive service,
add abuse controls and operational monitoring, then validate the real game
adapter and transport between separate machines before inviting players.
