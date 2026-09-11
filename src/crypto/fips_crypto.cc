/*
    Mosh: the mobile shell

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#include "src/include/config.h"

#include "src/crypto/crypto.h"
#include "src/crypto/fips_crypto.h"

#include <climits>
#include <cstring>
#include <string>

#if defined( HAVE_OPENSSL_FIPS_CRYPTO )
#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/params.h>
#include <openssl/provider.h>
#include <openssl/rand.h>
#endif

using namespace Crypto;

#if defined( HAVE_OPENSSL_FIPS_CRYPTO )

namespace {

const char STRICT_PROPERTIES[] = "fips=yes,provider=fips";

std::string openssl_error( const std::string& operation )
{
  std::string result = operation;
  unsigned long error = ERR_get_error();
  if ( error != 0 ) {
    char buffer[256];
    ERR_error_string_n( error, buffer, sizeof buffer );
    result += ": ";
    result += buffer;
  }
  return result;
}

class FipsProvider
{
private:
  OSSL_LIB_CTX* library_context;
  OSSL_PROVIDER* provider;
  EVP_CIPHER* aes_128_gcm;
  EVP_KDF* hkdf;
  bool random_provider_verified;

  void cleanup( void )
  {
    if ( hkdf ) {
      EVP_KDF_free( hkdf );
    }
    hkdf = NULL;
    if ( aes_128_gcm ) {
      EVP_CIPHER_free( aes_128_gcm );
    }
    aes_128_gcm = NULL;
    if ( provider ) {
      OSSL_PROVIDER_unload( provider );
    }
    provider = NULL;
    if ( library_context ) {
      OSSL_LIB_CTX_free( library_context );
    }
    library_context = NULL;
  }

  [[noreturn]] void fail( const std::string& operation )
  {
    const std::string message = openssl_error( operation );
    cleanup();
    throw CryptoException( message );
  }

  static bool provider_is_fips( const OSSL_PROVIDER* candidate )
  {
    const char* name = candidate ? OSSL_PROVIDER_get0_name( candidate ) : NULL;
    return name && strcmp( name, "fips" ) == 0;
  }

public:
  FipsProvider()
    : library_context( NULL ), provider( NULL ), aes_128_gcm( NULL ), hkdf( NULL ),
      random_provider_verified( false )
  {
    ERR_clear_error();
    library_context = OSSL_LIB_CTX_new();
    if ( !library_context ) {
      fail( "could not create the OpenSSL FIPS library context" );
    }

    /* Loading the default OpenSSL configuration is intentional.  A usable
       FIPS provider must have been installed and activated with its generated
       fipsmodule.cnf integrity data. */
    if ( OSSL_LIB_CTX_load_config( library_context, NULL ) != 1 ) {
      fail( "could not load the OpenSSL configuration for --fips-crypto" );
    }
    if ( EVP_set_default_properties( library_context, STRICT_PROPERTIES ) != 1 ) {
      fail( "could not restrict OpenSSL to the FIPS provider" );
    }

    provider = OSSL_PROVIDER_load( library_context, "fips" );
    if ( !provider ) {
      fail( "OpenSSL FIPS provider is not installed or configured" );
    }
    if ( OSSL_PROVIDER_self_test( provider ) != 1 ) {
      fail( "OpenSSL FIPS provider self-test failed" );
    }

    aes_128_gcm = EVP_CIPHER_fetch( library_context, "AES-128-GCM", STRICT_PROPERTIES );
    if ( !aes_128_gcm || !provider_is_fips( EVP_CIPHER_get0_provider( aes_128_gcm ) ) ) {
      fail( "could not fetch AES-128-GCM from the OpenSSL FIPS provider" );
    }

    hkdf = EVP_KDF_fetch( library_context, "HKDF", STRICT_PROPERTIES );
    if ( !hkdf || !provider_is_fips( EVP_KDF_get0_provider( hkdf ) ) ) {
      fail( "could not fetch HKDF from the OpenSSL FIPS provider" );
    }

    /* These calls must precede the first RAND operation in this dedicated
       library context. */
    if ( RAND_set_DRBG_type( library_context, "CTR-DRBG", STRICT_PROPERTIES, "AES-256-CTR", NULL ) != 1 ) {
      fail( "could not select the OpenSSL FIPS CTR-DRBG" );
    }
  }

  ~FipsProvider() { cleanup(); }

  OSSL_LIB_CTX* context( void ) const { return library_context; }
  const EVP_CIPHER* cipher( void ) const { return aes_128_gcm; }

  void random_bytes( void* destination, size_t size )
  {
    if ( size == 0 ) {
      return;
    }
    ERR_clear_error();
    if ( RAND_bytes_ex( library_context, static_cast<unsigned char*>( destination ), size, 128 ) != 1 ) {
      throw CryptoException( openssl_error( "OpenSSL FIPS DRBG could not generate random bytes" ) );
    }

    if ( !random_provider_verified ) {
      EVP_RAND_CTX* random_context = RAND_get0_public( library_context );
      const EVP_RAND* random = random_context ? EVP_RAND_CTX_get0_rand( random_context ) : NULL;
      if ( !random || !provider_is_fips( EVP_RAND_get0_provider( random ) ) ) {
        throw CryptoException( "OpenSSL random generation did not come from the FIPS provider" );
      }
      random_provider_verified = true;
    }
  }

  void derive_traffic_keys( const unsigned char* master_key,
                            unsigned char* client_to_server,
                            unsigned char* server_to_client ) const
  {
    static const unsigned char salt[] = "goblin-mosh fips crypto v1";
    static const unsigned char info[] = "AES-128-GCM client-to-server and server-to-client traffic keys";
    char digest[] = "SHA2-256";
    char properties[] = "fips=yes,provider=fips";
    char mode[] = "EXTRACT_AND_EXPAND";
    unsigned char derived[2 * FipsAes128Gcm::KEY_LEN];

    EVP_KDF_CTX* context = EVP_KDF_CTX_new( hkdf );
    if ( !context ) {
      throw CryptoException( openssl_error( "could not create the OpenSSL FIPS HKDF context" ) );
    }

    OSSL_PARAM parameters[7];
    OSSL_PARAM* parameter = parameters;
    *parameter++ = OSSL_PARAM_construct_utf8_string( OSSL_KDF_PARAM_DIGEST, digest, 0 );
    *parameter++ = OSSL_PARAM_construct_utf8_string( OSSL_KDF_PARAM_PROPERTIES, properties, 0 );
    *parameter++ = OSSL_PARAM_construct_utf8_string( OSSL_KDF_PARAM_MODE, mode, 0 );
    *parameter++ = OSSL_PARAM_construct_octet_string(
      OSSL_KDF_PARAM_KEY, const_cast<unsigned char*>( master_key ), FipsAes128Gcm::KEY_LEN );
    *parameter++ = OSSL_PARAM_construct_octet_string(
      OSSL_KDF_PARAM_SALT, const_cast<unsigned char*>( salt ), sizeof salt - 1 );
    *parameter++ = OSSL_PARAM_construct_octet_string(
      OSSL_KDF_PARAM_INFO, const_cast<unsigned char*>( info ), sizeof info - 1 );
    *parameter = OSSL_PARAM_construct_end();

    ERR_clear_error();
    const int result = EVP_KDF_derive( context, derived, sizeof derived, parameters );
    EVP_KDF_CTX_free( context );
    if ( result != 1 ) {
      OPENSSL_cleanse( derived, sizeof derived );
      throw CryptoException( openssl_error( "OpenSSL FIPS HKDF could not derive traffic keys" ) );
    }

    memcpy( client_to_server, derived, FipsAes128Gcm::KEY_LEN );
    memcpy( server_to_client, derived + FipsAes128Gcm::KEY_LEN, FipsAes128Gcm::KEY_LEN );
    OPENSSL_cleanse( derived, sizeof derived );
  }
};

