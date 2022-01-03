function(testprogram name)
    add_executable(${name} ${name}.c)
    target_link_libraries(${name} ${PROJECT_NAME})
    if(NOT MSVC)
        target_compile_options(${name} PRIVATE -Wno-unused-parameter)
    endif()
endfunction()

testprogram(jitterentropy-rng)
