#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
void c25519_keypair(const uint8_t seed[32], uint8_t pub[32], uint8_t priv[32]);
void c25519_shared(uint8_t out[32], const uint8_t sk[32], const uint8_t pk[32]);
void c25519_sign(uint8_t sig[64], const uint8_t sk[32], const uint8_t *msg, size_t n, const uint8_t *rnd);
int c25519_verify(const uint8_t pk[32], const uint8_t *msg, size_t n, const uint8_t sig[64]);
