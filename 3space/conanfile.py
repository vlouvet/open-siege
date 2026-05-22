from conans import ConanFile, CMake, tools
import glob
import os.path


class LocalConanFile(ConanFile):
    name = "3space"
    version = "0.6.3"
    url = "https://github.com/open-siege/open-siege"
    license = "MIT"
    author = "Matthew Rindel (matthew@thesiegehub.com)"
    build_requires = "cmake/3.22.0"
    settings = "os", "compiler", "build_type", "arch"
    requires = "nlohmann_json/3.10.5", "boost/1.78.0", "glm/0.9.9.8", "span-lite/0.10.3", "taocpp-pegtl/3.2.1", "libzip/1.8.0", "catch2/2.13.8"
    generators = "cmake_find_package"

    def requirements(self):
        # Pin openssl/1.1.1o on non-Windows to resolve the version conflict
        # between cmake/3.22.0 (requires 1.1.1o) and libzip (pulls 3.x).
        # On Windows we use libzip crypto=win32 so openssl is not needed.
        if self.settings.os != "Windows":
            self.requires("openssl/1.1.1o", override=True)
    exports_sources = "CMakeLists.txt", "include/*", "src/*"

    def configure(self):
        self.options["boost"].shared = False
        self.options["boost"].header_only = True
        self.options["boost"].bzip2 = False
        self.options["boost"].zlib = False
        self.options["boost"].numa = False
        # On Windows: use win32 CNG for libzip crypto (no external openssl needed).
        # openssl/1.1.1o fails to build against GCC 15 MinGW-w64 headers.
        # macOS/Linux keep their defaults (openssl via Conan or system).
        if self.settings.os == "Windows":
            self.options["libzip"].crypto = "win32"

    def build(self):
        cmake = CMake(self)
        cmake.configure(source_folder=os.path.abspath("."), build_folder=os.path.abspath("build"))
        cmake.build()
        cmake.test()

    def package(self):
        cmake = CMake(self)
        cmake.configure(source_folder=os.path.abspath("."), build_folder=os.path.abspath("build"))
        cmake.install()

    def package_info(self):
        self.cpp_info.libs.append("3space")

    def imports(self):
        tools.rmdir("cmake")
        tools.mkdir("cmake")
        [tools.rename(file, f"cmake/{file}") for file in glob.glob("*.cmake")]


