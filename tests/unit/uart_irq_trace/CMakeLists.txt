# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)
project(wch_irq_trace_unit LANGUAGES C)
enable_testing()
add_executable(trace_test main.c)
target_include_directories(trace_test PRIVATE ../../../drivers/serial)
target_compile_options(trace_test PRIVATE -Wall -Wextra -Werror)
add_test(NAME irq_trace_logic COMMAND trace_test)
