//===----------------------------------------------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

// <complex>

// template<Arithmetic T>
//   T
//   real(const T& x);

#include <complex>
#include <type_traits>
#include <cassert>

#include "../cases.h"

template <class T>
void
test(T x, typename std::enable_if<std::is_integral<T>::value>::type* = 0)
{
    static_assert((std::is_same<decltype(std::real(x)), double>::value), "");
    assert(std::real(x) == x);
}

template <class T>
void
test(T x, typename std::enable_if<!std::is_integral<T>::value>::type* = 0)
{
    static_assert((std::is_same<decltype(std::real(x)), T>::value), "");
    assert(std::real(x) == x);
}

template <class T>
void
test()
{
    test<T>(0);
    test<T>(1);
    test<T>(10);
}


int main()
{
    test<float>();
    test<double>();
    test<long double>();
    test<int>();
    test<unsigned>();
    test<long long>();
}
