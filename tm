#!/usr/bin/env bash
# tm: run Trackmania (2020) on Apple Silicon Macs.
# Ubisoft Connect + Trackmania -> Wine (CrossOver 24 based, Sikarugir build) -> D3DMetal or DXMT -> Metal.
# See README.md for how it works and why each setting exists.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DATA="${TM_DATA:-$ROOT/data}"
RUNTIME="$DATA/runtime"            # Sikarugir template Frameworks: dylibs, renderers, engine
WINE_DIR="$RUNTIME/wswine.bundle"
WINE="$WINE_DIR/bin/wine"
LOG="$DATA/wine.log"

SIKARUGIR="https://github.com/Sikarugir-App"
TEMPLATE_URL="$SIKARUGIR/Template/releases/download/v1.0/Template-1.0.19.tar.xz"
TEMPLATE_SHA256="ce9e4150fd2f0f3e8694814d82133eaaae1b1aabe5b8f2479155b88271cf5f9f"
ENGINE_URL="$SIKARUGIR/Engines/releases/download/v1.0/WS12WineCX24.0.7_7.tar.xz"
ENGINE_SHA256="203f9e9fd6c2cc77e6525d798a434ced326145db34a356355e05659d3445fd1c"
UBISOFT_URL="https://static3.cdn.ubi.com/orbit/launcher_installer/UbisoftConnectInstaller.exe"

export WINEPREFIX="$DATA/prefix"
export WINEDEBUG="${WINEDEBUG:--all}"
export WINEMSYNC=1
export WINE_SIMULATE_WRITECOPY=1                            # CEF expects PAGE_READWRITE, not PAGE_WRITECOPY
export DYLD_FALLBACK_LIBRARY_PATH="$RUNTIME:/usr/lib"       # the engine resolves its dylibs via @rpath
# Openplanet loads through a dinput8.dll next to the game. Falls back to Wine's own when absent.
export WINEDLLOVERRIDES="${WINEDLLOVERRIDES:+$WINEDLLOVERRIDES;}dinput8=n,b"

UBI_DIR="$WINEPREFIX/drive_c/Program Files (x86)/Ubisoft/Ubisoft Game Launcher"
UBI_EXE="$UBI_DIR/UbisoftConnect.exe"
GAME_DIR="$UBI_DIR/games/Trackmania"
GAME_EXE="$GAME_DIR/Trackmania.exe"
OP_LOADER=dinput8_openplanet.dll   # Openplanet's own loader while fastread is on

say() { printf '\033[1m==> %s\033[0m\n' "$*"; }
warn() { printf '\033[33mwarning: %s\033[0m\n' "$*" >&2; }
die() { printf '\033[31merror: %s\033[0m\n' "$*" >&2; exit 1; }

# Direct3D 11 translator for the game. It must be set before Ubisoft Connect starts:
# the game inherits its environment.
select_renderer() {
  case "${TM_RENDERER:-d3dmetal}" in
    d3dmetal)
      export WINEDLLPATH_PREPEND="$RUNTIME/renderer/d3dmetal/wine"
      export CX_APPLEGPTK_LIBD3DSHARED_PATH="$RUNTIME/renderer/d3dmetal/external/libd3dshared.dylib"
      export DYLD_FALLBACK_FRAMEWORK_PATH="$RUNTIME/renderer/d3dmetal/external"
      ;;
    dxmt) export WINEDLLPATH_PREPEND="$RUNTIME/renderer/dxmt/wine" ;;
    wined3d) ;;
    *) die "TM_RENDERER must be d3dmetal, dxmt or wined3d" ;;
  esac
}

check_requirements() {
  [[ "$(uname -m)" == arm64 ]] || die "Apple Silicon required."
  arch -x86_64 /usr/bin/true 2>/dev/null || die "Rosetta 2 missing: softwareupdate --install-rosetta --agree-to-license"
  python3 -c '' 2>/dev/null || die "python3 missing: xcode-select --install"
}

# download <url> <sha256> <destination>
download() {
  curl -fL --progress-bar -o "$3" "$1"
  echo "$2  $3" | shasum -a 256 -c --quiet || die "checksum mismatch for $1"
}

