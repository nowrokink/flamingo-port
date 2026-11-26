#!/bin/sh
# flamingo-worker on macOS: the Python helper (../flamingo_worker.py, standard library only) with a DXC for macOS.
#
# The DXC must be the main machine's version (it compiles the same HLSL into the DXIL the cache keeps): the Vulkan
# SDK for macOS of the same release as the main machine's ships it (bin/dxc, lib/libdxcompiler.dylib).
#
# Put in one folder: this file, flamingo_worker.py and worker.txt (the launcher's "Pasta do ajudante" writes
# worker.txt: the main machine's address and the password). Then:
#   sh setup_worker.sh              run the helper now, in this terminal
#   sh setup_worker.sh --install    also start it at every login (a launchd agent), log in ~/Library/Logs
#   sh setup_worker.sh --uninstall  remove that agent
# DXC is looked for in --dxc <path>, ./dxc/bin/dxc, $VULKAN_SDK/bin/dxc, ~/VulkanSDK/*/macOS/bin/dxc, then PATH.
# Jobs: the performance cores (an Apple Silicon Mac's efficiency cores are left to the system); --jobs N overrides.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
label=com.flamingo.worker
plist="$HOME/Library/LaunchAgents/$label.plist"
log="$HOME/Library/Logs/flamingo-worker.log"

mode=run
dxc=""
jobs=""
while [ $# -gt 0 ]; do
    case "$1" in
        --install) mode=install ;;
        --uninstall) mode=uninstall ;;
        --dxc) shift; dxc=$1 ;;
        --jobs) shift; jobs=$1 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
    shift
done

if [ "$mode" = uninstall ]; then
    launchctl unload "$plist" 2>/dev/null || true
    rm -f "$plist"
    echo "the helper no longer starts at login"
    exit 0
fi

command -v python3 >/dev/null 2>&1 || { echo "python3 not found: install the Xcode command line tools (xcode-select --install)" >&2; exit 1; }
[ -f "$here/flamingo_worker.py" ] || { echo "flamingo_worker.py isn't next to this file" >&2; exit 1; }
[ -f "$here/worker.txt" ] || { echo "worker.txt isn't next to this file (the launcher's \"Pasta do ajudante\" writes it)" >&2; exit 1; }

if [ -z "$dxc" ]; then
    for candidate in "$here/dxc/bin/dxc" "${VULKAN_SDK:-/nonexistent}/bin/dxc" "$HOME"/VulkanSDK/*/macOS/bin/dxc; do
        if [ -x "$candidate" ]; then dxc=$candidate; fi
    done
    [ -n "$dxc" ] || dxc=$(command -v dxc || true)
fi
[ -n "$dxc" ] && [ -x "$dxc" ] || { echo "dxc not found: install the Vulkan SDK for macOS (the main machine's release) or give --dxc <path>" >&2; exit 1; }

if [ -z "$jobs" ]; then
    jobs=$(sysctl -n hw.perflevel0.logicalcpu 2>/dev/null || sysctl -n hw.ncpu)
fi

echo "DXC: $dxc"
"$dxc" --version 2>&1 | head -2
echo "Compare that version with the main machine's (dxc --version there): they must be the same."
echo "Jobs: $jobs"

if [ "$mode" = install ]; then
    mkdir -p "$HOME/Library/LaunchAgents" "$HOME/Library/Logs"
    cat > "$plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key><string>$label</string>
    <key>ProgramArguments</key>
    <array>
        <string>$(command -v python3)</string>
        <string>-u</string>
        <string>$here/flamingo_worker.py</string>
        <string>--dxc</string><string>$dxc</string>
        <string>--jobs</string><string>$jobs</string>
    </array>
    <key>WorkingDirectory</key><string>$here</string>
    <key>RunAtLoad</key><true/>
    <key>KeepAlive</key><true/>
    <key>ProcessType</key><string>Background</string>
    <key>StandardOutPath</key><string>$log</string>
    <key>StandardErrorPath</key><string>$log</string>
</dict>
</plist>
EOF
    launchctl unload "$plist" 2>/dev/null || true
    launchctl load "$plist"
    echo "the helper starts at every login now; its log: $log"
    exit 0
fi

exec python3 -u "$here/flamingo_worker.py" --dxc "$dxc" --jobs "$jobs"
