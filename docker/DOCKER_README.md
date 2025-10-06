# ObsidianIRC UnrealIRCd Docker Deployment

This Docker deployment provides a ready-to-run UnrealIRCd server with integrated ObsidianIRC modules:
- **o-icon**: Provides server icon functionality via `icon` configuration
- **o-register**: Enables account registration with `/REGISTER` command

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
   # Example configuration
   SERVER_NAME=irc.mynetwork.com
   NETWORK_NAME=MyNetwork
   ADMIN_EMAIL=admin@mynetwork.com
   IRC_PORT=6669
   SSL_PORT=6697
   ICON_URL=https://mynetwork.com/favicon.ico
   MOTD_TEXT=Welcome to MyNetwork! Powered by ObsidianIRC.
   ```

3. **Start the server**:
   ```bash
   docker compose up -d
   ```

4. **Connect to your server**:
   - **Plain IRC**: `irc://localhost:6669`
   - **SSL/TLS**: `ircs://localhost:6697` (uses temporary self-signed cert)
   - **Default oper**: username `admin`, password `admin123`

### Using Docker Run

```bash
docker build -t obsidian-unrealircd .

docker run -d \
  --name obsidian-unrealircd \
  -p 6669:6667 \
  -p 6697:6697 \
  -v unrealircd_conf:/home/unrealircd/unrealircd/conf \
  -v unrealircd_data:/home/unrealircd/unrealircd/data \
  -v unrealircd_logs:/home/unrealircd/unrealircd/logs \
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
| `IRC_PORT` | `6667` | Plain text IRC port (internal) |
| `SSL_PORT` | `6697` | SSL/TLS IRC port (internal) |
| `ICON_URL` | *(empty)* | Server icon URL for o-icon module |
| `MOTD_TEXT` | `Welcome to our IRC server!` | Message of the Day |

### Volumes

The container uses the following persistent volumes:

- **`/home/unrealircd/unrealircd/conf`**: Configuration files
- **`/home/unrealircd/unrealircd/data`**: Account database and persistent data
- **`/home/unrealircd/unrealircd/logs`**: Server logs
- **`/home/unrealircd/unrealircd/tls`**: SSL certificates

### Ports

- **6667**: Plain text IRC (mapped to `$IRC_PORT` on host)
- **6697**: SSL/TLS IRC (mapped to `$SSL_PORT` on host)

## ObsidianIRC Modules

### o-icon Module
Provides server icon functionality. Configure in your `unrealircd.conf`:
```
icon {
    host "https://example.com/favicon.ico";
};
```
Or set via `ICON_URL` environment variable.

### o-register Module
Enables account registration with:
- `/REGISTER <account> <email> <password>` - Register new account
- `/LISTACC [account]` - List accounts (oper only)

Account data is stored in JSON format in `/home/unrealircd/unrealircd/data/obsidian-account.db`.

## First Run Behavior

On first run, the container will:

1. **Generate configuration** from template using environment variables
2. **Create self-signed SSL certificate** for secure connections
3. **Initialize data directory** for account storage
4. **Start UnrealIRCd** with ObsidianIRC modules loaded

Subsequent runs will preserve existing configuration and data.

## Operations

### Viewing Logs
```bash
# Real-time logs
docker compose logs -f unrealircd

# Container logs
docker logs obsidian-unrealircd
```

### Accessing Server Shell
```bash
docker compose exec unrealircd sh
```

### Updating Configuration
1. Edit files in the config volume or mount your own config:
   ```bash
   docker compose exec unrealircd vi /home/unrealircd/unrealircd/conf/unrealircd.conf
   ```

2. Restart to reload:
   ```bash
   docker compose restart unrealircd
   ```

### Backup Data
```bash
# Backup volumes
docker run --rm -v unrealircd_data:/data -v $(pwd):/backup alpine tar czf /backup/unrealircd-data.tar.gz -C /data .
```

## Testing the Installation

1. **Connect with IRC client**:
   ```
   /server localhost:6669
   ```

2. **Test registration module**:
   ```
   /REGISTER testuser test@example.com password123
   ```

3. **Verify icon module** (if configured):
   Check client's server info for icon URL.

4. **Test operator access**:
   ```
   /OPER admin admin123
   /LISTACC
   ```

## Troubleshooting

### Container won't start
- Check logs: `docker compose logs unrealircd`
- Verify environment variables in `.env`
- Ensure ports aren't already in use

### Configuration errors
- Validate config: `docker compose exec unrealircd ./unrealircd configtest`
- Reset config: Remove config volume and restart

### Module issues
- Check if modules loaded: Look for "o-icon" and "o-register" in logs
- Verify module dependencies are met

### SSL/TLS problems
- Check certificate generation in logs
- Verify TLS directory permissions
- For production, mount your own certificates to `/home/unrealircd/unrealircd/tls/`

### Using Production SSL Certificates

Mount your certificates as a volume:

```bash
# Place your certificates in ./ssl/ directory
# Files needed: server.cert.pem and server.key.pem

docker compose up -d

# Or with docker run:
docker run -d \
  -v ./ssl:/home/unrealircd/unrealircd/tls:ro \
  obsidian-unrealircd
```

## Security Notes

- Change default oper password in production
- Use proper SSL certificates instead of self-signed
- Configure firewall rules appropriately
- Regular security updates of base image

## Development

To modify modules or configuration:

1. Edit module source files (`*.c`)
2. Rebuild container: `docker compose build --no-cache`
3. Restart: `docker compose up -d`

## Support

For issues related to:
- **ObsidianIRC modules**: https://github.com/ObsidianIRC/UnrealIRCd-Modules/issues
- **UnrealIRCd core**: https://www.unrealircd.org/docs/
- **Docker setup**: Check container logs and environment configuration