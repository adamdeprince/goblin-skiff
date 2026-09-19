EXTRA_DIST = \
	hold-stdin print-exitstatus \
	e2e-test e2e-test-server \
	e2e-test-subrs \
	mosh-client mosh-server \
	local.test \
	$(displaytests) \
	emulation-attributes.test

AM_CXXFLAGS = -I$(top_srcdir)/ $(WARNING_CXXFLAGS) $(PICKY_CXXFLAGS) $(HARDEN_CFLAGS) $(MISC_CXXFLAGS) $(CODE_COVERAGE_CXXFLAGS)
AM_LDFLAGS  = $(HARDEN_LDFLAGS) $(CODE_COVERAGE_LIBS)

displaytests = \
	e2e-success.test \
	e2e-failure.test \
	emulation-ascii-iso-8859.test \
	emulation-80th-column.test \
	emulation-attributes-vt100.test \
	emulation-attributes-16color.test \
	emulation-attributes-256color8.test \
	emulation-attributes-256color248.test \
	emulation-attributes-truecolor.test \
	emulation-attributes-bce.test \
	emulation-attributes-osc8.test \
	emulation-back-tab.test \
	emulation-cursor-motion.test \
	emulation-multiline-scroll.test \
	emulation-scroll.test \
	emulation-wrap-across-frames.test \
	network-no-diff.test \
	prediction-unicode.test \
	pty-deadlock.test \
	repeat.test \
	repeat-with-input.test \
	server-network-timeout.test \
	server-signal-timeout.test \
	window-resize.test \
	unicode-combine-fallback-assert.test \
	unicode-later-combining.test \
	window-resize.test

check_PROGRAMS = ocb-aes encrypt-decrypt fips-crypto base64 nonce-incr inpty is-utf8-locale fec-codecs bulk-datagram skiffcp-protocol bulk-loss-sim transport-compression state-samples osc52-parse kitty-graphics terminal-geometry terminal-display tmux-control
TESTS = ocb-aes encrypt-decrypt fips-crypto base64 nonce-incr fec-codecs bulk-datagram skiffcp-protocol bulk-loss-sim transport-compression state-samples osc52-parse kitty-graphics terminal-geometry terminal-display tmux-control tmux-control-integration.test local.test $(displaytests)
XFAIL_TESTS = \
	e2e-failure.test \
	emulation-attributes-256color8.test

base64_vector.cc: $(srcdir)/genbase64.pl
	$(AM_V_GEN)echo '#include "base64_vector.h"' > base64_vector.cc || rm base64_vector.cc
	$(AM_V_GEN)perl $(srcdir)/genbase64.pl >> base64_vector.cc || rm base64_vector.cc

ocb_aes_SOURCES = ocb-aes.cc test_utils.cc test_utils.h
ocb_aes_CPPFLAGS = -I$(srcdir)/../crypto -I$(srcdir)/../util $(CRYPTO_CFLAGS)
ocb_aes_LDADD = ../crypto/libmoshcrypto.a ../util/libmoshutil.a $(CRYPTO_LIBS)

encrypt_decrypt_SOURCES = encrypt-decrypt.cc test_utils.cc test_utils.h
encrypt_decrypt_CPPFLAGS = -I$(srcdir)/../crypto -I$(srcdir)/../util
encrypt_decrypt_LDADD = ../crypto/libmoshcrypto.a ../util/libmoshutil.a $(CRYPTO_LIBS)

fips_crypto_SOURCES = fips-crypto.cc
fips_crypto_CPPFLAGS = -I$(top_srcdir) $(CRYPTO_CFLAGS)
fips_crypto_LDADD = ../crypto/libmoshcrypto.a ../util/libmoshutil.a $(CRYPTO_LIBS)

base64_SOURCES = base64.cc base64_vector.cc base64_vector.h genbase64.pl
base64_CPPFLAGS = $(ocb_aes_CPPFLAGS)
base64_LDADD = $(ocb_aes_LDADD)

nonce_incr_SOURCES = nonce-incr.cc
nonce_incr_CPPFLAGS = -I$(srcdir)/../network -I$(srcdir)/../crypto -I$(srcdir)/../util $(CRYPTO_CFLAGS)
nonce_incr_LDADD = ../network/libmoshnetwork.a ../crypto/libmoshcrypto.a ../util/libmoshutil.a $(CRYPTO_LIBS)

inpty_SOURCES = inpty.cc
inpty_CPPFLAGS = -I$(srcdir)/../util
inpty_LDADD = ../util/libmoshutil.a

is_utf8_locale_SOURCES = is-utf8-locale.cc
is_utf8_locale_CPPFLAGS = -I$(srcdir)/../util
is_utf8_locale_LDADD = ../util/libmoshutil.a $(LIBUTIL)

