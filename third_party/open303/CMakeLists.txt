# SPDX-License-Identifier: GPL-3.0-or-later
# Open303 (Robin Schmidt, MIT; THIRD_PARTY.md): the DSP core of the TB-303 emulation, vendored byte for byte, built for the HOST ONLY as the
# double-precision ORACLE of the TB-303 module's float port (tests/tb303_*_test.cpp, ADR 0002, the dsp-port skill's gate 2). It is never linked
# into an image. Not vendored: fft4g.c (a third-party FFT with no notice) and rosic_FourierTransformerRadix2.cpp (its only user); the two members of
# that class the wavetable generator needs are tests/support/open303_fft_stub.cpp. Compiled without warnings on purpose (2009 code).
set(_o3_open303_files
  GlobalFunctions.cpp rosic_Open303.cpp rosic_BlendOscillator.cpp rosic_MipMappedWaveTable.cpp rosic_TeeBeeFilter.cpp rosic_OnePoleFilter.cpp
  rosic_BiquadFilter.cpp rosic_AnalogEnvelope.cpp rosic_DecayEnvelope.cpp rosic_LeakyIntegrator.cpp rosic_EllipticQuarterBandFilter.cpp
  rosic_AcidSequencer.cpp rosic_AcidPattern.cpp rosic_MidiNoteEvent.cpp rosic_NumberManipulations.cpp rosic_RealFunctions.cpp
  rosic_FunctionTemplates.cpp rosic_Complex.cpp)
add_library(open303_oracle STATIC ${_o3_open303_files} ${PROJECT_SOURCE_DIR}/tests/support/open303_fft_stub.cpp)
target_include_directories(open303_oracle PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(open303_oracle PRIVATE -w -O2 -fno-sanitize=undefined -fno-sanitize-recover=all)
# The oracle is the original's code, not ours: no UBSan on it (it adds a TINY to doubles, converts, shifts as it likes); ASan stays.
