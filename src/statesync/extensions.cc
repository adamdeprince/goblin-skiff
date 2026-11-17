/*
    Goblin Skiff: Silkie's extensions on the wire
    Copyright 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#include "src/statesync/extensions.h"

#include <set>

#include "src/protobufs/hostinput.pb.h"
#include "src/protobufs/userinput.pb.h"

namespace Terminal {

void client_extensions_to_proto( const ClientExtensions& info, ClientBuffers::ClientExtensionsMessage* output )
{
  output->set_listing( info.listing );
  std::set<std::string> names;
  for ( const auto& item : info.versions ) {
    names.insert( item.first );
  }
  for ( const auto& item : info.caps ) {
    names.insert( item.first );
  }
  for ( const auto& item : info.grants ) {
    names.insert( item.first );
  }
  for ( const auto& name : names ) {
    ClientBuffers::ClientExtensionSelector* selector = output->add_selector();
    selector->set_name( name );
    const auto version = info.versions.find( name );
    if ( version != info.versions.end() ) {
      selector->set_version( version->second );
    }
    const auto caps = info.caps.find( name );
    if ( caps != info.caps.end() ) {
      selector->set_caps( caps->second.text() );
    }
    const auto grant = info.grants.find( name );
    if ( grant != info.grants.end() ) {
      selector->set_grant( grant->second );
    }
  }
  output->set_media( info.media );
  output->set_format( info.format );
  output->set_display( info.display.text() );
  output->set_image_gamut( info.image_gamut );
  output->set_pointer_shapes( info.pointer_shapes );
  output->set_graphics( info.graphics );
  output->set_vector_service( info.vector_service );
  if ( !info.notifications.empty() ) {
    output->set_notifications( info.notifications );
  }
  if ( info.notification_routes ) {
    output->set_notification_routes( true );
  }
}

ClientExtensions client_extensions_from_proto( const ClientBuffers::ClientExtensionsMessage& input )
{
  ClientExtensions info;
  info.listing = input.listing();
  for ( const auto& selector : input.selector() ) {
    if ( selector.has_version() ) {
      info.versions[selector.name()] = selector.version();
    }
    Ext::Fields caps;
    if ( selector.has_caps() && Ext::parse_fields( selector.caps(), caps ) ) {
      info.caps[selector.name()] = caps;
    }
    if ( selector.has_grant() ) {
      info.grants[selector.name()] = selector.grant();
    }
  }
  info.media = input.media();
  info.format = input.format();
  Ext::parse_fields( input.display(), info.display );
  info.image_gamut = input.image_gamut();
  info.pointer_shapes = input.pointer_shapes();
  info.graphics = input.graphics();
  info.vector_service = input.vector_service();
  info.notifications = input.notifications();
  info.notification_routes = input.notification_routes();
  return info;
}

void extension_state_to_proto( const ExtensionModes& modes, HostBuffers::ExtensionState* output )
{
  output->set_tablet( modes.tablet );
  output->set_gesture( modes.gesture );
  output->set_scroll( modes.scroll );
  output->set_frame( modes.frame );
  output->set_latency( modes.latency );
  output->set_display( modes.display );
  output->set_ink( modes.ink );
  if ( modes.brush.present() ) {
    HostBuffers::InkBrush* brush = output->mutable_brush();
    brush->set_image( modes.brush.image );
    brush->set_group( modes.brush.group );
    brush->set_next( modes.brush.next );
    brush->set_remaining( modes.brush.remaining );
    brush->set_tools( modes.brush.tools );
    brush->set_w0( modes.brush.w0 );
    brush->set_w1( modes.brush.w1 );
    brush->set_exponent( modes.brush.exponent );
    brush->set_fill( modes.brush.fill );
  }
  output->set_brush_gen( modes.brush_gen );
  output->set_frame_output( modes.frame_output );
  for ( const auto& shape : modes.pointer[0] ) {
    output->add_pointer_main( shape );
  }
  for ( const auto& shape : modes.pointer[1] ) {
    output->add_pointer_alternate( shape );
  }
  output->set_alternate( modes.alternate );
  for ( const auto& item : modes.pad_labels ) {
    HostBuffers::PadLabel* label = output->add_pad_label();
    label->set_device( std::get<0>( item.first ) );
    label->set_control( std::get<1>( item.first ) );
    label->set_index( std::get<2>( item.first ) );
    label->set_text( item.second );
  }
}

ExtensionModes extension_state_from_proto( const HostBuffers::ExtensionState& input )
{
  ExtensionModes modes;
  modes.tablet = input.tablet();
  modes.gesture = input.gesture();
  modes.scroll = input.scroll();
  modes.frame = input.frame();
  modes.latency = input.latency();
  modes.display = input.display();
  modes.ink = input.ink();
  if ( input.has_brush() ) {
    const HostBuffers::InkBrush& brush = input.brush();
    modes.brush.image = brush.image();
    modes.brush.group = brush.group();
    modes.brush.next = brush.next();
    modes.brush.remaining = brush.remaining();
    modes.brush.tools = brush.tools();
    modes.brush.w0 = brush.w0();
    modes.brush.w1 = brush.w1();
    modes.brush.exponent = brush.exponent();
    modes.brush.fill = brush.fill();
  }
  modes.brush_gen = input.brush_gen();
  modes.frame_output = input.frame_output();
  modes.pointer[0].assign( input.pointer_main().begin(), input.pointer_main().end() );
  modes.pointer[1].assign( input.pointer_alternate().begin(), input.pointer_alternate().end() );
  modes.alternate = input.alternate();
  for ( const auto& label : input.pad_label() ) {
    /* Only what a program could have given: four kinds of control, 0 to 255. */
    if ( label.control() < 4 && label.index() <= 255 && !label.text().empty() ) {
      modes.pad_labels[PadControl( label.device(), label.control(), label.index() )] = label.text();
    }
  }
  return modes;
}

}
