# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
project(wch_uart_contract_tests LANGUAGES C)
enable_testing()
add_executable(uart_contract main.c)
target_include_directories(uart_contract PRIVATE ../../../drivers/serial)
target_compile_features(uart_contract PRIVATE c_std_11)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(uart_contract PRIVATE -Wall -Wextra -Werror -Wconversion)
endif()
foreach(suite baud buffer progress)
  add_test(NAME uart_contract.${suite} COMMAND uart_contract ${suite})
endforeach()
