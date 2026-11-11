# nxmac::gfx: NVN, Vulkan and OpenGL on Metal (nxnative dm/gfx). Objective-C++ inside, plain C++ headers outside, so code that includes them
# never sees an Objective-C type. The host's own Metal shaders (nxnative dm/gfx/msl) are compiled here when they come over (xcrun metal).
add_library(nxmac_gfx STATIC
  metal_device.mm)
add_library(nxmac::gfx ALIAS nxmac_gfx)
target_include_directories(nxmac_gfx PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(nxmac_gfx
  PUBLIC nxmac::core
  PRIVATE "-framework Foundation" "-framework Metal")
nxmac_target_defaults(nxmac_gfx)
