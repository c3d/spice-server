TOP=../
BUILD=$(TOP)build/

SUBDIRS=	server docs

# Set srcdir which is used by GNUmakefile in this directory
srcdir=.
include $(BUILD)rules.mk
