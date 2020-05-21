// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/memory/checked_ptr.h"

class SomeClass;

class MyClass {
  // Expected rewrite: CheckedPtr<SomeClass> raw_ptr_field;
  CheckedPtr<SomeClass> raw_ptr_field;

  // No rewrite expected.
  int int_field;
};

struct MyStruct {
  // Expected rewrite: CheckedPtr<SomeClass> raw_ptr_field;
  CheckedPtr<SomeClass> raw_ptr_field;

  // No rewrite expected.
  int int_field;

  // "*" next to the field name.  This is non-standard formatting, so
  // "clang-format off" is used to make sure |git cl format| won't change this
  // testcase.
  //
  // Expected rewrite: CheckedPtr<SomeClass> raw_ptr_field;
  // clang-format off
  CheckedPtr<SomeClass> raw_ptr_field2;
  // clang-format on

  // No rewrite expected.  (The syntax below should be rare + it is difficult to
  // generate correct, non-overlapping replacements that cover both fields.)
  SomeClass *overlapping_fields_1a, *overlapping_fields_1b;
  int overlapping_fields_2a, *overlapping_fields_2b;
  int *overlapping_fields_3a, overlapping_fields_3b;
};

template <typename T>
class MyTemplate {
  // Expected rewrite: CheckedPtr<T> raw_ptr_field;
  CheckedPtr<T> raw_ptr_field;

  // No rewrite expected.
  int int_field;
};

// The field below won't compile without the |typename| keyword (because
// at this point we don't know if MaybeProvidesType<T>::Type is a type,
// value or something else).  Let's see if the rewriter will preserve
// preserve the |typename| keyword.
template <typename T>
struct MaybeProvidesType;
template <typename T>
struct DependentNameTest {
  // Expected rewrite: CheckedPtr<typename MaybeProvidesType<T>::Type> field;
  CheckedPtr<typename MaybeProvidesType<T>::Type> field;
};
