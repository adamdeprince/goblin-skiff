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
#include <set>
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
  if ( image.format == KITTY_FORMAT_VECTOR ) {
    output->set_vector( kitty_vector_records( image ) );
    output->set_vector_lineage( image.vector ? image.vector->lineage : 0 );
    output->set_vector_version( image.vector ? image.vector->version : 0 );
  } else if ( image.format == KITTY_FORMAT_HDR ) {
    output->set_hdr( image.data ? *image.data : std::string() );
    if ( image.tone_map ) {
      output->set_tone_map( true );
    }
  } else if ( image.format == KITTY_FORMAT_DJVU ) {
    output->set_djvu( image.data ? *image.data : std::string() );
    output->set_palette( image.palette );
  } else if ( image.format == KITTY_FORMAT_PNG ) {
    output->set_png( image.data ? *image.data : std::string() );
    if ( image.png_original ) {
      output->set_png_original( true );
    }
  } else {
    output->set_webp( image.data ? *image.data : std::string() );
  }
  if ( image.origin == ImageOrigin::Sixel ) { output->set_origin( KittyBuffers::Image::SIXEL ); }
  if ( image.gamut ) {
    output->set_gamut( image.gamut );
  }
}

/* A vector image the receiver has an older version of, of the same
   transmission: it is sent the steps it lacks. */
