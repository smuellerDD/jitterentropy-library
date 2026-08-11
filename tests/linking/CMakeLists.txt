# Consumer of the installed library, deliberately not part of the build in the
# root CMakeLists.txt: it resolves the library with find_package() out of an
# install tree, which is the thing under test. Building it from inside this
# project would resolve the target directly and prove nothing about the
# installed CMake package.
#
# Configure it against an install prefix:
#
#   cmake -S tests/linking -B build-linktest -DCMAKE_PREFIX_PATH=<prefix>
#
# The imported target is what carries everything a consumer needs and is
# therefore the whole point of linking against it rather than against a bare
# -ljitterentropy: the installed include directory, JENT_STATIC_LIB on a static
# Windows build so the public header stops saying dllimport, and the transitive
# link libraries (bcrypt, pthread) that a static link has to name itself.
cmake_minimum_required(VERSION 3.14)
project(jitterentropy-linktest C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# CONFIG mode explicitly: there is no FindJitterentropy module to fall back to,
# and without it a missing package config would be reported as a missing module
# instead of as the package this project actually failed to find.
find_package(jitterentropy REQUIRED CONFIG)

add_executable(jitterentropy-linktest jitterentropy-linktest.c)
target_link_libraries(jitterentropy-linktest PRIVATE jitterentropy::jitterentropy)
