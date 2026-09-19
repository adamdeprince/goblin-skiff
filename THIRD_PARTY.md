# Licenses and corresponding source

Goblin Skiff is derived from Mosh and remains GPL version 3 or later. See
[COPYING](COPYING) and individual source-file notices. Existing copyright
notices and linking exceptions must be retained. This is not an Apache-2.0-only
distribution.

Project source: <https://github.com/adamdeprince/goblin-skiff>

## FEC distribution policy

Prebuilt Debian/Ubuntu packages, RPMs and Homebrew bottles use the bundled
classical Reed-Solomon implementation. RaptorQ/RFC 6330 is excluded, not merely
disabled by a runtime default. Release recipes pass `--without-libraptorq` and
run `packaging/check-prebuilt-fec.sh` against the built executable; installed
package verification repeats that check. The build-time policy gate also runs
when optional package tests are skipped.

RaptorQ has an [IETF patent disclosure](https://datatracker.ietf.org/ipr/2554/)
with conditional licensing/non-assertion terms. A source-code license does not
by itself resolve those patent conditions. This exclusion is a distribution
policy, not a claim of comprehensive patent clearance for all shipped code.

RaptorQ is available only through explicit custom compilation with
`./configure --with-libraptorq=DIR` (or `--with-libraptorq` for installed
headers). Automatic detection is disabled, and the old `=check` mode is
rejected. Review the applicable patent terms before enabling it on either
endpoint. Do not publish such a build as an official prebuilt artifact.

## File synchronization dependency

The optional file-menu synchronizer dynamically links **librsync** (tested
with 2.3.4), licensed LGPL-2.1-or-later. It is a system dependency, not vendored
source. Preserve its upstream notices and corresponding-source/relinking
materials when distributing a binary with that library, following the release
checklist below. Upstream: <https://github.com/librsync/librsync/tree/v2.3.4>.
The synchronizer also uses the system OpenSSL 3 library for SHA-256; its
Apache-2.0 license and notices remain applicable. No MD4 signatures are used:
librsync's BLAKE2 block hashes identify matching data, while SHA-256 verifies
the final file. This is not a claim that librsync is a FIPS-validated module.

## Audio codec review

The codecs below are requested dependencies, **not yet vendored or linked by
this tree**. A startup notice must not be used to claim an unfinished source or
license audit is complete.

| Codec | Candidate revision | Upstream license |
| --- | --- | --- |
| Codec 2 1.2.0 | `06d4c11e699b0351765f10398abb4f663a984f36` | LGPL-2.1; preserve upstream notices |
| Opus 1.6.1 | `22244de5a79bd1d6d623c32e72bf1954b56235be` | BSD-style license; retain COPYING and patent grants |
| LPCNet | `7dc99429e246cca614708b5aa4990d827bacce2c` | BSD-3-Clause code; model provenance still to verify |
| Lyra 1.3.2 (v2 codec) | `47698dadf0010abff6a848e02642f55f806d4842` | Apache-2.0 code; audit model and transitive dependencies |

Upstream references:

- [Codec 2 license](https://github.com/drowe67/codec2/blob/main/COPYING)
- [Opus COPYING](https://github.com/xiph/opus/blob/main/COPYING)
- [LPCNet COPYING](https://github.com/xiph/LPCNet/blob/master/COPYING)
- [Lyra LICENSE](https://github.com/google/lyra/blob/main/LICENSE)

LGPL-2.1 is compatible with using Codec 2 as a library in GPLv3 Goblin Skiff. This is
not the GPLv2-only versus GPLv3 incompatibility. LGPL-2.1 section 3 also permits
applying GPL version 2 or a later GPL version to a copy; no such notice rewrite
is necessary for the planned separately maintained library dependency. See
the [FSF compatibility table](https://www.gnu.org/licenses/gpl-faq.en.html#AllCompatibility).

The packaging plan retains LGPL notices and meets source/relinking requirements.
Keep upstream codec license files; do not relabel everything Apache-2.0 or use
the startup line as a substitute for those files.

## Release checklist

Before distributing binaries containing vendored codecs:

1. Pin each source revision and model/dependency revision. Record archive
   hashes, local modifications, build options and model formats.
2. Include the actual upstream license, copyright and applicable patent/NOTICE
   files for everything shipped; audit the selected revision, not just upstream
   HEAD's repository badge. Preserve notices in individual files.
3. Publish complete matching source, local patches, build scripts and necessary
   model/build data for that binary. A moving branch or an upstream homepage
   alone is not the corresponding source for a patched binary.
4. When distributing an LGPL library, provide its corresponding source through
   a license-permitted mechanism. For static linking, provide the source and/or
   object materials and build instructions needed to modify the library and
   relink the executable. A suitable replaceable shared-library arrangement is
   another option; shipping that library still entails its source obligations.
5. Make the short startup source/license URL lead to this information and the
   exact release's materials. Include local copies of the licenses in binary
   packages; the URL is a convenience, not the entire compliance mechanism.

See [LGPL-2.1 section 6](https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html#SEC6)
and the [FSF linking guidance](https://www.gnu.org/licenses/gpl-faq.en.html#LGPLStaticVsDynamic).
Repository packages provide their matching source alongside the binaries at
<https://apt.goblinreactor.com/>. Enable the matching release's `deb-src`
entry and use `apt-get source goblin-skiff` to retrieve it. A workspace build
can differ from a published package; a moving branch alone is not a matching
source release. The unimplemented codec plan above is not a completed audit.
