"""Python API for pigzpp."""

import sys as _sys

from . import _pigzpp as _native
from ._pigzpp import *

__version__ = _native.__version__
png = _native.png
_sys.modules[f"{__name__}.png"] = png

__all__ = [
    name for name in dir(_native) if not name.startswith("_")
] + ["__version__"]
