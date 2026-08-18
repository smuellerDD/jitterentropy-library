# jent_gcd_* is internal and the shared library does not export it (see
# version.lds), so the sources are compiled in as tests/gcd/Makefile already
# does. The library must not also be linked, or the definitions collide.
add_executable(gcd
    gcd.c
    ../../src/jitterentropy-gcd.c
    ../../arch/jitterentropy-arch-memory.c
    # The establish-once semantics of the common timer GCD go through the
    # atomic accessors, which are a translation unit of their own.
    ../../arch/jitterentropy-arch-atomic.c)
target_compile_definitions(gcd PRIVATE JENT_STATIC_LIB)
target_compile_options(gcd PRIVATE ${JITTER_C_FLAGS})
target_include_directories(gcd PRIVATE ../.. ../../src ../../arch)

# What the absorbed memory backend needs with EXTERNAL_CRYPTO set: it allocates
# out of that library's secure heap. Empty in the default build.
if(EXTERNAL_CRYPTO)
    target_include_directories(gcd PRIVATE ${LIBCRYPTO_INCLUDE_DIR})
    target_link_libraries(gcd PRIVATE ${LIBCRYPTO_LIBRARY})
endif()
# A tool as well as the gcd-self-test program: installed only for the former.
if(ENABLE_TOOLS)
    install(TARGETS gcd)
endif()
