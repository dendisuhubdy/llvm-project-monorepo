//===----------------------------------------------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

// <ios>

// class ios_base

// ios_base& showpos(ios_base& str);

#include <ios>
#include <streambuf>
#include <cassert>

struct testbuf : public std::streambuf {};

int main()
{
    testbuf sb;
    std::ios ios(&sb);
    std::ios_base& r = std::showpos(ios);
    assert(&r == &ios);
    assert(ios.flags() & std::ios::showpos);
}
