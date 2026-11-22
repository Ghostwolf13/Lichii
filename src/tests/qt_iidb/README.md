# Testing the iiDB artwork picker

The tests use synthetic responses and images. They do not contact iiDB or need an API key.
Install Qt 6 with Widgets, Network, Test and the **Qt Image Formats** (`qtimageformats`) module.
The WebP image plugin must be discoverable by Qt.

To build just the picker tests from the repository root:

```sh
cmake -S src/tests/qt_iidb -B build-iidb -G Ninja -DCMAKE_PREFIX_PATH=/path/to/Qt
cmake --build build-iidb
(cd build-iidb && ctest --output-on-failure)
```

Configure the production build with both `BUILD_TESTING=ON` and `LICHII_TESTS=ON` to also
test the exact-ID artwork writer against a temporary profile:

```sh
cmake --build build --target iidb_artwork_tests
ctest --test-dir build -R '^iidb_artwork$' --output-on-failure --no-tests=error
```

For a multi-config generator, add `--config Release` to the build command and `-C Release` to
the CTest command. Use the same configuration in both.
