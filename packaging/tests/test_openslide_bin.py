"""End-to-end check that the openslide_bin wheel works with openslide-python.

Downloads one slide per scanner format from the OpenSlide test-data site,
verifies it against the published SHA-256, and exercises the
openslide-python API on it.  Downloads are cached across runs in
$OPENSLIDE_TESTDATA_DIR (default ~/.cache/openslide-testdata); the full set
is about 2.3 GB.

    pip install packaging/wheelhouse/openslide_bin-*.whl openslide-python pytest
    pytest packaging/tests -v
    pytest packaging/tests -v -k "aperio or mirax"   # just the small ones
"""

import hashlib
import json
import os
import shutil
import urllib.request
import zipfile
from pathlib import Path

import pytest

import openslide
import openslide.lowlevel
from openslide import OpenSlide
from openslide.deepzoom import DeepZoomGenerator

BASE_URL = "https://openslide.cs.cmu.edu/download/openslide-testdata/"
CACHE_DIR = Path(
    os.environ.get(
        "OPENSLIDE_TESTDATA_DIR", Path.home() / ".cache" / "openslide-testdata"
    )
)


class Case:
    def __init__(self, base, vendor, slide=None, quickhash=None, skip=None):
        self.base = base  # path relative to BASE_URL, as in index.json
        self.vendor = vendor  # expected openslide.vendor
        # File to open; for zips, the name of a file inside the archive.
        self.slide = slide or Path(base).name
        # From test/cases/*/config.yaml where upstream records it.
        self.quickhash = quickhash
        self.skip = skip

    @property
    def id(self):
        return self.base.split("/")[0]


# The smallest slide per format, except where a slightly larger one has an
# upstream quickhash to compare against.
CASES = [
    Case(
        "Aperio/CMU-1-Small-Region.svs",
        "aperio",
        quickhash="6335ea0e6cc54c2cba64bb265d3c713a50cd84484924e3a9c109558c13521d5c",
    ),
    Case(
        "DICOM/JP2K-33003-1.zip",
        "dicom",
        slide="DCM_4.dcm",
        quickhash="2431797d260ab5fab926a01f3cee1d35dbb7281f82aed46d97c5aafeff16079c",
    ),
    Case(
        "Generic-TIFF/CMU-1.tiff",
        "generic-tiff",
        quickhash="428aa6abf42c774234a463cb90e2cbf88423afc0217e46ec2e308f31e29f1a9f",
    ),
    Case(
        "Hamamatsu/CMU-1.ndpi",
        "hamamatsu",
        quickhash="f14cd7dcf9dff2b7b3551672a1f3248bd35cc4a2e57e3ca47a2b83eac9ad3987",
    ),
    Case(
        "Hamamatsu-vms/CMU-1.zip",
        "hamamatsu",
        slide="CMU-1-40x - 2010-01-12 13.24.05.vms",
        quickhash="6f461a1e85ca672477b791e1ce5155bfbeec83c28c91263cf596db82f80ec7a5",
    ),
    Case(
        "Leica/Leica-1.scn",
        "leica",
        quickhash="f600ae1d83abaa21b6fda9284ff35a51d7031f60854b5674fac88c8ac6d66298",
    ),
    Case(
        "Mirax/CMU-1-Saved-1_16.zip",
        "mirax",
        slide="CMU-1-Saved-1_16.mrxs",
        quickhash="7fa0eea07df29f449e5aa0ad5df56c5f411f6029d5293f9d68335d6add3fb00e",
    ),
    Case(
        "Philips-TIFF/Philips-4.tiff",
        "philips",
        quickhash="72fcf318a15f85a9b975c31153758d6c6f47e90a11676e7e62a1ab283da4f4c2",
    ),
    Case(
        "Trestle/CMU-1.zip",
        "trestle",
        slide="CMU-1.tif",
        quickhash="de8cb6d7ee9741ed76b08b11b68c20583d34e6e135d3586cf3000e2c7572b641",
    ),
    Case("Ventana/Ventana-1.bif", "ventana"),
    Case("Zeiss/Zeiss-5-SlidePreview-Zstd0.czi", "zeiss"),
    # Listed on the site but without a driver in OpenSlide 4.0.
    Case("Argos/Argos-1.avs", None, skip="no Argos driver in OpenSlide 4.0"),
    Case("Huron/Huron-1.tif", None, skip="no Huron driver in OpenSlide 4.0"),
    Case("Olympus/OS-2.zip", None, skip="no Olympus VSI driver in OpenSlide 4.0"),
]


