// SPDX-License-Identifier: MIT
/*
 * Copyright (C) 2018 Marco Antônio Bueno da Silva <bueno.mario@gmail.com>
 *
 * This file is part of batchpress.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#pragma once

/**
 * @file export.hpp
 * @brief Symbol visibility macros for the batchpress shared library.
 *
 * BATCHPRESS_API must be applied to every public symbol (class, function)
 * that should be accessible from outside the .so / .dll.
 *
 * Platform rules:
 *   Linux / Android  — GCC/Clang visibility attributes
 *   Windows          — __declspec(dllexport) when building the DLL,
 *                      __declspec(dllimport) when consuming it
 *   Other            — empty (symbols visible by default)
 */

#if defined(_WIN32) || defined(__CYGWIN__)
#   if defined(BATCHPRESS_BUILD_SHARED)          // set by CMake when building the DLL
#       define BATCHPRESS_API __declspec(dllexport)
#   else
#       define BATCHPRESS_API __declspec(dllimport)
#   endif
#elif defined(__GNUC__) || defined(__clang__)  // Linux, macOS, Android
#   define BATCHPRESS_API __attribute__((visibility("default")))
#else
#   define BATCHPRESS_API
#endif
