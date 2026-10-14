/*
    Mosh: the mobile shell
    Copyright 2012 Keith Winstein

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

    In addition, as a special exception, the copyright holders give
    permission to link the code of portions of this program with the
    OpenSSL library under certain conditions as described in each
    individual source file, and distribute linked combinations including
    the two.

    You must obey the GNU General Public License in all respects for all
    of the code used other than OpenSSL. If you modify file(s) with this
    exception, you may extend this exception to your version of the
    file(s), but you are not obligated to do so. If you do not wish to do
    so, delete this exception statement from your version. If you delete
    this exception statement from all source files in the program, then
    also delete it here.
*/

#ifndef TRANSPORT_FRAGMENT_HPP
#define TRANSPORT_FRAGMENT_HPP

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "src/protobufs/transportinstruction.pb.h"

namespace Network {
class StateSampleWriter;

using namespace TransportBuffers;

class Fragment
{
public:
  static const size_t frag_header_len = sizeof( uint64_t ) + sizeof( uint16_t );
  static const size_t large_frag_header_len = 2 * sizeof( uint64_t );

  uint64_t id;
  uint64_t fragment_num;
  bool final;
  bool large;

  bool initialized;

  std::string contents;

  Fragment() : id( -1 ), fragment_num( -1 ), final( false ), large( false ), initialized( false ), contents() {}

  Fragment( uint64_t s_id, uint64_t s_fragment_num, bool s_final, const std::string& s_contents, bool s_large = false )
    : id( s_id ), fragment_num( s_fragment_num ), final( s_final ), large( s_large ), initialized( true ), contents( s_contents )
  {}

  Fragment( const std::string& x );

  std::string tostring( void );

  bool operator==( const Fragment& x ) const;
};

class FragmentAssembly
{
private:
  std::map<uint64_t, Fragment> fragments;
  uint64_t current_id;
  uint64_t fragments_total;

public:
  FragmentAssembly() : fragments(), current_id( -1 ), fragments_total( 0 ) {}
  bool add_fragment( Fragment& inst );
  Instruction get_assembly( StateSampleWriter* sample_writer = NULL );
};

class Fragmenter
{
private:
  uint64_t next_instruction_id;
  Instruction last_instruction;
  size_t last_MTU;
  bool last_allow_zstd;
  bool last_allow_zstd_dictionary;
  bool last_allow_large_fragments = false;
  std::string last_zstd_dictionary_id;

public:
  Fragmenter()
    : next_instruction_id( 0 ), last_instruction(), last_MTU( -1 ), last_allow_zstd( false ),
      last_allow_zstd_dictionary( false ), last_zstd_dictionary_id()
  {
    last_instruction.set_old_num( -1 );
    last_instruction.set_new_num( -1 );
  }
  std::vector<Fragment> make_fragments( const Instruction& inst,
                                        size_t MTU,
                                        bool allow_zstd = false,
                                        bool allow_zstd_dictionary = false,
                                        const std::string& zstd_dictionary_id = "",
                                        bool allow_large_fragments = false );
  uint64_t last_ack_sent( void ) const { return last_instruction.ack_num(); }
};

}

#endif
