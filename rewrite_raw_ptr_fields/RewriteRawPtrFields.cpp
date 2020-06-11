// Copyright 2020 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// This is implementation of a clang tool that rewrites raw pointer fields into
// CheckedPtr<T>:
//     Pointee* field_
// becomes:
//     CheckedPtr<Pointee> field_
//
// For more details, see the doc here:
// https://docs.google.com/document/d/1chTvr3fSofQNV_PDPEHRyUgcJCQBgTDOOBriW9gIm9M

#include <assert.h>
#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "clang/AST/ASTContext.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/ASTMatchers/ASTMatchersMacros.h"
#include "clang/Basic/CharInfo.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Lex/Lexer.h"
#include "clang/Lex/MacroArgs.h"
#include "clang/Lex/PPCallbacks.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Refactoring.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorOr.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/LineIterator.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/TargetSelect.h"

using namespace clang::ast_matchers;

namespace {

// Include path that needs to be added to all the files where CheckedPtr<...>
// replaces a raw pointer.
const char kIncludePath[] = "base/memory/checked_ptr.h";

// Name of a cmdline parameter that can be used to specify a file listing fields
// that should not be rewritten to use CheckedPtr<T>.
//
// See also:
// - FilterEmitterHelper
// - FieldDeclFilterFile
const char kExcludeFieldsParamName[] = "exclude-fields";

// Helper for an out-of-band output that
//
// 1. Is delimited in a way that makes it easy to extract it with sed like so:
//    $ DELIM = ...
//    $ cat ~/scratch/rewriter.out \
//        | sed '/^==== BEGIN $DELIM ====$/,/^==== END $DELIM ====$/{//!b};d' \
//        | sort | uniq > ~/scratch/some-out-of-band-output.txt
//
// 2. Contains one filter-string per line of output, accompanied with a comment
//    listing a set of tags that help describe why this line of output was
//    emitted:
//        Some filter # tag1, tag2
//        Another filter # tag1, tag2, tag3
//
// See also:
// - FieldDeclFilterFile
class FilterEmitterHelper {
 public:
  explicit FilterEmitterHelper(llvm::StringRef output_delimiter)
      : output_delimiter_(output_delimiter.str()) {}

  void Add(llvm::StringRef filter, llvm::StringRef tag) {
    filter_to_tags_[filter].insert(tag);
  }

  void Emit() {
    if (filter_to_tags_.empty())
      return;

    llvm::outs() << "==== BEGIN " << output_delimiter_ << " ====\n";
    for (const llvm::StringRef& filter : GetSortedKeys(filter_to_tags_)) {
      llvm::outs() << filter;

      const llvm::StringSet<>& tags = filter_to_tags_[filter];
      if (!tags.empty()) {
        std::vector<llvm::StringRef> sorted_tags = GetSortedKeys(tags);
        std::string tags_comment =
            llvm::join(sorted_tags.begin(), sorted_tags.end(), ", ");
        llvm::outs() << "  # " << tags_comment;
      }

      llvm::outs() << "\n";
    }
    llvm::outs() << "==== END " << output_delimiter_ << " ====\n";
  }

 private:
  template <typename TValue>
  std::vector<llvm::StringRef> GetSortedKeys(
      const llvm::StringMap<TValue>& map) {
    std::vector<llvm::StringRef> sorted(map.keys().begin(), map.keys().end());
    std::sort(sorted.begin(), sorted.end());
    return sorted;
  }

  std::string output_delimiter_;
  llvm::StringMap<llvm::StringSet<>> filter_to_tags_;
};

// Output format is documented in //docs/clang_tool_refactoring.md
class OutputHelper : public clang::tooling::SourceFileCallbacks {
 public:
  OutputHelper() : field_decl_filter_helper_("FIELD FILTERS") {}
  ~OutputHelper() = default;

