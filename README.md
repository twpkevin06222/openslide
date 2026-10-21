# OpenSlide

OpenSlide is a C library for reading whole slide image files (also known as
virtual slides).  It provides a consistent and simple API for reading files
from multiple vendors.


## Features

OpenSlide can read brightfield whole slide images in [several formats][]:

* [Aperio][] (`.svs`, `.tif`)
* [DICOM][] (`.dcm`)
* [Hamamatsu][] (`.ndpi`, `.vms`, `.vmu`)
* [Leica][] (`.scn`)
* [MIRAX][] (`.mrxs`)
* [Philips][] (`.tiff`)
* [Sakura][] (`.svslide`)
* [Trestle][] (`.tif`)
* [Ventana][] (`.bif`, `.tif`)
* [Zeiss][] (`.czi`)
* [Generic tiled TIFF][] (`.tif`)

OpenSlide can also provide access to ICC profiles, textual metadata, and
associated images such as a slide label and thumbnail.

[several formats]: https://openslide.org/formats/
[Aperio]: https://openslide.org/formats/aperio/
[DICOM]: https://openslide.org/formats/dicom/
[Hamamatsu]: https://openslide.org/formats/hamamatsu/
[Leica]: https://openslide.org/formats/leica/
[MIRAX]: https://openslide.org/formats/mirax/
[Philips]: https://openslide.org/formats/philips/
[Sakura]: https://openslide.org/formats/sakura/
[Trestle]: https://openslide.org/formats/trestle/
[Ventana]: https://openslide.org/formats/ventana/
[Zeiss]: https://openslide.org/formats/zeiss/
[Generic tiled TIFF]: https://openslide.org/formats/generic-tiff/


## Documentation

The [API reference][API] is available on the web, and is also included as
`doc/html/openslide_8h.html` in the source tarball.  [Additional
documentation][docs] is available on the [OpenSlide website][website].

[API]: https://openslide.org/api/openslide_8h.html
[docs]: https://openslide.org/#documentation
[website]: https://openslide.org/


## License

OpenSlide is released under the terms of the [GNU Lesser General Public
License, version 2.1](https://openslide.org/license/).

OpenSlide is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for
more details.


## Compiling

To build OpenSlide, you will need:

- Meson
- cairo ≥ 1.2
- GDK-PixBuf
- glib ≥ 2.56
- libdicom ≥ 1.2.0 (automatically built if missing)
- libjpeg
- libpng
- libtiff ≥ 4.0
- libxml2
- libzip ≥ 1.1
- OpenJPEG ≥ 2.1
- SQLite ≥ 3.14
- zlib
- Zstandard

Then:

```
meson setup builddir
meson compile -C builddir
meson install -C builddir
```


## Acknowledgements

OpenSlide has been developed by Carnegie Mellon University and other
contributors.

OpenSlide has been supported by the National Institutes of Health and
the Clinical and Translational Science Institute at the University of
Pittsburgh.

Development of DICOM and ICC functionality was supported by NCI Imaging
Data Commons and has been funded in whole or in part with Federal funds
from the National Cancer Institute, National Institutes of Health, under
Task Order No. HHSN26110071 under Contract No. HHSN261201500003l.


## Installing as a Python package (`openslide_bin`)

This branch adds the PreciPoint VMIC driver and fixes for Ventana BIF
(DP 200 slides, tile placement) and DICOM concatenated instances.  To use
those changes from Python without compiling OpenSlide on every machine,
`packaging/` builds a self-contained wheel that
[openslide-python](https://github.com/openslide/openslide-python) picks up
automatically.

Upstream ships this package as [openslide-bin][], built from a separate
repository that vendors OpenSlide and all its dependencies.  The wheel here
is the same idea, but built from *this* source tree, so it carries the
patches above.  It takes the `openslide-bin` distribution name so that
`openslide-python` finds it, with a `+vmic` local version marking it as a
local build and keeping it un-uploadable to PyPI.

Note that `4.0.0+vmic.1` sorts *below* upstream's `4.0.1` under PEP 440, so
`pip install -U openslide-bin` would silently replace this build with the
unpatched upstream wheel.  Pin it in your requirements:

```
openslide-bin == 4.0.0+vmic.1
```

Installing `openslide-python` on its own will not pull in upstream
`openslide-bin` — its only hard dependency is Pillow.

[openslide-bin]: https://github.com/openslide/openslide-bin

### Building the wheel

Install the build dependencies listed under [Compiling](#compiling) — on
Debian/Ubuntu:

```
sudo apt install -y build-essential pkg-config zlib1g-dev libzstd-dev \
    libjpeg-turbo8-dev libpng-dev libopenjp2-7-dev libtiff-dev \
    libglib2.0-dev libcairo2-dev libgdk-pixbuf-2.0-dev libxml2-dev \
    libsqlite3-dev libzip-dev
```

libdicom does not need to be installed; it is built from
`subprojects/libdicom.wrap` and linked statically into the library.

Then:

```
./packaging/build-wheel.sh
```

The script provisions its own build venv (Meson, Ninja, `build`,
`auditwheel`), compiles OpenSlide, copies `libopenslide.so.1` into the
`openslide_bin` package, and runs `auditwheel repair` to bundle the
transitive shared libraries.  The result lands in
`packaging/wheelhouse/`:

```
openslide_bin-4.0.0+vmic.1-py3-none-manylinux_2_38_x86_64.whl
```

### Installing

```
pip install packaging/wheelhouse/openslide_bin-*.whl openslide-python
```

No `LD_LIBRARY_PATH` or system OpenSlide install is required — the wheel
carries the library and its dependencies, and `openslide-python` finds it
by importing `openslide_bin`.

Verify:

```
$ python -c "import openslide; print(openslide.__library_version__)"
4.0.0
```

### Portability

`auditwheel` tags the wheel against the glibc of the machine that built it.
A build on Ubuntu 24.04 yields `manylinux_2_38`, which installs on
glibc ≥ 2.38 (Ubuntu 23.10+, Debian 13+, RHEL/Rocky 10+) but **not** on
Ubuntu 22.04 or RHEL 8/9.  To support older distributions, run the build
inside a `manylinux_2_28` container, or use the upstream
[openslide-bin][] tooling, which builds against an EL 8 glibc.

glib, libX11 and a handful of other libraries are deliberately left
unbundled; the manylinux policy treats them as universally present.
