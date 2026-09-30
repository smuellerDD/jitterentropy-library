cmake_minimum_required(VERSION 3.22)

# Configure with the Xcode generator for iOS, see README.md:
#   cmake -S tests/ios -B build-ios -G Xcode -DCMAKE_SYSTEM_NAME=iOS
# For the app and the library alike.
set(CMAKE_OSX_DEPLOYMENT_TARGET 15.0 CACHE STRING "")
project(jitterentropy_example LANGUAGES C Swift)

# The iphoneos library directories CMake probed would break the x86_64
# simulator link; Xcode supplies those of the SDK it builds with.
set(CMAKE_C_IMPLICIT_LINK_DIRECTORIES "")
# The library's configure probes would otherwise link an unsigned executable.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
# Swift 5: Swift 6 strict concurrency would need annotations for handing the
# collector between queues that only obscure an example.
set(CMAKE_Swift_LANGUAGE_VERSION 5)

# The library is built from this tree through its own CMakeLists.txt, the way
# an app vendoring it would. Only the library: the tools and the test suite are
# host programs.
set(JENT_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(ENABLE_TOOLS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
# libjitterentropy-record, the raw noise recording of the Record tab, linked
# beside the library.
set(ENABLE_RECORDING ON CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF)
add_subdirectory(${JENT_ROOT} jitterentropy EXCLUDE_FROM_ALL)

# An App ID belongs to the first team that registers it, so a device build
# signed by another team needs a bundle identifier of its own.
set(BUNDLE_ID de.chronox.jitterentropy.example CACHE STRING
    "Bundle identifier of the example app")

add_executable(JitterEntropyExample MACOSX_BUNDLE
    JitterEntropyExampleApp.swift
    Collector.swift
    Recorder.swift)
set_target_properties(JitterEntropyExample PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in
    MACOSX_BUNDLE_GUI_IDENTIFIER ${BUNDLE_ID}
    MACOSX_BUNDLE_BUNDLE_NAME JitterEntropyExample
    MACOSX_BUNDLE_BUNDLE_VERSION 1
    MACOSX_BUNDLE_SHORT_VERSION_STRING 1.0
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER ${BUNDLE_ID}
    # iPhone and iPad
    XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
    # The C API reaches Swift through this header; the library target
    # supplies the include paths it needs.
    XCODE_ATTRIBUTE_SWIFT_OBJC_BRIDGING_HEADER
        ${CMAKE_CURRENT_SOURCE_DIR}/jitterentropy-bridging.h)
target_link_libraries(JitterEntropyExample PRIVATE jitterentropy
                                                   jitterentropy-record)
