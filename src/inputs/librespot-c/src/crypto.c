#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <assert.h>
#include <ctype.h> // for isdigit(), isupper(), islower()

#include "librespot-c-internal.h" // For endian compat functions
#include "crypto.h"


/* ----------------------------------- Crypto ------------------------------- */

#define SHA512_DIGEST_LENGTH 64
#define SHA1_DIGEST_LENGTH 20
#define bnum_new(bn)                                            \
    do {                                                        \
        if (!gcry_control(GCRYCTL_INITIALIZATION_FINISHED_P)) { \
            if (!gcry_check_version("1.5.4"))                   \
                abort();                                        \
            gcry_control(GCRYCTL_DISABLE_SECMEM, 0);            \
            gcry_control(GCRYCTL_INITIALIZATION_FINISHED, 0);   \
        }                                                       \
        bn = gcry_mpi_new(1);                                   \
    } while (0)
#define bnum_free(bn)                 gcry_mpi_release(bn)
#define bnum_num_bytes(bn)            (gcry_mpi_get_nbits(bn) + 7) / 8
#define bnum_is_zero(bn)              (gcry_mpi_cmp_ui(bn, (unsigned long)0) == 0)
#define bnum_bn2bin(bn, buf, len)     gcry_mpi_print(GCRYMPI_FMT_USG, buf, len, NULL, bn)
#define bnum_bin2bn(bn, buf, len)     gcry_mpi_scan(&bn, GCRYMPI_FMT_USG, buf, len, NULL)
#define bnum_hex2bn(bn, buf)          gcry_mpi_scan(&bn, GCRYMPI_FMT_HEX, buf, 0, 0)
#define bnum_random(bn, num_bits)     gcry_mpi_randomize(bn, num_bits, GCRY_WEAK_RANDOM)
#define bnum_add(bn, a, b)            gcry_mpi_add(bn, a, b)
#define bnum_sub(bn, a, b)            gcry_mpi_sub(bn, a, b)
#define bnum_mul(bn, a, b)            gcry_mpi_mul(bn, a, b)
#define bnum_mod(bn, a, b)            gcry_mpi_mod(bn, a, b)
typedef gcry_mpi_t bnum;
__attribute__((unused)) static void bnum_modexp(bnum bn, bnum y, bnum q, bnum p)
{
  gcry_mpi_powm(bn, y, q, p);
}
__attribute__((unused)) static void bnum_modadd(bnum bn, bnum a, bnum b, bnum m)
{
  gcry_mpi_addm(bn, a, b, m);
}

static const uint8_t generator_bytes[] = { 0x2 };
static const uint8_t prime_bytes[] =
{
  0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xc9, 0x0f, 0xda, 0xa2, 0x21, 0x68, 0xc2, 0x34,
  0xc4, 0xc6, 0x62, 0x8b, 0x80, 0xdc, 0x1c, 0xd1, 0x29, 0x02, 0x4e, 0x08, 0x8a, 0x67, 0xcc, 0x74,
  0x02, 0x0b, 0xbe, 0xa6, 0x3b, 0x13, 0x9b, 0x22, 0x51, 0x4a, 0x08, 0x79, 0x8e, 0x34, 0x04, 0xdd,
  0xef, 0x95, 0x19, 0xb3, 0xcd, 0x3a, 0x43, 0x1b, 0x30, 0x2b, 0x0a, 0x6d, 0xf2, 0x5f, 0x14, 0x37,
  0x4f, 0xe1, 0x35, 0x6d, 0x6d, 0x51, 0xc2, 0x45, 0xe4, 0x85, 0xb5, 0x76, 0x62, 0x5e, 0x7e, 0xc6,
  0xf4, 0x4c, 0x42, 0xe9, 0xa6, 0x3a, 0x36, 0x20, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
};

static void
crypto_debug(const char *fmt, ...)
{
  return;
}

/*
static void
crypto_hexdump(const char *msg, uint8_t *mem, size_t len)
{
  return;
}
*/

