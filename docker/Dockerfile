FROM alpine:3.19

# Install dependencies for building and running UnrealIRCd
RUN apk add --no-cache \
    build-base \
    openssl-dev \
    openssl \
    pcre2-dev \
    argon2-dev \
    libsodium-dev \
    jansson-dev \
    curl-dev \
    wget \
    tar \
    gettext \
    su-exec \
    netcat-openbsd \
    && rm -rf /var/cache/apk/*

# Create unrealircd user
RUN adduser -D -s /bin/sh unrealircd

# Set working directory
WORKDIR /tmp

# Download and extract UnrealIRCd source
RUN wget https://www.unrealircd.org/downloads/unrealircd-latest.tar.gz \
    && tar xzf unrealircd-latest.tar.gz \
    && rm unrealircd-latest.tar.gz

# Find the extracted directory and move it to a known location
RUN mv unrealircd-* unrealircd-source

WORKDIR /tmp/unrealircd-source

# Copy our custom modules from repository root
COPY --chown=unrealircd:unrealircd o-icon.c src/modules/third/
COPY --chown=unrealircd:unrealircd o-register.c src/modules/third/
COPY --chown=unrealircd:unrealircd obsidian.h include/

# Make Config executable and change ownership of entire source tree
RUN chmod +x Config \
    && chown -R unrealircd:unrealircd /tmp/unrealircd-source

# Switch to unrealircd user for building
USER unrealircd

# Configure UnrealIRCd
# Using non-interactive configuration with sane defaults
RUN echo "1" | ./Config \
    && make \
    && make install

# Switch back to root for final setup
USER root

# Create directories for runtime
RUN mkdir -p /home/unrealircd/unrealircd/conf \
    && mkdir -p /home/unrealircd/unrealircd/data \
    && mkdir -p /home/unrealircd/unrealircd/logs \
    && mkdir -p /home/unrealircd/unrealircd/tmp \
    && chown -R unrealircd:unrealircd /home/unrealircd

# Copy configuration template and entrypoint script from docker directory
COPY docker/docker-entrypoint.sh /usr/local/bin/
COPY docker/unrealircd.conf.template /etc/unrealircd/

RUN chmod +x /usr/local/bin/docker-entrypoint.sh

# Set default environment variables
ENV SERVER_NAME=irc.example.com \
    IRC_PORT=6667 \
    SSL_PORT=6697 \
    NETWORK_NAME=MyNetwork \
    ADMIN_EMAIL=admin@example.com \
    ICON_URL="" \
    MOTD_TEXT="Welcome to our IRC server!"

# Create volume mount points
VOLUME ["/home/unrealircd/unrealircd/conf", "/home/unrealircd/unrealircd/data", "/home/unrealircd/unrealircd/logs"]

# Set working directory but remain as root for entrypoint
WORKDIR /home/unrealircd/unrealircd

ENTRYPOINT ["docker-entrypoint.sh"]
CMD ["./unrealircd", "-F"]
