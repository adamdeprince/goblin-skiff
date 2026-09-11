#!/usr/bin/env perl
use strict;
use warnings;
open my $in, '<:raw', $ARGV[0] or die "$ARGV[0]: $!";
local $/;
my $data = <$in>;
print "/* Generated from goblin.webp; do not edit. */\n";
print "static const unsigned char goblin_webp[] = {\n";
while (length $data) {
    my $chunk = substr($data, 0, 16, '');
    print '  ', join(', ', map { sprintf '0x%02x', $_ } unpack('C*', $chunk)), ",\n";
}
print "};\n";
