# ObsidianIRC UnrealIRCd Docker Deployment

This Docker deployment provides a ready-to-run UnrealIRCd server with integrated ObsidianIRC modules:
- **obsidianirc**: Account registration, SASL authentication, server icon support
- **o-filehost**: File hosting with configurable backend URL

## Quick Start

### Using Docker Compose (Recommended)

1. **Clone and setup environment**:
   ```bash
   git clone https://github.com/ObsidianIRC/UnrealIRCd-Modules.git
   cd UnrealIRCd-Modules
   cp .env.example .env
   ```

2. **Edit `.env` file with your configuration**:
   ```bash
   SERVER_NAME=irc.mynetwork.com
   NETWORK_NAME=MyNetwork
   ADMIN_EMAIL=admin@mynetwork.com
   SSL_HOST_PORT=6697
   ICON_URL=https://mynetwork.com/favicon.ico
   FILEHOST_URL=https://files.mynetwork.com
   MOTD_TEXT=Welcome to MyNetwork! Powered by ObsidianIRC.
   ```

3. **Start the server**:
   ```bash
   docker compose up -d
   ```

4. **Connect to your server**:
   - **SSL/TLS IRC**: `ircs://localhost:6697` (uses self-signed cert by default)
   - **WebSocket**: `ws://localhost:8080` (plain; put behind a TLS reverse proxy for production)
   - **Default oper**: username `admin`, password `admin123`

### Using Docker Run

```bash
docker build -t obsidian-unrealircd .

docker run -d \
  --name obsidian-unrealircd \
  -p 6697:6697 \
  -v unrealircd_conf:/home/unrealircd/unrealircd/conf \
  -v unrealircd_data:/home/unrealircd/unrealircd/data \
  -v unrealircd_logs:/home/unrealircd/unrealircd/logs \
  -v unrealircd_tls:/home/unrealircd/unrealircd/tls \
  -e SERVER_NAME=irc.example.com \
  -e NETWORK_NAME=MyNetwork \
  -e ADMIN_EMAIL=admin@example.com \
  obsidian-unrealircd
```

## Configuration

### Environment Variables

| Variable | Default | Description |
|----------|---------|-------------|
| `SERVER_NAME` | `irc.example.com` | IRC server hostname |
| `NETWORK_NAME` | `ObsidianNetwork` | Network name displayed to users |
| `ADMIN_EMAIL` | `admin@example.com` | Administrator email address |
| `IRC_PORT` | `6667` | Plain text IRC port (internal, not published by default) |
| `SSL_PORT` | `6697` | SSL/TLS IRC port (internal container port) |
| `SSL_HOST_PORT` | `6697` | Host port mapped to SSL_PORT |
| `WS_PORT` | `8080` | WebSocket port (internal; route via reverse proxy for TLS) |
| `ICON_URL` | *(empty)* | Server icon URL |
| `FILEHOST_URL` | *(empty)* | File hosting backend URL for o-filehost module |
| `MOTD_TEXT` | `Welcome to our IRC server!` | Message of the Day |
| `CLOAK_KEY1/2/3` | *(generated)* | Host cloaking keys — set in production for consistency |
| `RPC_PASSWORD` | *(empty)* | Enable JSON-RPC API with this password (disabled if not set) |
| `RPC_PORT` | `8600` | Internal JSON-RPC port (not published; route via reverse proxy) |
| `CONF_BIND` | *(named volume)* | Host path for conf volume (bind mount override) |
| `DATA_BIND` | *(named volume)* | Host path for data volume (bind mount override) |
| `LOGS_BIND` | *(named volume)* | Host path for logs volume (bind mount override) |
| `TLS_BIND` | *(named volume)* | Host path for tls volume (bind mount override) |

### Volumes

The container uses the following persistent volumes:

- **`/home/unrealircd/unrealircd/conf`**: Configuration files (generated from template on first run)
- **`/home/unrealircd/unrealircd/data`**: Account database and persistent data
- **`/home/unrealircd/unrealircd/logs`**: Server logs
- **`/home/unrealircd/unrealircd/tls`**: SSL certificates (`server.cert.pem` + `server.key.pem`)

