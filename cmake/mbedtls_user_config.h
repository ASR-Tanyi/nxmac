// nxmac's additions to Mbed TLS's default config (MBEDTLS_USER_CONFIG_FILE, cmake/Dependencies.cmake).
// SHA-256 on the ARMv8 SHA instructions where the CPU has them (every Apple silicon Mac): the hash checks of an NCA read every byte of a
// game, so they run at the speed of SHA-256. Other CPUs keep the portable code. AES already uses the ARMv8 and AES-NI instructions by default.
#define MBEDTLS_SHA256_USE_ARMV8_A_CRYPTO_IF_PRESENT
