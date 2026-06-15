#!/bin/bash
# Start the srsRAN gNB Docker container (X310 + U50 VART offload).
set -euo pipefail

PROJECT_DIR="/home/ran/Documents/srsRAN_Project"
COMPOSE_FILE="docker/docker-compose-gnb-vart.yml"
CONTAINER_NAME="srsran-gnb-vart"

cd "$PROJECT_DIR"

echo "==> Stopping any existing $CONTAINER_NAME container..."
docker-compose -f "$COMPOSE_FILE" down 2>/dev/null || true

# Make sure a stale container from a manual run is also removed.
if docker ps -a --format '{{.Names}}' | grep -q "^${CONTAINER_NAME}$"; then
    echo "==> Removing stale container $CONTAINER_NAME..."
    docker rm -f "$CONTAINER_NAME" 2>/dev/null || true
fi

echo "==> Starting srsRAN gNB container..."
docker-compose -f "$COMPOSE_FILE" up -d

sleep 3

if docker ps --format '{{.Names}}' | grep -q "^${CONTAINER_NAME}$"; then
    echo "==> $CONTAINER_NAME is running."
    echo "==> Recent logs:"
    docker logs --tail 20 "$CONTAINER_NAME"
else
    echo "ERROR: $CONTAINER_NAME failed to start."
    exit 1
fi
