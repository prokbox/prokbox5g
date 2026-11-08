# Modified for ProkBox5g (2026); see docs/changes-from-rockbox.md.
#             __________               __   ___.
#   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
#   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
#   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
#   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
#                     \/            \/     \/    \/            \/
#

INCLUDES += -I$(APPSDIR) $(patsubst %,-I$(APPSDIR)/%,$(subst :, ,$(APPEXTRA)))
SRC += $(call preprocess, $(APPSDIR)/SOURCES)

# ProkBox P19 S3 (docs/design/p19-ui-on-cop.md): in an iPod Video shell
# firmware, developer builds only (HAVE_UI_THREAD_SPLIT; the release
# compiles the split out, #148), the shell's
# direct back-end calls cross to the CPU when the shell runs on the COP:
# the linker's --wrap options from apps/ipodui/febe_rpc.opts
# (scripts/febe_rpc_gen.py)
ifeq ($(MODELNAME),ipodvideo)
ifeq (,$(APP_TYPE))
ifneq (,$(findstring -DHAVE_IPOD_UI ,$(EXTRA_DEFINES) ))
ifneq (,$(findstring -DHAVE_IPOD_UI_DEVMODE ,$(EXTRA_DEFINES) ))
CORE_LDOPTS += @$(APPSDIR)/ipodui/febe_rpc.opts
# the shell modules link with the same options (modules.make mod_link)
FEBE_WRAP_FILE := $(APPSDIR)/ipodui/febe_rpc.opts
FEBE_WRAP_OPTS = @$(FEBE_WRAP_FILE)
endif
endif
endif
endif

# ProkBox (#106): shell modules (target builds with IPODUI_MODULES only; after
# FEBE_WRAP_FILE above: its keep rule names it as a prerequisite)
include $(APPSDIR)/ipodui/modules/modules.make

# apps/features.txt is a file that (is preprocessed and) lists named features
# based on defines in the config-*.h files. The named features will be passed
# to genlang and thus (translated) phrases can be used based on those names.
# button.h is included for the HAS_BUTTON_HOLD define.
#
# Kludge: depends on config.o which only depends on config-*.h to have config.h
# changes trigger a genlang re-run
#

ifneq (,$(USE_LTO))
$(BUILDDIR)/apps/features: PPCFLAGS += -DUSE_LTO
endif

$(BUILDDIR)/apps/features: $(APPSDIR)/features.txt  $(BUILDDIR)/firmware/common/config.o
	$(SILENT)mkdir -p $(BUILDDIR)/apps
	$(SILENT)mkdir -p $(BUILDDIR)/lang
	$(call PRINTS,PP $(<F))
	$(SILENT)$(CC) $(PPCFLAGS) \
                 -E -P -imacros "config.h" -imacros "button.h" -x c $< | \
		grep -v "^#" | grep -v "^ *$$" > $(BUILDDIR)/apps/features; \

$(BUILDDIR)/apps/genlang-features:  $(BUILDDIR)/apps/features
	$(call PRINTS,GEN $(subst $(BUILDDIR)/,,$@))tr \\n : < $< > $@
	$(SILENT)rm -f $(BUILDDIR)/apps/lang/english.list # genlang -b caches it by english.lang's date only: new features must renumber the .lng files too

ASMDEFS_SRC += $(APPSDIR)/core_asmdefs.c

# ProkBox P19 19-2: the drift as display jobs includes bcmfw/'s job
# contract (bcmjob.h, jobs.h); -idirafter, so it never shadows a Rockbox
# header (as firmware.make's bcmjob-*.o)
# (and P19 19-5/19-6: the marquee's PAN, art_bcm's DECODE; the bench's
# devbench_power.o, whose slp row rings the doorbell with a FENCE; and
# devbench.o, whose VF0 row runs SPECTRUM, sitting 4g; devbench_vcpwr.o,
# whose rows and watch use FENCE and the self-test)
BCMJOB_SHELL_OBJ := $(addprefix $(BUILDDIR)/apps/ipodui/,drift_jobs.o mq_jobs.o art_bcm.o \
	devbench_power.o viz.o viz_jviz.o devjobfail.o devbench.o devbench_vcpwr.o)
# #126: the visualizer's spectrum and bars (bcmfw/jobs/jviz.c, the PP's
# CPU path), and in a simulator with the job model its SPECTRUM and BARS
# (jref_viz.c), with the executor's JREF_VIZ as uisimulator.make sets it
ifdef APP_TYPE
$(BUILDDIR)/apps/ipodui/viz_jviz.o: CFLAGS += -DJREF_DECODE -DJREF_VIZ
endif
$(BCMJOB_SHELL_OBJ): CFLAGS += -idirafter $(ROOTDIR)/bcmfw/jobs -idirafter $(ROOTDIR)/bcmfw
# -idirafter dirs are system dirs to gcc, so -MMD leaves them out of the
# .d: name them, or an edit to bcmjob.h/jobs.h leaves these objects stale
$(BCMJOB_SHELL_OBJ): $(wildcard $(ROOTDIR)/bcmfw/jobs/*.h $(ROOTDIR)/bcmfw/*.h)
