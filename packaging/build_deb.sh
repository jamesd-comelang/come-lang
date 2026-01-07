#!/bin/bash
set -e

# Configuration
PKG_NAME="come"
PKG_VERSION="1.0.0"
ARCH=$(dpkg --print-architecture)
PKG_DIR="packaging/come-pkg"
DEB_NAME="packaging/${PKG_NAME}_${PKG_VERSION}_${ARCH}.deb"

# Ensure we are in the project root
if [ ! -f "Makefile" ]; then
    echo "Error: Run this script from the project root."
    exit 1
fi

# 1. Build the distribution
echo "Building distribution..."
make dist-local

# 2. Prepare package structure
echo "Preparing package structure..."
rm -rf "$PKG_DIR"
mkdir -p "$PKG_DIR/DEBIAN"
mkdir -p "$PKG_DIR/usr/local/comelang"
mkdir -p "$PKG_DIR/usr/bin"

# Copy distribution files
cp -r build/dist/* "$PKG_DIR/usr/local/comelang/"

# Create symlink in /usr/bin
ln -s ../local/comelang/bin/come "$PKG_DIR/usr/bin/come"

# 3. Create control file
echo "Creating control file..."
DEFAULT_MAINTAINER="Keep Trying <keeptrying@google.com>"
# Use environment variable for maintainer if set, otherwise default
MAINTAINER=${MAINTAINER:-$DEFAULT_MAINTAINER}

sed -e "s|\${PKG_NAME}|$PKG_NAME|g" \
    -e "s|\${PKG_VERSION}|$PKG_VERSION|g" \
    -e "s|\${ARCH}|$ARCH|g" \
    -e "s|\${MAINTAINER}|$MAINTAINER|g" \
    packaging/debian/control > "$PKG_DIR/DEBIAN/control"

# 4. Build the package
echo "Building .deb package..."
dpkg-deb --build "$PKG_DIR" "$DEB_NAME"

echo "Package created: $DEB_NAME"
# Verify contents
echo "Verifying contents..."
dpkg-deb --contents "$DEB_NAME"
