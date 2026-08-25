# Verilated -*- Makefile -*-
# DESCRIPTION: Verilator output: Makefile for building Verilated archive or executable
#
# Execute this makefile from the object directory:
#    make -f Vntt_core.mk

default: libntt_core_lib

### Constants...
# Perl executable (from $PERL, defaults to 'perl' if not set)
PERL = perl
# Python3 executable (from $PYTHON3, defaults to 'python3' if not set)
PYTHON3 = python3
# Path to Verilator kit (from $VERILATOR_ROOT)
VERILATOR_ROOT = /opt/homebrew/Cellar/verilator/5.050/share/verilator
# SystemC include directory with systemc.h (from $SYSTEMC_INCLUDE)
SYSTEMC_INCLUDE ?=
# SystemC library directory with libsystemc.a (from $SYSTEMC_LIBDIR)
SYSTEMC_LIBDIR ?=

### Switches...
# C++ code coverage  0/1 (from --prof-c)
VM_PROFC = 0
# SystemC output mode?  0/1 (from --sc)
VM_SC = 0
# Legacy or SystemC output mode?  0/1 (from --sc)
VM_SP_OR_SC = $(VM_SC)
# Deprecated
VM_PCLI = 1
# Deprecated: SystemC architecture to find link library path (from $SYSTEMC_ARCH)
VM_SC_TARGET_ARCH = linux

### Vars...
# Design prefix (from --prefix)
VM_PREFIX = Vntt_core
# Module prefix (from --prefix)
VM_MODPREFIX = Vntt_core
# User CFLAGS (from -CFLAGS on Verilator command line)
VM_USER_CFLAGS = \
	-fPIC \

# User LDLIBS (from -LDFLAGS on Verilator command line)
VM_USER_LDLIBS = \

# User .cpp files (from .cpp's on Verilator command line)
VM_USER_CLASSES = \

# User .cpp directories (from .cpp's on Verilator command line)
VM_USER_DIR = \
  .. \

### Default rules...
# Include list of all generated classes
include Vntt_core_classes.mk
# Include global rules
include $(VERILATOR_ROOT)/include/verilated.mk

### Library rules from --lib-create
libntt_core_lib.a: $(VK_OBJS) $(VK_USER_OBJS) $(VK_GLOBAL_OBJS) ntt_core_lib.o $(VM_HIER_LIBS)

libntt_core_lib.so: $(VK_OBJS) $(VK_USER_OBJS) $(VK_GLOBAL_OBJS) ntt_core_lib.o $(VM_HIER_LIBS)
ifeq ($(shell uname -s),Darwin)
	$(OBJCACHE) $(CXX) $(CXXFLAGS) $(CPPFLAGS) $(OPT_FAST) -undefined dynamic_lookup -shared $(LDFLAGS) -flat_namespace -o $@ $^ $(LDLIBS) $(LIBS)
else
	$(OBJCACHE) $(CXX) $(CXXFLAGS) $(CPPFLAGS) $(OPT_FAST) -shared $(LDFLAGS) -o $@ $^ $(LDLIBS) $(LIBS)
endif

libntt_core_lib: libntt_core_lib.a libntt_core_lib.so
# Verilated -*- Makefile -*-