install_runtime() {
  [[ -x "$WINE" ]] && return
  say "Downloading the Wine runtime (~260 MB)"
  local tmp; tmp="$(mktemp -d)"
  download "$TEMPLATE_URL" "$TEMPLATE_SHA256" "$tmp/template.tar.xz"
  download "$ENGINE_URL" "$ENGINE_SHA256" "$tmp/engine.tar.xz"
  tar -xf "$tmp/template.tar.xz" -C "$tmp"
  tar -xf "$tmp/engine.tar.xz" -C "$tmp"
  mv "$tmp"/Template-*.app/Contents/Frameworks "$RUNTIME"
  mv "$tmp/wswine.bundle" "$WINE_DIR"
  rm -rf "$tmp"
}

create_prefix() {
  [[ -d "$WINEPREFIX/drive_c" ]] && return
  say "Creating the Wine prefix"
  {
    "$WINE" wineboot --init
    # wined3d on Vulkan crashes Ubisoft Connect during GPU detection; OpenGL does not.
    "$WINE" reg add 'HKCU\Software\Wine\Direct3D' /v renderer /d gl /f
    # Log crashes instead of opening a debugger window for each one.
    "$WINE" reg add 'HKCU\Software\Wine\WineDbg' /v ShowCrashDialog /t REG_DWORD /d 0 /f
  } >>"$LOG" 2>&1
  "$WINE_DIR/bin/wineserver" -w
}

install_ubisoft_connect() {
  [[ -f "$UBI_EXE" ]] && return
  say "Installing Ubisoft Connect"
  local installer="$DATA/UbisoftConnectInstaller.exe"
  curl -fL --progress-bar -o "$installer" "$UBISOFT_URL"
  "$WINE" "$installer" /S >>"$LOG" 2>&1
  "$WINE_DIR/bin/wineserver" -w
  rm -f "$installer"
  [[ -f "$UBI_EXE" ]] || die "Ubisoft Connect install failed, see: ./tm logs"
}

# Ubisoft Connect updates itself and restores libcef.dll, so this runs before every launch.
patch_libcef() {
  local status
  status="$(python3 "$ROOT/patch_libcef.py" "$UBI_DIR/libcef.dll")" || true
  case "$status" in
    patched) say "Patched libcef.dll" ;;
    already) ;;
    *) warn "Unknown libcef.dll build (Ubisoft Connect update?). Its window may stay blank, see README." ;;
  esac
}

cmd_install() {
  check_requirements
  mkdir -p "$DATA"
  install_runtime
  create_prefix
  install_ubisoft_connect
  patch_libcef
  say "Ready. Run: ./tm run"
}

cmd_run() {
  [[ -f "$UBI_EXE" ]] || die "Not installed. Run: ./tm install"
  select_renderer
  patch_libcef
  if [[ -f "$GAME_EXE" ]]; then
    say "Launching Trackmania (${TM_RENDERER:-d3dmetal})"
    cd "$(dirname "$GAME_EXE")" && exec "$WINE" Trackmania.exe >>"$LOG" 2>&1
  fi
  say "Opening Ubisoft Connect: sign in and install Trackmania, then run ./tm run again"
  cd "$UBI_DIR" && exec "$WINE" UbisoftConnect.exe >>"$LOG" 2>&1
}

