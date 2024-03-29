.DEFAULT_GOAL := ecp5_infer_bram_outreg.so

# Either find yosys in system and use its path or use the given path
YOSYS_PATH ?= $(realpath $(dir $(shell command -v yosys))/..)

# Find yosys-config, throw an error if not found
YOSYS_CONFIG = $(YOSYS_PATH)/bin/yosys-config
ifeq (,$(wildcard $(YOSYS_CONFIG)))
  $(error "Didn't find 'yosys-config' under '$(YOSYS_PATH)'")
endif

CXX ?= $(shell $(YOSYS_CONFIG) --cxx)
CXXFLAGS := $(shell $(YOSYS_CONFIG) --cxxflags) $(CXXFLAGS) #-DSDC_DEBUG
LDFLAGS := $(shell $(YOSYS_CONFIG) --ldflags) $(LDFLAGS)
LDLIBS := $(shell $(YOSYS_CONFIG) --ldlibs) $(LDLIBS)
EXTRA_FLAGS ?=

YOSYS_DATA_DIR = $(DESTDIR)$(shell $(YOSYS_CONFIG) --datdir)
YOSYS_PLUGINS_DIR = $(YOSYS_DATA_DIR)/plugins

BUILD_DIR := $(PLUGIN_DIR)/build

ecp5_infer_bram_outreg.so: ecp5_infer_bram_outreg.o
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) $(EXTRA_FLAGS) -MMD -c -o ecp5_infer_bram_outreg.o ecp5_infer_bram_outreg.cc
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -shared -o ecp5_infer_bram_outreg.so ecp5_infer_bram_outreg.o $(LDLIBS)

clean:
	rm -f ecp5_infer_bram_outreg.d ecp5_infer_bram_outreg.o ecp5_infer_bram_outreg.so

install: ecp5_infer_bram_outreg.so
	install -d $(DESTDIR)$(PREFIX)/lib
	install -m 644 ecp5_infer_bram_outreg.so $(DESTDIR)$(PREFIX)/lib/ecp5_infer_bram_outreg.so

-include *.d