FipsProvider& fips_provider( void )
{
  static FipsProvider provider;
  return provider;
}

void require_size_fits_evp( size_t size, const char* description )
{
  if ( size > static_cast<size_t>( INT_MAX ) ) {
    throw CryptoException( std::string( description ) + " is too large for OpenSSL EVP" );
  }
}

}

class Crypto::FipsAes128Gcm::Impl
{
public:
  EVP_CIPHER_CTX* encrypt_context;
  EVP_CIPHER_CTX* decrypt_context;
  unsigned char encrypt_key[KEY_LEN];
  unsigned char decrypt_key[KEY_LEN];

  Impl( const unsigned char* master_key, bool server ) : encrypt_context( NULL ), decrypt_context( NULL )
  {
    unsigned char client_to_server[KEY_LEN];
    unsigned char server_to_client[KEY_LEN];
    fips_provider().derive_traffic_keys( master_key, client_to_server, server_to_client );

    memcpy( encrypt_key, server ? server_to_client : client_to_server, KEY_LEN );
    memcpy( decrypt_key, server ? client_to_server : server_to_client, KEY_LEN );
    OPENSSL_cleanse( client_to_server, sizeof client_to_server );
    OPENSSL_cleanse( server_to_client, sizeof server_to_client );

    encrypt_context = EVP_CIPHER_CTX_new();
    decrypt_context = EVP_CIPHER_CTX_new();
    if ( !encrypt_context || !decrypt_context ) {
      EVP_CIPHER_CTX_free( encrypt_context );
      EVP_CIPHER_CTX_free( decrypt_context );
      OPENSSL_cleanse( encrypt_key, sizeof encrypt_key );
      OPENSSL_cleanse( decrypt_key, sizeof decrypt_key );
      throw CryptoException( openssl_error( "could not allocate AES-128-GCM contexts" ) );
    }
  }

