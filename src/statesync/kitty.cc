/*
    Mosh: the mobile shell
    Copyright 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#include "src/statesync/kitty.h"

#include "src/protobufs/kitty.pb.h"
#include "src/terminal/kittygraphics.h"
#include "src/terminal/terminalframebuffer.h"

#include <map>
#include <memory>
#include <vector>

namespace Terminal {
namespace {
void image_to_proto( const KittyImage& image, KittyBuffers::Image* output )
{
  output->set_id( image.id );
  if ( image.number != 0 ) {
    output->set_number( image.number );
  }
  output->set_width( image.width );
  output->set_height( image.height );
  output->set_webp( image.data ? *image.data : std::string() );
  if ( image.origin == ImageOrigin::Sixel ) { output->set_origin( KittyBuffers::Image::SIXEL ); }
}

void placement_to_proto( const KittyPlacement& placement, KittyBuffers::Placement* output )
{
  output->set_image_id( placement.image_id );
  if ( placement.placement_id != 0 ) {
    output->set_placement_id( placement.placement_id );
  }
  output->set_row( placement.row );
  output->set_col( placement.col );
  if ( placement.columns != 0 ) {
    output->set_columns( placement.columns );
  }
  if ( placement.rows != 0 ) {
    output->set_rows( placement.rows );
  }
  if ( placement.src_x != 0 ) {
    output->set_src_x( placement.src_x );
  }
  if ( placement.src_y != 0 ) {
    output->set_src_y( placement.src_y );
  }
  if ( placement.src_w != 0 ) {
    output->set_src_w( placement.src_w );
  }
  if ( placement.src_h != 0 ) {
    output->set_src_h( placement.src_h );
  }
  if ( placement.cell_x != 0 ) {
    output->set_cell_x( placement.cell_x );
  }
  if ( placement.cell_y != 0 ) {
    output->set_cell_y( placement.cell_y );
  }
  if ( placement.z != 0 ) {
    output->set_z( placement.z );
  }
  if ( placement.cursor_hold ) {
    output->set_cursor_hold( true );
  }
  if ( placement.unicode_placeholder ) {
    output->set_unicode_placeholder( true );
  }
  if ( placement.parent_image != 0 ) {
    output->set_parent_image( placement.parent_image );
  }
  if ( placement.parent_placement != 0 ) {
    output->set_parent_placement( placement.parent_placement );
  }
  if ( placement.H != 0 ) {
    output->set_horizontal_offset( placement.H );
  }
  if ( placement.V != 0 ) {
    output->set_vertical_offset( placement.V );
  }
}

KittyPlacement placement_from_proto( const KittyBuffers::Placement& input )
{
  KittyPlacement placement;
  placement.image_id = input.image_id();
  placement.placement_id = input.placement_id();
  placement.row = input.row();
  placement.col = input.col();
  placement.columns = input.columns();
  placement.rows = input.rows();
  placement.src_x = input.src_x();
  placement.src_y = input.src_y();
  placement.src_w = input.src_w();
  placement.src_h = input.src_h();
  placement.cell_x = input.cell_x();
  placement.cell_y = input.cell_y();
  placement.z = input.z();
  placement.cursor_hold = input.cursor_hold();
  placement.unicode_placeholder = input.unicode_placeholder();
  placement.parent_image = input.parent_image();
  placement.parent_placement = input.parent_placement();
  placement.H = input.horizontal_offset();
  placement.V = input.vertical_offset();
  return placement;
}
}

bool kitty_state_delta_to_proto( const Framebuffer& existing,
                                 const Framebuffer& current,
                                 KittyBuffers::StateDelta* output,
                                 bool force_placements )
{
  bool changed = false;
  bool image_change_invalidates_placement = false;
  const std::map<uint32_t, KittyImage>& old_images = existing.get_kitty_images();
  const std::map<uint32_t, KittyImage>& new_images = current.get_kitty_images();

  for ( std::map<uint32_t, KittyImage>::const_iterator old = old_images.begin(); old != old_images.end(); ++old ) {
    const std::map<uint32_t, KittyImage>::const_iterator now = new_images.find( old->first );
    if ( now == new_images.end() || !( old->second == now->second ) ) {
      output->add_delete_image_id( old->first );
      changed = true;
      if ( now != new_images.end() ) {
        const std::vector<KittyPlacement>& placements = current.get_kitty_placements();
        for ( size_t i = 0; i < placements.size(); i++ ) {
          if ( placements[i].image_id == old->first ) {
            image_change_invalidates_placement = true;
            break;
          }
        }
      }
    }
  }

  for ( std::map<uint32_t, KittyImage>::const_iterator now = new_images.begin(); now != new_images.end(); ++now ) {
    const std::map<uint32_t, KittyImage>::const_iterator old = old_images.find( now->first );
    if ( old == old_images.end() || !( old->second == now->second ) ) {
      image_to_proto( now->second, output->add_image() );
      changed = true;
    }
  }

  if ( force_placements || image_change_invalidates_placement
       || existing.get_kitty_placements() != current.get_kitty_placements() ) {
    output->set_replace_placements( true );
    const std::vector<KittyPlacement>& placements = current.get_kitty_placements();
    for ( size_t i = 0; i < placements.size(); i++ ) {
      placement_to_proto( placements[i], output->add_placement() );
    }
    changed = true;
  }

  return changed;
}

void apply_kitty_state_delta( const KittyBuffers::StateDelta& input, Framebuffer& framebuffer )
{
  for ( int i = 0; i < input.delete_image_id_size(); i++ ) {
    framebuffer.erase_kitty_image( input.delete_image_id( i ) );
  }

  for ( int i = 0; i < input.image_size(); i++ ) {
    const KittyBuffers::Image& source = input.image( i );
    uint32_t width = 0, height = 0;
    if ( source.id() == 0 || !kitty_webp_dimensions( source.webp(), width, height ) || width != source.width()
         || height != source.height() ) {
      continue;
    }
    KittyImage image;
    image.id = source.id();
    image.number = source.number();
    image.format = KITTY_FORMAT_WEBP;
    image.width = width;
    image.height = height;
    image.origin = source.origin() == KittyBuffers::Image::SIXEL ? ImageOrigin::Sixel : ImageOrigin::Kitty;
    image.data = std::make_shared<std::string>( source.webp() );
    framebuffer.put_kitty_image( image );
  }

  if ( input.replace_placements() ) {
    std::vector<KittyPlacement> placements;
    placements.reserve( input.placement_size() );
    for ( int i = 0; i < input.placement_size(); i++ ) {
      const KittyPlacement placement = placement_from_proto( input.placement( i ) );
      if ( placement.image_id != 0 && framebuffer.find_kitty_image( placement.image_id ) != NULL ) {
        placements.push_back( placement );
      }
    }
    framebuffer.replace_kitty_placements( placements );
  }
}
}
