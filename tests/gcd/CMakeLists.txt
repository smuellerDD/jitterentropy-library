add_executable(gcd gcd.c)
target_include_directories(gcd PRIVATE ../../src)
target_link_libraries(gcd PRIVATE ${PROJECT_NAME})
install(TARGETS gcd)