  void PrintReplacement(const clang::SourceManager& source_manager,
                        const clang::SourceRange& replacement_range,
                        std::string replacement_text,
                        bool should_add_include = false) {
    if (ShouldSuppressOutput())
      return;

    clang::tooling::Replacement replacement(
        source_manager, clang::CharSourceRange::getCharRange(replacement_range),
        replacement_text);
    llvm::StringRef file_path = replacement.getFilePath();
    assert(!file_path.empty());

    std::replace(replacement_text.begin(), replacement_text.end(), '\n', '\0');
    llvm::outs() << "r:::" << file_path << ":::" << replacement.getOffset()
                 << ":::" << replacement.getLength()
                 << ":::" << replacement_text << "\n";

    if (should_add_include) {
      bool was_inserted = false;
      std::tie(std::ignore, was_inserted) =
          files_with_already_added_includes_.insert(file_path.str());
      if (was_inserted)
        llvm::outs() << "include-user-header:::" << file_path
                     << ":::-1:::-1:::" << kIncludePath << "\n";
    }
  }

  void AddFilteredField(const clang::FieldDecl& field_decl,
                        llvm::StringRef filter_tag) {
    std::string qualified_name = field_decl.getQualifiedNameAsString();
    field_decl_filter_helper_.Add(qualified_name, filter_tag);
  }

 private:
  // clang::tooling::SourceFileCallbacks override:
  bool handleBeginSource(clang::CompilerInstance& compiler) override {
    const clang::FrontendOptions& frontend_options = compiler.getFrontendOpts();

    assert((frontend_options.Inputs.size() == 1) &&
           "run_tool.py should invoke the rewriter one file at a time");
    const clang::FrontendInputFile& input_file = frontend_options.Inputs[0];
    assert(input_file.isFile() &&
           "run_tool.py should invoke the rewriter on actual files");

    current_language_ = input_file.getKind().getLanguage();

    if (!ShouldSuppressOutput())
      llvm::outs() << "==== BEGIN EDITS ====\n";

    return true;  // Report that |handleBeginSource| succeeded.
  }

  // clang::tooling::SourceFileCallbacks override:
  void handleEndSource() override {
    if (ShouldSuppressOutput())
      return;

    llvm::outs() << "==== END EDITS ====\n";
    field_decl_filter_helper_.Emit();
  }

  bool ShouldSuppressOutput() {
    switch (current_language_) {
      case clang::Language::Unknown:
      case clang::Language::Asm:
      case clang::Language::LLVM_IR:
      case clang::Language::OpenCL:
      case clang::Language::CUDA:
      case clang::Language::RenderScript:
      case clang::Language::HIP:
        // Rewriter can't handle rewriting the current input language.
        return true;

      case clang::Language::C:
      case clang::Language::ObjC:
        // CheckedPtr requires C++.  In particular, attempting to #include
        // "base/memory/checked_ptr.h" from C-only compilation units will lead
        // to compilation errors.
        return true;

      case clang::Language::CXX:
      case clang::Language::ObjCXX:
        return false;
    }

    assert(false && "Unrecognized clang::Language");
    return true;
  }

  llvm::StringSet<> files_with_already_added_includes_;
  FilterEmitterHelper field_decl_filter_helper_;
  clang::Language current_language_ = clang::Language::Unknown;
};

llvm::StringRef GetFilePath(const clang::SourceManager& source_manager,
                            const clang::FieldDecl& field_decl) {
  clang::SourceLocation loc = field_decl.getSourceRange().getBegin();
  if (loc.isInvalid() || !loc.isFileID())
    return llvm::StringRef();

  clang::FileID file_id = source_manager.getDecomposedLoc(loc).first;
  const clang::FileEntry* file_entry =
      source_manager.getFileEntryForID(file_id);
  if (!file_entry)
    return llvm::StringRef();

  return file_entry->getName();
}

AST_MATCHER(clang::FieldDecl, isInThirdPartyLocation) {
  llvm::StringRef file_path =
      GetFilePath(Finder->getASTContext().getSourceManager(), Node);

  // Blink is part of the Chromium git repo, even though it contains
  // "third_party" in its path.
  if (file_path.contains("third_party/blink/"))
    return false;

  // V8 needs to be considered "third party", even though its paths do not
  // contain the "third_party" substring.  In particular, the rewriter should
  // not append |.get()| to references to |v8::RegisterState::pc|, because
  // //v8/include/v8.h will *not* get rewritten.
  if (file_path.contains("v8/include/"))
    return true;

  // Otherwise, just check if the paths contains the "third_party" substring.
  return file_path.contains("third_party");
}

AST_MATCHER(clang::FieldDecl, isInGeneratedLocation) {
  llvm::StringRef file_path =
      GetFilePath(Finder->getASTContext().getSourceManager(), Node);

  return file_path.startswith("gen/") || file_path.contains("/gen/");
}

// Represents a filter file specified via cmdline, that can be used to filter
// out specific FieldDecls.
//
// See also:
// - kExcludeFieldsParamName
// - FilterEmitterHelper
class FieldDeclFilterFile {
 public:
  explicit FieldDeclFilterFile(const std::string& filepath) {
    if (!filepath.empty())
      ParseInputFile(filepath);
  }

