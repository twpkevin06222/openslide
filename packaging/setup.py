# The wheel contains no Python extension module, only a plain shared
# library loaded via ctypes.  So it must be tagged:
#   - platform-specific (not py3-none-any), so auditwheel accepts it and
#     pip won't offer it on the wrong architecture; and
#   - ABI-agnostic (py3-none-<platform>, not cp312-cp312-<platform>), so
#     it installs on any Python 3, matching upstream openslide-bin.
# setuptools gives us the first via has_ext_modules and the second by
# overriding the wheel's tag.
from setuptools import setup
from setuptools.command.bdist_wheel import bdist_wheel
from setuptools.dist import Distribution


class BinaryDistribution(Distribution):
    def has_ext_modules(self):
        return True


class WheelABIAgnostic(bdist_wheel):
    def finalize_options(self):
        super().finalize_options()
        self.root_is_pure = False

    def get_tag(self):
        _python, _abi, plat = super().get_tag()
        return 'py3', 'none', plat


setup(distclass=BinaryDistribution, cmdclass={'bdist_wheel': WheelABIAgnostic})
