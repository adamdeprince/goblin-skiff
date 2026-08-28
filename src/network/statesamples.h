/*
    Mosh: the mobile shell
    Copyright 2026

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
*/

#ifndef STATE_SAMPLES_H
#define STATE_SAMPLES_H

#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

namespace Network {
class StateSampleWriter
{
private:
  FILE* file;
  void* zstd_stream;
  std::vector<char> out_buffer;
  size_t min_size;
  bool closed;

  void write_compressed( const void* data, size_t size, int directive );

public:
  StateSampleWriter();
  StateSampleWriter( const std::string& path, size_t s_min_size );
  ~StateSampleWriter();

  void open( const std::string& path, size_t s_min_size );
  void close( void );
  bool enabled( void ) const { return file != NULL; }
  void write_record( const std::string& sample );

  StateSampleWriter( const StateSampleWriter& );
  StateSampleWriter& operator=( const StateSampleWriter& );
};

std::vector<std::string> read_state_sample_file( const std::string& path, size_t max_samples );
std::string train_state_dictionary( const std::vector<std::string>& samples, size_t dictionary_size );
std::string compress_state_dictionary_file( const std::string& dictionary );
std::string read_state_dictionary_file( const std::string& path );
std::string read_file_bytes( const std::string& path );
std::string state_dictionary_id( const std::string& dictionary );
}

#endif
