/*
    Mosh: the mobile shell
    Copyright 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#ifndef STATESYNC_KITTY_HPP
#define STATESYNC_KITTY_HPP

namespace KittyBuffers {
class StateDelta;
}

namespace Terminal {
class Framebuffer;

bool kitty_state_delta_to_proto( const Framebuffer& existing,
                                 const Framebuffer& current,
                                 KittyBuffers::StateDelta* output,
                                 bool force_placements = false );
void apply_kitty_state_delta( const KittyBuffers::StateDelta& input, Framebuffer& framebuffer );
}

#endif
