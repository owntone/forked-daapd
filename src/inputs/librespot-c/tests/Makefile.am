TEST_CFLAGS = $(CFLAGS) $(JSON_C_CFLAGS) $(LIBCURL_CFLAGS) $(LIBEVENT_CFLAGS) $(LIBGCRYPT_CFLAGS) $(LIBPROTOBUF_C_CFLAGS)
TEST_LIBS = $(LIBS) $(JSON_C_LIBS) $(LIBCURL_LIBS) $(LIBEVENT_LIBS) $(LIBGCRYPT_LIBS) $(LIBPROTOBUF_C_LIBS)

AM_CPPFLAGS = -I$(top_srcdir)

test1_SOURCES = test1.c
test1_LDADD = $(top_builddir)/librespot-c.a -lpthread $(TEST_LIBS)
test1_CFLAGS = $(TEST_CFLAGS)

test2_SOURCES = test2.c
test2_LDADD = $(top_builddir)/librespot-c.a -lpthread $(TEST_LIBS)
test2_CFLAGS = $(TEST_CFLAGS)

check_PROGRAMS = test1 test2
