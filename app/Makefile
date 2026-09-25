# Builds only the C param bridge; lib/tun_probe and lib/netstack_proxy are
# prebuilt by the Dockerfile before acap-build runs this.

PROG      = OpenVPN_VPN
BRIDGE_SRC = openvpn_bridge.c

STRIP ?= strip

PKGS      = glib-2.0 gio-2.0 axparameter
# -std=gnu11 disables the GCC 13 / Ubuntu 24.04 C23 stdlib redirect that would
# otherwise bind strtoul() -> __isoc23_strtoul@GLIBC_2.38 (absent below OS 12.10).
CFLAGS   += -Wall -Wextra -O2 -std=gnu11
CFLAGS   += $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) pkg-config --cflags $(PKGS))
BRIDGE_LIBS = $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) pkg-config --libs $(PKGS))

all: $(PROG)
	$(STRIP) $(PROG)

$(PROG): $(BRIDGE_SRC)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ $(BRIDGE_LIBS) -o $@

clean:
	rm -f $(PROG) *.o *.eap* *_LICENSE.txt package.conf* param.conf tmp*
