# Runtime image for the openslide_bin wheel: Ubuntu 24.04, Python 3.12, a
# uv-managed venv with openslide-bin, openslide-python and pytest.
#
# Build the wheel first (./packaging/build-wheel.sh), then from the repo root:
#
#   docker build -t openslide-bin-test packaging
#   docker run --rm \
#       -v ~/.cache/openslide-testdata:/testdata:ro \
#       openslide-bin-test
#
# Without the volume the tests download the slides into the container.

FROM ubuntu:24.04

# python3 is 3.12 on 24.04.  The wheel bundles everything except the
# libraries manylinux assumes are present: glib and the X11 client libraries.
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        ca-certificates \
        python3 \
        libglib2.0-0t64 \
        libx11-6 \
        libxext6 \
        libxrender1 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=ghcr.io/astral-sh/uv:0.7.13 /uv /uvx /usr/local/bin/

ENV UV_PYTHON_DOWNLOADS=never \
    UV_LINK_MODE=copy \
    VIRTUAL_ENV=/opt/venv \
    PATH=/opt/venv/bin:$PATH \
    OPENSLIDE_TESTDATA_DIR=/testdata

WORKDIR /app

COPY wheelhouse/ /tmp/wheelhouse/
RUN uv venv --python 3.12 /opt/venv \
    && uv pip install --no-cache /tmp/wheelhouse/openslide_bin-*.whl \
        openslide-python pytest \
    && rm -rf /tmp/wheelhouse

COPY tests/ tests/

CMD ["pytest", "tests", "-v", "-rs", "-p", "no:cacheprovider"]
