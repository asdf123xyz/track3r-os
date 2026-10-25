# SPDX-License-Identifier: GPL-3.0-or-later
#
# Target test layer (OS-007a): unit-test suites cross-built for the Cortex-M4F with the MK66 flags and run on QEMU's
# `mps2-an386` machine. Included by CMakeLists.txt when O3OS_PLATFORM=qemu. See README.md ("Target tests on
# QEMU") for how a library adds its suite.

find_program(O3OS_QEMU NAMES qemu-system-arm DOC "qemu-system-arm, used to run the target test suites")
if(NOT O3OS_QEMU)
  # Still register the tests (with the bare command name): a missing emulator must fail ctest, not skip it.
  message(WARNING "qemu-system-arm not found: the qemu_* tests will fail until it is installed")
  set(O3OS_QEMU qemu-system-arm)
endif()

# One command line for every suite.
#   -semihosting-config  gives the guest its console and exit code (SYS_EXIT_EXTENDED).
#   -monitor/-serial none  keep QEMU off the terminal's stdin, which `-nographic` alone would grab.
#   -icount shift=5      virtual time follows the instructions executed (32 ns each, about the model's 25 MHz core)
#                        instead of the host's wall clock, so a test that waits on SysTick gives the same result on a
#                        loaded machine; the suites also run faster, since the guest never idles for real.
#                        While the guest sleeps in WFI the default (`sleep=on`) still lets virtual time follow the host's
#                        clock, so a suite that idles between interrupts can see a late or doubled tick whenever the host
#                        stalls: such a suite asks for VIRTUAL_IDLE (below).
set(O3OS_QEMU_ARGS -machine mps2-an386 -nographic -monitor none -serial none
    -semihosting-config enable=on,target=native -icount shift=5)

# o3os_target_suite(<name> SOURCES <test source>... [DEPENDS <library>...] [TIMEOUT <seconds>]
#                          [EXPECT_FAILURE test_failure|stack_overflow] [VIRTUAL_IDLE])
#
# Builds <name>_suite.elf from doctest test sources and runs it on QEMU as the ctest `qemu_<name>`.
#   SOURCES  test files, relative to the calling directory or absolute; a wildcard is expanded. Each is compiled with
#            the strict first-party warnings. The same *_test.cpp files that run on the host run here: the runner
#            below is doctest's own, so TEST_CASE / CHECK / REQUIRE are unchanged.
#   DEPENDS  the libraries under test (core, eve, usb, ...). Link order and usage requirements as usual.
#   TIMEOUT  ctest timeout, default 60 s (a suite takes well under a second of QEMU time).
#   VIRTUAL_IDLE  runs QEMU with `-icount shift=5,sleep=off`: the guest's idle time (WFI) costs no real time and virtual
#            time advances only by instructions executed and by jumps to the next timer deadline, so a suite that waits for
#            timer interrupts (the OS-005a fake I2S: a block clock, a render, a deadline) is exactly repeatable on any host
#            load, and faster. Only for suites that idle on purpose; the default leaves every other suite as it was.
#   EXPECT_FAILURE  for the harness probes only: the test passes if the suite fails in the named way, and only then
#            (O3osQemuExpectFailure.cmake): `test_failure` is a failed doctest case (QEMU status 1), `stack_overflow`
#            is a touch of the stack guard (status 3, "FAULT stack overflow"). They prove that failures propagate.
# The test passes when the suite exits 0 (semihosting exit status), ran at least one test case, and reported no
# fault. A suite that does not fit the target's memory fails to link; split it, do not trim it.
function(o3os_target_suite name)
  cmake_parse_arguments(ARG "VIRTUAL_IDLE" "TIMEOUT;EXPECT_FAILURE" "SOURCES;DEPENDS" ${ARGN})
  if(NOT ARG_SOURCES)
    message(FATAL_ERROR "o3os_target_suite(${name}): SOURCES is required")
  endif()
  if(NOT ARG_TIMEOUT)
    set(ARG_TIMEOUT 60)
  endif()

  set(sources "")
  foreach(src IN LISTS ARG_SOURCES)
    if(NOT IS_ABSOLUTE "${src}")
      set(src "${CMAKE_CURRENT_SOURCE_DIR}/${src}")
    endif()
    file(GLOB expanded CONFIGURE_DEPENDS "${src}")
    if(NOT expanded)
      message(FATAL_ERROR "o3os_target_suite(${name}): no file matches ${src}")
    endif()
    list(APPEND sources ${expanded})
  endforeach()

  # The runner's suite name (printed with the memory report) comes from one generated line per suite.
  set(name_src "${CMAKE_CURRENT_BINARY_DIR}/qemu_suite_${name}_name.cpp")
  file(CONFIGURE OUTPUT ${name_src} CONTENT "extern \"C\" const char o3os_suite_name[] = \"@name@\";\n" @ONLY)

  set(qemu_args_list ${O3OS_QEMU_ARGS})
  if(ARG_VIRTUAL_IDLE)
    list(TRANSFORM qemu_args_list REPLACE "^shift=5$" "shift=5,sleep=off")
  endif()

  set(target ${name}_suite)
  add_executable(${target} ${sources} ${name_src})
  set_target_properties(${target} PROPERTIES OUTPUT_NAME ${name}_suite SUFFIX .elf)
  target_link_libraries(${target} PRIVATE platform_qemu platform_qemu_runner ${ARG_DEPENDS})
  o3os_strict_warnings(${target})
  # Production code is built -ffp-contract=off (audio/CMakeLists.txt, the modules' packer): the suites run the same unfused arithmetic, so a test that compares two float loops (or an oracle to a loop) sees the
  # code the unit runs, not GCC's fused VFMA (OS-CPUOPT1: a reference loop built without it was a different filter).
  target_compile_options(${target} PRIVATE -ffp-contract=off)
  target_link_options(${target} PRIVATE -Wl,-Map=$<TARGET_FILE:${target}>.map)
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND ${CMAKE_SIZE} $<TARGET_FILE:${target}>
    VERBATIM)

  if(ARG_EXPECT_FAILURE)
    string(REPLACE ";" "|" qemu_args "${qemu_args_list}")
    add_test(NAME qemu_${name}
      COMMAND ${CMAKE_COMMAND} -DQEMU=${O3OS_QEMU} "-DQEMU_ARGS=${qemu_args}" -DKERNEL=$<TARGET_FILE:${target}>
              -DTIMEOUT=${ARG_TIMEOUT} -DEXPECT=${ARG_EXPECT_FAILURE} -P ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/O3osQemuExpectFailure.cmake)
    set_tests_properties(qemu_${name} PROPERTIES LABELS "qemu;probe" TIMEOUT ${ARG_TIMEOUT})
  else()
    add_test(NAME qemu_${name} COMMAND ${O3OS_QEMU} ${qemu_args_list} -kernel $<TARGET_FILE:${target}>)
    # FAIL_REGULAR_EXPRESSION: a suite that selected no test case (renamed or emptied) or died in a fault must not
    # pass on a zero exit status alone.
    set_tests_properties(qemu_${name} PROPERTIES
      LABELS qemu
      TIMEOUT ${ARG_TIMEOUT}
      FAIL_REGULAR_EXPRESSION "test cases: +0 ;o3os-qemu: FAULT")
  endif()
endfunction()
