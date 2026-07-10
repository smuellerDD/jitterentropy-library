set(CMAKE_C_STANDARD 11)

# The extractlsb source is shared between validation-runtime and
# validation-restart and therefore lives in the common parent directory.
add_executable(extractlsb ../extractlsb.c)
install(TARGETS extractlsb)
