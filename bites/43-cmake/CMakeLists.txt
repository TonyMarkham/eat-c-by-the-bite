# Bite 43 — CMake + tests: modern builds and a repeatable test suite.
#
# BUILD & RUN (out-of-source: build trees never touch the source tree):
#   cmake -S . -B build          configure (creates build/, git-ignore it)
#   cmake --build build          compile
#   ctest --test-dir build       run the test suite
# or in one line from anywhere:
#   cd /tmp && cmake /mnt/d/git/c-prep/bites/43-cmake && make && ctest
#
# ASan + UBSan variant (the Debug config adds the sanitizers):
#   cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug
#   cmake --build build-debug && ctest --test-dir build-debug
#
# BREAK IT ON PURPOSE: flip one expected value in tests.c (say, change
# the 26 in `assert(parse_hex_byte("1A") == 26)` to 27), rebuild, and
# run ctest. The suite fails LOUDLY, naming the test binary and the
# exact assert line. A test suite's value = the confidence of its red.
#
# Why both make and CMake: make (bite 38) taught you what a build IS -
# targets, dependencies, timestamps. CMake generates those makefiles
# (or ninja files) for you and speaks the dialect every library you
# will ever find_package() understands. You need both, in that order.

cmake_minimum_required(VERSION 3.16)
project(cprep_bite43 C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Warnings-as-errors, day one (the plan's rule 6), plus debug symbols.
add_compile_options(-Wall -Wextra -Werror -g)

# Sanitizers in the Debug config: -DCMAKE_BUILD_TYPE=Debug.
# (Empty build type = no sanitizers; Debug = ASan+UBSan on every test run.)
set(CMAKE_C_FLAGS_DEBUG "-g -fsanitize=address,undefined -fno-omit-frame-pointer")

# The library: the functions the tests target (not any UI).
add_library(core STATIC
    mystring.c
    util.c
    strbuf.c
    intvec.c
    hashmap.c
)

# No framework needed at this scale: an assert-based tests.c that
# exits non-zero on failure. A test that can fail loudly beats a
# framework you avoid using.
add_executable(tests tests.c)
target_link_libraries(tests PRIVATE core)

enable_testing()
add_test(NAME unit_tests COMMAND tests)
