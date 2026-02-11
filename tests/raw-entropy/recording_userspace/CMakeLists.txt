cmake_minimum_required(VERSION 3.14)

project(jitterentropy C)
set(CMAKE_C_STANDARD 11)

function(testprogram name)
    add_executable(${name} ${name}.c)
    target_link_libraries(${name} ${PROJECT_NAME})
    if(MINGW AND INTERNAL_TIMER)
        target_link_libraries(${name} "pthread")
    endif()
    target_link_directories(${name} PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../../build>)
    target_include_directories(${name} PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../..>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../../src>)
    target_compile_options(${name} PRIVATE ${JITTER_C_FLAGS})
    include(GNUInstallDirs)
    install(TARGETS ${name})
endfunction()

testprogram(jitterentropy-hashtime)
testprogram(jitterentropy-osr)
testprogram(jitterentropy-rng)