  ~Impl()
  {
    EVP_CIPHER_CTX_free( encrypt_context );
    EVP_CIPHER_CTX_free( decrypt_context );
    OPENSSL_cleanse( encrypt_key, sizeof encrypt_key );
    OPENSSL_cleanse( decrypt_key, sizeof decrypt_key );
  }
};

bool Crypto::fips_crypto_compiled( void ) { return true; }

void Crypto::ensure_fips_crypto_available( void )
{
  (void)fips_provider();
}

void Crypto::fill_fips_random( void* destination, size_t size )
{
  fips_provider().random_bytes( destination, size );
}

FipsAes128Gcm::FipsAes128Gcm( const unsigned char* master_key, bool server ) : impl( NULL )
{
  ensure_fips_crypto_available();
  impl = new Impl( master_key, server );
}

FipsAes128Gcm::~FipsAes128Gcm()
{
  delete impl;
}

int FipsAes128Gcm::encrypt( const void* associated_data,
                            size_t associated_data_len,
                            const void* plaintext,
                            size_t plaintext_len,
                            void* iv,
                            size_t iv_capacity,
                            void* ciphertext,
                            size_t ciphertext_capacity )
{
  if ( iv_capacity < IV_LEN ) {
    throw CryptoException( "AES-128-GCM IV buffer is too small" );
  }
  if ( ciphertext_capacity < plaintext_len + TAG_LEN ) {
    throw CryptoException( "AES-128-GCM ciphertext buffer is too small" );
  }
  require_size_fits_evp( associated_data_len, "AES-128-GCM associated data" );
  require_size_fits_evp( plaintext_len, "AES-128-GCM plaintext" );

  EVP_CIPHER_CTX* context = impl->encrypt_context;
  ERR_clear_error();
  if ( EVP_CIPHER_CTX_reset( context ) != 1
       /* A NULL IV asks the provider to generate it inside the module. */
       || EVP_EncryptInit_ex2( context, fips_provider().cipher(), impl->encrypt_key, NULL, NULL ) != 1
       || EVP_CIPHER_CTX_get_iv_length( context ) != static_cast<int>( IV_LEN ) ) {
    throw CryptoException( openssl_error( "could not initialize AES-128-GCM encryption" ) );
  }

  int ciphertext_len = 0;
  int produced = 0;
  if ( associated_data_len > 0
       && EVP_EncryptUpdate( context,
                             NULL,
                             &produced,
                             static_cast<const unsigned char*>( associated_data ),
                             static_cast<int>( associated_data_len ) )
            != 1 ) {
    throw CryptoException( openssl_error( "AES-128-GCM associated-data authentication failed" ) );
  }
  if ( plaintext_len > 0
       && EVP_EncryptUpdate( context,
                             static_cast<unsigned char*>( ciphertext ),
                             &produced,
                             static_cast<const unsigned char*>( plaintext ),
                             static_cast<int>( plaintext_len ) )
            != 1 ) {
    throw CryptoException( openssl_error( "AES-128-GCM encryption failed" ) );
  }
  ciphertext_len += produced;
  if ( EVP_EncryptFinal_ex( context, static_cast<unsigned char*>( ciphertext ) + ciphertext_len, &produced ) != 1 ) {
    throw CryptoException( openssl_error( "AES-128-GCM finalization failed" ) );
  }
  ciphertext_len += produced;
  if ( EVP_CIPHER_CTX_get_original_iv( context, iv, IV_LEN ) != 1 ) {
    throw CryptoException( openssl_error( "could not obtain the provider-generated AES-128-GCM IV" ) );
  }
#if defined( OSSL_CIPHER_PARAM_AEAD_IV_GENERATED )
  const OSSL_PARAM* gettable_parameters = EVP_CIPHER_CTX_gettable_params( context );
  if ( gettable_parameters != nullptr
       && OSSL_PARAM_locate_const( gettable_parameters, OSSL_CIPHER_PARAM_AEAD_IV_GENERATED ) != nullptr ) {
    unsigned int iv_generated = 0;
    OSSL_PARAM indicator_parameters[] = {
      OSSL_PARAM_construct_uint( OSSL_CIPHER_PARAM_AEAD_IV_GENERATED, &iv_generated ), OSSL_PARAM_construct_end()
    };
    if ( EVP_CIPHER_CTX_get_params( context, indicator_parameters ) != 1 || iv_generated != 1 ) {
      throw CryptoException( "OpenSSL FIPS provider did not confirm internal AES-GCM IV generation" );
    }
  }
#endif
  if ( ciphertext_len != static_cast<int>( plaintext_len )
       || EVP_CIPHER_CTX_ctrl( context,
                               EVP_CTRL_AEAD_GET_TAG,
                               TAG_LEN,
                               static_cast<unsigned char*>( ciphertext ) + ciphertext_len )
            != 1 ) {
    throw CryptoException( openssl_error( "could not obtain the AES-128-GCM authentication tag" ) );
  }
  return ciphertext_len + TAG_LEN;
}

