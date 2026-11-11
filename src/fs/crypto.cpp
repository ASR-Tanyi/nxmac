#include "nxmac/fs/crypto.h"

#include <cstdio>
#include <cstdlib>

#include "bytes.h"
#include "mbedtls/aes.h"
#include "mbedtls/sha256.h"

namespace nxmac::fs {
namespace {

// Mbed TLS's AES and SHA-256 calls fail only on a wrong key length or a wrong data length, which the types and the
// callers' checks rule out: a failure is a bug, not a condition to report.
void Check(int rc, const char* what) {
    if (rc != 0) {
        std::fprintf(stderr, "nxmac::fs: %s failed: Mbed TLS error -%#x\n", what, static_cast<unsigned>(-rc));
        std::abort();
    }
}

class AesContext {
public:
    AesContext() { mbedtls_aes_init(&ctx_); }
    ~AesContext() { mbedtls_aes_free(&ctx_); }
    AesContext(const AesContext&) = delete;
    AesContext& operator=(const AesContext&) = delete;
    mbedtls_aes_context* get() { return &ctx_; }

private:
    mbedtls_aes_context ctx_;
};

class XtsContext {
public:
    XtsContext() { mbedtls_aes_xts_init(&ctx_); }
    ~XtsContext() { mbedtls_aes_xts_free(&ctx_); }
    XtsContext(const XtsContext&) = delete;
    XtsContext& operator=(const XtsContext&) = delete;
    mbedtls_aes_xts_context* get() { return &ctx_; }

private:
    mbedtls_aes_xts_context ctx_;
};

void AesEcb(const Aes128Key& key, bool encrypt, std::span<const std::uint8_t> in, std::span<std::uint8_t> out) {
    if (in.size() != out.size() || in.size() % 16 != 0) Check(-1, "AES-ECB on a partial block");
    AesContext ctx;
    if (encrypt) {
        Check(mbedtls_aes_setkey_enc(ctx.get(), key.data(), 128), "AES key setup");
    } else {
        Check(mbedtls_aes_setkey_dec(ctx.get(), key.data(), 128), "AES key setup");
    }
    for (std::size_t i = 0; i < in.size(); i += 16) {
        Check(mbedtls_aes_crypt_ecb(ctx.get(), encrypt ? MBEDTLS_AES_ENCRYPT : MBEDTLS_AES_DECRYPT, in.data() + i,
                                    out.data() + i),
              "AES-ECB");
    }
}

void AesXts(const AesXtsKey& key, bool encrypt, std::uint64_t sector, std::size_t sector_size,
            std::span<std::uint8_t> data) {
    if (sector_size < 16 || data.size() % sector_size != 0) Check(-1, "AES-XTS on a partial sector");
    XtsContext ctx;
    if (encrypt) {
        Check(mbedtls_aes_xts_setkey_enc(ctx.get(), key.data(), 256), "AES-XTS key setup");
    } else {
        Check(mbedtls_aes_xts_setkey_dec(ctx.get(), key.data(), 256), "AES-XTS key setup");
    }
    for (std::size_t i = 0; i < data.size(); i += sector_size, ++sector) {
        std::uint8_t tweak[16] = {};
        internal::StoreBe64(tweak + 8, sector);
        Check(mbedtls_aes_crypt_xts(ctx.get(), encrypt ? MBEDTLS_AES_ENCRYPT : MBEDTLS_AES_DECRYPT, sector_size, tweak,
                                    data.data() + i, data.data() + i),
              "AES-XTS");
    }
}

}  // namespace

void AesEcbDecrypt(const Aes128Key& key, std::span<const std::uint8_t> in, std::span<std::uint8_t> out) {
    AesEcb(key, false, in, out);
}

void AesEcbEncrypt(const Aes128Key& key, std::span<const std::uint8_t> in, std::span<std::uint8_t> out) {
    AesEcb(key, true, in, out);
}

void AesXtsDecrypt(const AesXtsKey& key, std::uint64_t first_sector, std::size_t sector_size,
                   std::span<std::uint8_t> data) {
    AesXts(key, false, first_sector, sector_size, data);
}

void AesXtsEncrypt(const AesXtsKey& key, std::uint64_t first_sector, std::size_t sector_size,
                   std::span<std::uint8_t> data) {
    AesXts(key, true, first_sector, sector_size, data);
}

void AesCtrTransform(const Aes128Key& key, std::uint64_t counter_upper, std::uint64_t offset,
                     std::span<std::uint8_t> data) {
    if (data.empty()) return;
    AesContext ctx;
    Check(mbedtls_aes_setkey_enc(ctx.get(), key.data(), 128), "AES key setup");
    std::uint8_t counter[16];
    std::uint8_t stream[16] = {};
    internal::StoreBe64(counter, counter_upper);
    internal::StoreBe64(counter + 8, offset / 16);
    std::size_t stream_offset = offset % 16;
    if (stream_offset != 0) {
        // Start inside a block: its key stream, then the counter of the next block, as Mbed TLS keeps them between
        // calls
        Check(mbedtls_aes_crypt_ecb(ctx.get(), MBEDTLS_AES_ENCRYPT, counter, stream), "AES-CTR");
        for (int i = 15; i >= 0; --i) {
            if (++counter[i] != 0) break;
        }
    }
    Check(mbedtls_aes_crypt_ctr(ctx.get(), data.size(), &stream_offset, counter, stream, data.data(), data.data()),
          "AES-CTR");
}

Sha256Hash Sha256(std::span<const std::uint8_t> data) {
    Sha256Hash hash;
    Check(mbedtls_sha256(data.data(), data.size(), hash.data(), 0), "SHA-256");
    return hash;
}

}  // namespace nxmac::fs
