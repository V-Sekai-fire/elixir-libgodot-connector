// Load libiceoryx2_ffi_c at start, or fail with a message a person can act on.
//
// Nothing links iceoryx2. The generated table in ../gen dlopens it, so a missing library
// is a start-up failure and not a link failure. That is the point: weft's build graph
// holds no Rust artifact, and a plane that cannot find the bus says so in one line.
//
// SPDX-License-Identifier: Apache-2.0
#ifndef WEFT_BUS_HPP
#define WEFT_BUS_HPP

#include "native/harness/iceoryx2_stubs.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace weft {

// The names to try, in order. A soname first, because that is what an installed package
// gives. The bare name second, so the loader search path and a local build prefix work.
//
// The names are the platform's, which they were not until macOS was run. Only the two ELF
// names were listed, so on a Mac the probe could not succeed however the library was
// installed: `dlopen("libiceoryx2_ffi_c.so")` has nothing to find when the file cargo
// builds is `libiceoryx2_ffi_c.dylib`. The env var still worked, which is why the first
// macOS run passed and the defect stayed invisible -- WEFT_ICEORYX2_PATH is prepended
// above and is tried first, so setting it skips the list entirely. Windows was run the
// same way and never exercised the list either.
//
// That is the shape this repository's README already calls out about scans: the check
// was right about everything it looked at, and silent about the case it could not see.
inline bool load_bus() {
    std::vector<std::string> paths;

    if (const char* from_env = std::getenv("WEFT_ICEORYX2_PATH")) {
        paths.emplace_back(from_env);
    }
#if defined(__APPLE__)
    paths.emplace_back("libiceoryx2_ffi_c.dylib");
    constexpr const char* kSearchPathVar = "DYLD_LIBRARY_PATH";
#elif defined(_WIN32)
    paths.emplace_back("iceoryx2_ffi_c.dll");
    constexpr const char* kSearchPathVar = "PATH";
#else
    paths.emplace_back("libiceoryx2_ffi_c.so");
    paths.emplace_back("libiceoryx2_ffi_c.so.0");
    constexpr const char* kSearchPathVar = "LD_LIBRARY_PATH";
#endif

    native_harness::StubPathMap map;
    map[native_harness::kModuleIceoryx2] = paths;

    if (!native_harness::InitializeStubs(map)) {
        // The variable named is the one this platform's loader actually reads. It used to
        // say LD_LIBRARY_PATH everywhere, which is advice that does nothing on a Mac.
        std::fprintf(stderr,
                     "weft: could not load libiceoryx2_ffi_c.\n"
                     "  Set WEFT_ICEORYX2_PATH to the file, or put its directory on\n"
                     "  %s. See native/harness/README.md.\n",
                     kSearchPathVar);
        return false;
    }
    return true;
}

} // namespace weft

#endif
