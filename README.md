Claude API Reverse Proxy
========================

This server reverse proxies Anthropic API requests through Caddy.
Tokens are protected via AES-256-GCM encryption. Only tokens
encrypted with the server's private key are accepted.

Endpoint: https://claude.storage.ac.cn

Issuing a Token
---------------

1. Run `claude setup-token` to get the raw API token.
2. Encrypt it:
     claude-token-encrypt "sk-ant-xxx..."
3. This outputs a storage- prefixed token (e.g. storage-a1b2c3...).
4. Give the storage- token to the user. They use it as their
   API key with the proxy endpoint.

The raw API key is never stored on the server. It is encrypted
inside the token itself and decrypted on each request.

Key Management
--------------

The encryption key is at /etc/claude-tokens/encryption.key
(256-bit AES key, hex-encoded). Keep this file secret.

If the key is rotated, all previously issued tokens become
invalid and must be re-encrypted.

Services
--------

claude-token-verify.service
  HTTP server on 127.0.0.1:9123 that decrypts and validates
  tokens. Caddy uses forward_auth to check every request
  against this service.

  systemctl status claude-token-verify
  systemctl restart claude-token-verify

claude-ratelimit-dashboard.service
  HTTP server on 127.0.0.1:9124 that monitors Caddy access
  logs and tracks rate limit status for all tokens.

  Dashboard: https://claude.storage.ac.cn/status
  JSON API:  https://claude.storage.ac.cn/status/api

  How it works:
    - Background thread tails /var/log/caddy/access.log
    - Processes ALL responses (not just 429s) to extract
      Anthropic-Ratelimit-Unified-* headers (utilization,
      status, reset time) from every API response
    - Updates retry_after and reset_ts only on 429 responses
    - Tokens persist forever in a JSON state file, surviving
      service restarts
    - Time-based auto-reset: tokens show "Available" once the
      reset time passes, even without a new request
    - Per-token "Last Updated" timestamps
    - Appends to ratelimit.log only on 429 responses

  Caddy routes /status* to this service via reverse_proxy.
  An unmatched handle_response block in the Caddyfile adds
  an X-Debug-Token header to ALL responses so the token
  prefix appears in the access log.

  CLI flags:
    -p PORT        HTTP port (default 9124)
    -l LOG_PATH    Caddy access log path
    -s STATE_PATH  JSON state file path

  systemctl status claude-ratelimit-dashboard
  systemctl restart claude-ratelimit-dashboard

Source Code
-----------

claude-token-utils:
  C sources in ~/claude-token-utils/. To rebuild:
    cd ~/claude-token-utils && make clean && make && make install
    systemctl restart claude-token-verify

claude-ratelimit-dashboard:
  C source in ~/claude-ratelimit-dashboard/. To rebuild:
    cd ~/claude-ratelimit-dashboard && make clean && make && make install
    systemctl restart claude-ratelimit-dashboard

Logs & State
------------

/var/log/caddy/access.log            Caddy access log (JSON)
/var/log/caddy/ratelimit.log         Rate limit 429 events (human-readable)
/var/lib/caddy/ratelimit-state.json  Persisted token state (survives restarts)

Testing
-------

Use claude-test.tth37.xyz for testing Caddyfile changes.
Never modify the production block directly. Add a test site
block, verify, then migrate to production.

Test client: set ANTHROPIC_BASE_URL on node196 to
https://claude-test.tth37.xyz and make a request.

Remember to chown caddy:caddy any new log files/dirs before
reloading Caddy (it runs as the caddy user).
