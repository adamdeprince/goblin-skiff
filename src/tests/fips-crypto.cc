/*
    Mosh: the mobile shell

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#include <cstdio>
#include <cstdlib>
#include <string>

#include "src/crypto/crypto.h"

using namespace Crypto;

static bool rejects( Session& session, std::string packet, size_t position )
{
  packet[position] ^= 1;
  try {
    session.decrypt( packet );
  } catch ( const CryptoException& error ) {
    return !error.fatal;
  }
  return false;
}

int main( void )
{
  try {
    ensure_mode_available( Mode::FipsAES128GCM );
  } catch ( const CryptoException& error ) {
    fprintf( stderr, "SKIP: %s\n", error.what() );
    return 77;
  }

  try {
    Base64Key key( "AAAAAAAAAAAAAAAAAAAAAA" );
    Session client( key, Mode::FipsAES128GCM, Endpoint::Client );
    Session server( key, Mode::FipsAES128GCM, Endpoint::Server );

    const std::string client_text( "client to server" );
    const std::string first = client.encrypt( Message( Nonce( 7 ), client_text ) );
    const std::string second = client.encrypt( Message( Nonce( 7 ), client_text ) );
    if ( first.size() != client_text.size() + 36 || first == second ) {
      return EXIT_FAILURE;
    }

    const Message at_server = server.decrypt( first );
    const bool rejects_tampered_iv = rejects( server, first, 0 );
    const bool rejects_tampered_sequence = rejects( server, first, 12 );
    const bool rejects_tampered_ciphertext = rejects( server, first, first.size() / 2 );
    const bool rejects_tampered_tag = rejects( server, first, first.size() - 1 );
    if ( at_server.nonce.val() != 7 || at_server.text != client_text || !rejects_tampered_iv
         || !rejects_tampered_sequence || !rejects_tampered_ciphertext || !rejects_tampered_tag ) {
      return EXIT_FAILURE;
    }

    const std::string server_text( "server to client" );
    const std::string reply = server.encrypt( Message( Nonce( uint64_t( 1 ) << 63 ), server_text ) );
    const Message at_client = client.decrypt( reply );
    if ( at_client.nonce.val() != ( uint64_t( 1 ) << 63 ) || at_client.text != server_text ) {
      return EXIT_FAILURE;
    }

    /* AAD-only packets authenticate the sequence and tag without introducing
       payload bytes. Exercise empty packets before and after ordinary data,
       and verify that a failed authentication does not poison the context. */
    for ( const size_t size : { size_t( 0 ), size_t( 1 ), size_t( 16 ), size_t( 1200 ), size_t( 0 ) } ) {
      const std::string text( size, 'x' );
      const auto request = client.encrypt( Message( Nonce( 9 + size ), text ) );
      if ( request.size() != size + 36 || !rejects( server, request, 0 )
           || !rejects( server, request, 12 ) || !rejects( server, request, request.size() - 1 )
           || server.decrypt( request ).text != text ) {
        return EXIT_FAILURE;
      }
      const auto response = server.encrypt( Message( Nonce( ( uint64_t( 1 ) << 63 ) | ( 9 + size ) ), text ) );
      if ( response.size() != size + 36 || !rejects( client, response, response.size() - 1 )
           || client.decrypt( response ).text != text ) {
        return EXIT_FAILURE;
      }
    }

    /* Directional traffic keys prevent a peer from accepting its own packet. */
    try {
      client.decrypt( first );
      return EXIT_FAILURE;
    } catch ( const CryptoException& error ) {
      if ( error.fatal ) {
        return EXIT_FAILURE;
      }
    }
  } catch ( const CryptoException& error ) {
    fprintf( stderr, "Crypto exception: %s\n", error.what() );
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
