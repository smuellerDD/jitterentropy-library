# Fuzzing of the public API. Unlike the programs under tests/unit these link
# the library rather than absorbing its sources: what is fuzzed is the surface
# jitterentropy.h declares, and a harness that included the sources could reach
# past it and report defects no application could provoke.
#
# Two programs are built from the one harness:
#
#   - fuzz-api-standalone, always. It has a main() of its own, needs no
#     libFuzzer and therefore no particular compiler, and runs a fixed sweep of
#     inputs - which is what the suite registers, so that the contract the
#     harness asserts is checked on every platform the project builds on. It
#     also replays input files given on the command line, which is how a crash
#     libFuzzer found is reproduced.
#
#   - fuzz-api, with -DENABLE_FUZZING=ON and Clang. This is the coverage-guided
#     one; the root CMakeLists.txt instruments the whole build for it.
#
# Both must keep their assertions - the harness states the API contract in
# assert() and a build that defines NDEBUG would fuzz nothing but crashes. The
# source undefines it itself, so a Release build needs no flag here.

add_executable(fuzz-api-standalone fuzz-api.c)
target_compile_definitions(fuzz-api-standalone PRIVATE JENT_FUZZ_STANDALONE)
target_compile_options(fuzz-api-standalone PRIVATE ${JITTER_C_FLAGS})
target_link_libraries(fuzz-api-standalone PRIVATE ${CMAKE_PROJECT_NAME})
add_test(NAME fuzz-api-standalone COMMAND fuzz-api-standalone)

if(ENABLE_FUZZING)
    add_executable(fuzz-api fuzz-api.c)
    target_compile_options(fuzz-api PRIVATE ${JITTER_C_FLAGS}
                           -fsanitize=fuzzer)
    target_link_options(fuzz-api PRIVATE -fsanitize=fuzzer)
    target_link_libraries(fuzz-api PRIVATE ${CMAKE_PROJECT_NAME})

    # Not registered with add_test(): a fuzzing run has no end of its own, and
    # a suite is not where one belongs. It is driven by hand:
    #
    #   ./tests/fuzz/fuzz-api -max_total_time=300 corpus/
    #
    # A crash writes its input to a file, which fuzz-api-standalone replays.
endif()

# jitterentropy-rng has the same problem for the same reason: on a Windows
# shared build the DLL is left in the top-level output directory while the test
# program is built into its own, and nothing puts the two together at run time.
# See the rng-* tests in the root CMakeLists.txt.
if(WIN32 AND BUILD_SHARED_LIBS)
    set_tests_properties(fuzz-api-standalone PROPERTIES ENVIRONMENT_MODIFICATION
        "PATH=path_list_prepend:$<TARGET_FILE_DIR:${CMAKE_PROJECT_NAME}>")
endif()
