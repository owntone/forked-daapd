SUBDIRS = tests

noinst_LIBRARIES = librespot-c.a

SHANNON_SRC = \
	src/shannon/ShannonFast.c src/shannon/Shannon.h src/shannon/ShannonInternal.h

PROTO_SRC = \
	src/proto/keyexchange.pb-c.c src/proto/keyexchange.pb-c.h \
	src/proto/authentication.pb-c.c src/proto/authentication.pb-c.h \
	src/proto/mercury.pb-c.c src/proto/mercury.pb-c.h \
	src/proto/metadata.pb-c.c src/proto/metadata.pb-c.h

HTTP_PROTO_SRC = \
	src/proto/connectivity.pb-c.c src/proto/connectivity.pb-c.h \
	src/proto/clienttoken.pb-c.c src/proto/clienttoken.pb-c.h \
	src/proto/login5_user_info.pb-c.h src/proto/login5_user_info.pb-c.c \
	src/proto/login5.pb-c.h src/proto/login5.pb-c.c \
	src/proto/login5_identifiers.pb-c.h src/proto/login5_identifiers.pb-c.c \
	src/proto/login5_credentials.pb-c.h src/proto/login5_credentials.pb-c.c \
	src/proto/login5_client_info.pb-c.h src/proto/login5_client_info.pb-c.c \
	src/proto/login5_challenges_hashcash.pb-c.h src/proto/login5_challenges_hashcash.pb-c.c \
	src/proto/login5_challenges_code.pb-c.h src/proto/login5_challenges_code.pb-c.c \
	src/proto/google_duration.pb-c.h src/proto/google_duration.pb-c.c \
	src/proto/storage_resolve.pb-c.h src/proto/storage_resolve.pb-c.c

CORE_SRC = \
	src/librespot-c.c src/connection.c src/channel.c src/crypto.c src/commands.c \
	src/http.c

librespot_c_a_SOURCES =  \
	$(CORE_SRC)      \
	$(SHANNON_SRC)   \
	$(PROTO_SRC)     \
	$(HTTP_PROTO_SRC)

noinst_HEADERS =	 \
	librespot-c.h src/librespot-c-internal.h src/connection.h \
	src/channel.h src/crypto.h src/commands.h src/http.h

EXTRA_DIST = README.md LICENSE