bool vector_patch( const KittyImage& before, const KittyImage& after, KittyBuffers::VectorPatch* output )
{
  std::vector<Vector::Step> steps;
  if ( before.format != KITTY_FORMAT_VECTOR || after.format != KITTY_FORMAT_VECTOR || !before.vector || !after.vector
       || before.serial != after.serial || before.width != after.width || before.height != after.height
       || !Vector::steps_between( before.vector->lineage, before.vector->version, after.vector, steps ) ) {
    return false;
  }
  output->set_image_id( after.id );
  output->set_lineage( before.vector->lineage );
  output->set_base_version( before.vector->version );
  for ( const auto& step : steps ) {
    KittyBuffers::VectorStep* out = output->add_step();
    out->set_records( step.records );
    if ( step.terminal_applied ) {
      out->set_terminal_applied( true );
    }
  }
  return true;
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

/* A raster image as the wire has it: WebP, palette DjVu or PNG. */
bool image_from_proto( const KittyBuffers::Image& source, KittyImage& image )
{
  if ( int( source.has_webp() ) + int( source.has_djvu() ) + int( source.has_png() ) != 1
       || source.has_palette() != source.has_djvu() || source.palette().size() > GRAPHICS_MAX_PALETTE * 4 ) {
    return false;
  }
  image.id = source.id();
  image.number = source.number();
  image.format = source.has_djvu() ? KITTY_FORMAT_DJVU : source.has_png() ? KITTY_FORMAT_PNG : KITTY_FORMAT_WEBP;
  image.width = source.width();
  image.height = source.height();
  image.origin = source.origin() == KittyBuffers::Image::SIXEL ? ImageOrigin::Sixel : ImageOrigin::Kitty;
  image.data = std::make_shared<std::string>(
    source.has_djvu() ? source.djvu() : source.has_png() ? source.png() : source.webp() );
  image.palette = source.palette();
  image.gamut = source.gamut();
  image.png_original = source.has_png() && source.png_original();
  return true;
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

  std::set<uint32_t> patched;
  for ( std::map<uint32_t, KittyImage>::const_iterator old = old_images.begin(); old != old_images.end(); ++old ) {
    const std::map<uint32_t, KittyImage>::const_iterator now = new_images.find( old->first );
    KittyBuffers::VectorPatch patch;
    if ( now != new_images.end() && !( old->second == now->second ) && vector_patch( old->second, now->second, &patch ) ) {
      output->add_vector_patch()->Swap( &patch );
      patched.insert( old->first );
      changed = true;
      continue;
    }
    /* Only a rectangle changed since the version the receiver has. */
    const ImageDelta* delta = now != new_images.end() ? now->second.delta.get() : nullptr;
    if ( delta && delta->base_serial == old->second.serial && old->second.width == now->second.width
         && old->second.height == now->second.height && now->second.origin == ImageOrigin::Kitty ) {
      KittyBuffers::ImagePatch* image_patch = output->add_image_patch();
      image_patch->set_image_id( now->first );
      image_patch->set_x( delta->x );
      image_patch->set_y( delta->y );
      image_to_proto( delta->rect, image_patch->mutable_rect() );
      patched.insert( old->first );
      changed = true;
      continue;
    }
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
    if ( patched.count( now->first ) ) {
      continue;
    }
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

  for ( const auto& patch : input.image_patch() ) {
    /* The rectangle is written over the image the receiver has, which is
       kept as plain RGBA from then on. */
    const KittyImage* base = framebuffer.find_kitty_image( patch.image_id() );
    KittyImage rect;
    std::string pixels, rect_pixels;
    if ( !base || !image_from_proto( patch.rect(), rect ) || !kitty_image_to_rgba( *base, pixels )
         || !kitty_image_to_rgba( rect, rect_pixels ) || uint64_t( patch.x() ) + rect.width > base->width
         || uint64_t( patch.y() ) + rect.height > base->height ) {
      continue;
    }
    for ( uint32_t row = 0; row < rect.height; row++ ) {
      pixels.replace( ( size_t( patch.y() + row ) * base->width + patch.x() ) * 4, size_t( rect.width ) * 4,
                      rect_pixels, size_t( row ) * rect.width * 4, size_t( rect.width ) * 4 );
    }
    KittyImage image = *base;
    image.format = KITTY_FORMAT_RAW;
    image.palette.clear();
    image.data = std::make_shared<std::string>( std::move( pixels ) );
    auto delta = std::make_shared<ImageDelta>();
    delta->base_serial = base->serial;
    delta->x = patch.x();
    delta->y = patch.y();
    delta->rect.format = KITTY_FORMAT_RAW;
    delta->rect.width = rect.width;
    delta->rect.height = rect.height;
    delta->rect.data = std::make_shared<std::string>( std::move( rect_pixels ) );
    image.delta = delta;
    framebuffer.refresh_kitty_image( image );
  }

  for ( int i = 0; i < input.image_size(); i++ ) {
    const KittyBuffers::Image& source = input.image( i );
    if ( source.id() != 0 && ( source.has_vector() || source.has_hdr() ) ) {
      KittyImage image;
      image.id = source.id();
      image.number = source.number();
      image.width = source.width();
      image.height = source.height();
      if ( source.has_vector() ) {
        std::vector<Vector::Record> records;
        std::string error;
        auto table = std::make_shared<Vector::Table>();
        if ( !Vector::parse_records( source.vector(), records, error ) ) {
          continue;
        }
        table->apply( records );
        image.format = KITTY_FORMAT_VECTOR;
        image.vector = Vector::make_doc( source.vector_lineage(), source.vector_version(), image.width,
                                         image.height, table );
      } else {
        image.format = KITTY_FORMAT_HDR;
        image.data = std::make_shared<std::string>( source.hdr() );
        image.tone_map = source.tone_map();
      }
      framebuffer.put_kitty_image( image );
      continue;
    }
    KittyImage image;
    std::string rgba;
    if ( source.id() == 0 || !image_from_proto( source, image ) || !kitty_image_to_rgba( image, rgba ) ) {
      continue;
    }
    framebuffer.put_kitty_image( image );
  }

  for ( const auto& patch : input.vector_patch() ) {
    const KittyImage* image = framebuffer.find_kitty_image( patch.image_id() );
    if ( !image || image->format != KITTY_FORMAT_VECTOR || !image->vector
         || image->vector->lineage != patch.lineage() || image->vector->version != patch.base_version() ) {
      continue; /* not the version the sender meant */
    }
    std::shared_ptr<const Vector::Doc> doc = image->vector;
    for ( const auto& source : patch.step() ) {
      Vector::Step step;
      step.records = source.records();
      step.terminal_applied = source.terminal_applied();
      std::string error;
      doc = Vector::edit_doc( doc, step, error );
    }
    framebuffer.replace_vector_doc( patch.image_id(), doc );
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
