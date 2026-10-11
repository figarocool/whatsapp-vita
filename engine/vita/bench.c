#include <stdio.h>
#include <string.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/processmgr.h>
#include "../src/curve25519.h"
void old25519_keypair(const uint8_t seed[32], uint8_t pub[32], uint8_t priv[32]);
void old25519_shared(uint8_t out[32], const uint8_t sk[32], const uint8_t pk[32]);
int main(void) {
  uint8_t seed[32], pub[32], priv[32], pub2[32], priv2[32], sh1[32], sh2[32]; for (int i = 0; i < 32; i++) seed[i] = i * 7 + 1;
  unsigned long long t0 = sceKernelGetProcessTimeWide(); for (int i = 0; i < 40; i++) { seed[0] = i; old25519_keypair(seed, pub, priv); } unsigned long long t1 = sceKernelGetProcessTimeWide();
  for (int i = 0; i < 40; i++) { seed[0] = i; c25519_keypair(seed, pub2, priv2); } unsigned long long t2 = sceKernelGetProcessTimeWide();
  old25519_shared(sh1, priv, pub); c25519_shared(sh2, priv, pub); int same = !memcmp(pub, pub2, 32) && !memcmp(sh1, sh2, 32);
  sceClibPrintf("BENCH vecchia: %.2f ms a chiave | nuova: %.2f ms a chiave | risultati uguali: %d | rapporto %.2fx\n", (t1 - t0) / 40000.0, (t2 - t1) / 40000.0, same, (double)(t1 - t0) / (double)(t2 - t1));
  sceKernelDelayThread(3000000); sceKernelExitProcess(0); return 0;
}
