#ifndef __CRYPTO_H__
#define __CRYPTO_H__

#include <inttypes.h>
#include <stddef.h>
#include <gcrypt.h>
#include <time.h>

#include "shannon/Shannon.h"

struct crypto_cipher
{
  shn_ctx shannon;
  uint8_t key[32];
  uint32_t nonce;
  uint8_t last_header[3]; // uint8 cmd and uint16 BE size

  void (*logmsg)(const char *fmt, ...);
};

struct crypto_aes_cipher
{
  gcry_cipher_hd_t aes;
  uint8_t key[16];
  uint8_t aes_iv[16];
};

struct crypto_keys
{
  uint8_t private_key[96];
  uint8_t public_key[96];

  uint8_t *shared_secret;
  size_t shared_secret_len;
};

struct crypto_hashcash_challenge
{
  uint8_t *ctx;
  size_t ctx_len;
  uint8_t prefix[16];

  // Required number of trailing zero bits in the SHA1 of prefix and suffix.
  // More bits -> more difficult.
  int wanted_zero_bits;

  // Give up limit
  int max_iterations;
};

struct crypto_hashcash_solution
{
  uint8_t suffix[16];
  struct timespec duration;
};


void
crypto_shared_secret(uint8_t **shared_secret_bytes, size_t *shared_secret_bytes_len,
                     uint8_t *private_key_bytes, size_t private_key_bytes_len,
                     uint8_t *server_key_bytes, size_t server_key_bytes_len);

int
crypto_challenge(uint8_t **challenge, size_t *challenge_len,
                 uint8_t *send_key, size_t send_key_len,
                 uint8_t *recv_key, size_t recv_key_len,
                 uint8_t *packets, size_t packets_len,
                 uint8_t *shared_secret, size_t shared_secret_len);

int
crypto_keys_set(struct crypto_keys *keys);

ssize_t
crypto_encrypt(uint8_t *buf, size_t buf_len, size_t plain_len, struct crypto_cipher *cipher);

ssize_t
crypto_decrypt(uint8_t *encrypted, size_t encrypted_len, struct crypto_cipher *cipher);


void
crypto_aes_free(struct crypto_aes_cipher *cipher);

int
crypto_aes_new(struct crypto_aes_cipher *cipher, uint8_t *key, size_t key_len, uint8_t *iv, size_t iv_len, const char **errmsg);

int
crypto_aes_seek(struct crypto_aes_cipher *cipher, size_t seek, const char **errmsg);

int
crypto_aes_decrypt(uint8_t *encrypted, size_t encrypted_len, struct crypto_aes_cipher *cipher, const char **errmsg);


int
crypto_base62_to_bin(uint8_t *out, size_t out_len, const char *in);

int
crypto_hashcash_solve(struct crypto_hashcash_solution *solution, struct crypto_hashcash_challenge *challenge, const char **errmsg);

#endif /* __CRYPTO_H__ */
