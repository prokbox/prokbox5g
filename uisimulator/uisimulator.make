#             __________               __   ___.
#   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
#   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
#   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
#   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
#                     \/            \/     \/    \/            \/
#

INCLUDES += -I$(ROOTDIR)/uisimulator/bitmaps -I$(ROOTDIR)/uisimulator/common -I$(ROOTDIR)/uisimulator/buttonmap $\
	-I$(FIRMDIR)/include -I$(FIRMDIR)/export $(TARGET_INC) -I$(BUILDDIR) -I$(APPSDIR)

SIMFLAGS += $(INCLUDES) $(DEFINES) -DHAVE_CONFIG_H $(GCCOPTS)

SIMSRC += $(call preprocess, $(ROOTDIR)/uisimulator/common/SOURCES)
SIMSRC += $(call preprocess, $(ROOTDIR)/uisimulator/buttonmap/SOURCES)
SIMOBJ = $(call c2obj,$(SIMSRC))
OTHER_SRC += $(SIMSRC)

SIMLIB = $(BUILDDIR)/uisimulator/libuisimulator.a
ifeq (yes,$(APPLICATION))
UIBMP=
else
UIBMP=$(BUILDDIR)/UI256.bmp
endif

.SECONDEXPANSION: # $$(OBJ) is not populated until after this

$(SIMLIB): $$(SIMOBJ) $(UIBMP)
	$(SILENT)$(shell rm -f $@)
	$(call PRINTS,AR $(@F))$(AR) rcs $@ $(SIMOBJ) >/dev/null

$(BUILDDIR)/$(BINARY): $$(OBJ) $(FIRMLIB) $(VOICESPEEXLIB) $(CORE_LIBS) $(SIMLIB)
ifeq ($(UNAME), Darwin)
	$(call PRINTS,LD $(BINARY))$(CC) -o $@ $^ $(LDOPTS) $(GLOBAL_LDOPTS) -Wl,$(LDMAP_OPT),$(BUILDDIR)/rockbox.map
else
	$(call PRINTS,LD $(BINARY))$(CC) -o $@ -Wl,--start-group $^ -Wl,--end-group $(LDOPTS) $(GLOBAL_LDOPTS) \
	-Wl,$(LDMAP_OPT),$(BUILDDIR)/rockbox.map
endif
	$(SILENT)$(call objcopy,$@,$@)

# -MMD -MP: a generated header (lang.h's LANG_LAST_INDEX_IN_ARRAY, which
# sim_cmd_text.c's str() range check reads) must rebuild the sim objects;
# make.dep is a one-time snapshot and had no .d for them (gatefix2).
$(BUILDDIR)/uisimulator/%.o: $(ROOTDIR)/uisimulator/%.c
	$(SILENT)mkdir -p $(dir $@)
	$(call PRINTS,CC $(subst $(ROOTDIR)/,,$<))$(CC) $(SIMFLAGS) -MMD -MP -c $< -o $@
	$(SILENT)$(fix_dep_target)

$(UIBMP): $(ROOTDIR)/uisimulator/bitmaps/UI-$(MODELNAME).bmp
	$(call PRINTS,CP $(@F))cp $< $@

# ProkBox P19 W0-5: the BCM2722 job model (sim_bcm_job.c wraps
# bcmfw/jobs/jmodel.c) and its script commands include bcmfw/'s headers;
# -idirafter, so they never shadow a Rockbox header.
$(BUILDDIR)/uisimulator/common/sim_bcm_job.o \
$(BUILDDIR)/uisimulator/common/sim_cmd_bcm.o: \
	SIMFLAGS += -idirafter $(ROOTDIR)/bcmfw/jobs -idirafter $(ROOTDIR)/bcmfw
# the model offers DECODE (jmodel.h), and so does the executor it shares
# with bcmjob-ref.o (firmware.make): the same JREF_DECODE for both
$(BUILDDIR)/uisimulator/common/sim_bcm_job.o: SIMFLAGS += -DJREF_DECODE -DJREF_VIZ
# The dependency pass (-MM, tools/functions.make) preprocesses every source
# with PPCFLAGS only, not the per-object SIMFLAGS, so it would trip jmodel.h's
# DECODE check (a loud #error, but one that only the dependency pass saw).
# Give that pass the same define; it compiles nothing.
ifneq (,$(filter -DHAVE_BCM_JOBS_MODEL,$(EXTRA_DEFINES)))
PPCFLAGS += -DJREF_DECODE
endif

