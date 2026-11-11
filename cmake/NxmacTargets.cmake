# What every nxmac target gets: its warnings, ARC for Objective-C++, warnings as errors and clang-tidy when they are on.
# Libraries call it after add_library; tools and tests get it through nxmac_add_tool (src/tools) and nxmac_add_test (tests).
function(nxmac_target_defaults target)
  set(c_family "$<COMPILE_LANGUAGE:C,CXX,OBJC,OBJCXX>")
  set(cxx_family "$<COMPILE_LANGUAGE:CXX,OBJCXX>")
  target_compile_options(${target} PRIVATE
    "$<${c_family}:-Wall;-Wextra;-Wshadow;-Wimplicit-fallthrough;-Wformat=2;-Wundef>"
    "$<${cxx_family}:-Wnon-virtual-dtor;-Woverloaded-virtual>"
    # Clang's thread-safety analysis checks Abseil's lock annotations (ABSL_GUARDED_BY, ABSL_EXCLUSIVE_LOCKS_REQUIRED)
    "$<$<AND:${c_family},$<CXX_COMPILER_ID:Clang,AppleClang>>:-Wthread-safety>"
    "$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>")
  set_target_properties(${target} PROPERTIES COMPILE_WARNING_AS_ERROR ${NXMAC_WARNINGS_AS_ERRORS})
  if(NXMAC_CLANG_TIDY)
    set_target_properties(${target} PROPERTIES
      CXX_CLANG_TIDY "${NXMAC_CLANG_TIDY_COMMAND}"
      OBJCXX_CLANG_TIDY "${NXMAC_CLANG_TIDY_COMMAND}")
  endif()
endfunction()
