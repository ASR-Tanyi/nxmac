# nxmac::fs: the Switch's content formats, read and checked (nxnative's nx_extract did this with LibHac): prod.keys and
# tickets, NSP / PFS0 and HFS0, NCAs with their AES-XTS headers, AES-CTR sections and hash trees, update patches (BKTR),
# RomFS and nxnative's DMROMFS1 image of it, content metas, control data. Portable C++ on Mbed TLS and zlib: it builds and
# is tested on any platform.
add_library(nxmac_fs STATIC
  content_meta.cpp
  crypto.cpp
  integrity.cpp
  keys.cpp
  nacp.cpp
  nca.cpp
  package.cpp
  partition_fs.cpp
  romfs.cpp
  romfs_image.cpp
  section_storage.cpp
  storage.cpp
  ticket.cpp)
add_library(nxmac::fs ALIAS nxmac_fs)
target_include_directories(nxmac_fs PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(nxmac_fs
  PUBLIC nxmac::core absl::status absl::statusor absl::flat_hash_map
  PRIVATE MbedTLS::mbedcrypto ZLIB::ZLIB absl::strings absl::str_format)
nxmac_target_defaults(nxmac_fs)
