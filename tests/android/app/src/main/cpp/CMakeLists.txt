cmake_minimum_required(VERSION 3.22)

project(jitterentropy_example LANGUAGES C)

# The library is built from this tree through its own CMakeLists.txt, the way
# an app vendoring it would. Only the library: the tools and the test suite are
# host programs.
set(JENT_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../..)
set(ENABLE_TOOLS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
# libjitterentropy-record, the raw noise recording of the Record tab, linked
# beside the library.
set(ENABLE_RECORDING ON CACHE BOOL "" FORCE)
# Linked into the JNI library, so the APK carries one .so per ABI.
set(BUILD_SHARED_LIBS OFF)
add_subdirectory(${JENT_ROOT} jitterentropy EXCLUDE_FROM_ALL)

add_library(jitterentropy_jni SHARED jitterentropy-jni.c)
target_link_libraries(jitterentropy_jni PRIVATE jitterentropy jitterentropy-record
                                               log)
