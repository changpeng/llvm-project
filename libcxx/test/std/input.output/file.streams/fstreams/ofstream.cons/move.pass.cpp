//===----------------------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// <fstream>

// template <class charT, class traits = char_traits<charT> >
// class basic_ofstream

// basic_ofstream(basic_ofstream&& rhs);

// XFAIL: FROZEN-CXX03-HEADERS-FIXME

#include <fstream>
#include <cassert>

#include "test_macros.h"
#include "platform_support.h"
#include "operator_hijacker.h"

int main(int, char**)
{
    std::string temp = get_temp_file_name();
    {
        std::ofstream fso(temp.c_str());
        std::ofstream fs = std::move(fso);
        fs << 3.25;
    }
    {
        std::ifstream fs(temp.c_str());
        double x = 0;
        fs >> x;
        assert(x == 3.25);
    }
    std::remove(temp.c_str());

    {
      std::basic_ofstream<char, operator_hijacker_char_traits<char> > fso(temp.c_str());
      std::basic_ofstream<char, operator_hijacker_char_traits<char> > fs = std::move(fso);
      fs << "3.25";
    }
    {
      std::ifstream fs(temp.c_str());
      double x = 0;
      fs >> x;
      assert(x == 3.25);
    }
    std::remove(temp.c_str());

#ifndef TEST_HAS_NO_WIDE_CHARACTERS
    {
        std::wofstream fso(temp.c_str());
        std::wofstream fs = std::move(fso);
        fs << 3.25;
    }
    {
        std::wifstream fs(temp.c_str());
        double x = 0;
        fs >> x;
        assert(x == 3.25);
    }
    std::remove(temp.c_str());

    {
      std::basic_ofstream<wchar_t, operator_hijacker_char_traits<wchar_t> > fso(temp.c_str());
      std::basic_ofstream<wchar_t, operator_hijacker_char_traits<wchar_t> > fs = std::move(fso);
      fs << L"3.25";
    }
    {
      std::wifstream fs(temp.c_str());
      double x = 0;
      fs >> x;
      assert(x == 3.25);
    }
    std::remove(temp.c_str());
#endif

  return 0;
}