  bool Contains(const clang::FieldDecl& field_decl) const {
    std::string qualified_name = field_decl.getQualifiedNameAsString();
    auto it = fields_to_filter_.find(qualified_name);
    return it != fields_to_filter_.end();
  }

 private:
  // Expected file format:
  // - '#' character starts a comment (which gets ignored).
  // - Blank or whitespace-only or comment-only lines are ignored.
  // - Other lines are expected to contain a fully-qualified name of a field
  //   like:
  //       autofill::AddressField::address1_ # some comment
  // - Templates are represented without template arguments, like:
  //       WTF::HashTable::table_ # some comment
  void ParseInputFile(const std::string& filepath) {
    llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> file_or_err =
        llvm::MemoryBuffer::getFile(filepath);
    if (std::error_code err = file_or_err.getError()) {
      llvm::errs() << "ERROR: Cannot open the file specified in --"
                   << kExcludeFieldsParamName << " argument: " << filepath
                   << ": " << err.message() << "\n";
      assert(false);
      return;
    }

    llvm::line_iterator it(**file_or_err, true /* SkipBlanks */, '#');
    for (; !it.is_at_eof(); ++it) {
      llvm::StringRef line = *it;

      // Remove trailing comments.
      size_t comment_start_pos = line.find('#');
      if (comment_start_pos != llvm::StringRef::npos)
        line = line.substr(0, comment_start_pos);
      line = line.trim();

      if (line.empty())
        continue;

      fields_to_filter_.insert(line);
    }
  }

