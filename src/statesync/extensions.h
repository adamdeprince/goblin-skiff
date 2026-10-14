/*
    Goblin Skiff: Silkie's extensions on the wire
    Copyright 2026

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.
*/

#ifndef STATESYNC_EXTENSIONS_HPP
#define STATESYNC_EXTENSIONS_HPP

#include "src/terminal/extensions.h"

namespace ClientBuffers {
class ClientExtensionsMessage;
}
namespace HostBuffers {
class ExtensionState;
}

namespace Terminal {
void client_extensions_to_proto( const ClientExtensions& info, ClientBuffers::ClientExtensionsMessage* output );
ClientExtensions client_extensions_from_proto( const ClientBuffers::ClientExtensionsMessage& input );
void extension_state_to_proto( const ExtensionModes& modes, HostBuffers::ExtensionState* output );
ExtensionModes extension_state_from_proto( const HostBuffers::ExtensionState& input );
}

#endif
