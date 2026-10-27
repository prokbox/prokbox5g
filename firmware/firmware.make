#             __________               __   ___.
#   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
#   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
#   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
#   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
#                     \/            \/     \/    \/            \/
#

INCLUDES += -I$(FIRMDIR) -I$(FIRMDIR)/export -I$(FIRMDIR)/drivers \
			-I$(FIRMDIR)/include -I$(FIRMDIR)/kernel/include
ifndef APP_TYPE
INCLUDES += -I$(FIRMDIR)/libc/include
endif

ifneq ($(filter reggen,$(TOOLSET)),)
  include $(FIRMDIR)/reggen/reggen.make
endif

FIRMLIB_SRC += $(call preprocess, $(FIRMDIR)/asm/SOURCES)
FIRMLIB_SRC += $(call preprocess, $(FIRMDIR)/SOURCES)
FIRMLIB_OBJ := $(call c2obj, $(FIRMLIB_SRC))
FIRMLIB_OBJ += $(BUILDDIR)/sysfont.o
OTHER_SRC += $(FIRMLIB_SRC)

FIRMLIB = $(BUILDDIR)/firmware/libfirmware.a

ifeq ($(SYSFONT),)
SYSFONT = 08-Schumacher-Clean
endif
SYSFONTX = $(ROOTDIR)/fonts/$(SYSFONT).bdf

CLEANOBJS += $(BUILDDIR)/sysfont.* $(BUILDDIR)/version.*

# Limits for the built-in sysfont: ASCII for bootloaders, ISO8859-1 for normal builds
ifneq (,$(findstring -DBOOTLOADER,$(EXTRA_DEFINES)))
	MAXCHAR = 127
else
	MAXCHAR = 255
endif

$(FIRMLIB): $(FIRMLIB_OBJ)
	$(SILENT)$(shell rm -f $@)
	$(call PRINTS,AR $(@F))$(AR) rcs $@ $^ >/dev/null

$(BUILDDIR)/sysfont.h: $(SYSFONTX) $(TOOLS) $(BUILDDIR)/firmware/common/config.o
	$(call PRINTS,CONVBDF $(subst $(ROOTDIR)/,,$<))$(TOOLSDIR)/convbdf -l $(MAXCHAR) -h -o $@ $<

$(BUILDDIR)/sysfont.o: $(SYSFONTX) $(BUILDDIR)/sysfont.h
	$(call PRINTS,CONVBDF $(subst $(ROOTDIR)/,,$<))$(TOOLSDIR)/convbdf -l $(MAXCHAR) -c -o $(BUILDDIR)/sysfont.c $<
	$(call PRINTS,CC $(subst $(ROOTDIR)/,,$(BUILDDIR)/sysfont.c))$(CC) $(CFLAGS) -c $(BUILDDIR)/sysfont.c -o $@

# ProkBox P19: the BCM2722 job driver and the reference executor include
# bcmfw/'s headers (jobs.h, jref.h, hostproto.h); -idirafter, so they never
# shadow a Rockbox header.  The executor and the driver's logic are Thumb
# (gap G4; the brief's 9 KB): the driver's time is the host port's (about
# 10 PP cycles a 16-bit transfer), not its instructions.
$(BUILDDIR)/firmware/target/arm/ipod/video/bcmjob-%.o \
$(BUILDDIR)/firmware/target/arm/ipod/video/lcd-video.o: \
	CFLAGS += -idirafter $(ROOTDIR)/bcmfw/jobs -idirafter $(ROOTDIR)/bcmfw
# the dependency scan (mkdepfile, -MG) sees no per-object CFLAGS
OTHER_INC += -idirafter $(ROOTDIR)/bcmfw/jobs -idirafter $(ROOTDIR)/bcmfw
ifndef APP_TYPE
BCMJOB_OBJFLAGS := -mthumb
$(BUILDDIR)/firmware/target/arm/ipod/video/bcmjob-ref.o \
$(BUILDDIR)/firmware/target/arm/ipod/video/bcmjob-viz.o \
$(BUILDDIR)/firmware/target/arm/ipod/video/bcmjob-core.o: CFLAGS += -mthumb
else
# the simulator's executor is also the host model's (sim_bcm_job.c), which
# offers DECODE: every file of it sees JREF_DECODE (jref.h), the device's
# CPU path none
# (and #126: the model's SPECTRUM and BARS, JREF_VIZ, the same for both)
BCMJOB_OBJFLAGS := -DJREF_DECODE -DJREF_VIZ
$(BUILDDIR)/firmware/target/arm/ipod/video/bcmjob-%.o: CFLAGS += -DJREF_DECODE -DJREF_VIZ
endif

# Stale-object guard (bcmfw headers + per-object flags).  -idirafter dirs
# are system directories to the compiler, so neither -MMD (the per-object
# .d) nor -MM (mkdepfile) lists bcmfw/jobs/jref.c, jref.h, ... at all: an
# edit there rebuilt nothing (a stale bcmjob-ref.o failed to link, and on
# the device would have silently kept old code).  Name them as
# prerequisites of the explicit objects (not a pattern rule: that would
# cancel the implicit compile rule), plus a flags stamp (rewritten only
# when the per-object flags text changes) so e.g. -DJREF_DECODE or
# -mthumb changes rebuild.  scripts/check_bcmdeps.sh checks it.
BCMJOB_STAMP := $(BUILDDIR)/bcmjob-fw-flags.stamp
BCMJOB_STAMPTXT := $(BCMJOB_OBJFLAGS) bcmfw=$(ROOTDIR)/bcmfw
$(shell mkdir -p $(BUILDDIR); [ "`cat $(BCMJOB_STAMP) 2>/dev/null`" = '$(BCMJOB_STAMPTXT)' ] || echo '$(BCMJOB_STAMPTXT)' > $(BCMJOB_STAMP))
BCMJOB_OBJS := $(patsubst $(ROOTDIR)/%.c,$(BUILDDIR)/%.o,$(wildcard $(ROOTDIR)/firmware/target/arm/ipod/video/bcmjob-*.c)) \
	$(BUILDDIR)/firmware/target/arm/ipod/video/lcd-video.o
$(BCMJOB_OBJS): $(wildcard $(ROOTDIR)/bcmfw/jobs/*.[ch] $(ROOTDIR)/bcmfw/*.h) $(BCMJOB_STAMP)

ifdef USE_LTO
$(BUILDDIR)/firmware/asm/%.o: CFLAGS += -fno-lto
$(BUILDDIR)/firmware/kernel/thread.o: CFLAGS += -fno-lto
endif

# GNU make (at least) has a bug/feature that exported variable are not available
# in the shell function (but are in recipe). Thus we need to explicitely pass
# the VERSION environement variable
SVNVERSION:=$(shell VERSION='$(VERSION)' $(TOOLSDIR)/version.sh $(ROOTDIR))
OLDSVNVERSION:=$(shell grep 'RBVERSION' $(BUILDDIR)/rbversion.h 2>/dev/null|cut -d '"' -f 2 || echo "NOREVISION")

CORE_LIBS:=$(FIRMLIB)

ifneq ($(SVNVERSION),$(OLDSVNVERSION))
.PHONY: $(BUILDDIR)/rbversion.h
endif

$(BUILDDIR)/rbversion.h:
	$(call PRINTS,GEN $(@F))$(TOOLSDIR)/genversion.sh $(BUILDDIR) $(SVNVERSION)