# cmd_openplanet <installer.exe>
# The NSIS installer rejects the game path under Wine ("not where Trackmania is installed"),
# so extract its payload into the game directory instead. That is all the installer copies.
cmd_openplanet() {
  local installer="${1:-}"
  [[ -f "$installer" ]] || die "usage: ./tm openplanet <OpenplanetNext_x.y.z.exe> (download it from openplanet.dev)"
  [[ -f "$GAME_EXE" ]] || die "Install Trackmania first: ./tm run"
  command -v 7zz >/dev/null || die "7-Zip missing: brew install sevenzip"
  say "Installing Openplanet"
  # shellcheck disable=SC2016  # $PLUGINSDIR is a literal NSIS folder name
  7zz x -y -o"$(dirname "$GAME_EXE")" "$installer" -x'!$PLUGINSDIR' -x'!OpenplanetUninstall.exe' >>"$LOG" 2>&1 \
    || die "could not extract $installer"
  [[ -f "$(dirname "$GAME_EXE")/Openplanet.dll" ]] || die "Openplanet.dll not found in $installer"
  # The installer brings a fresh loader: put the shim back in front of it.
  if [[ -f "$GAME_DIR/$OP_LOADER" ]]; then
    mv -f "$GAME_DIR/dinput8.dll" "$GAME_DIR/$OP_LOADER"
    cp "$DATA/fastread/dinput8.dll" "$GAME_DIR/dinput8.dll"
  fi
  say "Openplanet installed. Restart the game: ./tm kill && ./tm run, then press F3 in game"
}

# cmd_fastread on|off
# Openplanet reads game memory with ReadProcessMemory, which Wine routes through wineserver.
# fastread/fastread.c is a dinput8.dll shim that serves those reads in-process; Openplanet's own
# loader moves to dinput8_openplanet.dll. "off" puts the loader back.
cmd_fastread() {
  [[ -f "$GAME_DIR/Openplanet.dll" ]] || die "Install Openplanet first: ./tm openplanet <installer.exe>"
  case "${1:-}" in
    on)
      command -v x86_64-w64-mingw32-gcc >/dev/null || die "mingw-w64 missing: brew install mingw-w64"
      mkdir -p "$DATA/fastread"
      x86_64-w64-mingw32-gcc -O2 -shared -s -static-libgcc -o "$DATA/fastread/dinput8.dll" \
        "$ROOT/fastread/fastread.c" "$ROOT/fastread/dinput8.def" || die "could not build the shim"
      [[ -f "$GAME_DIR/$OP_LOADER" ]] || mv "$GAME_DIR/dinput8.dll" "$GAME_DIR/$OP_LOADER"
      cp "$DATA/fastread/dinput8.dll" "$GAME_DIR/dinput8.dll"
      say "fastread on. Restart the game: ./tm kill && ./tm run"
      ;;
    off)
      [[ -f "$GAME_DIR/$OP_LOADER" ]] && mv -f "$GAME_DIR/$OP_LOADER" "$GAME_DIR/dinput8.dll"
      say "fastread off. Restart the game: ./tm kill && ./tm run"
      ;;
    *) die "usage: ./tm fastread on|off" ;;
  esac
}

cmd_reset() {
  read -rp "Delete the Wine prefix, including Ubisoft Connect and the game? [y/N] " answer
  [[ "$answer" == y ]] || exit 0
  "$WINE_DIR/bin/wineserver" -k 2>/dev/null || true
  rm -rf "$WINEPREFIX"
  say "Prefix deleted. Run: ./tm install"
}

usage() {
  cat <<EOF
usage: ./tm <command>

  install      download the Wine runtime, create the prefix, install Ubisoft Connect
  run          launch Trackmania, or Ubisoft Connect if the game is not installed yet
  kill         stop every Wine process
  logs         follow the Wine log (run with WINEDEBUG=err+all for more detail)
  wine <args>  run a command in the prefix, e.g. ./tm wine winecfg
  openplanet <installer.exe>
               install Openplanet (download the installer from openplanet.dev)
  fastread on|off
               speed up Openplanet plugins (experimental, needs mingw-w64)
  reset        delete the prefix (keeps the runtime)

environment:
  TM_RENDERER  d3dmetal (default), dxmt or wined3d. Run ./tm kill before switching.
  TM_DATA      data directory (default: ./data)
EOF
}

case "${1:-}" in
  install) cmd_install ;;
  run) cmd_run ;;
  kill) exec "$WINE_DIR/bin/wineserver" -k ;;
  logs) touch "$LOG" && tail -f "$LOG" ;;
  wine) shift; exec "$WINE" "$@" ;;
  openplanet) cmd_openplanet "${2:-}" ;;
  fastread) cmd_fastread "${2:-}" ;;
  reset) cmd_reset ;;
  -h|--help|help) usage ;;
  *) usage >&2; exit 1 ;;
esac