int FipsAes128Gcm::decrypt( const void* iv,
                            size_t iv_len,
                            const void* associated_data,
                            size_t associated_data_len,
                            const void* ciphertext,
                            size_t ciphertext_len,
                            void* plaintext,
                            size_t plaintext_capacity )
{
  if ( iv_len != IV_LEN || ciphertext_len < TAG_LEN ) {
    return -1;
  }
  const size_t encrypted_len = ciphertext_len - TAG_LEN;
  if ( plaintext_capacity < encrypted_len ) {
    throw CryptoException( "AES-128-GCM plaintext buffer is too small" );
  }
  require_size_fits_evp( associated_data_len, "AES-128-GCM associated data" );
  require_size_fits_evp( encrypted_len, "AES-128-GCM ciphertext" );

  EVP_CIPHER_CTX* context = impl->decrypt_context;
  ERR_clear_error();
  if ( EVP_CIPHER_CTX_reset( context ) != 1
       || EVP_DecryptInit_ex2( context,
                              fips_provider().cipher(),
                              impl->decrypt_key,
                              static_cast<const unsigned char*>( iv ),
                              NULL )
            != 1 ) {
    throw CryptoException( openssl_error( "could not initialize AES-128-GCM decryption" ) );
  }

  int plaintext_len = 0;
  int produced = 0;
  if ( associated_data_len > 0
       && EVP_DecryptUpdate( context,
                             NULL,
                             &produced,
                             static_cast<const unsigned char*>( associated_data ),
                             static_cast<int>( associated_data_len ) )
            != 1 ) {
    throw CryptoException( openssl_error( "AES-128-GCM associated-data authentication failed" ) );
  }
  if ( encrypted_len > 0
       && EVP_DecryptUpdate( context,
                             static_cast<unsigned char*>( plaintext ),
                             &produced,
                             static_cast<const unsigned char*>( ciphertext ),
                             static_cast<int>( encrypted_len ) )
            != 1 ) {
    throw CryptoException( openssl_error( "AES-128-GCM decryption failed" ) );
  }
  plaintext_len += produced;

  const unsigned char* tag = static_cast<const unsigned char*>( ciphertext ) + encrypted_len;
  if ( EVP_CIPHER_CTX_ctrl( context, EVP_CTRL_AEAD_SET_TAG, TAG_LEN, const_cast<unsigned char*>( tag ) ) != 1 ) {
    throw CryptoException( openssl_error( "could not set the AES-128-GCM authentication tag" ) );
  }
  if ( EVP_DecryptFinal_ex( context, static_cast<unsigned char*>( plaintext ) + plaintext_len, &produced ) != 1 ) {
    OPENSSL_cleanse( plaintext, plaintext_capacity );
    ERR_clear_error();
    return -1;
  }
  plaintext_len += produced;
  return plaintext_len == static_cast<int>( encrypted_len ) ? plaintext_len : -1;
}

#else /* !HAVE_OPENSSL_FIPS_CRYPTO */

class Crypto::FipsAes128Gcm::Impl
{};

bool Crypto::fips_crypto_compiled( void ) { return false; }

void Crypto::ensure_fips_crypto_available( void )
{
  throw CryptoException( "--fips-crypto requires a build with OpenSSL 3 libcrypto support" );
}

void Crypto::fill_fips_random( void*, size_t )
{
  ensure_fips_crypto_available();
}

FipsAes128Gcm::FipsAes128Gcm( const unsigned char*, bool ) : impl( NULL )
{
  ensure_fips_crypto_available();
}

FipsAes128Gcm::~FipsAes128Gcm()
{
  delete impl;
}

int FipsAes128Gcm::encrypt( const void*, size_t, const void*, size_t, void*, size_t, void*, size_t )
{
  ensure_fips_crypto_available();
  return -1;
}

int FipsAes128Gcm::decrypt( const void*, size_t, const void*, size_t, const void*, size_t, void*, size_t )
{
  ensure_fips_crypto_available();
  return -1;
}

#endif
