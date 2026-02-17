# OAuth Authentication Guide

The GizmoSQL ODBC Driver supports OAuth authentication through a server-side discovery flow. This allows users to authenticate via their organization's identity provider (IdP) without the driver needing to know OAuth client credentials.

## How It Works

The OAuth flow uses a server-side discovery pattern:

1. **Discovery** — The driver connects to the GizmoSQL server with a special username (`__discover__`). The server recognizes this as an OAuth discovery request and responds with the OAuth authorization URL.

2. **Browser authentication** — The driver opens the user's default web browser to the OAuth authorization URL. The user authenticates with their identity provider.

3. **Token exchange** — The GizmoSQL server acts as an OAuth confidential client, handling the authorization code exchange with the IdP. The server obtains an access token on behalf of the user.

4. **Token retrieval** — The driver polls the server until it receives the bearer token. This token is then used for all subsequent requests.

```
┌──────────┐     ┌──────────────┐     ┌──────────────┐     ┌─────┐
│  Driver   │     │ GizmoSQL     │     │  Identity    │     │User │
│  (ODBC)   │     │ Server       │     │  Provider    │     │     │
└─────┬─────┘     └──────┬───────┘     └──────┬───────┘     └──┬──┘
      │                  │                    │                │
      │ 1. __discover__  │                    │                │
      │─────────────────>│                    │                │
      │                  │                    │                │
      │ 2. OAuth URL     │                    │                │
      │<─────────────────│                    │                │
      │                  │                    │                │
      │ 3. Open browser ─────────────────────────────────────>│
      │                  │                    │                │
      │                  │                    │ 4. User login  │
      │                  │                    │<───────────────│
      │                  │                    │                │
      │                  │ 5. Auth code       │                │
      │                  │<───────────────────│                │
      │                  │                    │                │
      │                  │ 6. Exchange token   │                │
      │                  │───────────────────>│                │
      │                  │                    │                │
      │                  │ 7. Access token    │                │
      │                  │<───────────────────│                │
      │                  │                    │                │
      │ 8. Bearer token  │                    │                │
      │<─────────────────│                    │                │
      │                  │                    │                │
      │ 9. Authenticated │                    │                │
      │     requests     │                    │                │
      │─────────────────>│                    │                │
```

## Configuration

### Connection string

```
Driver=GizmoSQL ODBC Driver;host=gizmosql.example.com;port=443;authType=external;useEncryption=true
```

### DSN configuration (macOS/Linux)

```ini
[GizmoSQL-OAuth]
Driver        = GizmoSQL ODBC Driver
host          = gizmosql.example.com
port          = 443
authType      = external
useEncryption = true
```

### DSN configuration (Windows)

In the ODBC Data Source Administrator, select **OAuth / External** from the Authentication Type dropdown.

### Python (pyodbc)

```python
import pyodbc

conn = pyodbc.connect(
    "Driver=GizmoSQL ODBC Driver;"
    "host=gizmosql.example.com;"
    "port=443;"
    "authType=external;"
    "useEncryption=true"
)

cursor = conn.cursor()
cursor.execute("SELECT * FROM my_table")
```

## Server Requirements

The GizmoSQL server must be configured to support the OAuth discovery flow:

1. The server must recognize the `__discover__` username as an OAuth discovery request.
2. The server must respond with the OAuth authorization URL.
3. The server must be configured as an OAuth confidential client with the identity provider.
4. The server must handle the authorization code exchange and token issuance.

## Troubleshooting

### Browser does not open

The driver uses platform-specific commands to open the browser:
- **macOS**: `open`
- **Linux**: `xdg-open`
- **Windows**: `ShellExecute`

If the browser does not open automatically:
- On Linux, ensure `xdg-open` is installed and a default browser is configured.
- On headless servers, OAuth authentication is not supported (a browser is required).

### Authentication times out

The driver polls for up to 2 minutes waiting for the user to complete browser authentication. If authentication times out:
- Check that the GizmoSQL server is properly configured for OAuth.
- Ensure the identity provider is reachable from the server.
- Verify that the browser-based login completed successfully.

### Connection refused during discovery

Ensure the GizmoSQL server is running and the `host` and `port` are correct. The discovery step requires a successful connection to the server.

### TLS errors

When using OAuth with encryption enabled (recommended), ensure TLS is properly configured:
- Use `useEncryption=true` (default)
- Configure certificates via `trustedCerts` or `useSystemTrustStore` as needed
- See [CONFIGURATION.md](CONFIGURATION.md) for TLS property details
