from distutils.core import setup, Extension

setup(
    name = "xstruct",
    author = "Robin Boerdijk",
    author_email = "boerdijk@my-deja.com",
    maintainer = "Kiran Bandla",
    maintainer_email = "kbandla@in2void.com",
    license = "unknown",
    version = "0.2.0",
    description = "an extension of the standard Python 'struct' module",
    url = "http://www.github.com/kbandla/xstruct",
    ext_modules = [Extension(
        "xstruct",
        sources = ["xstructmodule.c"]
        ) ],
)
