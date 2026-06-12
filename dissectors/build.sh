#!/usr/bin/env bash
set -euo pipefail

PLUGIN_VERSION=2.0.0
WIRESHARK_VERSION=$(pkg-config --modversion wireshark)
WIRESHARK_ABI_VERSION=${WIRESHARK_VERSION%.*}
WIRESHARK_CONFIG="$(pkg-config --variable=includedir wireshark)/wireshark/config.h"
WIRESHARK_PLUGINS_FOLDER="$HOME/.local/lib/wireshark/plugins/$WIRESHARK_ABI_VERSION/epan"

read -r -a WIRESHARK_CFLAGS <<< "$(pkg-config wireshark --cflags)"
read -r -a WIRESHARK_LIBS <<< "$(pkg-config wireshark --libs)"

COMMON_CFLAGS=(
	-DG_DISABLE_DEPRECATED
	-DG_DISABLE_SINGLE_INCLUDES
	-DHAVE_PLUGINS
	-DPLUGIN_VERSION=\"$PLUGIN_VERSION\"
	-Dh4bcm_EXPORTS
	-include "$WIRESHARK_CONFIG"
	"${WIRESHARK_CFLAGS[@]}"
	-I.
	-fvisibility=hidden
	-Qunused-arguments
	-Wall
	-Wextra
	-Wendif-labels
	-Wpointer-arith
	-Wformat-security
	-fwrapv
	-fno-strict-overflow
	-Wvla
	-Waddress
	-Wattributes
	-Wdiv-by-zero
	-Wignored-qualifiers
	-Wpragmas
	-Wno-overlength-strings
	-Wno-long-long
	-Wheader-guard
	-Wcomma
	-Wshorten-64-to-32
	-Wframe-larger-than=32768
	-Wc++-compat
	-Wunused-const-variable
	-Wshadow
	-Wold-style-definition
	-Wstrict-prototypes
	-Werror=implicit
	-Wno-pointer-sign
	-std=gnu99
	-fno-stack-protector
	-fpic
	-Wall
	-Wno-braced-scalar-init
	-Wno-unused-variable
	-Wno-reorder
	-O2
	-g
	-DNDEBUG
	-fPIC
	-fcolor-diagnostics
	-w
	-std=gnu11
	-Werror
)

mkdir -p build

echo "Building packet-h4bcm.o"
clang "${COMMON_CFLAGS[@]}" -o build/packet-h4bcm.c.o -c packet-h4bcm.c

echo "Building packet-btbrlmp.o"
clang "${COMMON_CFLAGS[@]}" -o build/packet-btbrlmp.c.o -c packet-btbrlmp.c

echo "Building plugin.o"
clang "${COMMON_CFLAGS[@]}" -o build/plugin.c.o -c plugin.c

echo "Building h4bcm.so"
clang --std=gnu11 -fPIC -w -O3 -shared -o h4bcm.so build/packet-btbrlmp.c.o build/packet-h4bcm.c.o build/plugin.c.o "${WIRESHARK_LIBS[@]}"

mkdir -p "$WIRESHARK_PLUGINS_FOLDER"
echo "Copying h4bcm.so to $WIRESHARK_PLUGINS_FOLDER"
cp h4bcm.so "$WIRESHARK_PLUGINS_FOLDER/"

# Set permission to dumpcap
if [ -e /usr/bin/dumpcap ]; then
	if [ -x /usr/bin/dumpcap ]; then
		echo "dumpcap is already executable"
	elif command -v sudo >/dev/null 2>&1 && sudo -n true 2>/dev/null; then
		echo "Setting permission to dumpcap: sudo chmod +x /usr/bin/dumpcap"
		sudo chmod +x /usr/bin/dumpcap
	elif [ -t 0 ] && command -v sudo >/dev/null 2>&1; then
		echo "Setting permission to dumpcap: sudo chmod +x /usr/bin/dumpcap"
		sudo chmod +x /usr/bin/dumpcap
	else
		echo "Skipping dumpcap chmod in non-interactive shell."
		echo "Run manually if packet capture needs it: sudo chmod +x /usr/bin/dumpcap"
	fi
fi
