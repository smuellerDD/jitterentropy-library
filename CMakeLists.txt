cmake_minimum_required(VERSION 2.8.12)

project(jitterentropy C)
set(CMAKE_C_STANDARD 99)

option(STACK_PROTECTOR "Compile Jitter with stack protector enabled" ON)
option(INTERNAL_TIMER "Compile Jitter with the internal thread based timer" ON)
option(EXTERNAL_CRYPTO "Compile Jitter and use an external libcrypto, valid options are [AWSLC, OPENSSL, LIBGCRYPT]")
option(OLDER_GLIBC "Compile Jitter and use an older versions of pthread symbols (create and join)" OFF)
option(AARCH64_NSTIME_REGISTER "Name of the register that should be used in jent_get_nstime function")

# CMake defines the variable MSVC to true automatically when building with MSVC, replicate that for other compilers
if(CMAKE_C_COMPILER_ID MATCHES "Clang")
    set(CLANG 1)
elseif(CMAKE_C_COMPILER_ID MATCHES "GNU")
    set(GCC 1)
elseif(CMAKE_C_COMPILER_ID MATCHES "SunPro")
    set(SUNPRO 1)
endif()

if(INTERNAL_TIMER)
    list(APPEND JITTER_C_FLAGS -DJENT_CONF_ENABLE_INTERNAL_TIMER)
endif()

if(EXTERNAL_CRYPTO)
    list(APPEND JITTER_C_FLAGS  -D${EXTERNAL_CRYPTO})
endif()

if(OLDER_GLIBC)
    list(APPEND JITTER_C_FLAGS  -DUSE_OLDER_GLIBC)
endif()

if (AARCH64_NSTIME_REGISTER)
    list(APPEND JITTER_C_FLAGS -DAARCH64_NSTIME_REGISTER="${AARCH64_NSTIME_REGISTER}")
endif()

if(WIN32)
    if(MSVC)
        list(APPEND JITTER_C_FLAGS  /Od /W4 /DYNAMICBASE)
    else()
        list(APPEND JITTER_C_FLAGS  -fwrapv --param ssp-buffer-size=4 -fvisibility=hidden -Wcast-align
        -Wmissing-field-initializers -Wshadow -Wswitch-enum -Wextra -Wall -pedantic -O0 -fwrapv -Wconversion)
    endif()
