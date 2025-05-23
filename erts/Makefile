# 
# %CopyrightBegin%
#
# SPDX-License-Identifier: Apache-2.0
#
# Copyright Ericsson AB 2006-2025. All Rights Reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# %CopyrightEnd%
# 

.NOTPARALLEL:

include $(ERL_TOP)/make/target.mk
include vsn.mk
include $(ERL_TOP)/make/$(TARGET)/otp.mk

# ----------------------------------------------------------------------

# Other erts dirs than the emulator dir...
ERTSDIRS = etc epmd lib_src
XINSTDIRS = preloaded
ifeq ($(NO_START_SCRIPTS),)
ERTSDIRS += start_scripts
endif

.PHONY: all
all: $(if $(FLAVOR),$(FLAVOR),$(PRIMARY_FLAVOR))

.PHONY: docs
docs:
	$(V_at)( cd doc && $(MAKE) $@ )

.PHONY: debug opt lcnt clean test
$(TYPES) clean:
	$(V_at)for d in emulator $(ERTSDIRS); do \
		if test -d $$d; then \
			( cd $$d && $(MAKE) $@ ) || exit $$?; \
		fi ; \
	done
	(cd preloaded/src && $(MAKE) ../../ebin/erts.app)


.PHONY: test
test:
	TEST_NEEDS_RELEASE=true TYPE=$(TYPE) \
	  $(ERL_TOP)/make/test_target_script.sh $(ERL_TOP)

.PHONY: $(FLAVORS)
$(FLAVORS):
	$(V_at)for type in $(DEFAULT_TYPES); do \
		( $(MAKE) FLAVOR=$@ $$type ) || exit $$?; \
	done

# Make erl script and erlc in $(ERL_TOP)/bin which runs the compiled version
# Note that erlc is not a script and requires extra handling on cygwin.
# also note that this file is not created by autoconf, that's why @EXEEXT@
# is not used.

