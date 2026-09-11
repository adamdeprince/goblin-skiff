/*
    Mosh: the mobile shell

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#ifndef FIPS_CRYPTO_HPP
#define FIPS_CRYPTO_HPP

#include <cstddef>

namespace Crypto {

/* AES-128-GCM backed exclusively by an OpenSSL provider whose algorithms
   carry both the "fips=yes" and "provider=fips" properties. */
class FipsAes128Gcm
{
private:
  class Impl;
  Impl* impl;

public:
  static const size_t KEY_LEN = 16;
  static const size_t IV_LEN = 12;
  static const size_t TAG_LEN = 16;

  FipsAes128Gcm( const unsigned char* master_key, bool server );
  ~FipsAes128Gcm();

  int encrypt( const void* associated_data,
               size_t associated_data_len,
               const void* plaintext,
               size_t plaintext_len,
               void* iv,
               size_t iv_capacity,
               void* ciphertext,
               size_t ciphertext_capacity );
  int decrypt( const void* iv,
               size_t iv_len,
               const void* associated_data,
               size_t associated_data_len,
               const void* ciphertext,
               size_t ciphertext_len,
               void* plaintext,
               size_t plaintext_capacity );

private:
  FipsAes128Gcm( const FipsAes128Gcm& );
  FipsAes128Gcm& operator=( const FipsAes128Gcm& );
};

bool fips_crypto_compiled( void );
void ensure_fips_crypto_available( void );
void fill_fips_random( void* destination, size_t size );

}

#endif