int
crypto_keys_set(struct crypto_keys *keys)
{
  bnum generator;
  bnum prime;
  bnum private_key;
  bnum public_key;

  bnum_bin2bn(generator, generator_bytes, sizeof(generator_bytes));
  bnum_bin2bn(prime, prime_bytes, sizeof(prime_bytes));
  bnum_new(private_key);
  bnum_new(public_key);

//  bnum_random(private_key, 8 * (sizeof(keys->private_key) - 1)); // Not sure why it is 95 bytes?
  bnum_random(private_key, 8 * sizeof(keys->private_key));

  bnum_modexp(public_key, generator, private_key, prime);

  memset(keys, 0, sizeof(struct crypto_keys));
  bnum_bn2bin(private_key, keys->private_key, sizeof(keys->private_key));
  bnum_bn2bin(public_key, keys->public_key, sizeof(keys->public_key));

  bnum_free(generator);
  bnum_free(prime);
  bnum_free(private_key);
  bnum_free(public_key);

  return 0;
}

void
crypto_shared_secret(uint8_t **shared_secret_bytes, size_t *shared_secret_bytes_len,
                     uint8_t *private_key_bytes, size_t private_key_bytes_len,
                     uint8_t *server_key_bytes, size_t server_key_bytes_len)
{
  bnum private_key;
  bnum server_key;
  bnum prime;
  bnum shared_secret;

  bnum_bin2bn(private_key, private_key_bytes, private_key_bytes_len);
  bnum_bin2bn(server_key, server_key_bytes, server_key_bytes_len);
  bnum_bin2bn(prime, prime_bytes, sizeof(prime_bytes));
  bnum_new(shared_secret);

  bnum_modexp(shared_secret, server_key, private_key, prime);

  *shared_secret_bytes_len = bnum_num_bytes(shared_secret);
  *shared_secret_bytes = malloc(*shared_secret_bytes_len);
  bnum_bn2bin(shared_secret, *shared_secret_bytes, *shared_secret_bytes_len);

  bnum_free(private_key);
  bnum_free(server_key);
  bnum_free(prime);
  bnum_free(shared_secret);
}

// Calculates challenge and send/receive keys. The challenge is allocated,
// caller must free
int
crypto_challenge(uint8_t **challenge, size_t *challenge_len,
                 uint8_t *send_key, size_t send_key_len,
                 uint8_t *recv_key, size_t recv_key_len,
                 uint8_t *packets, size_t packets_len,
                 uint8_t *shared_secret, size_t shared_secret_len)
{
  gcry_mac_hd_t hd = NULL;
  uint8_t data[0x64];
  uint8_t i;
  size_t offset;
  size_t len;

  if (gcry_mac_open(&hd, GCRY_MAC_HMAC_SHA1, 0, NULL) != GPG_ERR_NO_ERROR)
    goto error;

  if (gcry_mac_setkey(hd, shared_secret, shared_secret_len) != GPG_ERR_NO_ERROR)
    goto error;

  offset = 0;
  for (i = 1; i <= 6; i++)
    {
      gcry_mac_write(hd, packets, packets_len);
      gcry_mac_write(hd, &i, sizeof(i));
      len = sizeof(data) - offset;
      gcry_mac_read(hd, data + offset, &len);
      offset += len;
      gcry_mac_reset(hd);
    }

  gcry_mac_close(hd);
  hd = NULL;

  assert(send_key_len == 32);
  assert(recv_key_len == 32);

  memcpy(send_key, data + 20, send_key_len);
  memcpy(recv_key, data + 52, recv_key_len);

  // Calculate challenge
  if (gcry_mac_open(&hd, GCRY_MAC_HMAC_SHA1, 0, NULL) != GPG_ERR_NO_ERROR)
    goto error;

  if (gcry_mac_setkey(hd, data, 20) != GPG_ERR_NO_ERROR)
    goto error;

  gcry_mac_write(hd, packets, packets_len);

  *challenge_len = gcry_mac_get_algo_maclen(GCRY_MAC_HMAC_SHA1);
  *challenge = malloc(*challenge_len);
  gcry_mac_read(hd, *challenge, challenge_len);
  gcry_mac_close(hd);

  return 0;

 error:
  if (hd)
    gcry_mac_close(hd);
  return -1;
}

// Inplace encryption, buf_len must be larger than plain_len so that the mac
// can be added
ssize_t
crypto_encrypt(uint8_t *buf, size_t buf_len, size_t plain_len, struct crypto_cipher *cipher)
{
  uint32_t nonce;
  uint8_t mac[4];
  size_t encrypted_len;

  encrypted_len = plain_len + sizeof(mac);
  if (encrypted_len > buf_len)
    return -1;

  shn_key(&cipher->shannon, cipher->key, sizeof(cipher->key));

  nonce = htobe32(cipher->nonce);
  shn_nonce(&cipher->shannon, (uint8_t *)&nonce, sizeof(nonce));

  shn_encrypt(&cipher->shannon, buf, plain_len);
  shn_finish(&cipher->shannon, mac, sizeof(mac));

  memcpy(buf + plain_len, mac, sizeof(mac));

  cipher->nonce++;

  return encrypted_len;
}

