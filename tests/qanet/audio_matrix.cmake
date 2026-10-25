# SPDX-License-Identifier: GPL-3.0-or-later
# OS-QANET C1 and C2 (docs/specs/OS-QANET.md part C): the differential audio matrix, a doctest binary of its own over the simulator, the card of the Kit (sim/CMakeLists.txt, target e2e_pro_fixture) and the
# TB-303 host module. It is sharded by cell (below). Labels audio, app and qanet (cmake/O3osLabels.cmake). The findings are compared with baseline_audio.txt.
get_property(_qa_cards GLOBAL PROPERTY O3SIM_CARDS_DIR)
if(TARGET sim AND _qa_cards)
  add_executable(o3os_qanet_audio qanet/audio_matrix_test.cpp qanet/audio_probe.cpp)
  target_include_directories(o3os_qanet_audio SYSTEM PRIVATE ${PROJECT_SOURCE_DIR}/third_party)  # <doctest/doctest.h>
  target_link_libraries(o3os_qanet_audio PRIVATE
    core usb hal eve model formats platform_host midi seq module o3m_builtin ui app sim formats_wav Threads::Threads ${CMAKE_DL_LIBS})
  o3os_strict_warnings(o3os_qanet_audio)
  add_dependencies(o3os_qanet_audio e2e_pro_fixture o3m_tb303_host)
  target_compile_definitions(o3os_qanet_audio PRIVATE
    O3_QANET_CARDS_DIR="${_qa_cards}"
    O3_QANET_AUDIO_BASELINE="${CMAKE_CURRENT_SOURCE_DIR}/qanet/baseline_audio.txt")
  # One ctest a cell (QANET_ONLY): a cell with its fresh runs is under a minute on the remote machine; the self-tests are one more. The list is held equal to cells() by a test of the binary.
  set(_qa_cells
    sample.one_shot sample.loop_fwd sample.loop_bwd sample.ping_pong sample.amiga_engine
    slice.beat_even slice.beat_add slice.beat_auto slice.slice_mode wavetable.wavetable grain.granular module.tb303 file.preview
    edit.mode_loop_bwd edit.mode_wavetable edit.mode_granular edit.slice_del edit.undo_add edit.undo_even edit.undo_auto edit.undo_slice_del edit.undo_mode edit.load_empty edit.load_over_sliced)
  string(REPLACE ";" " " _qa_cells_text "${_qa_cells}")
  # The ring cells (C3 more, a regression test of OS-BUG-SLICEPITCH: the edit is heard by the next pad although a voice still sounds): kind x edit, a ctest each; the test case has its own name filter.
  set(_qa_rings)
  foreach(_kind tail held series play)
    foreach(_edit even add auto)
      list(APPEND _qa_rings ring.${_kind}_${_edit})
    endforeach()
  endforeach()
  string(REPLACE ";" " " _qa_rings_text "${_qa_rings}")
  target_compile_definitions(o3os_qanet_audio PRIVATE O3_QANET_CELLS="${_qa_cells_text}" O3_QANET_RING_CELLS="${_qa_rings_text}")
  foreach(_cell IN LISTS _qa_cells)
    add_test(NAME qanet_audio_matrix.${_cell} COMMAND ${runner} $<TARGET_FILE:o3os_qanet_audio> "-tc=*pad against Pattern step*")
    set_tests_properties(qanet_audio_matrix.${_cell} PROPERTIES TIMEOUT 300 ENVIRONMENT "QANET_ONLY=${_cell}" FAIL_REGULAR_EXPRESSION "test cases: +0 ")
  endforeach()
  foreach(_cell IN LISTS _qa_rings)
    add_test(NAME qanet_audio_matrix.${_cell} COMMAND ${runner} $<TARGET_FILE:o3os_qanet_audio> "-tc=*voice still sounding*")
    set_tests_properties(qanet_audio_matrix.${_cell} PROPERTIES TIMEOUT 300 ENVIRONMENT "QANET_ONLY=${_cell}" FAIL_REGULAR_EXPRESSION "test cases: +0 ")
  endforeach()
  add_test(NAME qanet_audio_matrix.selftest COMMAND ${runner} $<TARGET_FILE:o3os_qanet_audio> "-tce=*pad against Pattern step*,*voice still sounding*")
  set_tests_properties(qanet_audio_matrix.selftest PROPERTIES TIMEOUT 300 FAIL_REGULAR_EXPRESSION "test cases: +0 ")
endif()
