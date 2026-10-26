# Bite 44 — CHECKPOINT: Contact Book v3.
#
# BUILD & RUN:
#   cmake -S . -B build && cmake --build build && ./build/contactbook
# TESTS:
#   ctest --test-dir build
# ASan+UBSan build (the checkpoint requires green under it):
#   cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug
#   cmake --build build-debug && ctest --test-dir build-debug
# Full story, usage, format spec and abuse script: see README.md.
#
# BREAK IT ON PURPOSE: hand this project (README only) to a fresh
# you-tomorrow. Whatever they get wrong is a documentation bug - fix
# the README. Also: flip an expected value in tests.c and watch the
# red; run the README's valgrind command and confirm zero leaks.

cmake_minimum_required(VERSION 3.16)
project(cprep_bite44 C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_compile_options(-Wall -Wextra -Werror -g)
set(CMAKE_C_FLAGS_DEBUG "-g -fsanitize=address,undefined -fno-omit-frame-pointer")

# The modules. main.c (UI) and tests.c sit on top; neither knows how
# the others work inside - only the documented headers.
add_library(cbcore STATIC
    contact.c
    storage.c
    strbuf.c
    util.c
)

add_executable(contactbook main.c)
target_link_libraries(contactbook PRIVATE cbcore)

add_executable(cbtests tests.c)
target_link_libraries(cbtests PRIVATE cbcore)

enable_testing()
add_test(NAME unit_tests COMMAND cbtests)