fec_codecs_SOURCES = fec-codecs.cc
fec_codecs_CPPFLAGS = -I$(top_srcdir) -I$(srcdir)/../fec
fec_codecs_LDADD = ../fec/libmoshfec.a

bulk_datagram_SOURCES = bulk-datagram.cc
bulk_datagram_CPPFLAGS = -I$(top_srcdir) -I$(srcdir)/../network
bulk_datagram_LDADD = ../network/libmoshnetwork.a ../crypto/libmoshcrypto.a ../util/libmoshutil.a $(CRYPTO_LIBS)

skiffcp_protocol_SOURCES = moshcp-protocol.cc
skiffcp_protocol_CPPFLAGS = -I$(top_srcdir) -I$(srcdir)/../moshcp -I$(srcdir)/../fec
skiffcp_protocol_LDADD = ../moshcp/libmoshcp.a ../fec/libmoshfec.a

bulk_loss_sim_SOURCES = bulk-loss-sim.cc
bulk_loss_sim_CPPFLAGS = -I$(top_srcdir) -I$(srcdir)/../network -I$(srcdir)/../fec
bulk_loss_sim_LDADD = ../fec/libmoshfec.a ../network/libmoshnetwork.a ../crypto/libmoshcrypto.a ../util/libmoshutil.a $(CRYPTO_LIBS)

transport_compression_SOURCES = transport-compression.cc
transport_compression_CPPFLAGS = -I$(top_srcdir) -I$(srcdir)/../network $(protobuf_CFLAGS) $(ZSTD_CFLAGS)
transport_compression_LDADD = ../network/libmoshnetwork.a ../protobufs/libmoshprotos.a ../crypto/libmoshcrypto.a ../util/libmoshutil.a $(protobuf_LIBS) $(CRYPTO_LIBS) $(ZSTD_LIBS)

check_PROGRAMS += session-version
TESTS += session-version
session_version_SOURCES = session-version.cc
session_version_CPPFLAGS = -I$(top_builddir) $(transport_compression_CPPFLAGS)
session_version_LDADD = $(transport_compression_LDADD)

state_samples_SOURCES = state-samples.cc
state_samples_CPPFLAGS = -I$(top_srcdir) -I$(srcdir)/../network $(protobuf_CFLAGS)
state_samples_LDADD = ../network/libmoshnetwork.a ../protobufs/libmoshprotos.a ../crypto/libmoshcrypto.a ../util/libmoshutil.a $(protobuf_LIBS) $(CRYPTO_LIBS) $(ZSTD_LIBS)

osc52_parse_SOURCES = osc52-parse.cc
osc52_parse_CPPFLAGS = -I$(top_srcdir) $(protobuf_CFLAGS) $(ZSTD_CFLAGS)
osc52_parse_LDADD = ../statesync/libmoshstatesync.a ../terminal/libmoshterminal.a ../protobufs/libmoshprotos.a ../util/libmoshutil.a $(protobuf_LIBS) $(TINFO_LIBS) $(ZSTD_LIBS)

kitty_graphics_SOURCES = kitty-graphics.cc
kitty_graphics_CPPFLAGS = -I$(top_srcdir) $(protobuf_CFLAGS)
kitty_graphics_LDADD = ../statesync/libmoshstatesync.a ../terminal/libmoshterminal.a ../protobufs/libmoshprotos.a ../util/libmoshutil.a $(protobuf_LIBS) $(TINFO_LIBS) $(ZSTD_LIBS)

terminal_geometry_SOURCES = terminal-geometry.cc
terminal_geometry_CPPFLAGS = -I$(top_srcdir) $(protobuf_CFLAGS)
terminal_geometry_LDADD = ../statesync/libmoshstatesync.a ../terminal/libmoshterminal.a ../protobufs/libmoshprotos.a ../util/libmoshutil.a $(protobuf_LIBS) $(TINFO_LIBS) $(ZSTD_LIBS)

terminal_display_SOURCES = terminal-display.cc
terminal_display_CPPFLAGS = -I$(top_srcdir) $(protobuf_CFLAGS)
terminal_display_LDADD = ../statesync/libmoshstatesync.a ../terminal/libmoshterminal.a ../protobufs/libmoshprotos.a ../util/libmoshutil.a $(protobuf_LIBS) $(TINFO_LIBS) $(ZSTD_LIBS)

tmux_control_SOURCES = tmux-control.cc
tmux_control_CPPFLAGS = $(terminal_display_CPPFLAGS)
tmux_control_LDADD = $(terminal_display_LDADD)

