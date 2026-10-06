# libjitterentropy-record, with ENABLE_RECORDING: the raw noise recording of
# jitterentropy-record.c, shared or static as the library is. It reaches
# internals a shared libjitterentropy does not export, so it carries a copy of
# the library of its own - and exports nothing but the functions of
# jitterentropy-record.h, so that it links beside libjitterentropy without
# either seeing the other. Here under tests/, not beside the library, so that
# nothing reaches it by accident.
#
# The copy and the recording are one translation unit,
# jitterentropy-record-lib.c, compiled as a private copy: every function of
# the copy is static there, so the object it yields defines the recording and
# nothing else, and the archive or shared library needs no linker to hide
# anything. It is compiled as the library is: the compile settings are copied
# from the finished library target, so that nothing configured for it in the
# top-level CMakeLists.txt has to be repeated here.

# The translation unit names the library's sources itself, and the library
# takes them from a glob: a source added to src/ or arch/ that it does not
# include would be missing from the copy, and fail no sooner than at the link
# of a program using the recording.
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/jitterentropy-record-lib.c _jent_value)
foreach(_jent_src ${JITTER_SRC})
    get_filename_component(_jent_src ${_jent_src} NAME)
    string(FIND "${_jent_value}" "/${_jent_src}\"" _jent_found)
    if(_jent_found EQUAL -1)
        message(FATAL_ERROR
            "ENABLE_RECORDING: jitterentropy-record-lib.c does not include "
            "${_jent_src}, which the library is built from")
    endif()
endforeach()

add_library(${PROJECT_NAME}-record
    ${CMAKE_CURRENT_SOURCE_DIR}/jitterentropy-record-lib.c)
foreach(_jent_prop COMPILE_OPTIONS COMPILE_DEFINITIONS INCLUDE_DIRECTORIES
                   POSITION_INDEPENDENT_CODE)
    get_target_property(_jent_value ${PROJECT_NAME} ${_jent_prop})
    if(NOT _jent_value STREQUAL "_jent_value-NOTFOUND")
        set_property(TARGET ${PROJECT_NAME}-record
                     PROPERTY ${_jent_prop} "${_jent_value}")
    endif()
endforeach()
target_include_directories(${PROJECT_NAME}-record PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})

# On Windows the recording is exported through JENT_RECORD_API from the DLL
# alone - the copy, being static, exports nothing whatever the library's
# JENT_BUILDING_DLL says. The archive is compiled with JENT_RECORD_STATIC_LIB
# and hands it on to its consumers, as the library does JENT_STATIC_LIB:
# neither dllexport directives in it for a DLL linking it to re-export, nor
# dllimport declarations in a consumer for __imp_ symbols an archive never has.
if(WIN32)
    if(BUILD_SHARED_LIBS)
        target_compile_definitions(${PROJECT_NAME}-record PRIVATE
            JENT_RECORD_BUILDING_DLL)
    else()
        target_compile_definitions(${PROJECT_NAME}-record PRIVATE
            JENT_RECORD_STATIC_LIB)
    endif()
endif()

# The export set of the shared library: the API of jitterentropy-record.h, and
# none of jitterentropy.h. Checked against the header here, as version.lds is
# against jitterentropy.h, and against the built library by the
# exported-symbols-record test of the top-level CMakeLists.txt.
set(JITTER_RECORD_VERSION_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/record.lds")
set(JITTER_RECORD_VERSION_SCRIPT "${JITTER_RECORD_VERSION_SCRIPT}" PARENT_SCOPE)
jent_check_version_script(
    "${CMAKE_CURRENT_SOURCE_DIR}/jitterentropy-record.h"
    "${JITTER_RECORD_VERSION_SCRIPT}" JENT_RECORD_API "")

if(BUILD_SHARED_LIBS)
    foreach(_jent_prop LINK_OPTIONS LINK_LIBRARIES VERSION SOVERSION
                       INSTALL_NAME_DIR)
        get_target_property(_jent_value ${PROJECT_NAME} ${_jent_prop})
        if(NOT _jent_value STREQUAL "_jent_value-NOTFOUND")
            set_property(TARGET ${PROJECT_NAME}-record
                         PROPERTY ${_jent_prop} "${_jent_value}")
        endif()
    endforeach()

    # The library's version script is among the link options copied above,
    # and it lists the API of jitterentropy.h, which is static in the copy:
    # record.lds takes its place.
    if(JITTER_VERSION_SCRIPT_USABLE)
        get_target_property(_jent_value ${PROJECT_NAME}-record LINK_OPTIONS)
        list(REMOVE_ITEM _jent_value
             "-Wl,--version-script=${JITTER_VERSION_SCRIPT}")
        set_target_properties(${PROJECT_NAME}-record PROPERTIES
            LINK_OPTIONS "${_jent_value}"
            LINK_DEPENDS "${JITTER_RECORD_VERSION_SCRIPT}")
        target_link_options(${PROJECT_NAME}-record PRIVATE
            "-Wl,--version-script=${JITTER_RECORD_VERSION_SCRIPT}")
    endif()
endif()

# What a consumer links on top, as for the library: the timer's threads, the
# external crypto, bcrypt, the stack protector of a static link.
foreach(_jent_prop INTERFACE_LINK_LIBRARIES INTERFACE_LINK_OPTIONS)
    get_target_property(_jent_value ${PROJECT_NAME} ${_jent_prop})
    if(NOT _jent_value STREQUAL "_jent_value-NOTFOUND")
        set_property(TARGET ${PROJECT_NAME}-record
                     PROPERTY ${_jent_prop} "${_jent_value}")
    endif()
endforeach()
# jitterentropy-record.h alone, which includes nothing of the library's:
# jitterentropy.h comes from the library, which every consumer links beside
# this one.
target_include_directories(${PROJECT_NAME}-record INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)
set_target_properties(${PROJECT_NAME}-record PROPERTIES
    PUBLIC_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/jitterentropy-record.h")
if(WIN32 AND NOT BUILD_SHARED_LIBS)
    target_compile_definitions(${PROJECT_NAME}-record INTERFACE
        JENT_RECORD_STATIC_LIB)
endif()

install(TARGETS ${PROJECT_NAME}-record
    EXPORT jitterentropyTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

# Linked beside libjitterentropy: each keeps its own state, and neither
# resolves a symbol of the other.
if(BUILD_TESTING)
    add_executable(record-colink record-colink.c)
    target_link_libraries(record-colink PRIVATE ${PROJECT_NAME}
                                                ${PROJECT_NAME}-record)
    add_test(NAME record-colink
             COMMAND record-colink ${CMAKE_CURRENT_BINARY_DIR}/record-colink)
    set_tests_properties(record-colink PROPERTIES SKIP_RETURN_CODE 77)
    if(WIN32 AND BUILD_SHARED_LIBS)
        set_tests_properties(record-colink PROPERTIES ENVIRONMENT_MODIFICATION
            "PATH=path_list_prepend:$<TARGET_FILE_DIR:${PROJECT_NAME}>;PATH=path_list_prepend:$<TARGET_FILE_DIR:${PROJECT_NAME}-record>")
    endif()
endif()
