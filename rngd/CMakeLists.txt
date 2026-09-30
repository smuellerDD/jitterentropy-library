# jitterentropy-rngd, with ENABLE_RNGD: the daemon feeding the Jitter RNG into
# the Linux /dev/random, and its systemd unit. It uses the API of
# jitterentropy.h alone and links the library as any consumer does.

add_executable(jitterentropy-rngd jitterentropy-rngd.c)
target_compile_options(jitterentropy-rngd PRIVATE ${JITTER_C_FLAGS})
target_link_libraries(jitterentropy-rngd PRIVATE ${PROJECT_NAME})
install(TARGETS jitterentropy-rngd
        RUNTIME DESTINATION ${CMAKE_INSTALL_SBINDIR})

# The unit names the installed daemon. Below the prefix by default, as
# everything else is; a distribution names the directory systemd reads.
set(JENT_SYSTEMD_UNITDIR "lib/systemd/system" CACHE STRING
    "Where the systemd unit of jitterentropy-rngd is installed, relative to the prefix unless absolute")
set(PATH "${CMAKE_INSTALL_FULL_SBINDIR}")
configure_file(jitterentropy.service.in
               ${CMAKE_CURRENT_BINARY_DIR}/jitterentropy.service @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/jitterentropy.service
        DESTINATION ${JENT_SYSTEMD_UNITDIR})

# Needs neither root nor the kernel: the daemon links and reports the
# library's version. The service itself is tested in a VM, see nix/rngd.nix.
if(BUILD_TESTING)
    add_test(NAME rngd-version COMMAND jitterentropy-rngd --version)
    set_tests_properties(rngd-version PROPERTIES
        PASS_REGULAR_EXPRESSION "jitterentropy-rngd ${PROJECT_VERSION}")
endif()