# ProkBox P19 19-6: the model's DECODE runs jdec (bcmfw/jobs/jdec.c over
# the shell's decoder and resizer in bcmfw/jpeg/env's build), as the
# display-only image does, so a cover the shell decodes "on the BCM" in
# the simulator is the image's pixels, not a stand-in.  Those files are
# the simulator's own decoder built a second time, so they are linked
# into one object with only jdec's entry points left global
# (sim_bcm_job.c's hook; a script turns the offer on: bcm_decode on).
ifneq (,$(filter -DHAVE_BCMFW_VIDEO,$(EXTRA_DEFINES)))
SIMJDEC_SRC := $(addprefix $(ROOTDIR)/,apps/recorder/jpeg_load.c \
	apps/recorder/resize.c apps/recorder/bmp.c bcmfw/jobs/jdec.c)
SIMJDEC_DIR := $(BUILDDIR)/uisimulator/jdec
SIMJDEC := $(BUILDDIR)/uisimulator/sim_jdec.o
SIMJDEC_INC := -I$(ROOTDIR)/bcmfw/jpeg/env -I$(ROOTDIR)/apps/recorder \
	-I$(ROOTDIR)/bcmfw/jobs -I$(ROOTDIR)/bcmfw -I$(ROOTDIR)/bcmfw/jpeg
$(SIMJDEC): $(SIMJDEC_SRC) $(wildcard $(ROOTDIR)/bcmfw/jpeg/env/*.h \
	$(ROOTDIR)/bcmfw/jpeg/*.h $(ROOTDIR)/bcmfw/jobs/jdec.h \
	$(ROOTDIR)/bcmfw/jobs/jscan.h $(ROOTDIR)/apps/recorder/*.h)
	$(SILENT)mkdir -p $(SIMJDEC_DIR)
	$(call PRINTS,CC jdec (sim))for f in $(SIMJDEC_SRC); do \
		$(CC) -std=gnu99 -O2 -w $(SIMJDEC_INC) -c $$f \
			-o $(SIMJDEC_DIR)/`basename $$f .c`.o || exit 1; done
ifeq ($(UNAME), Darwin)
	$(SILENT)ld -r -o $@ $(SIMJDEC_DIR)/*.o \
		-exported_symbol _jdec_run -exported_symbol _jdec_mem_src
else
	$(SILENT)$(CC) -r -nostdlib -o $@.tmp $(SIMJDEC_DIR)/*.o \
		&& objcopy --keep-global-symbol=jdec_run \
			--keep-global-symbol=jdec_mem_src $@.tmp $@ && rm -f $@.tmp
endif
$(BUILDDIR)/$(BINARY): $(SIMJDEC)
endif

# the dependency scan (tools/functions.make mkdepfile, -MG) knows no
# per-object CFLAGS: without this, bcmjob-ref.c's "jref.c" is listed as a
# generated file in the build dir and make stops.  firmware.make's device
# build needs the same line.
OTHER_INC += -idirafter $(ROOTDIR)/bcmfw/jobs -idirafter $(ROOTDIR)/bcmfw

# Stale-object guard, as in firmware.make: -idirafter dirs are system
# directories, so no dependency scan lists bcmfw/'s files; and the
# uisimulator rule above has no per-object .d at all.  Name them, plus a
# stamp of the per-object flags.
BCMSIM_STAMP := $(BUILDDIR)/bcmjob-sim-flags.stamp
BCMSIM_STAMPTXT := -DJREF_DECODE -DJREF_VIZ bcmfw=$(ROOTDIR)/bcmfw
$(shell mkdir -p $(BUILDDIR); [ "`cat $(BCMSIM_STAMP) 2>/dev/null`" = '$(BCMSIM_STAMPTXT)' ] || echo '$(BCMSIM_STAMPTXT)' > $(BCMSIM_STAMP))
$(BUILDDIR)/uisimulator/common/sim_bcm_job.o \
$(BUILDDIR)/uisimulator/common/sim_cmd_bcm.o: \
	$(wildcard $(ROOTDIR)/bcmfw/jobs/*.[ch] $(ROOTDIR)/bcmfw/*.h) $(BCMSIM_STAMP)
