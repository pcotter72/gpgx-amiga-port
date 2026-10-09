#!/bin/bash
#############################################################################
#
#  Genesis Plus GX - AmigaOS Build Script
#  For bebbo's amiga-gcc (MSYS2/Linux/macOS)
#
#  Usage:
#    ./build.sh              Build for 68030
#    ./build.sh 68060        Build for 68060
#    ./build.sh clean        Clean build
#    ./build.sh info         Show configuration
#
#############################################################################

echo ""
echo "Genesis Plus GX - AmigaOS Build Script"
echo "======================================="
echo ""

# Check for compiler
if ! command -v m68k-amigaos-gcc &> /dev/null; then
    echo "ERROR: m68k-amigaos-gcc not found in PATH"
    echo ""
    echo "Please install bebbo's amiga-gcc:"
    echo "  Windows: http://franke.ms/download/setup-amiga-gcc.exe"
    echo "  Linux/macOS: https://github.com/bebbo/amiga-gcc"
    echo ""
    echo "Make sure the bin directory is in your PATH."
    exit 1
fi

# Show compiler version
echo "Compiler: $(m68k-amigaos-gcc --version | head -1)"
echo ""

# Parse arguments
CPU="68030"
TARGET="all"

case "$1" in
    68040|68060|68080)
        CPU="$1"
        ;;
    clean)
        TARGET="clean"
        ;;
    info)
        TARGET="info"
        ;;
    debug)
        TARGET="debug"
        ;;
    help|--help|-h)
        echo "Usage: $0 [CPU|clean|info|debug]"
        echo ""
        echo "  CPU options: 68030 (default), 68040, 68060, 68080"
        echo "  clean: Remove build files"
        echo "  info:  Show build configuration"
        echo "  debug: Build with debug symbols"
        echo ""
        exit 0
        ;;
esac

# MSYS2 quirk: must use absolute paths
# Check if we're in MSYS2 and warn if using relative path
if [[ "$OSTYPE" == "msys" ]] || [[ "$OSTYPE" == "cygwin" ]]; then
    case "$PWD" in
        /[a-zA-Z]/*)
            # Good - absolute Windows path like /c/projects/gpgx
            ;;
        *)
            echo "WARNING: On MSYS2, use absolute paths!"
            echo "Current: $PWD"
            echo ""
            echo "Try: cd /c/path/to/gpgx-amiga"
            echo ""
            ;;
    esac
fi

# Build
if [ "$TARGET" = "all" ] || [ "$TARGET" = "debug" ]; then
    echo "Building for CPU: $CPU"
    echo ""
fi

make $TARGET CPU=$CPU USE_ASM_AUDIO=1 USE_ASM_VIDEO=1 USE_ASM_BLIP=1

if [ $? -eq 0 ]; then
    if [ "$TARGET" = "all" ]; then
        echo ""
        echo "Build successful!"
        echo "Output: GenesisPlusGX"
        ls -la GenesisPlusGX 2>/dev/null
    fi
else
    echo ""
    echo "BUILD FAILED!"
    exit 1
fi
