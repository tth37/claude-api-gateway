# Claude API Gateway

A reverse proxy gateway for the Anthropic API built with Caddy. Tokens are protected via AES-256-GCM encryption — only tokens encrypted with the server's private key are accepted.

## Usage

```
claude-api-gateway <command> [args]

Commands:
  encrypt <raw-token>          Encrypt an API token
  start server [-p port]       Start the combined server (verifier + dashboard)
               [-l log] [-s state]
  start verifier [-p port]     Start the token verifier only
  start dashboard [-p port]    Start the dashboard only
                  [-l log] [-s state]
  help                         Show usage
```

## Issuing a Token

1. Get the raw API token (e.g. `sk-ant-xxx...`).
2. Encrypt it:
   ```bash
   claude-api-gateway encrypt "sk-ant-xxx..."
   ```
3. This outputs a `storage-` prefixed token (e.g. `storage-a1b2c3...`).
4. Give the `storage-` token to the user. They use it as their API key with the proxy endpoint.

The raw API key is never stored on the server. It is encrypted inside the token itself and decrypted on each request.

## Key Management

The encryption key is at `/etc/claude-api-gateway/encryption.key` (256-bit AES key, hex-encoded). Keep this file secret.

If the key is rotated, all previously issued tokens become invalid and must be re-encrypted.

## Server

Combined HTTP server on `127.0.0.1:9123` that handles both token verification and the rate limit dashboard.

- `/verify` — token decryption and validation (used by Caddy `forward_auth`)
- `/` — dashboard HTML
- `/api`, `/json` — dashboard JSON API

```bash
systemctl status claude-api-gateway
systemctl restart claude-api-gateway
```

**Dashboard features:**

- Background thread tails `/var/log/caddy/access.log`
- Processes ALL responses to extract `Anthropic-Ratelimit-Unified-*` headers
- Detects banned tokens (HTTP 403) and marks them with a "Banned" badge
- Tokens persist in a JSON state file, surviving service restarts
- Time-based auto-reset: tokens show "Available" once the reset time passes

**CLI flags:**

| Flag | Description | Default |
|------|-------------|---------|
| `-p` | HTTP port | `9123` |
| `-l` | Caddy access log path | `/var/log/caddy/access.log` |
| `-s` | JSON state file path | `/var/lib/caddy/ratelimit-state.json` |

## Building & Installation

```bash
make clean && make
sudo make install
sudo claude-api-gateway service install
sudo claude-api-gateway service start
```

### Service Management

```bash
claude-api-gateway service install     # Install and enable systemd service
claude-api-gateway service uninstall   # Stop, disable, and remove service
claude-api-gateway service start       # Start the service
claude-api-gateway service stop        # Stop the service
claude-api-gateway service restart     # Restart the service
claude-api-gateway service status      # Show service status
```

## Logs & State

| Path | Description |
|------|-------------|
| `/var/log/caddy/access.log` | Caddy access log (JSON) |
| `/var/log/caddy/ratelimit.log` | Rate limit 429 events (human-readable) |
| `/var/lib/caddy/ratelimit-state.json` | Persisted token state (survives restarts) |

## Testing

Use a test domain (e.g. `claude-test.tth37.xyz`) for Caddyfile changes. Never modify the production block directly — add a test site block, verify, then migrate to production.

Remember to `chown caddy:caddy` any new log files/dirs before reloading Caddy (it runs as the `caddy` user).