else()
    list(APPEND JITTER_C_FLAGS  -fvisibility=hidden -Wcast-align -Wmissing-field-initializers -Wshadow -Wswitch-enum -Wextra -Wall -pedantic -O0 -Wconversion)

    if(NOT APPLE)
        if(SUNPRO)
            list(APPEND JITTER_LINKER_FLAGS -Wl,-z,now)
        else()
            list(APPEND JITTER_LINKER_FLAGS -Wl,-z,now,-z,relro)
        endif()
    endif()
    if(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
        list(APPEND JITTER_LINKER_FLAGS -pthread)
    endif()
    if(SUNPRO)
        list(APPEND JITTER_C_FLAGS  -m64 -Kpic -errtags=yes -erroff=E_KW_IS_AN_EXTENSION_OF_ANSI,E_ASM_UNUSED_PARAM)
    else()
        list(APPEND JITTER_C_FLAGS  -fwrapv --param ssp-buffer-size=4 -fPIE -fPIC)
    endif()

    if ((NOT GCC) OR (GCC AND CMAKE_C_COMPILER_VERSION VERSION_GREATER "4.3"))
        # -Wconversion was changed from GCC version 4.3. Prior it was meant as
        # an aid in translating code from old C to modern C. It was not meant
        # to help detect troublesome implicit conversions.
        # https://gcc.gnu.org/wiki/NewWconversion.
        set(JITTER_COMPILE_FLAGS "${JITTER_COMPILE_FLAGS} -Wconversion")
    endif()
endif()

if(STACK_PROTECTOR)
    if(GCC)
        if(CMAKE_C_COMPILER_VERSION VERSION_GREATER "4.9.0")
            list(APPEND JITTER_C_FLAGS -fstack-protector-strong)
        else()
            list(APPEND JITTER_C_FLAGS -fstack-protector-all)
        endif()
    elseif(CLANG)
        list(APPEND JITTER_C_FLAGS -fstack-protector-strong)
    endif()
endif()

file(GLOB JITTER_SRC "src/*.c")
add_library(${PROJECT_NAME})
target_sources(${PROJECT_NAME} PRIVATE ${JITTER_SRC})
set_target_properties(${PROJECT_NAME} PROPERTIES PUBLIC_HEADER "jitterentropy.h;jitterentropy-base-user.h;jitterentropy-extra-sha3.h;jitterentropy-extra-hashtime.h")
target_compile_options(${PROJECT_NAME} PRIVATE ${JITTER_C_FLAGS})
target_link_libraries(${PROJECT_NAME} PRIVATE ${JITTER_LINKER_FLAGS})
install(TARGETS ${PROJECT_NAME})

target_include_directories(${PROJECT_NAME} PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>)

if(EXTERNAL_CRYPTO STREQUAL "AWSLC")
    # This header is unique from OpenSSL
    set(EXTERNAL_CRYPTO_HEADER "openssl/base.h")
    set(EXTERNAL_CRYPTO_LIBRARY_NAME libcrypto)
elseif(EXTERNAL_CRYPTO STREQUAL "OPENSSL")
    set(EXTERNAL_CRYPTO_HEADER "openssl/crypto.h")
    set(EXTERNAL_CRYPTO_LIBRARY_NAME libcrypto)
elseif(EXTERNAL_CRYPTO STREQUAL "LIBGCRYPT")
    set(EXTERNAL_CRYPTO_HEADER "g10lib.h")
    set(EXTERNAL_CRYPTO_LIBRARY_NAME libgcrypt)
elseif(NOT EXTERNAL_CRYPTO)
    message(STATUS "Using internal functions for everything")
else()
    message(FATAL_ERROR "Unknown EXTERNAL_CRYPTO option ${EXTERNAL_CRYPTO}")
endif()

if(EXTERNAL_CRYPTO)
    find_path(LIBCRYPTO_INCLUDE_DIR
            NAMES "${EXTERNAL_CRYPTO_HEADER}"
            PATH_SUFFIXES include
            )
    message(STATUS "Found external crypto headers at ${LIBCRYPTO_INCLUDE_DIR}")
    target_include_directories(${PROJECT_NAME} PUBLIC ${LIBCRYPTO_INCLUDE_DIR})
    if(BUILD_SHARED_LIBS)
        find_library(LIBCRYPTO_LIBRARY
                NAMES "${EXTERNAL_CRYPTO_LIBRARY_NAME}.so" "${EXTERNAL_CRYPTO_LIBRARY_NAME}.dylib" "${EXTERNAL_CRYPTO_LIBRARY_NAME}.dll"
                HINTS "${CMAKE_INSTALL_PREFIX}"
                PATH_SUFFIXES build/crypto build lib64 lib
                )
    else()
        find_library(LIBCRYPTO_LIBRARY
                NAMES "${EXTERNAL_CRYPTO_LIBRARY_NAME}.so" "${EXTERNAL_CRYPTO_LIBRARY_NAME}.dylib" "${EXTERNAL_CRYPTO_LIBRARY_NAME}.dll"
                HINTS "${CMAKE_INSTALL_PREFIX}"
                PATH_SUFFIXES build/crypto build lib64 lib
                )
    endif()
    message(STATUS "Found external crypto library ${LIBCRYPTO_LIBRARY}")
    target_link_libraries(${PROJECT_NAME} PUBLIC ${LIBCRYPTO_LIBRARY})

endif()

if(INTERNAL_TIMER AND NOT ${ANDROID})
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
        target_link_libraries(${PROJECT_NAME} PUBLIC pthread)
    endif()
endif()

add_subdirectory(tests/raw-entropy/recording_userspace)
