#!/bin/bash
#
# Build a pip-installable openslide-bin wheel from the patched OpenSlide
# source tree, bundling libopenslide and its shared dependencies.
#
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
# Default to the OpenSlide tree this directory lives in.
SRC="${OPENSLIDE_SRC:-$(cd "$HERE/.." && pwd)}"
BUILD="$HERE/build-openslide"
STAGE="$HERE/stage"
VENV="$HERE/venv"

echo "== source tree: $SRC"

# 1. Python build tooling in a dedicated venv, kept separate from any
#    working environment.  Prefer uv when available: on distros without the
#    python3-venv package, `python3 -m venv` has no ensurepip and fails.
TOOLS="wheel setuptools meson ninja build auditwheel patchelf"
if [ ! -x "$VENV/bin/python" ]; then
    UV="${UV:-$(command -v uv || echo "$HOME/.local/bin/uv")}"
    if [ -x "$UV" ]; then
        "$UV" venv --python 3.12 "$VENV"
        # shellcheck disable=SC2086
        VIRTUAL_ENV="$VENV" "$UV" pip install -q $TOOLS
    else
        python3 -m venv "$VENV"
        # shellcheck disable=SC2086
        "$VENV/bin/pip" install -q --upgrade pip $TOOLS
    fi
fi
export PATH="$VENV/bin:$PATH"

# 2. Build OpenSlide. libdicom comes from subprojects/libdicom.wrap
#    (>=1.2.0, needed by the DICOM concatenation fix); everything else
#    comes from system dev packages.
rm -rf "$BUILD" "$STAGE"
meson setup "$BUILD" "$SRC" \
    --prefix=/usr \
    --libdir=lib \
    --buildtype=release \
    -Ddoc=disabled \
    -Dtest=disabled \
    -Ddefault_library=shared \
    -Dlibdicom:default_library=static \
    --force-fallback-for=libdicom
meson compile -C "$BUILD"
DESTDIR="$STAGE" meson install -C "$BUILD" --tags runtime,devel

# 3. Drop the built library next to openslide_bin/__init__.py
rm -f "$HERE"/openslide_bin/*.so*
cp -av "$STAGE/usr/lib/"libopenslide.so.1* "$HERE/openslide_bin/"
# __init__.py dlopens the bare soname, so make sure that exact name exists
if [ ! -e "$HERE/openslide_bin/libopenslide.so.1" ]; then
    echo "ERROR: libopenslide.so.1 not found in staged install" >&2
    exit 1
fi

# 4. Build the wheel, then bundle transitive deps into it
rm -rf "$HERE/dist" "$HERE/wheelhouse"
python -m build --wheel --no-isolation --outdir "$HERE/dist" "$HERE"
auditwheel repair "$HERE"/dist/*.whl -w "$HERE/wheelhouse"

echo
echo "== done"
ls -lh "$HERE/wheelhouse"
