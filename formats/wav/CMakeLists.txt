# SPDX-License-Identifier: GPL-3.0-or-later
# formats_wav: the WAV sample importer (OS-002c), a resumable begin / step / report reader over core::ByteSource that
# converts into a caller-provided int16 buffer. A library of its own (beside formats/native and, with OS-004b, formats/og)
# so the sizes show separately; it needs nothing but core and registers into the format registry with one row once that
# exists. Header: <o3os/formats/wav.h>.
o3os_library(formats_wav SOURCES src/wav_importer.cpp DEPENDS core)
# The ranked layers get -fsingle-precision-constant (O3OS_RANKED_LAYERS); this library is in the formats layer too.
if(TARGET formats_wav)
  target_compile_options(formats_wav PRIVATE -fsingle-precision-constant)
  target_compile_options(formats_wav_hdrcheck PRIVATE -fsingle-precision-constant)
endif()
