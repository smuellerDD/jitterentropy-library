# Two fuzzing targets, and they are built in opposite ways on purpose.
#
# fuzz-api drives the public API and therefore links the library rather than
# absorbing its sources, unlike the programs under tests/unit: what is fuzzed
# is the surface jitterentropy.h declares, and a harness that included the
# sources could reach past it and report defects no application could provoke.
#
# fuzz-health drives the SP800-90B health tests over time stamps the fuzzer
# chooses. That entry point is internal, so this one does absorb the source, as
# tests/health and the unit tests do. It exists because fuzz-api cannot search:
# every operation there measures the machine's real timer and the run manages
# single-digit executions per second, while nothing in fuzz-health measures
# anything and it runs four orders of magnitude faster.
#
# Two programs are built from each harness:
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

# Absorbs src/jitterentropy-health.c to reach jent_health_insert_timestamp()
# and jent_stuck(), and so must NOT also link the library - the duplicates
# collide on Windows and are masked only by -fvisibility=hidden on ELF. Same
# arrangement, and same reason, as tests/health.
add_executable(fuzz-health-standalone fuzz-health.c)
target_compile_definitions(fuzz-health-standalone PRIVATE
                           JENT_FUZZ_STANDALONE JENT_STATIC_LIB)
target_compile_options(fuzz-health-standalone PRIVATE ${JITTER_C_FLAGS})
target_include_directories(fuzz-health-standalone PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../..
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src
    ${CMAKE_CURRENT_SOURCE_DIR}/../../arch)
add_test(NAME fuzz-health-standalone COMMAND fuzz-health-standalone)

if(ENABLE_FUZZING)
    add_executable(fuzz-api fuzz-api.c)
    target_compile_options(fuzz-api PRIVATE ${JITTER_C_FLAGS}
                           -fsanitize=fuzzer)
    target_link_options(fuzz-api PRIVATE -fsanitize=fuzzer)
    target_link_libraries(fuzz-api PRIVATE ${CMAKE_PROJECT_NAME})

    add_executable(fuzz-health fuzz-health.c)
    target_compile_definitions(fuzz-health PRIVATE JENT_STATIC_LIB)
    target_compile_options(fuzz-health PRIVATE ${JITTER_C_FLAGS}
                           -fsanitize=fuzzer)
    target_link_options(fuzz-health PRIVATE -fsanitize=fuzzer)
    target_include_directories(fuzz-health PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../..
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src
        ${CMAKE_CURRENT_SOURCE_DIR}/../../arch)

    # Not registered with add_test(): a fuzzing run has no end of its own, and
    # a suite is not where one belongs. They are driven by hand:
    #
    #   ./tests/fuzz/fuzz-health -max_total_time=300 corpus-health/
    #   ./tests/fuzz/fuzz-api    -max_total_time=300 corpus-api/
    #
    # A crash writes its input to a file, which the matching *-standalone
    # program replays.
    #
    # Pairing ENABLE_FUZZING with ENABLE_SANITIZERS costs fuzz-api about a
    # factor of ten in executions per second, which is most of what it has.
    # The cheaper order is to build the corpus without the sanitizers and
    # replay it under them afterwards - the standalone programs take the
    # corpus files as arguments, and the harnesses check their contracts
    # either way.
endif()

# jitterentropy-rng has the same problem for the same reason: on a Windows
# shared build the DLL is left in the top-level output directory while the test
# program is built into its own, and nothing puts the two together at run time.
# See the rng-* tests in the root CMakeLists.txt.
#
# Only fuzz-api-standalone needs it: fuzz-health-standalone absorbs the source
# it exercises and links no library at all.
if(WIN32 AND BUILD_SHARED_LIBS)
    set_tests_properties(fuzz-api-standalone PROPERTIES ENVIRONMENT_MODIFICATION
        "PATH=path_list_prepend:$<TARGET_FILE_DIR:${CMAKE_PROJECT_NAME}>")
endif()
