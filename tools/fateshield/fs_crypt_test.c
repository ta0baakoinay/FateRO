/* Standalone round-trip test of the FateShield session cipher, extracted
 * verbatim from src/common/socket.cpp + src/common/socket.hpp constants. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#define  SESSION_CONST_1  0x5E8EBDC4
#define  SESSION_CONST_2  0x60CE2F03
#define  ALGO_KEY_1  0x93
#define  ALGO_KEY_2  0x17
#define  ALGO_KEY_3  0x6B

struct fateshield_crypt_unit {
	unsigned int flag;
	unsigned char pos_1, pos_2, pos_3;
	unsigned char key[256];
};

static void fateshield_session_unit(unsigned int seed, struct fateshield_crypt_unit* unit) {
	unsigned int i;
	for (i = 0; i < 256; ++i)
		unit->key[i] = (((seed -= seed * SESSION_CONST_1 - SESSION_CONST_2 * i) >> 11) & 0xFF);
	unit->pos_1 = seed % 20;
	unit->pos_2 = seed % 30;
	unit->pos_3 = seed % 50;
}

static void fateshield_enc_dec(unsigned char* data, uint32_t data_size, struct fateshield_crypt_unit* unit) {
	unsigned int i;
	for (i = 0; i < data_size; ++i) {
		unit->key[unit->pos_3] ^= unit->pos_1;
		unit->pos_2 -= unit->key[unit->pos_3] + ALGO_KEY_1;
		unit->pos_1 += unit->key[unit->pos_2] - ALGO_KEY_2;
		unit->key[unit->pos_1] ^= unit->pos_2;
		unit->pos_1 -= unit->pos_2 ^ ALGO_KEY_3;
		data[i] ^= unit->key[unit->pos_2];
		unit->pos_2 ^= unit->pos_1 - (data_size % 0xFF);
		unit->pos_3++;
	}
}

int main(void) {
	int fail = 0;
	for (int t = 0; t < 5000; ++t) {
		unsigned short seed = (unsigned short)(rand() & 0xFFFF);
		uint32_t n = 1 + (rand() % 800);
		unsigned char *a = malloc(n), *b = malloc(n);
		for (uint32_t i = 0; i < n; ++i) a[i] = b[i] = rand() & 0xFF;

		struct fateshield_crypt_unit e, d;
		/* sender and receiver derive identical state from the same session key */
		fateshield_session_unit(seed, &e);
		fateshield_session_unit(seed, &d);

		fateshield_enc_dec(a, n, &e);          /* encrypt */
		int changed = memcmp(a, b, n) != 0;    /* ciphertext should differ (usually) */
		fateshield_enc_dec(a, n, &d);          /* decrypt with peer state */

		if (memcmp(a, b, n) != 0) { fail++; if (fail<4) printf("  ROUNDTRIP FAIL seed=%u n=%u\n", seed, n); }
		(void)changed;
		free(a); free(b);
	}
	if (fail) { printf("FAIL: %d/5000 round-trips mismatched\n", fail); return 1; }
	printf("PASS: 5000/5000 encrypt->decrypt round-trips reproduced plaintext\n");
	return 0;
}