static size_t
payload_len_get(uint8_t *header)
{
  uint16_t be;
  memcpy(&be, header + 1, sizeof(be));
  return (size_t)be16toh(be);
}

// *encrypted will consist of a header (3 bytes, encrypted), payload length (2
// bytes, encrypted, BE), the encrypted payload and then the mac (4 bytes, not
// encrypted). The return will be the number of bytes decrypted (incl mac if a
// whole packet was decrypted). Zero means not enough data for a packet.
ssize_t
crypto_decrypt(uint8_t *encrypted, size_t encrypted_len, struct crypto_cipher *cipher)
{
  uint32_t nonce;
  uint8_t mac[4];
  size_t header_len = sizeof(cipher->last_header);
  size_t payload_len;

  crypto_debug("Decrypting %zu bytes with nonce %u\n", encrypted_len, cipher->nonce);
//  crypto_hexdump("Key\n", cipher->key, sizeof(cipher->key));
//  crypto_hexdump("Encrypted\n", encrypted, encrypted_len);

  // In case we didn't even receive the basics, header and mac, then return.
  if (encrypted_len < header_len + sizeof(mac))
    {
      crypto_debug("Waiting for %zu header bytes, have %zu\n", header_len + sizeof(mac), encrypted_len);
      return 0;
    }

  // Will be zero if this is the first pass
  payload_len = payload_len_get(cipher->last_header);
  if (!payload_len)
    {
      shn_key(&cipher->shannon, cipher->key, sizeof(cipher->key));

      nonce = htobe32(cipher->nonce);
      shn_nonce(&cipher->shannon, (uint8_t *)&nonce, sizeof(nonce));

      // Decrypt header to get the size, save it in case another pass will be
      // required
      shn_decrypt(&cipher->shannon, encrypted, header_len);
      memcpy(cipher->last_header, encrypted, header_len);

      payload_len = payload_len_get(cipher->last_header);

//      crypto_debug("Payload len is %zu\n", payload_len);
//      crypto_hexdump("Decrypted header\n", encrypted, header_len);
    }

  // At this point the header is already decrypted, so now decrypt the payload
  encrypted += header_len;
  encrypted_len -= header_len + sizeof(mac);

  // Not enough data for decrypting the entire packet
  if (payload_len > encrypted_len)
    {
      crypto_debug("Waiting for %zu payload bytes, have %zu\n", payload_len, encrypted_len);
      return 0;
    }

  shn_decrypt(&cipher->shannon, encrypted, payload_len);

//  crypto_hexdump("Decrypted payload\n", encrypted, payload_len);

  shn_finish(&cipher->shannon, mac, sizeof(mac));
//  crypto_hexdump("mac in\n", encrypted + payload_len, sizeof(mac));
//  crypto_hexdump("mac our\n", mac, sizeof(mac));
  if (memcmp(mac, encrypted + payload_len, sizeof(mac)) != 0)
    {
      crypto_debug("MAC validation failed\n");
      memset(cipher->last_header, 0, header_len);
      return -1;
    }

  cipher->nonce++;
  memset(cipher->last_header, 0, header_len);

  return header_len + payload_len + sizeof(mac);
}

void
crypto_aes_free(struct crypto_aes_cipher *cipher)
{
  if (!cipher || !cipher->aes)
    return;

  gcry_cipher_close(cipher->aes);
}

int
crypto_aes_new(struct crypto_aes_cipher *cipher, uint8_t *key, size_t key_len, uint8_t *iv, size_t iv_len, const char **errmsg)
{
  gcry_error_t err;

  err = gcry_cipher_open(&cipher->aes, GCRY_CIPHER_AES128, GCRY_CIPHER_MODE_CTR, 0);
  if (err)
    {
      *errmsg = "Error initialising AES 128 CTR decryption";
      goto error;
    }

  err = gcry_cipher_setkey(cipher->aes, key, key_len);
  if (err)
    {
      *errmsg = "Could not set key for AES 128 CTR";
      goto error;
    }

  err = gcry_cipher_setctr(cipher->aes, iv, iv_len);
  if (err)
    {
      *errmsg = "Could not set iv for AES 128 CTR";
      goto error;
    }

  memcpy(cipher->aes_iv, iv, iv_len);

  return 0;

 error:
  crypto_aes_free(cipher);
  return -1;
}

