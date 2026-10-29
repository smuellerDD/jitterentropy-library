# jitterentropy-rngd, with ENABLE_RNGD: the daemon feeding the Jitter RNG into
# the kernel's /dev/random, and the service file of the system it is built
# for. It links the library as any consumer does.
#
# The second source is how that system's kernel takes the data. Each system's
# service file is in the directory of its name.

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(_jent_rngd_src jitterentropy-rngd-linux.c)
    # The unit names the installed daemon. Below the prefix by default, as
    # everything else is; a distribution names the directory systemd reads.
    set(JENT_SYSTEMD_UNITDIR "lib/systemd/system" CACHE STRING
        "Where the systemd unit of jitterentropy-rngd is installed, relative to the prefix unless absolute")
    set(_jent_service_in linux/jitterentropy.service.in)
    set(_jent_service jitterentropy.service)
    set(_jent_service_dir ${JENT_SYSTEMD_UNITDIR})
    set(_jent_service_kind FILES)
else()
    set(_jent_rngd_src jitterentropy-rngd-bsd.c)
    # Each where its system looks. The rc.d scripts are executables.
    if(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
        set(_jent_service_in freebsd/jitterentropy_rngd.in)
        set(_jent_service jitterentropy_rngd)
        set(_jent_service_default "etc/rc.d")
        set(_jent_service_kind PROGRAMS)
    elseif(CMAKE_SYSTEM_NAME STREQUAL "NetBSD")
        set(_jent_service_in netbsd/jitterentropy_rngd.in)
        set(_jent_service jitterentropy_rngd)
        set(_jent_service_default "/etc/rc.d")
        set(_jent_service_kind PROGRAMS)
    elseif(CMAKE_SYSTEM_NAME STREQUAL "OpenBSD")
        set(_jent_service_in openbsd/jitterentropy_rngd.in)
        set(_jent_service jitterentropy_rngd)
        set(_jent_service_default "/etc/rc.d")
        set(_jent_service_kind PROGRAMS)
    else()
        set(_jent_service_in macos/jitterentropy-rngd.plist.in)
        set(_jent_service jitterentropy-rngd.plist)
        set(_jent_service_default "/Library/LaunchDaemons")
        set(_jent_service_kind FILES)
    endif()
    set(JENT_RNGD_SERVICEDIR "${_jent_service_default}" CACHE STRING
        "Where the rc.d script or launchd plist of jitterentropy-rngd is installed, relative to the prefix unless absolute")
    set(_jent_service_dir ${JENT_RNGD_SERVICEDIR})
endif()

add_executable(jitterentropy-rngd jitterentropy-rngd.c ${_jent_rngd_src})
target_compile_options(jitterentropy-rngd PRIVATE ${JITTER_C_FLAGS})
target_link_libraries(jitterentropy-rngd PRIVATE ${PROJECT_NAME})
# The install component rngd is the daemon and its service file alone: linked
# statically, the daemon needs nothing of the library installed.
install(TARGETS jitterentropy-rngd
        RUNTIME DESTINATION ${CMAKE_INSTALL_SBINDIR} COMPONENT rngd)

# The prefix is filled in at install time, as for jitterentropy.pc, so that
# the service file follows "cmake --install --prefix" to where the daemon goes.
if(IS_ABSOLUTE "${CMAKE_INSTALL_SBINDIR}")
    set(PATH "${CMAKE_INSTALL_SBINDIR}")
else()
    set(PATH "@JENT_UNIT_PREFIX@/${CMAKE_INSTALL_SBINDIR}")
endif()
configure_file(${_jent_service_in}
               ${CMAKE_CURRENT_BINARY_DIR}/${_jent_service}.install @ONLY)
set(_jent_unit_installed
    ${CMAKE_CURRENT_BINARY_DIR}/install/${_jent_service})
install(CODE "
  set(_jent_unit_in \"${CMAKE_CURRENT_BINARY_DIR}/${_jent_service}.install\")
  set(_jent_unit_out \"${_jent_unit_installed}\")
" COMPONENT rngd)
install(CODE [[
  file(READ "${_jent_unit_in}" _jent_unit)
  string(REPLACE "@JENT_UNIT_PREFIX@" "${CMAKE_INSTALL_PREFIX}" _jent_unit
         "${_jent_unit}")
  file(WRITE "${_jent_unit_out}" "${_jent_unit}")
]] COMPONENT rngd)
install(${_jent_service_kind} ${_jent_unit_installed}
        DESTINATION ${_jent_service_dir} COMPONENT rngd)

# Needs neither root nor the kernel: the daemon links and reports the
# library's version.
if(BUILD_TESTING)
    add_test(NAME rngd-version COMMAND jitterentropy-rngd --version)
    set_tests_properties(rngd-version PROPERTIES
        PASS_REGULAR_EXPRESSION "jitterentropy-rngd ${PROJECT_VERSION}")
    # Nor does --generate, which writes the bytes out instead of seeding.
    add_test(NAME rngd-generate
             COMMAND ${CMAKE_COMMAND}
                     -DRNGD=$<TARGET_FILE:jitterentropy-rngd>
                     -DDIR=${CMAKE_CURRENT_BINARY_DIR}
                     -P ${PROJECT_SOURCE_DIR}/tests/rngd/generate.cmake)
    # --device stands in for /dev/random. Linux has none: the nix VM tests.
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_test(NAME rngd-signals
                 COMMAND sh ${PROJECT_SOURCE_DIR}/tests/rngd/signals.sh
                         $<TARGET_FILE:jitterentropy-rngd>
                         ${CMAKE_CURRENT_BINARY_DIR}/rngd-signals)
        set_tests_properties(rngd-signals PROPERTIES TIMEOUT 900)
    endif()
endif()
