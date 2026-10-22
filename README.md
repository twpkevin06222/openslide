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

---

## Note
!!! The section belows are experimental and never got merge to the main branch. Do use it with caution.

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

### Requirements

The wheel is tagged `py3-none-manylinux_2_38_x86_64`, which means:

| | Requirement |
| --- | --- |
| Python | **3.10 or newer** (3.9 works, but see below) |
| OS | **Ubuntu 24.04 LTS or newer** (glibc ≥ 2.38) |
| Architecture | **x86_64** only |

A single wheel covers every Python 3 minor version — it ships a shared
library loaded through `ctypes`, not a Python extension module, so there is
nothing to rebuild per interpreter.  Verified working on Python 3.9, 3.12
and 3.13 from the same file.

The wheel itself declares `Requires-Python: >=3.9`, but `openslide-python`
1.4.3 and later require 3.10, so on Python 3.9 pip silently falls back to
`openslide-python` 1.4.2.  Use **Python 3.10+** unless you specifically
need 3.9.

On the OS side, `manylinux_2_38` needs glibc ≥ 2.38.  That covers:

- Ubuntu 24.04 LTS and newer (glibc 2.39) — recommended
- Ubuntu 23.10 (glibc 2.38) — end of life
- Debian 13 and newer, RHEL/Rocky/Alma 10 and newer

It will **not** install on Ubuntu 22.04 (glibc 2.35), Ubuntu 20.04
(glibc 2.31), Debian 12 (glibc 2.36), or RHEL 8/9 — pip reports no
compatible wheel found.  See [Portability](#portability) for options there.

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

### Testing

`packaging/tests/test_openslide_bin.py` checks the installed wheel end to
end through `openslide-python`.  It downloads one slide per scanner format
from the [OpenSlide test data][testdata] site, verifies each download
against the SHA-256 in the site's `index.json`, and then:

- confirms `libopenslide.so.1` is loaded from the `openslide_bin` package
  rather than a system copy;
- checks format detection and `openslide.vendor`;
- compares `openslide.quickhash-1` with the value upstream records in
  `test/cases/*/config.yaml`, where there is one;
- reads a level-0 region, the lowest-resolution level, a thumbnail, the
  associated images, the colour profile and a Deep Zoom tile.

| Format | Slide | Quickhash checked |
| --- | --- | --- |
| Aperio | `CMU-1-Small-Region.svs` | yes |
| DICOM | `JP2K-33003-1.zip` | yes |
| Generic TIFF | `CMU-1.tiff` | yes |
| Hamamatsu NDPI | `CMU-1.ndpi` | yes |
| Hamamatsu VMS | `CMU-1.zip` | yes |
| Leica | `Leica-1.scn` | yes |
| MIRAX | `CMU-1-Saved-1_16.zip` | yes |
| Philips TIFF | `Philips-4.tiff` | yes |
| Trestle | `CMU-1.zip` | yes |
| Ventana | `Ventana-1.bif` | no |
| Zeiss CZI | `Zeiss-5-SlidePreview-Zstd0.czi` | no |

Argos, Huron and Olympus are listed on the site but have no driver in
OpenSlide 4.0, so they are reported as skipped and not downloaded.
PreciPoint VMIC has no public test slide and is not covered.

Run it from an environment with the wheel installed:

```
pip install packaging/wheelhouse/openslide_bin-*.whl openslide-python pytest
pytest packaging/tests -v
```

The full set is about 2.3 GB.  Downloads are cached in
`~/.cache/openslide-testdata` (override with `OPENSLIDE_TESTDATA_DIR`), so
only the first run pays for them.  To run a quick subset:

```
pytest packaging/tests -v -k "library or Aperio or Mirax or Zeiss"
```

If `PYTHONPATH` points at another distribution's site-packages (ROS, for
example), pytest may try to load that distribution's plugins and fail
before collecting.  Isolate the run with:

```
env -u PYTHONPATH PYTEST_DISABLE_PLUGIN_AUTOLOAD=1 pytest packaging/tests -v
```

[testdata]: https://openslide.cs.cmu.edu/download/openslide-testdata/

### Portability

The glibc floor above is not fixed by this repository — `auditwheel` tags
the wheel against the glibc of whichever machine built it, and a build on
Ubuntu 24.04 yields `manylinux_2_38`.  Building on an older distribution
lowers the floor accordingly.

To support Ubuntu 22.04 or RHEL 8/9, run the build inside a
`manylinux_2_28` container, or use the upstream [openslide-bin][] tooling,
which builds against an EL 8 glibc.  Likewise, an `aarch64` wheel requires
running the build on an `aarch64` machine.

glib, libX11 and a handful of other libraries are deliberately left
unbundled; the manylinux policy treats them as universally present.
