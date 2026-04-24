#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${1:-"${ROOT_DIR}/build"}"
DIST_DIR="${ROOT_DIR}/dist"
APPDIR="${DIST_DIR}/JackLooper.AppDir"

cmake --build "${BUILD_DIR}"

rm -rf "${APPDIR}"
cmake --install "${BUILD_DIR}" --prefix "${APPDIR}/usr"

cat > "${APPDIR}/AppRun" <<'APPRUN'
#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export LD_LIBRARY_PATH="${HERE}/usr/lib:${HERE}/usr/lib64:${LD_LIBRARY_PATH:-}"
exec "${HERE}/usr/bin/JackLooper" "$@"
APPRUN
chmod +x "${APPDIR}/AppRun"

cp "${APPDIR}/usr/share/applications/jacklooper.desktop" "${APPDIR}/JackLooper.desktop"
cp "${APPDIR}/usr/share/icons/hicolor/scalable/apps/jacklooper.svg" "${APPDIR}/jacklooper.svg"

if command -v appimagetool >/dev/null 2>&1; then
    appimagetool "${APPDIR}" "${DIST_DIR}/JackLooper-x86_64.AppImage"
    echo "Created ${DIST_DIR}/JackLooper-x86_64.AppImage"
else
    echo "AppDir created at ${APPDIR}"
    echo "Install appimagetool and rerun this script to create an AppImage."
fi
