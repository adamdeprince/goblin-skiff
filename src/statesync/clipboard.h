/*
    Mosh: the mobile shell
    Copyright 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#ifndef CLIPBOARD_HPP
#define CLIPBOARD_HPP

#include "src/terminal/osc52.h"

namespace ClipboardBuffers {
class ClipboardEvent;
}

namespace Terminal {
static const unsigned CLIPBOARD_ZSTD_LEVEL = 22;

void clipboard_event_to_proto( ClipboardBuffers::ClipboardEvent* proto, const ClipboardEvent& event );
ClipboardEvent clipboard_event_from_proto( const ClipboardBuffers::ClipboardEvent& proto );
bool clipboard_should_transmit( const ClipboardEvent& event );
}

#endif
