# Unit tests for src/ and arch/. Each absorbs the sources it exercises rather
# than linking the library, as the AMALGAMATED programs do: what is under test
# is internal, which a shared build exports none of, and some of it is static
# and reachable no other way. So they must NOT also link the library - the
# duplicates collide on Windows, masked only by -fvisibility=hidden on ELF -
# and they carry the absorbed sources' dependencies themselves.
function(jent_unit_test name)
    add_executable(${name} ${name}.c)

    target_compile_definitions(${name} PRIVATE JENT_STATIC_LIB)
    target_compile_options(${name} PRIVATE ${JITTER_C_FLAGS})
    # .. is tests/, for jitterentropy-memlock.h - shared with the recording
    # tools and included by unit.h.
    target_include_directories(${name} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../..
        ${CMAKE_CURRENT_SOURCE_DIR}/../../src
        ${CMAKE_CURRENT_SOURCE_DIR}/../../arch
        ${CMAKE_CURRENT_SOURCE_DIR}/..)

    # What the library's link interface would have supplied. See
    # JITTER_SYSTEM_LIBRARIES in the top-level CMakeLists.txt.
    target_link_libraries(${name} PRIVATE ${JITTER_SYSTEM_LIBRARIES})
    if(WIN32)
        # The UUID backend calls BCryptGenRandom().
        target_link_libraries(${name} PRIVATE bcrypt)
    endif()
    if(EXTERNAL_CRYPTO)
        target_include_directories(${name} PRIVATE ${LIBCRYPTO_INCLUDE_DIR})
        target_link_libraries(${name} PRIVATE ${LIBCRYPTO_LIBRARY})
    endif()

    add_test(NAME ${name} COMMAND ${name})
    # 77 is a skip, as the automake convention has it: a program that finds
    # the machine unable to run it at all - unit-fault when the startup self
    # test rejects this machine's timer or memlock limit - says so rather than
    # fail. A startup that fails for any other reason still fails (see
    # JENT_UT_MACHINE_VERDICT in unit.h).
    set_tests_properties(${name} PROPERTIES SKIP_RETURN_CODE 77)
endfunction()

jent_unit_test(unit-sha3)
jent_unit_test(unit-gcd)

# One program per arch/ backend rather than one for all of them: each is a
# ctest case of its own, so a failure names the backend in the JUnit report
# instead of arriving as one blob.
jent_unit_test(unit-arch-cache)
jent_unit_test(unit-arch-fips)
jent_unit_test(unit-arch-memory)
jent_unit_test(unit-arch-ncpu)
jent_unit_test(unit-arch-sched)
jent_unit_test(unit-arch-timer)
jent_unit_test(unit-uuid)
jent_unit_test(unit-base-api)
jent_unit_test(unit-base-config)
jent_unit_test(unit-base-gen)
jent_unit_test(unit-error)
jent_unit_test(unit-fault)
jent_unit_test(unit-notime)
jent_unit_test(unit-concurrency)
jent_unit_test(unit-zeroize)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # libjitterentropy-kernel, whatever ENABLE_KERNEL_LIB says.
    jent_unit_test(unit-kernel-lib)
endif()

# MOCK_TIMER is a library build option, off by default; these define it for
# their own absorbed copy rather than for the whole build.
jent_unit_test(unit-mock)
target_compile_definitions(unit-mock PRIVATE JENT_CONF_ENABLE_MOCK_TIMER)

# The RCT-with-memory recovery loop generating real blocks, on a mocked clock.
jent_unit_test(unit-health)
target_compile_definitions(unit-health PRIVATE JENT_CONF_ENABLE_MOCK_TIMER)

# Mocked timestamps carry a magic, so a find on the stack is attributable.
jent_unit_test(unit-stack-residue)
target_compile_definitions(unit-stack-residue
                           PRIVATE JENT_CONF_ENABLE_MOCK_TIMER)
