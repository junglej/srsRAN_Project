#!/bin/bash
# Restart the srsRAN gNB Docker container (X310 + U50 VART offload).
# Faster than start_srsran_gnb.sh because it does not recreate the container.
set -euo pipefail

PROJECT_DIR="/home/ran/Documents/srsRAN_Project"
COMPOSE_FILE="docker/docker-compose-gnb-vart.yml"
CONTAINER_NAME="srsran-gnb-vart"

cd "$PROJECT_DIR"

echo "==> Restarting $CONTAINER_NAME container..."
docker-compose -f "$COMPOSE_FILE" restart

sleep 3

if docker ps --format '{{.Names}}' | grep -q "^${CONTAINER_NAME}$"; then
    echo "==> $CONTAINER_NAME is running."
    echo "==> Recent logs:"
    docker logs --tail 20 "$CONTAINER_NAME"
else
    echo "ERROR: $CONTAINER_NAME failed to start."
    exit 1
fi