int
crypto_aes_seek(struct crypto_aes_cipher *cipher, size_t seek, const char **errmsg)
{
  gcry_error_t err;
  uint64_t be64;
  uint64_t ctr;
  uint8_t iv[16];
  size_t iv_len;
  size_t num_blocks;
  size_t offset;

  assert(cipher->aes);

  iv_len = gcry_cipher_get_algo_blklen(GCRY_CIPHER_AES128);

  assert(iv_len == sizeof(iv));

  memcpy(iv, cipher->aes_iv, iv_len);
  num_blocks = seek / iv_len;
  offset = seek % iv_len;

  // Advance the block counter
  memcpy(&be64, iv + iv_len / 2, iv_len / 2);
  ctr = be64toh(be64);
  ctr += num_blocks;
  be64 = htobe64(ctr);
  memcpy(iv + iv_len / 2, &be64, iv_len / 2);

  err = gcry_cipher_setctr(cipher->aes, iv, iv_len);
  if (err)
    {
      *errmsg = "Could not set iv for AES 128 CTR";
      return -1;
    }

  // Advance if the seek is into a block. iv is used because we have it already,
  // it could be any buffer as long as it big enough
  err = gcry_cipher_decrypt(cipher->aes, iv, offset, NULL, 0);
  if (err)
    {
      *errmsg = "Error CTR offset while seeking";
      return -1;
    }

  return 0;
}

int
crypto_aes_decrypt(uint8_t *encrypted, size_t encrypted_len, struct crypto_aes_cipher *cipher, const char **errmsg)
{
  gcry_error_t err;

  err = gcry_cipher_decrypt(cipher->aes, encrypted, encrypted_len, NULL, 0);
  if (err)
    {
      *errmsg = "Error CTR decrypting";
      return -1;
    }

  return 0;
}

static unsigned char
crypto_base62_digit(char c)
{
  if (isdigit(c))
    return c - '0';
  else if (islower(c))
    return c - 'a' + 10;
  else if (isupper(c))
    return c - 'A' + 10 + 26;
  else
    return 0xff;
}

// base 62 to bin: 4gtj0ZuMWRw8WioT9SXsC2 -> 8c283882b29346829b8d021f52f5c2ce
//                 00AdHZ94Jb7oVdHVJmJsIU -> 004f421c7e934635aaf778180a8fd068
// (note that the function prefixes with zeroes)
int
crypto_base62_to_bin(uint8_t *out, size_t out_len, const char *in)
{
  uint8_t u8;
  bnum n;
  bnum base;
  bnum digit;
  const char *ptr;
  size_t len;

  u8 = 62;
  bnum_bin2bn(base, &u8, sizeof(u8));
  bnum_new(n);

  for (ptr = in; *ptr; ptr++)
    {
      // n = 62 * n + base62_digit(*p);
      bnum_mul(n, n, base);
      u8 = crypto_base62_digit(*ptr);

      // Heavy on alloc's, but means we can use bnum compability wrapper
      bnum_bin2bn(digit, &u8, sizeof(u8));
      bnum_add(n, n, digit);
      bnum_free(digit);
    }

  len = bnum_num_bytes(n);
  if (len > out_len)
    goto error;

  memset(out, 0, out_len - len);
  bnum_bn2bin(n, out + out_len - len, len);

  bnum_free(n);
  bnum_free(base);

  return (int)out_len;

 error:
  bnum_free(n);
  bnum_free(base);
  return -1;
}

static int
count_trailing_zero_bits(uint8_t *data, size_t data_len)
{
  int zero_bits = 0;
  size_t idx;
  int bit;

  for (idx = data_len - 1; idx >= 0; idx--)
    {
      for (bit = 0; bit < 8; bit++)
	{
	  if (data[idx] & (1 << bit))
	    return zero_bits;

	  zero_bits++;
        }
    }

    return zero_bits;
}