By default named Docker volumes are used. Set `CONF_BIND`, `DATA_BIND`, `LOGS_BIND`, or `TLS_BIND` to absolute paths to use bind mounts instead — useful when you need to know the exact location on disk (e.g. for cert sync scripts).

### Ports

- **6697** (host `SSL_HOST_PORT`): SSL/TLS IRC — connect IRC clients here
- **8080** (internal only): Plain WebSocket — put behind a TLS-terminating reverse proxy (nginx, Traefik, Caddy) for browser clients

## ObsidianIRC Modules

### obsidianirc Module
Provides account management and SASL authentication:
- `/REGISTER <account> <email> <password>` — Register a new account
- `/IDENTIFY <account> <password>` — Log in
- `/LOGOUT` — Log out
- `/LISTACC [account]` — List accounts (oper only)
- SASL PLAIN authentication support

Account data is stored in `/home/unrealircd/unrealircd/data/obsidian-account.db`.

Configure server icon via `ICON_URL` environment variable.

### o-filehost Module
Enables file hosting integration. Set `FILEHOST_URL` to your file hosting backend URL. When configured:
```
filehosts {
    host "https://files.example.com";
};
```

## First Run Behavior

On first run the container will:

1. **Generate configuration** from template using environment variables
2. **Generate self-signed SSL certificate** if none exists in the tls volume
3. **Start UnrealIRCd** with ObsidianIRC modules loaded

Subsequent runs preserve existing configuration and data. To regenerate config, remove the conf volume or delete `/home/unrealircd/unrealircd/conf/.docker_initialized`.

## Operations

### Viewing Logs
```bash
docker compose logs -f unrealircd
```

### Accessing Server Shell
```bash
docker compose exec unrealircd sh
```

### Updating Configuration
Edit config directly, then reload:
```bash
docker compose exec unrealircd vi /home/unrealircd/unrealircd/conf/unrealircd.conf
docker compose restart unrealircd
```

Or `/REHASH` from IRC as an oper (for most changes without a full restart).

### Backup Data
```bash
docker run --rm \
  -v unrealircd_data:/data \
  -v $(pwd):/backup \
  alpine tar czf /backup/unrealircd-data.tar.gz -C /data .
```

## Production SSL Certificates

By default a self-signed certificate is generated (valid 1 day). For production, provide real certificates.

**Option 1 — Bind mount from a cert sync script:**
```bash
# In .env
TLS_BIND=/path/to/certs
```
Place `server.cert.pem` and `server.key.pem` in that directory.

**Option 2 — Copy into the tls volume after first run:**
```bash
docker cp server.cert.pem obsidian-unrealircd:/home/unrealircd/unrealircd/tls/
docker cp server.key.pem  obsidian-unrealircd:/home/unrealircd/unrealircd/tls/
docker compose restart unrealircd
```

## Troubleshooting

### Container won't start
```bash
docker compose logs unrealircd
docker compose exec unrealircd ./unrealircd configtest
```

### Reset to defaults
Remove the conf volume and restart — the template is re-applied:
```bash
docker compose down -v
docker compose up -d
```

### SSL/TLS problems
- Verify cert files exist at `/home/unrealircd/unrealircd/tls/server.cert.pem`
- Check permissions (key should be `600`)
- Self-signed cert expires after 1 day — restart to regenerate, or mount real certs

## Security Notes

- Change the default oper password (`admin123`) before exposing to the internet
- Use real SSL certificates in production
- The plain IRC port (6667) is not published to the host by default — add it to `.env` if needed
- WebSocket port (8080) is not published; route through a TLS reverse proxy

## Support

- **ObsidianIRC modules**: https://github.com/ObsidianIRC/UnrealIRCd-Modules/issues
- **UnrealIRCd core**: https://www.unrealircd.org/docs/
