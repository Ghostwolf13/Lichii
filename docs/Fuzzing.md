# Fuzzing the game-file parsers

`-DLICHII_FUZZERS=ON` builds nine libFuzzer harnesses in `src/fuzz/` and compiles the whole build with AddressSanitizer and UndefinedBehaviorSanitizer. It needs clang with its compiler-rt runtimes (`libclang-rt-<version>-dev` on Ubuntu). Use a separate build directory, since every object is instrumented.

| Harness | Input |
|---|---|
| `fuzz_partition_romfs` | Raw PFS0/HFS0 partition, then the same bytes as a raw RomFS image |
| `fuzz_nca` | One NCA, on its own, as an update NCA with a missing base, and inside an NSP; `FUZZ_NCA_SEED=<dir>` writes made-up seeds |
| `fuzz_package` | A whole NSP, then the same bytes as an XCI; `FUZZ_PACKAGE_SEED=<dir>` writes made-up seeds |
| `fuzz_cheats` | Cheat text, as in a mod's `cheats/<build id>.txt` |
| `fuzz_ips` | IPS/IPS32 patch and IPSwitch (`.pchtxt`) text; the first byte sets the patched file's size in 16-byte units |
| `fuzz_meta` | One file through the NACP, NPDM, CNMT, KIP1 and INI1 readers |
| `fuzz_nup` | A `.nup` container; `FUZZ_NUP_SEED=<file>` writes a seed packed from a made-up dump |
| `fuzz_ncz` | An NCZ: a 0x4000-byte NCA header, then `NCZSECTN` and, for block streams, `NCZBLOCK`; `FUZZ_NCZ_SEED=<dir>` writes made-up seeds |
| `fuzz_keys` | A keys file, written as both `prod.keys` and `title.keys` in a scratch keys directory |

NCA headers are normally encrypted. The NCA reader also accepts a plaintext header (magic `NCA3` at 0x200) and sections with encryption type None, so fuzz NCA inputs in that form. `fuzz_nca` and `fuzz_package` point the key manager at a scratch keys directory with made-up key area keys, so plaintext NCAs pass the key check. They never read or write your own keys. `fuzz_keys`, `fuzz_nup` and `fuzz_ncz` also point `HOME`, the XDG directories and `TMPDIR` at a scratch directory under the system temp directory, so nothing they write lands in your user data.

```sh
cmake -S . -B build-fuzz -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
    -DLICHII_FUZZERS=ON -DENABLE_WERROR=OFF -DENABLE_QT=OFF -DLICHII_CMD=OFF -DLICHII_ROOM=OFF
cmake --build build-fuzz --target fuzz_partition_romfs fuzz_nca fuzz_package fuzz_cheats fuzz_ips \
    fuzz_meta fuzz_nup fuzz_ncz fuzz_keys
FUZZ_NCA_SEED=corpus/nca build-fuzz/bin/fuzz_nca
ASAN_OPTIONS=detect_leaks=0 \
    build-fuzz/bin/fuzz_nca -rss_limit_mb=3072 -malloc_limit_mb=3072 -timeout=25 corpus/nca
```

Start `fuzz_nca`, `fuzz_package` and `fuzz_ncz` from their `FUZZ_*_SEED` seeds (`src/fuzz/fuzz_seeds.h`): plaintext NCAs, NSPs, an XCI and NCZs built from scratch. From an empty corpus, `fuzz_nca` and `fuzz_ncz` never get past the magic at 0x200 or 0x4000. Add other seeds only if you made them yourself, never game dumps. A crash writes `crash-<sha1>` to the working directory. To replay it, pass the file to the harness: `build-fuzz/bin/fuzz_nca crash-<sha1>`. Turn each fixed crash into a case in `src/tests/core/file_sys_parsers.cpp` or `src/tests/core/parser_regressions.cpp` that builds the input from scratch.

Each harness file starts with the flags it was run with. `fuzz_meta` and `fuzz_ncz` reach header fields that size allocations, so give them `-rss_limit_mb` and `-malloc_limit_mb`, and use `-fork=1 -ignore_ooms=1` to keep going past out-of-memory reports. `fuzz_keys` needs `ASAN_OPTIONS=detect_leaks=0`: LeakSanitizer reports about 65 bytes allocated at start-up, once per process, not per input.

With `-DLICHII_TESTS=ON`, the same build directory also builds `tests` under the sanitizers.