  // Stores fully-namespace-qualified names of fields matched by the filter.
  llvm::StringSet<> fields_to_filter_;
};

AST_MATCHER_P(clang::FieldDecl,
              isListedInFilterFile,
              FieldDeclFilterFile,
              Filter) {
  return Filter.Contains(Node);
}

AST_MATCHER(clang::Decl, isInExternCContext) {
  return Node.getLexicalDeclContext()->isExternCContext();
}

// Given:
//   template <typename T, typename T2> class MyTemplate {};  // Node1 and Node4
//   template <typename T2> class MyTemplate<int, T2> {};     // Node2
//   template <> class MyTemplate<int, char> {};              // Node3
//   void foo() {
//     // This creates implicit template specialization (Node4) out of the
//     // explicit template definition (Node1).
//     MyTemplate<bool, double> v;
//   }
// with the following AST nodes:
//   ClassTemplateDecl MyTemplate                                       - Node1
//   | |-CXXRecordDecl class MyTemplate definition
//   | `-ClassTemplateSpecializationDecl class MyTemplate definition    - Node4
//   ClassTemplatePartialSpecializationDecl class MyTemplate definition - Node2
//   ClassTemplateSpecializationDecl class MyTemplate definition        - Node3
//
// Matches AST node 4, but not AST node2 nor node3.
AST_MATCHER(clang::ClassTemplateSpecializationDecl,
            isImplicitClassTemplateSpecialization) {
  return !Node.isExplicitSpecialization();
}

// Given:
//   template <typename T, typename T2> void foo(T t, T2 t2) {};  // N1 and N4
//   template <typename T2> void foo<int, T2>(int t, T2 t) {};    // N2
//   template <> void foo<int, char>(int t, char t2) {};          // N3
//   void foo() {
//     // This creates implicit template specialization (N4) out of the
//     // explicit template definition (N1).
//     foo<bool, double>(true, 1.23);
//   }
// with the following AST nodes:
//   FunctionTemplateDecl foo
//   |-FunctionDecl 0x191da68 foo 'void (T, T2)'         // N1
//   `-FunctionDecl 0x194bf08 foo 'void (bool, double)'  // N4
//   FunctionTemplateDecl foo
//   `-FunctionDecl foo 'void (int, T2)'                 // N2
//   FunctionDecl foo 'void (int, char)'                 // N3
//
// Matches AST node N4, but not AST nodes N1, N2 nor N3.
AST_MATCHER(clang::FunctionDecl, isImplicitFunctionTemplateSpecialization) {
  switch (Node.getTemplateSpecializationKind()) {
    case clang::TSK_ImplicitInstantiation:
      return true;
    case clang::TSK_Undeclared:
    case clang::TSK_ExplicitSpecialization:
    case clang::TSK_ExplicitInstantiationDeclaration:
    case clang::TSK_ExplicitInstantiationDefinition:
      return false;
  }
}

AST_MATCHER(clang::Type, anyCharType) {
  return Node.isAnyCharacterType();
}

AST_POLYMORPHIC_MATCHER(isInMacroLocation,
                        AST_POLYMORPHIC_SUPPORTED_TYPES(clang::Decl,
                                                        clang::Stmt,
                                                        clang::TypeLoc)) {
  return Node.getBeginLoc().isMacroID();
}

// If |field_decl| declares a field in an implicit template specialization, then
// finds and returns the corresponding FieldDecl from the template definition.
// Otherwise, just returns the original |field_decl| argument.
const clang::FieldDecl* GetExplicitDecl(const clang::FieldDecl* field_decl) {
  if (field_decl->isAnonymousStructOrUnion())
    return field_decl;  // Safe fallback - |field_decl| is not a pointer field.

  const clang::CXXRecordDecl* record_decl =
      clang::dyn_cast<clang::CXXRecordDecl>(field_decl->getParent());
  if (!record_decl)
    return field_decl;  // Non-C++ records are never template instantiations.

  const clang::CXXRecordDecl* pattern_decl =
      record_decl->getTemplateInstantiationPattern();
  if (!pattern_decl)
    return field_decl;  // |pattern_decl| is not a template instantiation.

  if (record_decl->getTemplateSpecializationKind() !=
      clang::TemplateSpecializationKind::TSK_ImplicitInstantiation) {
    return field_decl;  // |field_decl| was in an *explicit* specialization.
  }

  // Find the field decl with the same name in |pattern_decl|.
  clang::DeclContextLookupResult lookup_result =
      pattern_decl->lookup(field_decl->getDeclName());
  assert(!lookup_result.empty());
  const clang::NamedDecl* found_decl = lookup_result.front();
  assert(found_decl);
  field_decl = clang::dyn_cast<clang::FieldDecl>(found_decl);
  assert(field_decl);
  return field_decl;
}

AST_MATCHER_P(clang::FieldDecl,
              hasExplicitDecl,
              clang::ast_matchers::internal::Matcher<clang::FieldDecl>,
              InnerMatcher) {
  const clang::FieldDecl* explicit_field_decl = GetExplicitDecl(&Node);
  return InnerMatcher.matches(*explicit_field_decl, Finder, Builder);
}

// Returns |true| if and only if:
// 1. |a| and |b| are in the same file (e.g. |false| is returned if any location
//    is within macro scratch space or a similar location;  similarly |false| is
//    returned if |a| and |b| are in different files).
// 2. |a| and |b| overlap.
bool IsOverlapping(const clang::SourceManager& source_manager,
                   const clang::SourceRange& a,
                   const clang::SourceRange& b) {
  clang::FullSourceLoc a1(a.getBegin(), source_manager);
  clang::FullSourceLoc a2(a.getEnd(), source_manager);
  clang::FullSourceLoc b1(b.getBegin(), source_manager);
  clang::FullSourceLoc b2(b.getEnd(), source_manager);

  // Are all locations in a file?
  if (!a1.isFileID() || !a2.isFileID() || !b1.isFileID() || !b2.isFileID())
    return false;

  // Are all locations in the same file?
  if (a1.getFileID() != a2.getFileID() || a2.getFileID() != b1.getFileID() ||
      b1.getFileID() != b2.getFileID()) {
    return false;
  }

  // Check the 2 cases below:
  // 1. A: |============|
  //    B:      |===============|
  //       a1   b1      a2      b2
  // or
  // 2. A: |====================|
  //    B:      |=======|
  //       a1   b1      b2      a2
  bool b1_is_inside_a_range = a1.getFileOffset() <= b1.getFileOffset() &&
                              b1.getFileOffset() <= a2.getFileOffset();

  // Check the 2 cases below:
  // 1. B: |============|
  //    A:      |===============|
  //       b1   a1      b2      a2
  // or
  // 2. B: |====================|
  //    A:      |=======|
  //       b1   a1      a2      b2
  bool a1_is_inside_b_range = b1.getFileOffset() <= a1.getFileOffset() &&
                              a1.getFileOffset() <= b2.getFileOffset();

  return b1_is_inside_a_range || a1_is_inside_b_range;
}

// Matcher for FieldDecl that has a SourceRange that overlaps other declarations
// within the parent RecordDecl.
//
// Given
//   struct MyStruct {
//     int f;
//     int f2, f3;
//     struct S { int x } f4;
//   };
// - doesn't match |f|
// - matches |f2| and |f3| (which overlap each other's location)
// - matches |f4| (which overlaps the location of |S|)
AST_MATCHER(clang::FieldDecl, overlapsOtherDeclsWithinRecordDecl) {
  const clang::FieldDecl& self = Node;
  const clang::SourceManager& source_manager =
      Finder->getASTContext().getSourceManager();

  const clang::RecordDecl* record_decl = self.getParent();
  clang::SourceRange self_range(self.getBeginLoc(), self.getEndLoc());

  auto is_overlapping_sibling = [&](const clang::Decl* other_decl) {
    if (other_decl == &self)
      return false;

    clang::SourceRange other_range(other_decl->getBeginLoc(),
                                   other_decl->getEndLoc());
    return IsOverlapping(source_manager, self_range, other_range);
  };
  bool has_sibling_with_overlapping_location =
      std::any_of(record_decl->decls_begin(), record_decl->decls_end(),
                  is_overlapping_sibling);
  return has_sibling_with_overlapping_location;
}

// Rewrites |SomeClass* field| (matched as "affectedFieldDecl") into
// |CheckedPtr<SomeClass> field| and for each file rewritten in such way adds an
// |#include "base/memory/checked_ptr.h"|.
class FieldDeclRewriter : public MatchFinder::MatchCallback {
 public:
  explicit FieldDeclRewriter(OutputHelper* output_helper)
      : output_helper_(output_helper) {}

