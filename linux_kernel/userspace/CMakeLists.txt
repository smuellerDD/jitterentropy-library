# libjitterentropy-kernel and the jitter_rng tool, with ENABLE_KERNEL_LIB: user
# space access to the jitter_rng kernel module - its character device, the
# ioctls of ../jitterentropy_uapi.h and /proc/jitterentropy. No Jitter RNG of
# its own and no link to libjitterentropy: the instances live in the kernel.

add_library(${PROJECT_NAME}-kernel
    ${CMAKE_CURRENT_SOURCE_DIR}/jitterentropy-kernel.c)
target_compile_options(${PROJECT_NAME}-kernel PRIVATE ${JITTER_C_FLAGS})
target_include_directories(${PROJECT_NAME}-kernel INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)
# The UAPI header goes with it, for a caller issuing the ioctls itself.
set_target_properties(${PROJECT_NAME}-kernel PROPERTIES
    PUBLIC_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/jitterentropy-kernel.h;${CMAKE_CURRENT_SOURCE_DIR}/../jitterentropy_uapi.h")

# The export set of the shared library, checked against the header here and
# against the built library by the exported-symbols-kernel test of the
# top-level CMakeLists.txt.
set(JITTER_KERNEL_VERSION_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/kernel.lds")
set(JITTER_KERNEL_VERSION_SCRIPT "${JITTER_KERNEL_VERSION_SCRIPT}" PARENT_SCOPE)
jent_check_version_script(
    "${CMAKE_CURRENT_SOURCE_DIR}/jitterentropy-kernel.h"
    "${JITTER_KERNEL_VERSION_SCRIPT}" JENT_KERNEL_API "")

# Versioned and hardened as the library is, with a version script of its own.
if(BUILD_SHARED_LIBS)
    foreach(_jent_prop LINK_OPTIONS VERSION SOVERSION)
        get_target_property(_jent_value ${PROJECT_NAME} ${_jent_prop})
        if(NOT _jent_value STREQUAL "_jent_value-NOTFOUND")
            set_property(TARGET ${PROJECT_NAME}-kernel
                         PROPERTY ${_jent_prop} "${_jent_value}")
        endif()
    endforeach()

    if(JITTER_VERSION_SCRIPT_LINKS)
        get_target_property(_jent_value ${PROJECT_NAME}-kernel LINK_OPTIONS)
        list(REMOVE_ITEM _jent_value
             "-Wl,--version-script=${JITTER_VERSION_SCRIPT}")
        set_target_properties(${PROJECT_NAME}-kernel PROPERTIES
            LINK_OPTIONS "${_jent_value}"
            LINK_DEPENDS "${JITTER_KERNEL_VERSION_SCRIPT}")
        target_link_options(${PROJECT_NAME}-kernel PRIVATE
            "-Wl,--version-script=${JITTER_KERNEL_VERSION_SCRIPT}")
    endif()
endif()

install(TARGETS ${PROJECT_NAME}-kernel
    EXPORT jitterentropyTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

# The module and an instance of it on the command line.
add_executable(jitter_rng jitter_rng.c)
target_compile_options(jitter_rng PRIVATE ${JITTER_C_FLAGS})
target_link_libraries(jitter_rng PRIVATE ${PROJECT_NAME}-kernel)
install(TARGETS jitter_rng)
