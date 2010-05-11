//===----------------------------------------------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

// <complex>

// template<class T>
//   T
//   imag(const complex<T>& x);

#include <complex>
#include <cassert>

template <class T>
void
test()
{
    std::complex<T> z(1.5, 2.5);
    assert(imag(z) == 2.5);
}

int main()
{
    test<float>();
    test<double>();
    test<long double>();
}