  void run(const MatchFinder::MatchResult& result) override {
    const clang::ASTContext& ast_context = *result.Context;
    const clang::SourceManager& source_manager = *result.SourceManager;

    const clang::FieldDecl* field_decl =
        result.Nodes.getNodeAs<clang::FieldDecl>("affectedFieldDecl");
    assert(field_decl && "matcher should bind 'fieldDecl'");

    const clang::TypeSourceInfo* type_source_info =
        field_decl->getTypeSourceInfo();
    assert(type_source_info && "assuming |type_source_info| is always present");

    clang::QualType pointer_type = type_source_info->getType();
    assert(type_source_info->getType()->isPointerType() &&
           "matcher should only match pointer types");

    // Calculate the |replacement_range|.
    //
    // Consider the following example:
    //      const Pointee* const field_name_;
    //      ^--------------------^  = |replacement_range|
    //                           ^  = |field_decl->getLocation()|
    //      ^                       = |field_decl->getBeginLoc()|
    //                   ^          = PointerTypeLoc::getStarLoc
    //            ^------^          = TypeLoc::getSourceRange
    //
    // We get the |replacement_range| in a bit clumsy way, because clang docs
    // for QualifiedTypeLoc explicitly say that these objects "intentionally do
    // not provide source location for type qualifiers".
    clang::SourceRange replacement_range(field_decl->getBeginLoc(),
                                         field_decl->getLocation());

    // Calculate |replacement_text|.
    std::string replacement_text = GenerateNewText(ast_context, pointer_type);
    if (field_decl->isMutable())
      replacement_text.insert(0, "mutable ");

    // Generate and print a replacement.
    output_helper_->PrintReplacement(source_manager, replacement_range,
                                     replacement_text,
                                     true /* should_add_include */);
  }

