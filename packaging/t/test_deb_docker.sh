#!/bin/bash
set -e

# Ensure we are in the project root
if [ -f "Makefile" ]; then
    echo "Running from project root."
elif [ -f "../../Makefile" ] && [ -d "../../packaging" ]; then
    echo "Running from packaging/t/, moving to root..."
    cd ../..
else
    echo "Error: Could not find project root (Makefile not found)."
    echo "Please run this script from the project root or packaging/t/."
    exit 1
fi

# Build the docker image
# We need to be in the root so we can copy the .deb file
DEB_FILE=$(ls packaging/come_*.deb | head -n 1)

if [ -z "$DEB_FILE" ]; then
    echo "Error: .deb file not found in packaging/ directory."
    exit 1
fi

echo "Check for docker permissions..."
if ! docker info > /dev/null 2>&1; then
    echo "Error: Unable to connect to Docker daemon."
    echo "You may need to run this script with sudo, or add your user to the 'docker' group:"
    echo "  sudo $0"
    echo "  sudo usermod -aG docker \$USER"
    exit 1
fi

echo "Building Docker image..."
docker build -t come-verify -f packaging/t/Dockerfile --build-arg DEB_FILE=$DEB_FILE .

echo "Running verification container..."
docker run --rm come-verify
