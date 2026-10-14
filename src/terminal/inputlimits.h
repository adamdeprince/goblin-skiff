/* Optional bounds on terminal input. Zero means no limit.
   The same names are csi_limit, escape_limit and vector_depth_limit in
   Silkie and csi-limit, escape-limit in Goblin View. */
#ifndef MOSH_INPUTLIMITS_H
#define MOSH_INPUTLIMITS_H

#include <cstddef>

namespace Terminal
{
extern size_t csi_byte_limit;
extern size_t escape_byte_limit;
extern unsigned vector_depth_limit;

/* Reads MOSH_CSI_LIMIT, MOSH_ESCAPE_LIMIT and MOSH_VECTOR_DEPTH_LIMIT.
   An empty or unset variable leaves the current value. A value that is
   not a non-negative integer is ignored. */
void load_input_limits_from_env( void );
}

#endif