# The copying of beam.dll should be removed when the beam dll need no longer be
# in the same directory...
.PHONY: local_setup
local_setup:
	@cd start_scripts && $(MAKE)
	$(V_colon)@echo `ls $(ERL_TOP)/bin/`
	@rm -f $(ERL_TOP)/bin/erl $(ERL_TOP)/bin/erlc $(ERL_TOP)/bin/cerl \
		$(ERL_TOP)/bin/erl.exe $(ERL_TOP)/bin/erlc.exe \
		$(ERL_TOP)/bin/escript $(ERL_TOP)/bin/escript.exe \
		$(ERL_TOP)/bin/dialyzer $(ERL_TOP)/bin/dialyzer.exe \
		$(ERL_TOP)/bin/typer $(ERL_TOP)/bin/typer.exe \
		$(ERL_TOP)/bin/ct_run $(ERL_TOP)/bin/ct_run.exe \
		$(ERL_TOP)/bin/start*.boot $(ERL_TOP)/bin/start*.script
	@if [ "X$(TARGET)" = "Xwin32" ]; then \
		cp $(ERL_TOP)/bin/$(TARGET)/dialyzer.exe $(ERL_TOP)/bin/dialyzer.exe; \
		cp $(ERL_TOP)/bin/$(TARGET)/typer.exe $(ERL_TOP)/bin/typer.exe; \
		cp $(ERL_TOP)/bin/$(TARGET)/ct_run.exe $(ERL_TOP)/bin/ct_run.exe; \
		cp $(ERL_TOP)/bin/$(TARGET)/erlc.exe $(ERL_TOP)/bin/erlc.exe; \
		cp $(ERL_TOP)/bin/$(TARGET)/erl.exe $(ERL_TOP)/bin/erl.exe; \
		cp $(ERL_TOP)/bin/$(TARGET)/erl_call.exe $(ERL_TOP)/bin/erl_call.exe; \
		cp $(ERL_TOP)/bin/$(TARGET)/escript.exe $(ERL_TOP)/bin/escript.exe; \
	 	chmod 755 $(ERL_TOP)/bin/erl.exe $(ERL_TOP)/bin/erlc.exe; \
		make_local_ini.sh $(ERL_TOP); \
		cp $(ERL_TOP)/bin/erl.ini $(ERL_TOP)/bin/$(TARGET)/erl.ini; \
	else \
	cd etc/unix && $(MAKE); \
	sed	-e "s;%FINAL_ROOTDIR%;$(ERL_TOP);"   \
		-e "s;erts-.*/bin;bin/$(TARGET);"    \
		-e "s;EMU=.*;EMU=beam$(TYPEMARKER);" \
		-e "s;%DYN_ERL_PATH%;$(TARGET)/dyn_erl;" \
		-e "s;%DYN_ROOTDIR_BASE_EXT%;;" \
	        $(ERL_TOP)/erts/etc/unix/erl.src.src > $(ERL_TOP)/bin/erl; \
	sed	-e "s;%SRC_ROOTDIR%;$(ERL_TOP);"   \
		-e "s;%TARGET%;$(TARGET);"    \
		-e "s;%VSN%;$(VSN);"    \
		-e "s;%DYN_ERL_PATH%;$(TARGET)/dyn_erl;" \
	        $(ERL_TOP)/erts/etc/unix/cerl.src > $(ERL_TOP)/bin/cerl; \
		cp $(ERL_TOP)/bin/$(TARGET)/erl_call $(ERL_TOP)/bin/erl_call; \
		cp $(ERL_TOP)/bin/$(TARGET)/dialyzer $(ERL_TOP)/bin/dialyzer; \
		cp $(ERL_TOP)/bin/$(TARGET)/typer $(ERL_TOP)/bin/typer; \
		cp $(ERL_TOP)/bin/$(TARGET)/ct_run $(ERL_TOP)/bin/ct_run; \
		cp $(ERL_TOP)/bin/$(TARGET)/erlc $(ERL_TOP)/bin/erlc; \
		cp $(ERL_TOP)/bin/$(TARGET)/escript $(ERL_TOP)/bin/escript; \
	 	chmod 755 $(ERL_TOP)/bin/erl $(ERL_TOP)/bin/erlc \
			$(ERL_TOP)/bin/cerl; \
	fi
	@cd start_scripts && $(MAKE) $(ERL_TOP)/bin/start.script \
		$(ERL_TOP)/bin/start_sasl.script \
		$(ERL_TOP)/bin/start_clean.script \
		$(ERL_TOP)/bin/no_dot_erlang.script
# On Android, the shell is found in /system/bin/sh instead of /bin/sh
	@case "$(TARGET)" in \
		*-android*) \
		sed -i'' -e "s;/bin/sh;/system/bin/sh;" \
			$(ERL_TOP)/bin/erl; \
		sed -i'' -e "s;/bin/sh;/system/bin/sh;" \
			$(ERL_TOP)/bin/cerl; \
		;; \
	esac

# ----------------------------------------------------------------------
# These are "convenience targets", provided as shortcuts for developers
# - don't use them in scripts or assume they will always stay like this!
#

# Run the configure script
.PHONY: configure
configure:
	@set -e ; cd autoconf && $(MAKE)

# Remake the makefiles, if you already have configured but you have edited
# a "Makefile.in".
.PHONY: makefiles
makefiles:
	@set -e ; cd autoconf && $(MAKE) $@

# ----------------------------------------------------------------------
# Release targets
#

.PHONY: release
release:
ifeq ($(TYPE),)
	for t in $(DEFAULT_TYPES); do \
	  ( cd emulator && $(MAKE) release TYPE=$$t ) || exit $$?; \
	done
else
	( cd emulator && $(MAKE) release TYPE=$(TYPE) ) || exit $$?;
endif
	$(V_at)for d in $(ERTSDIRS) $(XINSTDIRS); do \
		if test -d $$d; then \
			( cd $$d && $(MAKE) $@ ) || exit $$? ; \
		fi ; \
	 done
	( $(MAKE) -f "$(ERL_TOP)/make/otp_released_app.mk"	\
		APP_PWD="$(ERL_TOP)/erts" APP_VSN=VSN APP=erts	\
		TESTROOT="$(TESTROOT)" update)			\
	 || exit $$?

.PHONY: release_docs
release_docs:
	$(V_at)( cd doc && $(MAKE) $@ )
