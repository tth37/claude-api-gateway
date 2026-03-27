# Claude API Gateway

A reverse proxy gateway for the Anthropic API built with Caddy. Tokens are protected via AES-256-GCM encryption — only tokens encrypted with the server's private key are accepted.

## Usage

```
claude-api-gateway <command> [args]

Commands:
  encrypt <raw-token>          Encrypt an API token
  start verifier [-p port]     Start the token verification server
  start dashboard [-p port]    Start the rate limit dashboard
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

## Services

### Verifier

HTTP server on `127.0.0.1:9123` that decrypts and validates tokens. Caddy uses `forward_auth` to check every request against this service.

```bash
systemctl status claude-api-gateway-verifier
systemctl restart claude-api-gateway-verifier
```

### Dashboard

HTTP server on `127.0.0.1:9124` that monitors Caddy access logs and tracks rate limit status for all tokens.

- **Dashboard:** `https://<your-domain>/status`
- **JSON API:** `https://<your-domain>/status/api`

**How it works:**

- Background thread tails `/var/log/caddy/access.log`
- Processes ALL responses (not just 429s) to extract `Anthropic-Ratelimit-Unified-*` headers (utilization, status, reset time)
- Updates `retry_after` and `reset_ts` only on 429 responses
- Tokens persist in a JSON state file, surviving service restarts
- Time-based auto-reset: tokens show "Available" once the reset time passes, even without a new request
- Appends to `ratelimit.log` only on 429 responses

**CLI flags:**

| Flag | Description | Default |
|------|-------------|---------|
| `-p` | HTTP port | `9124` |
| `-l` | Caddy access log path | `/var/log/caddy/access.log` |
| `-s` | JSON state file path | `/var/lib/caddy/ratelimit-state.json` |

## Building

```bash
make clean && make
sudo make install
```

Source code is deployed to `/opt/claude-api-gateway/` on the server.

```bash
cd /opt/claude-api-gateway
make clean && make && make install
systemctl restart claude-api-gateway-verifier
systemctl restart claude-api-gateway-dashboard
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
