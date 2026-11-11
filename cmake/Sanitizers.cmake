# NXMAC_SANITIZE: the sanitizers everything is built with, dependencies included: "address,undefined" (the asan preset) or "thread" (tsan).
# Applied globally, before the dependencies are fetched: TSan reports races through locks it cannot see, so Abseil must be instrumented too.
# Exceptions live in sanitizers/, as nxnative's dm/sanitizers: ignorelist.txt (ASan and UBSan, at compile time) and tsan.supp (TSan, at run
# time, from the tsan test preset). New findings in nxmac's own code are fixed, not listed.
set(NXMAC_SANITIZE "" CACHE STRING "Sanitizers to build with: empty, address,undefined, or thread")
set_property(CACHE NXMAC_SANITIZE PROPERTY STRINGS "" "address,undefined" "address" "undefined" "thread")

if(NXMAC_SANITIZE)
  if(NXMAC_SANITIZE MATCHES "thread" AND NXMAC_SANITIZE MATCHES "address")
    message(FATAL_ERROR "NXMAC_SANITIZE=${NXMAC_SANITIZE}: ThreadSanitizer cannot be combined with AddressSanitizer")
  endif()
  set(_nxmac_san_flags "-fsanitize=${NXMAC_SANITIZE}" -fno-omit-frame-pointer -g)
  if(NXMAC_SANITIZE MATCHES "address|undefined" AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    list(APPEND _nxmac_san_flags "-fsanitize-ignorelist=${PROJECT_SOURCE_DIR}/sanitizers/ignorelist.txt")
  endif()
  if(NXMAC_SANITIZE MATCHES "undefined")
    list(APPEND _nxmac_san_flags -fno-sanitize-recover=undefined)   # a UBSan finding fails the test that hit it
  endif()
  if(NXMAC_SANITIZE MATCHES "thread")
    # Every function keeps its own frame, so tsan.supp's entries match whatever the inliner does (nxnative dm/sanitizers/sanitizers.mk)
    list(APPEND _nxmac_san_flags -fno-inline)
  endif()
  # Not on assembly (.S), which takes no sanitizer flags
  add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX,OBJC,OBJCXX>:${_nxmac_san_flags}>")
  add_link_options("-fsanitize=${NXMAC_SANITIZE}")
  unset(_nxmac_san_flags)
endif()