 private:
  std::string GenerateNewText(const clang::ASTContext& ast_context,
                              const clang::QualType& pointer_type) {
    std::string result;

    assert(pointer_type->isPointerType() && "caller must pass a pointer type!");
    clang::QualType pointee_type = pointer_type->getPointeeType();

    // Preserve qualifiers.
    assert(!pointer_type.isRestrictQualified() &&
           "|restrict| is a C-only qualifier and CheckedPtr<T> needs C++");
    if (pointer_type.isConstQualified())
      result += "const ";
    if (pointer_type.isVolatileQualified())
      result += "volatile ";

    // Convert pointee type to string.
    clang::PrintingPolicy printing_policy(ast_context.getLangOpts());
    printing_policy.SuppressScope = 1;  // s/blink::Pointee/Pointee/
    std::string pointee_type_as_string =
        pointee_type.getAsString(printing_policy);
    result += llvm::formatv("CheckedPtr<{0}> ", pointee_type_as_string);

    return result;
  }

  OutputHelper* const output_helper_;
};

// Rewrites |my_struct.ptr_field| (matched as "affectedMemberExpr") into
// |my_struct.ptr_field.get()|.
class AffectedExprRewriter : public MatchFinder::MatchCallback {
 public:
  explicit AffectedExprRewriter(OutputHelper* output_helper)
      : output_helper_(output_helper) {}

  void run(const MatchFinder::MatchResult& result) override {
    const clang::SourceManager& source_manager = *result.SourceManager;

    const clang::MemberExpr* member_expr =
        result.Nodes.getNodeAs<clang::MemberExpr>("affectedMemberExpr");
    assert(member_expr && "matcher should bind 'affectedMemberExpr'");

    clang::SourceLocation member_name_start = member_expr->getMemberLoc();
    size_t member_name_length = member_expr->getMemberDecl()->getName().size();
    clang::SourceLocation insertion_loc =
        member_name_start.getLocWithOffset(member_name_length);

    clang::SourceRange replacement_range(insertion_loc, insertion_loc);

    output_helper_->PrintReplacement(source_manager, replacement_range,
                                     ".get()");
  }

 private:
  OutputHelper* const output_helper_;
};

// Emits problematic fields (matched as "affectedFieldDecl") as filtered fields.
class FilteredExprWriter : public MatchFinder::MatchCallback {
 public:
  FilteredExprWriter(OutputHelper* output_helper, llvm::StringRef filter_tag)
      : output_helper_(output_helper), filter_tag_(filter_tag) {}

  void run(const MatchFinder::MatchResult& result) override {
    const clang::FieldDecl* field_decl =
        result.Nodes.getNodeAs<clang::FieldDecl>("affectedFieldDecl");
    assert(field_decl && "matcher should bind 'affectedFieldDecl'");

    output_helper_->AddFilteredField(*field_decl, filter_tag_);
  }

 private:
  OutputHelper* const output_helper_;
  llvm::StringRef filter_tag_;
};

}  // namespace

