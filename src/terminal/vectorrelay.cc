/*
    Goblin Skiff: the session fonts of the vector service, on the server
    Copyright 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

/* The part of the relay that keeps the fonts a program uploads to the vector
   service (a=v,f=200) as terminal state. An upload is the terminal's to
   accept, so it goes to the terminal and is kept once the terminal says OK;
   a release the relay answers itself, from what it keeps, and the client
   releases the font in its terminal after the frame that may still use it.
   See kitty/fonts/vector.py in Silkie for the requests and their answers. */

#include "src/terminal/extensions.h"
#include "src/terminal/kittygraphics.h"
#include "src/terminal/osc52.h"
#include "src/terminal/terminalframebuffer.h"
#include "src/terminal/vectorfonts.h"

#include <chrono>
#include <cmath>
#include <new>

namespace Terminal {

void ExtensionRelay::service_error( uint32_t id, uint32_t quiet, const std::string& message, std::string& to_host )
{
  /* json.dumps({'error': ...}), with its default separators */
  if ( quiet < 2 ) {
    to_host += kitty_response(
      id, 0, "EINVAL:" + base64_encode_osc52( "{\"error\": " + Vector::json_dumps_string( message ) + "}" ) );
  }
}

bool ExtensionRelay::font_request( const std::string& json,
                                   Framebuffer& fb,
                                   ServiceRequest& request,
                                   std::string& to_host )
{
  Vector::Json doc;
  bool parsed = false;
  try {
    parsed = Vector::json_loads( json, doc );
  } catch ( const std::bad_alloc& ) {
    parsed = false; /* then it goes as it is, and the terminal answers */
  }
  if ( !parsed || doc.root().kind != Vector::Json::Object ) {
    return false; /* the terminal says what is wrong with it */
  }
  const Vector::Json::Node* op = doc.get( doc.root(), "op" );
  const Vector::Json::Node* id = doc.get( doc.root(), "id" );
  if ( !op || op->kind != Vector::Json::String || !id || id->kind != Vector::Json::String ) {
    return false;
  }
  const std::string& name = id->text;
  if ( op->text == "upload" ) {
    if ( !name.empty() ) {
      /* Kept at once, as the terminal will have it before anything the
         program sends after it, under the id the request is about to get. */
      request.font_op = 'u';
      request.font = name;
      request.upload = std::make_shared<const std::string>( json );
      const auto old = fb.get_vector_fonts().find( name );
      if ( old != fb.get_vector_fonts().end() ) {
        request.previous = std::make_shared<const Vector::SessionFont>( old->second );
      }
      fb.commit_vector_font( name, request.upload, service_next, nullptr );
    }
    return false;
  }
  if ( op->text != "release" ) {
    return false;
  }
  bool in_flight = false; /* whether the terminal holds the font depends on its answer */
  for ( const auto& pending : service_requests ) {
    in_flight = in_flight || ( pending.second.font_op && pending.second.font == name );
  }
  const auto& fonts = fb.get_vector_fonts();
  const auto font = fonts.find( name );
  if ( in_flight || font == fonts.end() || font->second.released ) {
    /* The terminal answers, KeyError and all, and the answer is heard. */
    request.font_op = 'r';
    request.font = name;
    return false;
  }
  fb.release_vector_font( name );
  if ( request.quiet == 0 ) {
    to_host += kitty_response(
      request.id, 0, "OK;" + base64_encode_osc52( "{\"id\":" + Vector::json_dumps_string( name ) + "}" ) );
  }
  return true;
}

void ExtensionRelay::font_answer( uint32_t relay_id,
                                  const ServiceRequest& request,
                                  const std::string& body,
                                  bool ok,
                                  Framebuffer& fb )
{
  if ( !ok ) {
    font_refused( relay_id, request, fb );
    return;
  }
  if ( request.font_op == 'r' ) {
    fb.release_vector_font( request.font );
    return;
  }
  if ( request.font_op != 'u' ) {
    return;
  }
  /* The answer describes the font: {"id":..., "axes":[{"tag", "minimum",
     "default", "maximum"}, ...], ...}. Its axes are what a layout may set. */
  auto info = std::make_shared<Vector::FontInfo>();
  std::string decoded;
  Vector::Json doc;
  bool described = false;
  try {
    described = body.compare( 0, 3, "OK;" ) == 0 && base64_decode_osc52( body.substr( 3 ), decoded )
                && Vector::json_loads( decoded, doc ) && doc.root().kind == Vector::Json::Object;
  } catch ( const std::bad_alloc& ) {
    described = false; /* kept without its axes, which the terminal checks */
  }
  const Vector::Json::Node* axes = described ? doc.get( doc.root(), "axes" ) : nullptr;
  described = axes && axes->kind == Vector::Json::Array;
  for ( size_t i = 0; described && i < axes->items.size(); i++ ) {
    const Vector::Json::Node& axis = doc.nodes[axes->items[i]];
    const Vector::Json::Node* tag = doc.get( axis, "tag" );
    const Vector::Json::Node* minimum = doc.get( axis, "minimum" );
    const Vector::Json::Node* maximum = doc.get( axis, "maximum" );
    if ( !tag || tag->kind != Vector::Json::String || !minimum || minimum->kind != Vector::Json::Number || !maximum
         || maximum->kind != Vector::Json::Number ) {
      described = false;
      break;
    }
    info->axes[tag->text] = Vector::FontInfo::Axis { minimum->number, maximum->number };
  }
  const auto font = fb.get_vector_fonts().find( request.font );
  if ( described && font != fb.get_vector_fonts().end() && font->second.request == relay_id ) {
    Vector::SessionFont confirmed = font->second;
    confirmed.info = info;
    fb.put_vector_font( request.font, confirmed );
  }
}

void ExtensionRelay::font_refused( uint32_t relay_id, const ServiceRequest& request, Framebuffer& fb )
{
  if ( request.font_op != 'u' ) {
    return;
  }
  /* The terminal kept what it had, which is what the relay keeps again,
     unless a later upload has taken the name since: then that one falls back
     to it, should it be refused too. */
  const auto font = fb.get_vector_fonts().find( request.font );
  if ( font != fb.get_vector_fonts().end() && font->second.request == relay_id ) {
    if ( request.previous ) {
      fb.put_vector_font( request.font, *request.previous );
    } else {
      fb.erase_vector_font( request.font );
    }
    fb.prune_vector_fonts();
  }
  for ( auto& pending : service_requests ) {
    if ( pending.second.previous && pending.second.font == request.font
         && pending.second.previous->request == relay_id ) {
      pending.second.previous = request.previous;
    }
  }
}

Vector::FontCheck ExtensionRelay::font_check( const Framebuffer& fb ) const
{
  /* An upload on its way may be named already, as the terminal will have it
     by then; until it answers, its axes are the terminal's to check. */
  Vector::FontCheck check;
  for ( const auto& font : fb.get_vector_fonts() ) {
    if ( !font.second.released ) {
      check.fonts[font.first] = font.second.info;
    }
  }
  return check;
}

bool ExtensionRelay::service_overdue( uint64_t ms ) const
{
  const int64_t now
    = std::chrono::duration_cast<std::chrono::milliseconds>( std::chrono::steady_clock::now().time_since_epoch() )
        .count();
  for ( const auto& pending : service_requests ) {
    if ( now - pending.second.sent_ms >= int64_t( ms ) ) {
      return true;
    }
  }
  return false;
}

}