EXTRA_DIST += tmux-control-integration.test tmux-control-integration.py

clean-local: clean-local-check

check_PROGRAMS += control-panel mascot
check_PROGRAMS += file-transfer
TESTS += file-transfer
TESTS += file-transfer-integration.test
EXTRA_DIST += file-transfer-integration.test
file_transfer_SOURCES = file-transfer.cc
file_transfer_CPPFLAGS = -I$(top_builddir) $(terminal_display_CPPFLAGS)
file_transfer_LDADD = $(control_panel_LDADD)
check_PROGRAMS += link-budget
check_PROGRAMS += udp-relay
check_PROGRAMS += socks5-proxy
TESTS += socks5-proxy socks5-integration.test socks5-session.test
EXTRA_DIST += socks5-integration.test socks5-integration.py socks5-session.test
socks5_proxy_SOURCES = socks5-proxy.cc
socks5_proxy_CPPFLAGS = -I$(top_srcdir) $(CRYPTO_CFLAGS)
socks5_proxy_LDADD = ../network/libmoshnetwork.a ../crypto/libmoshcrypto.a ../util/libmoshutil.a $(CRYPTO_LIBS)
TESTS += udp-relay udp-relay-integration.test udp-jump-wrapper.test
EXTRA_DIST += udp-relay-integration.test udp-jump-wrapper.test udp-jump-wrapper.py
udp_relay_SOURCES = udp-relay.cc
udp_relay_CPPFLAGS = -I$(top_srcdir) $(CRYPTO_CFLAGS)
udp_relay_LDADD = ../network/libmoshnetwork.a ../crypto/libmoshcrypto.a ../util/libmoshutil.a $(CRYPTO_LIBS)
link_budget_SOURCES = link-budget.cc
link_budget_CPPFLAGS = -I$(top_srcdir)
check_PROGRAMS += sixel-state
check_PROGRAMS += terminal-extensions
check_PROGRAMS += download
check_PROGRAMS += download-forward
TESTS += download
TESTS += download-forward
TESTS += download-integration.test
EXTRA_DIST += download-integration.py download-integration.test
download_SOURCES = download.cc
download_CPPFLAGS = -I$(top_builddir) $(terminal_display_CPPFLAGS) $(CRYPTO_CFLAGS) $(ZSTD_CFLAGS)
download_LDADD = ../frontend/libmoshcontrol.a ../network/libmoshnetwork.a ../crypto/libmoshcrypto.a $(terminal_display_LDADD) ../fec/libmoshfec.a $(CRYPTO_LIBS)
download_forward_SOURCES = download-forward.cc
download_forward_CPPFLAGS = $(download_CPPFLAGS)
download_forward_LDADD = $(download_LDADD)
TESTS += terminal-extensions
TESTS += graphics-integration.test
EXTRA_DIST += graphics-integration.py graphics-integration.test
terminal_extensions_SOURCES = terminal-extensions.cc
terminal_extensions_SOURCES += unicode16-tests.h
terminal_extensions_CPPFLAGS = $(terminal_display_CPPFLAGS)
terminal_extensions_LDADD = $(terminal_display_LDADD)
TESTS += sixel-state
sixel_state_SOURCES = sixel-state.cc
sixel_state_CPPFLAGS = $(terminal_display_CPPFLAGS)
sixel_state_LDADD = $(terminal_display_LDADD)
TESTS += control-panel mascot
TESTS += link-budget
TESTS += link-budget-integration.test
EXTRA_DIST += link-budget-integration.test
TESTS += control-panel-integration.test
TESTS += startup-screen.test
TESTS += kittycairo.test
EXTRA_DIST += kittycairo.test
EXTRA_DIST += startup-screen.py startup-screen.test
EXTRA_DIST += control-panel-integration.py control-panel-integration.test
control_panel_SOURCES = control-panel.cc
control_panel_CPPFLAGS = -I$(top_builddir) $(terminal_display_CPPFLAGS)
control_panel_LDADD = ../frontend/libmoshcontrol.a ../network/libmoshnetwork.a ../crypto/libmoshcrypto.a $(terminal_display_LDADD) ../fec/libmoshfec.a $(CRYPTO_LIBS) $(LIBRSYNC_LIBS) $(FILECRYPTO_LIBS)
mascot_SOURCES = mascot.cc
mascot_CPPFLAGS = -I$(top_builddir) $(terminal_display_CPPFLAGS)
mascot_LDADD = ../frontend/libmoshmascot.a $(terminal_display_LDADD)
.PHONY: clean-local-check
clean-local-check:
	-for i in $(displaytests); do rm -rf $$i.d/; done

CLEANFILES = base64_vector.cc
