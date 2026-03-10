# File: deploy/docker/Dockerfile
#
# Multi-stage build for minimal image size.
# Kairos Orchestration Daemon container image.
#
# Build:
#   docker build -f deploy/docker/Dockerfile -t kairos:latest .
#
# Run:
#   docker run -d --name kairos \
#     -v /path/to/kairos.toml:/etc/kairos/kairos.toml:ro \
#     -v kairos-data:/var/lib/kairos \
#     -p 8420:8420 \
#     kairos:latest
#
# Spec reference: §28.8

# ── Stage 1: Build ──────────────────────────────────────────────────────────
FROM ubuntu:24.04 AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        cmake \
        git \
        pkg-config \
        libsqlite3-dev \
        libsystemd-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN cmake -S . -B build \
        -DCMAKE_BUILD_TYPE=Release \
        -DKAIROS_HTTP=ON \
        -DKAIROS_VAULT=ON \
        -DKAIROS_DOCKER=ON \
        -DKAIROS_EMBED_ASSETS=ON \
        -DKAIROS_BUILD_TESTS=OFF \
    && cmake --build build --parallel "$(nproc)" \
    && cmake --install build --prefix /install

# ── Stage 2: Runtime ────────────────────────────────────────────────────────
FROM ubuntu:24.04 AS runtime

RUN apt-get update && apt-get install -y --no-install-recommends \
        libsqlite3-0 \
        libssl3t64 \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --no-create-home kairos

COPY --from=builder /install/bin/kairos /usr/bin/kairos
COPY --from=builder /install/share/kairos/ /usr/share/kairos/

# Create data directories.
RUN mkdir -p /var/lib/kairos /var/log/kairos /etc/kairos \
    && chown -R kairos:kairos /var/lib/kairos /var/log/kairos

# Default config.
COPY deploy/kairos.toml.example /etc/kairos/kairos.toml

USER kairos
WORKDIR /var/lib/kairos

EXPOSE 8420

HEALTHCHECK --interval=30s --timeout=5s --retries=3 \
    CMD ["/usr/bin/kairos", "status"]

ENTRYPOINT ["/usr/bin/kairos"]
CMD ["start", "--config", "/etc/kairos/kairos.toml"]
