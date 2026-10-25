// SPDX-License-Identifier: GPL-3.0-or-later
//
// The two members of rosic::FourierTransformerRadix2 that Open303's wavetable generator calls, written here (OS-012a): the vendored class is a
// wrapper over Takuya Ooura's FFT (fft4g.c), a file whose notice the radar card (radar card `tb303-engine`) says not to take, so the
// oracle is built without it. Same interface and layout as the original, so the oracle's tables are the original's:
//   transformRealSignal(signal, buf):      buf[0] = DC, buf[1] = the Nyquist bin (both real), buf[2k] = Re X[k], buf[2k+1] = Im X[k] for
//                                          k = 1 .. N/2-1 (the real DFT, no scaling, standard sign convention),
//   transformSymmetricSpectrum(buf, out):  the inverse, scaled 1/N.
// The sign of the imaginary part cancels between the two (the generator only zeroes bins in between), and the round trip is the identity, as the
// original's is (it multiplies by 2/N for Ooura's unscaled inverse). Host only; double precision is the point.
#include "rosic_FourierTransformerRadix2.h"

#include <cmath>
#include <complex>
#include <vector>

namespace rosic {

namespace {

void fft(std::vector<std::complex<double>>& a, bool inverse) {
  const std::size_t n = a.size();
  for (std::size_t i = 1, j = 0; i < n; ++i) {
    std::size_t bit = n >> 1;
    for (; j & bit; bit >>= 1) {
      j ^= bit;
    }
    j ^= bit;
    if (i < j) {
      std::swap(a[i], a[j]);
    }
  }
  for (std::size_t len = 2; len <= n; len <<= 1) {
    const double ang = 2.0 * 3.14159265358979323846 / static_cast<double>(len) * (inverse ? 1.0 : -1.0);
    const std::complex<double> wlen(std::cos(ang), std::sin(ang));
    for (std::size_t i = 0; i < n; i += len) {
      std::complex<double> w(1.0, 0.0);
      for (std::size_t k = 0; k < len / 2; ++k) {
        const std::complex<double> u = a[i + k], v = a[i + k + len / 2] * w;
        a[i + k] = u + v;
        a[i + k + len / 2] = u - v;
        w *= wlen;
      }
    }
  }
}

}  // namespace

FourierTransformerRadix2::FourierTransformerRadix2() {
  N = 0;
  logN = 0;
  direction = FORWARD;
  normalizationMode = NORMALIZE_ON_INVERSE_TRAFO;
  normalizationFactor = 1.0;
  w = nullptr;
  ip = nullptr;
  tmpBuffer = nullptr;
  setBlockSize(256);
}

FourierTransformerRadix2::~FourierTransformerRadix2() {}

void FourierTransformerRadix2::setBlockSize(int newBlockSize) {
  if (newBlockSize >= 2 && (newBlockSize & (newBlockSize - 1)) == 0) {
    N = newBlockSize;
  }
}

void FourierTransformerRadix2::transformRealSignal(double* signal, double* reAndIm) {
  std::vector<std::complex<double>> a(static_cast<std::size_t>(N));
  for (int n = 0; n < N; ++n) {
    a[static_cast<std::size_t>(n)] = signal[n];
  }
  fft(a, false);
  reAndIm[0] = a[0].real();
  reAndIm[1] = a[static_cast<std::size_t>(N / 2)].real();
  for (int k = 1; k < N / 2; ++k) {
    reAndIm[2 * k] = a[static_cast<std::size_t>(k)].real();
    reAndIm[2 * k + 1] = a[static_cast<std::size_t>(k)].imag();
  }
}

void FourierTransformerRadix2::transformSymmetricSpectrum(double* reAndIm, double* signal) {
  std::vector<std::complex<double>> a(static_cast<std::size_t>(N));
  a[0] = reAndIm[0];
  a[static_cast<std::size_t>(N / 2)] = reAndIm[1];
  for (int k = 1; k < N / 2; ++k) {
    const std::complex<double> x(reAndIm[2 * k], reAndIm[2 * k + 1]);
    a[static_cast<std::size_t>(k)] = x;
    a[static_cast<std::size_t>(N - k)] = std::conj(x);
  }
  fft(a, true);
  for (int n = 0; n < N; ++n) {
    signal[n] = a[static_cast<std::size_t>(n)].real() / static_cast<double>(N);
  }
}

}  // namespace rosic
