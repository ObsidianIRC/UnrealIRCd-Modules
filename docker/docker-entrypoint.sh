#!/bin/sh
set -eu

# Default values for environment variables
export SERVER_NAME="${SERVER_NAME:-irc.example.com}"
export IRC_PORT="${IRC_PORT:-6667}"
export SSL_PORT="${SSL_PORT:-6697}"
export NETWORK_NAME="${NETWORK_NAME:-ObsidianNetwork}"
export ADMIN_EMAIL="${ADMIN_EMAIL:-admin@example.com}"
export ICON_URL="${ICON_URL:-}"
export MOTD_TEXT="${MOTD_TEXT:-Welcome to our IRC server!}"

# Configuration file paths
CONF_DIR="/home/unrealircd/unrealircd/conf"
TEMPLATE_FILE="/etc/unrealircd/unrealircd.conf.template"
CONFIG_FILE="$CONF_DIR/unrealircd.conf"
TLS_DIR="/home/unrealircd/unrealircd/conf/tls"
DATA_DIR="/home/unrealircd/unrealircd/data"

echo "Starting ObsidianIRC UnrealIRCd Docker container..."

# Create necessary directories and set ownership
mkdir -p "$CONF_DIR" "$TLS_DIR" "$DATA_DIR"
# The entrypoint runs as root initially, so we can set proper ownership
chown unrealircd:unrealircd "$CONF_DIR" "$TLS_DIR" "$DATA_DIR"

# Generate icon configuration if ICON_URL is provided
if [ -n "$ICON_URL" ]; then
    export ICON_CONFIG="icon { host \"$ICON_URL\"; };"
    echo "Icon configuration enabled: $ICON_URL"
else
    export ICON_CONFIG=""
    echo "Icon configuration disabled"
fi

# Generate cloak keys if not provided
if [ -z "${CLOAK_KEY1:-}" ] || [ -z "${CLOAK_KEY2:-}" ] || [ -z "${CLOAK_KEY3:-}" ]; then
    echo "Generating random cloak keys..."
    # Generate 80+ character mixed alphanumeric keys (a-zA-Z0-9) as required by UnrealIRCd 6.x
    # Use /dev/urandom with tr to generate proper mixed case keys
    export CLOAK_KEY1=$(cat /dev/urandom | tr -dc 'a-zA-Z0-9' | fold -w 80 | head -n 1)
    export CLOAK_KEY2=$(cat /dev/urandom | tr -dc 'a-zA-Z0-9' | fold -w 80 | head -n 1)
    export CLOAK_KEY3=$(cat /dev/urandom | tr -dc 'a-zA-Z0-9' | fold -w 80 | head -n 1)
    echo "WARNING: Using generated cloak keys for testing. In production, set CLOAK_KEY1, CLOAK_KEY2, CLOAK_KEY3 environment variables."
fi

# Check if this is a fresh volume (no custom config yet)
# We use a marker file to detect if we've already generated config for this volume
FIRST_RUN_MARKER="$CONF_DIR/.docker_initialized"

if [ ! -f "$FIRST_RUN_MARKER" ]; then
    echo "Fresh volume detected - generating configuration from template..."

    # Process template with environment variables
    envsubst < "$TEMPLATE_FILE" > "$CONFIG_FILE"

    echo "Configuration generated at $CONFIG_FILE"
    echo "Server configuration:"
    echo "  Server Name: $SERVER_NAME"
    echo "  Network Name: $NETWORK_NAME"
    echo "  IRC Port: $IRC_PORT"
    echo "  SSL Port: $SSL_PORT"
    echo "  Admin Email: $ADMIN_EMAIL"

    # Handle SSL certificates
    if [ ! -f "$TLS_DIR/server.cert.pem" ] || [ ! -f "$TLS_DIR/server.key.pem" ]; then
        echo "No SSL certificates found in $TLS_DIR"
        echo "For production, mount your certificates to $TLS_DIR"
        echo "For testing, generating minimal self-signed certificate..."

        # Create minimal SSL certificate for testing only
        openssl req -x509 -newkey rsa:2048 -keyout "$TLS_DIR/server.key.pem" \
            -out "$TLS_DIR/server.cert.pem" -days 1 -nodes \
            -subj "/CN=$SERVER_NAME"

        # Set proper permissions
        chmod 600 "$TLS_DIR/server.key.pem"
        chmod 644 "$TLS_DIR/server.cert.pem"

        echo "Temporary SSL certificate generated (valid for 1 day)"
        echo "WARNING: This is for testing only! Use proper certificates in production."
    else
        echo "Using existing SSL certificates from $TLS_DIR"
    fi

    # Mark this volume as initialized
    touch "$FIRST_RUN_MARKER"

else
    echo "Volume already initialized, using existing configuration"
    echo "To regenerate configuration, remove the config volume or delete $FIRST_RUN_MARKER"
fi

# Check if configuration is valid as the unrealircd user
echo "Validating configuration..."
if su-exec unrealircd ./unrealircd configtest; then
    echo "Configuration is valid"
else
    echo "ERROR: Configuration validation failed!"
    echo "Please check your configuration file at $CONFIG_FILE"
    exit 1
fi

# Create PID directory
mkdir -p /home/unrealircd/unrealircd/tmp

echo "Starting UnrealIRCd with ObsidianIRC modules..."

# Switch to unrealircd user and execute the main command
# For docker, we need to run in foreground and handle signals properly
if [ "$1" = "./unrealircd" ] && [ "$2" = "-F" ]; then
    # Start unrealircd and tail the logs to keep container running
    su-exec unrealircd ./unrealircd start
    exec su-exec unrealircd tail -f logs/ircd.log
else
    exec su-exec unrealircd "$@"
fi