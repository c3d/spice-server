TOP=../../

PRODUCTS=	spice-server.dll
SPICE_SERVER_VERSION=\"$(shell ../build-aux/git-version-gen ../.tarball-version)\"

PKGCONFIGS=	spice-protocol				\
		pixman-1				\
		openssl					\
		liblz4?					\
		gobject-2.0				\
		glib-2.0				\
		gio-2.0					\
		zlib					\
		opus					\
		libjpeg

CONFIG=		<lz4.h>					\
		lz4_compress_fast_continue		\
		pthread_setname_np_1arg

GENERATED=	spice-server-enums.h			\
		spice-server-enums.c			\
		spice-version.h

SOURCES=	$(filter %.c, $(GENERATED))		\
		agent-msg-filter.c			\
		char-device.c				\
		common-graphics-channel.c		\
		cursor-channel.c			\
		cursor-channel-client.c			\
		dcc.c					\
		dcc-send.c				\
		dispatcher.c				\
		display-channel.c			\
		event-loop.c				\
		glz-encoder.c				\
		glz-encoder-dict.c			\
		image-cache.c				\
		image-encoders.c			\
		inputs-channel.c			\
		inputs-channel-client.c			\
		jpeg-encoder.c				\
		main-channel.c				\
		main-channel-client.c			\
		main-dispatcher.c			\
		memslot.c				\
		mjpeg-encoder.c				\
		net-utils.c				\
		pixmap-cache.c				\
		red-channel.c				\
		red-channel-capabilities.c		\
		red-channel-client.c			\
		red-client.c				\
		red-parse-qxl.c				\
		red-pipe-item.c				\
		red-qxl.c				\
		red-record-qxl.c			\
		red-replay-qxl.c			\
		reds.c					\
		red-stream.c				\
		red-worker.c				\
		sound.c					\
		spice-bitmap-utils.c			\
		spicevmc.c				\
		stat-file.c				\
		video-stream.c				\
		stream-channel.c			\
		red-stream-device.c			\
		sw-canvas.c				\
		tree.c					\
		utils.c					\
		zlib-encoder.c				\
		$(HAVE_LZ4_H:%=				\
			lz4-encoder.c)			\
		$(HAVE_SMARTCARD:%=			\
			smartcard.c			\
			smartcard-channel-client.c)	\
		$(HAVE_GSTREAMER:%=			\
			gstreamer-encoder.c)		\

DEFINES=	HAVE_CONFIG_H				\
		SPICE_SERVER_INTERNAL			\
		$(DEFINES_$(OS_NAME))			\
		$(EXTRA_CHECKS_$(TARGET))		\
		VERSION=$(SPICE_SERVER_VERSION)

DEFINES_macosx=	MSG_NOSIGNAL=0

EXTRA_CHECKS_debug=	ENABLE_EXTRA_CHECKS=1
EXTRA_CHECKS_opt=	ENABLE_EXTRA_CHECKS=0
EXTRA_CHECKS_release=	ENABLE_EXTRA_CHECKS=0

INCLUDES=	.					\
		$(TOP)protocol				\
		../spice-common

PREFIX_HDR=	$(PREFIX)include/spice-server/
HDR_INSTALL=	spice-audio.h				\
		spice-char.h				\
		spice-core.h				\
		spice-experimental.h			\
		spice-input.h				\
		spice-migration.h			\
		spice-qxl.h				\
		spice-server.h				\
		spice-version.h				\
		spice-replay.h				\
		spice.h

TO_CLEAN=	$(GENERATED)

include make-it-quick/rules.mk

prebuild: $(GENERATED)

spice-server-enums.c: spice-server.h spice-server-enums.c.tmpl
	$(PRINT_GENERATE) glib-mkenums --template spice-server-enums.c.tmpl $< > $@

spice-server-enums.h: spice-server.h spice-server-enums.h.tmpl
	$(PRINT_GENERATE) glib-mkenums --template spice-server-enums.h.tmpl $< > $@

spice-version.h: spice-version.h.in $(MAKEFILE_DEPS)
	$(PRINT_GENERATE) $(GEN_$@)
GEN_spice-version.h=							\
	sed < $< > $@ 							\
	-e 's|@SPICE_SERVER_VERSION@|$(SPICE_SERVER_VERSION)|g'
