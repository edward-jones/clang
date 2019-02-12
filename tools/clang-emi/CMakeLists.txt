set(LLVM_LINK_COMPONENTS
  Core
  Option
  Support
  ProfileData
  )

add_clang_tool(clang-emi
  ClangEMI.cpp
  )

target_link_libraries(clang-emi
  PRIVATE
  clangAST
  clangASTMatchers
  clangBasic
  clangFormat
  clangFrontend
  clangLex
  clangRewrite
  clangTooling
  clangToolingCore
  clangToolingRefactor
  )
