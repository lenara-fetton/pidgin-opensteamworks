# Standalone test programs (not installed). Run from steam-mobile/tests:  make && ./test_proto
PKG_CONFIG ?= pkg-config
CFLAGS ?= -O0 -g
COMMON_CFLAGS = -Wall -I.. -I. $(shell $(PKG_CONFIG) --cflags glib-2.0 json-glib-1.0 purple nss zlib) $(CFLAGS)
COMMON_LIBS   = $(shell $(PKG_CONFIG) --libs glib-2.0 json-glib-1.0 purple nss zlib)

# Each test lists the plugin objects it links against (built in ..).
TESTS = test_load test_proto test_auth test_cm

all: $(TESTS)

../%.o: ../%.c
	$(MAKE) -C .. $*.o

%.o: %.c
	$(CC) $(COMMON_CFLAGS) -c $< -o $@

test_proto: test_proto.o ../steam_proto.o ../steam_msgs.o
	$(CC) $(COMMON_CFLAGS) $^ -o $@ $(COMMON_LIBS)

# steam_rsa.c is #included by steam_auth.c, not linked separately.
test_auth: test_auth.o purple_harness.o ../steam_auth.o ../steam_connection.o ../steam_proto.o
	$(CC) $(COMMON_CFLAGS) $^ -o $@ $(COMMON_LIBS)

test_ws: test_ws.o purple_harness.o ../steam_ws.o
	$(CC) $(COMMON_CFLAGS) $^ -o $@ $(COMMON_LIBS)

test_cm: test_cm.o purple_harness.o ../steam_cm.o ../steam_ws.o ../steam_msgs.o ../steam_proto.o ../steam_connection.o
	$(CC) $(COMMON_CFLAGS) $^ -o $@ $(COMMON_LIBS)

clean:
	rm -f *.o $(TESTS)

.PHONY: all clean

test_load: test_load.o purple_harness.o ../libsteam.so
	$(CC) $(COMMON_CFLAGS) test_load.o purple_harness.o -o $@ $(COMMON_LIBS)

../libsteam.so:
	$(MAKE) -C ..
