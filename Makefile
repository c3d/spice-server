PACKAGE_NAME=spice-server
PACKAGE_VERSION=0.13
PACKAGE_DESCRIPTION=SPICE server library
PACKAGE_REQUIRES=spice-protocol >= 0.12.14
PACKAGE_URL=http://spice-space.org

SUBDIRS=	spice-common server docs

# Set srcdir which is used by GNUmakefile in this directory
srcdir=.
include make-it-quick/rules.mk