def _params():
    for case in CASES:
        marks = [pytest.mark.skip(reason=case.skip)] if case.skip else []
        yield pytest.param(case, id=case.id, marks=marks)


@pytest.fixture(scope="session")
def index():
    with urllib.request.urlopen(BASE_URL + "index.json", timeout=60) as resp:
        return json.load(resp)


def _download(url, dest, sha256):
    if dest.exists() and _sha256(dest) == sha256:
        return
    dest.parent.mkdir(parents=True, exist_ok=True)
    part = dest.with_name(dest.name + ".part")
    with urllib.request.urlopen(url, timeout=60) as resp, open(part, "wb") as f:
        shutil.copyfileobj(resp, f, 1 << 20)
    got = _sha256(part)
    if got != sha256:
        part.unlink()
        raise AssertionError(f"{url}: SHA-256 {got}, expected {sha256}")
    part.replace(dest)


def _sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


@pytest.fixture(params=_params())
def case_slide(request, index):
    """(case, path to the file to open), downloading as needed."""
    case = request.param
    meta = index[case.base]
    archive = CACHE_DIR / case.base
    _download(BASE_URL + case.base, archive, meta["sha256"])
    if archive.suffix != ".zip":
        return case, archive

    extracted = archive.with_suffix("")
    if not extracted.is_dir():
        tmp = extracted.with_name(extracted.name + ".part")
        shutil.rmtree(tmp, ignore_errors=True)
        with zipfile.ZipFile(archive) as z:
            z.extractall(tmp)
        tmp.replace(extracted)
    matches = list(extracted.rglob(case.slide))
    assert len(matches) == 1, f"{case.slide} in {archive}: {matches}"
    return case, matches[0]


def test_library_comes_from_openslide_bin():
    import openslide_bin

    lib = Path(openslide.lowlevel._lib._name).resolve()
    assert lib.parent == Path(openslide_bin.__file__).parent.resolve()
    assert openslide.__library_version__.startswith("4.")


def test_slide(case_slide):
    case, path = case_slide

    assert OpenSlide.detect_format(path) == case.vendor

    with OpenSlide(path) as slide:
        props = slide.properties
        assert props[openslide.PROPERTY_NAME_VENDOR] == case.vendor
        if case.quickhash:
            assert props[openslide.PROPERTY_NAME_QUICKHASH1] == case.quickhash

        # Pyramid geometry.
        assert slide.level_count >= 1
        assert len(slide.level_dimensions) == slide.level_count
        assert len(slide.level_downsamples) == slide.level_count
        assert slide.dimensions == slide.level_dimensions[0]
        assert slide.level_downsamples[0] == pytest.approx(1.0)
        assert all(w > 0 and h > 0 for w, h in slide.level_dimensions)
        assert list(slide.level_downsamples) == sorted(slide.level_downsamples)

        # Read at the centre of level 0, which forces tile decoding.
        w, h = slide.dimensions
        size = (256, 256)
        region = slide.read_region((w // 2, h // 2), 0, size)
        assert region.mode == "RGBA"
        assert region.size == size

        # Whole lowest-resolution level, capped so huge slides stay cheap.
        top = slide.level_count - 1
        tw, th = slide.level_dimensions[top]
        region = slide.read_region((0, 0), top, (min(tw, 2048), min(th, 2048)))
        assert region.size == (min(tw, 2048), min(th, 2048))

        thumb = slide.get_thumbnail((512, 512))
        assert thumb.mode == "RGB"
        assert max(thumb.size) <= 512 and min(thumb.size) > 0

        for name, image in slide.associated_images.items():
            assert image.mode == "RGBA", name
            assert image.size[0] > 0 and image.size[1] > 0, name

        # color_profile is None or a PIL ImageCmsProfile; just make sure the
        # accessor doesn't raise.
        slide.color_profile

        dz = DeepZoomGenerator(slide, tile_size=254, overlap=1)
        assert dz.level_count > 0
        tile = dz.get_tile(dz.level_count - 1, (0, 0))
        assert tile.size[0] > 0 and tile.size[1] > 0