static void
sha1_sum(uint8_t *digest, uint8_t *data, size_t data_len, gcry_md_hd_t hdl)
{
  gcry_md_reset(hdl);

  gcry_md_write(hdl, data, data_len);
  gcry_md_final(hdl);

  memcpy(digest, gcry_md_read(hdl, GCRY_MD_SHA1), SHA1_DIGEST_LENGTH);
}

static void
sha1_two_part_sum(uint8_t *digest, uint8_t *data1, size_t data1_len, uint8_t *data2, size_t data2_len, gcry_md_hd_t hdl)
{
  gcry_md_reset(hdl);

  gcry_md_write(hdl, data1, data1_len);
  gcry_md_write(hdl, data2, data2_len);
  gcry_md_final(hdl);

  memcpy(digest, gcry_md_read(hdl, GCRY_MD_SHA1), SHA1_DIGEST_LENGTH);
}

static inline void
increase_hashcash(uint8_t *data, int idx)
{
  while (++data[idx] == 0 && idx > 0)
    idx--;
}

static void
timespec_sub(struct timespec *a, struct timespec *b, struct timespec *result)
{
  result->tv_sec  = a->tv_sec  - b->tv_sec;
  result->tv_nsec = a->tv_nsec - b->tv_nsec;
  if (result->tv_nsec < 0)
    {
      --result->tv_sec;
      result->tv_nsec += 1000000000L;
    }
}

// Example challenge:
// - loginctx 0300c798435c4b0beb91e3b1db591d0a7f2e32816744a007af41cc7c8043b9295e1ed8a13cc323e4af2d0a3c42463b7a358ed116c33695989e0bfade0dab9c6bc6f7f928df5d49069e8ca4c04c34034669fc97e93da1ca17a7c11b2ffbb9b85f2265b10f6c83f7ef672240cb535eb122265da9b6f8d1a55af522fcbb40efc4eb753756ea38a63aff95d3228219afb0ab887075ac2fe941f7920fd19d32226052fe0956c71f0cb63ba702dd72d50d769920cd99ec6a45e00c85af5287b5d0031d6be4072efe71c59dffa5baa4077cd2eab4f22143eff18c31c69b8647e7f517468c84ed9548943fb1ba6b750ef63cdf9ce0a0fd07cb22d19484f4baa8ee6fa35fc573d9
// - prefix 48859603d6c16c3202292df155501c55
// - length (difficulty) 10
// Solution:
// - suffix 7f7e558bd10c37d200000000000002c7
int
crypto_hashcash_solve(struct crypto_hashcash_solution *solution, struct crypto_hashcash_challenge *challenge, const char **errmsg)
{
  gcry_md_hd_t hdl;
  struct timespec start_ts;
  struct timespec stop_ts;
  uint8_t digest[SHA1_DIGEST_LENGTH];
  bool solution_found = false;
  int i;

  // 1. Hash loginctx
  // 2. Create a 16 byte suffix, fill first 8 bytes with last 8 bytes of hash, last with zeroes
  // 3. Hash challenge prefix + suffix
  // 4. Check if X last bits of hash is zeroes, where X is challenge length
  // 5. If not, increment both 8-byte parts of suffix and goto 3

  memset(solution, 0, sizeof(struct crypto_hashcash_solution));

  if (gcry_md_open(&hdl, GCRY_MD_SHA1, 0) != GPG_ERR_NO_ERROR)
    {
      *errmsg = "Error initialising SHA1 hasher";
      return -1;
    }

  sha1_sum(digest, challenge->ctx, challenge->ctx_len, hdl);

  memcpy(solution->suffix, digest + SHA1_DIGEST_LENGTH - 8, 8);

  clock_gettime(CLOCK_MONOTONIC, &start_ts);

  for (i = 0; i < challenge->max_iterations; i++)
    {
      sha1_two_part_sum(digest, challenge->prefix, sizeof(challenge->prefix), solution->suffix, sizeof(solution->suffix), hdl);

      solution_found = (count_trailing_zero_bits(digest, SHA1_DIGEST_LENGTH) >= challenge->wanted_zero_bits);
      if (solution_found)
	break;

      increase_hashcash(solution->suffix, 7);
      increase_hashcash(solution->suffix + 8, 7);
    }

  clock_gettime(CLOCK_MONOTONIC, &stop_ts);

  timespec_sub(&stop_ts, &start_ts, &solution->duration);

  gcry_md_close(hdl);

  if (!solution_found)
    {
      *errmsg = "Could not find a hashcash solution";
      return -1;
    }

  return 0;
}