int main(int argc, const char* argv[]) {
  // TODO(dcheng): Clang tooling should do this itself.
  // http://llvm.org/bugs/show_bug.cgi?id=21627
  llvm::InitializeNativeTarget();
  llvm::InitializeNativeTargetAsmParser();
  llvm::cl::OptionCategory category(
      "rewrite_raw_ptr_fields: changes |T* field_| to |CheckedPtr<T> field_|.");
  llvm::cl::opt<std::string> exclude_fields_param(
      kExcludeFieldsParamName, llvm::cl::value_desc("filepath"),
      llvm::cl::desc("file listing fields to be blocked (not rewritten)"));
  clang::tooling::CommonOptionsParser options(argc, argv, category);
  clang::tooling::ClangTool tool(options.getCompilations(),
                                 options.getSourcePathList());

  MatchFinder match_finder;
  OutputHelper output_helper;

  // Supported pointer types =========
  // Given
  //   struct MyStrict {
  //     int* int_ptr;
  //     int i;
  //     char* char_ptr;
  //     int (*func_ptr)();
  //     int (MyStruct::* member_func_ptr)(char);
  //     int (*ptr_to_array_of_ints)[123]
  //     StructOrClassWithDeletedOperatorNew* stack_or_gc_ptr;
  //     struct { int i }* ptr_to_non_free_standing_record_or_union_or_class;
  //   };
  // matches |int*|, but not the other types.
  auto record_with_deleted_allocation_operator_type_matcher =
      recordType(hasDeclaration(cxxRecordDecl(
          hasMethod(allOf(hasOverloadedOperatorName("new"), isDeleted())))));
  auto supported_pointer_types_matcher =
      pointerType(unless(pointee(hasUnqualifiedDesugaredType(anyOf(
          record_with_deleted_allocation_operator_type_matcher, functionType(),
          memberPointerType(), anyCharType(), arrayType())))));

  // Implicit field declarations =========
  // Matches field declarations that do not explicitly appear in the source
  // code:
  // 1. fields of classes generated by the compiler to back capturing lambdas,
  // 2. fields within an implicit class or function template specialization
  //    (e.g. when a template is instantiated by a bit of code and there's no
  //    explicit specialization for it).
  auto implicit_class_specialization_matcher =
      classTemplateSpecializationDecl(isImplicitClassTemplateSpecialization());
  auto implicit_function_specialization_matcher =
      functionDecl(isImplicitFunctionTemplateSpecialization());
  auto implicit_field_decl_matcher = fieldDecl(hasParent(cxxRecordDecl(anyOf(
      isLambda(), implicit_class_specialization_matcher,
      hasAncestor(decl(anyOf(implicit_class_specialization_matcher,
                             implicit_function_specialization_matcher)))))));

  // Field declarations =========
  // Given
  //   struct S {
  //     int* y;
  //   };
  // matches |int* y|.  Doesn't match:
  // - non-pointer types
  // - fields of lambda-supporting classes
  // - fields listed in the --exclude-fields cmdline param
  // - "implicit" fields (i.e. field decls that are not explicitly present in
  //   the source code)
  FieldDeclFilterFile fields_to_exclude(exclude_fields_param);
  auto field_decl_matcher =
      fieldDecl(
          allOf(hasType(supported_pointer_types_matcher),
                unless(anyOf(overlapsOtherDeclsWithinRecordDecl(),
                             isInThirdPartyLocation(), isInGeneratedLocation(),
                             isExpansionInSystemHeader(), isInMacroLocation(),
                             isInExternCContext(),
                             isListedInFilterFile(fields_to_exclude),
                             implicit_field_decl_matcher))))
          .bind("affectedFieldDecl");
  FieldDeclRewriter field_decl_rewriter(&output_helper);
  match_finder.addMatcher(field_decl_matcher, &field_decl_rewriter);

  // Matches expressions that used to return a value of type |SomeClass*|
  // but after the rewrite return an instance of |CheckedPtr<SomeClass>|.
  // Many such expressions might need additional changes after the rewrite:
  // - Some expressions (printf args, const_cast args, etc.) might need |.get()|
  //   appended.
  // - Using such expressions in specific contexts (e.g. as in-out arguments or
  //   as a return value of a function returning references) may require
  //   additional work and should cause related fields to be emitted as
  //   candidates for the --field-filter-file parameter.
  auto affected_member_expr_matcher =
      memberExpr(member(fieldDecl(hasExplicitDecl(field_decl_matcher))))
          .bind("affectedMemberExpr");
  auto affected_implicit_expr_matcher = implicitCastExpr(has(expr(anyOf(
      // Only single implicitCastExpr is present in case of:
      // |auto* v = s.ptr_field;|
      expr(affected_member_expr_matcher),
      // 2nd nested implicitCastExpr is present in case of:
      // |const auto* v = s.ptr_field;|
      expr(implicitCastExpr(has(affected_member_expr_matcher)))))));
  auto affected_expr_matcher =
      expr(anyOf(affected_member_expr_matcher, affected_implicit_expr_matcher));

  // Places where |.get()| needs to be appended =========
  // Given
  //   void foo(const S& s) {
  //     printf("%p", s.y);
  //     const_cast<...>(s.y)
  //     reinterpret_cast<...>(s.y)
  //   }
  // matches the |s.y| expr if it matches the |affected_expr_matcher| above.
  auto affected_expr_that_needs_fixing_matcher = expr(allOf(
      affected_expr_matcher,
      hasParent(expr(anyOf(callExpr(callee(functionDecl(isVariadic()))),
                           cxxConstCastExpr(), cxxReinterpretCastExpr())))));
  AffectedExprRewriter affected_expr_rewriter(&output_helper);
  match_finder.addMatcher(affected_expr_that_needs_fixing_matcher,
                          &affected_expr_rewriter);

  // Affected ternary operator args =========
  // Given
  //   void foo(const S& s) {
  //     cond ? s.y : ...
  //   }
  // binds the |s.y| expr if it matches the |affected_expr_matcher| above.
  auto affected_ternary_operator_arg_matcher =
      conditionalOperator(eachOf(hasTrueExpression(affected_expr_matcher),
                                 hasFalseExpression(affected_expr_matcher)));
  match_finder.addMatcher(affected_ternary_operator_arg_matcher,
                          &affected_expr_rewriter);

  // Calls to templated functions =========
  // Given
  //   struct S { int* y; };
  //   template <typename T>
  //   void templatedFunc(T* arg) {}
  //   void foo(const S& s) {
  //     templatedFunc(s.y);
  //   }
  // binds the |s.y| expr if it matches the |affected_expr_matcher| above.
  auto templated_function_arg_matcher = forEachArgumentWithParam(
      affected_expr_matcher, parmVarDecl(hasType(qualType(allOf(
                                 findAll(qualType(substTemplateTypeParmType())),
                                 unless(referenceType()))))));
  match_finder.addMatcher(callExpr(templated_function_arg_matcher),
                          &affected_expr_rewriter);
  match_finder.addMatcher(cxxConstructExpr(templated_function_arg_matcher),
                          &affected_expr_rewriter);

  // |auto| type declarations =========
  // Given
  //   struct S { int* y; };
  //   void foo(const S& s) {
  //     auto* p = s.y;
  //   }
  // binds the |s.y| expr if it matches the |affected_expr_matcher| above.
  auto auto_var_decl_matcher = declStmt(forEach(
      varDecl(allOf(hasType(pointerType(pointee(autoType()))),
                    hasInitializer(anyOf(
                        affected_expr_matcher,
                        initListExpr(hasInit(0, affected_expr_matcher))))))));
  match_finder.addMatcher(auto_var_decl_matcher, &affected_expr_rewriter);

  // address-of(affected-expr) =========
  // Given
  //   ... &s.y ...
  // matches the |s.y| expr if it matches the |affected_member_expr_matcher|
  // above.
  auto affected_addr_of_expr_matcher = expr(allOf(
      affected_expr_matcher, hasParent(unaryOperator(hasOperatorName("&")))));
  FilteredExprWriter filtered_addr_of_expr_writer(&output_helper, "addr-of");
  match_finder.addMatcher(affected_addr_of_expr_matcher,
                          &filtered_addr_of_expr_writer);

  // in-out reference arg =========
  // Given
  //   struct S { SomeClass* ptr_field; };
  //   void foo(SomeClass*& in_out_arg) { ... }
  //   void bar() {
  //     S s;
  //     foo(s.ptr_field)
  //   }
  // matches the |s.ptr_field| expr if it matches the
  // |affected_member_expr_matcher| and is passed as a function argument that
  // has |FooBar*&| type.
  auto affected_in_out_ref_arg_matcher = callExpr(forEachArgumentWithParam(
      affected_expr_matcher.bind("expr"),
      parmVarDecl(hasType(referenceType(pointee(pointerType()))))));
  FilteredExprWriter filtered_in_out_ref_arg_writer(&output_helper,
                                                    "in-out-param-ref");
  match_finder.addMatcher(affected_in_out_ref_arg_matcher,
                          &filtered_in_out_ref_arg_writer);

  // Prepare and run the tool.
  std::unique_ptr<clang::tooling::FrontendActionFactory> factory =
      clang::tooling::newFrontendActionFactory(&match_finder, &output_helper);
  int result = tool.run(factory.get());
  if (result != 0)
    return result;

  return 0;
}
