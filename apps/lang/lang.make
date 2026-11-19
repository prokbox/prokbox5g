# Modified for ProkBox5g (2026); see docs/changes-from-rockbox.md.
#             __________               __   ___.
#   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
#   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
#   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
#   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
#                     \/            \/     \/    \/            \/
#

LANGS := $(call preprocess, $(APPSDIR)/lang/SOURCES)
LANGOBJ := $(LANGS:$(ROOTDIR)/%.lang=$(BUILDDIR)/%.lng)
VOICEOBJ := $(LANGS:$(ROOTDIR)/%.lang=$(BUILDDIR)/%.vstrings)
LANG_O = $(BUILDDIR)/lang/lang_core.o
ENGLISH := english

CLEANOBJS += $(BUILDDIR)/lang/max_language_size.h $(BUILDDIR)/lang/lang*

# $(BUILDDIR)/apps/lang must exist before we create dependencies on it,
# otherwise make will simply ignore those dependencies.
# Therefore we create it here.
#DUMMY := $(shell mkdir -p $(BUILDDIR)/apps/lang)

# Calculate the maximum language size. Currently based on the file size
# of the largest lng file. Subtract 10 due to HEADER_SIZE and
# SUBHEADER_SIZE.
# TODO: In the future generate this file within genlang or another script
# in order to only calculate the maximum size based on the core strings.
$(BUILDDIR)/lang/max_language_size.h: $(LANGOBJ) $(BUILDDIR)/apps/lang/voicestrings.zip
	$(call PRINTS,GEN $(subst $(BUILDDIR)/,,$@))
	$(SILENT)echo "#define MAX_LANGUAGE_SIZE `ls -ln $(BUILDDIR)/apps/lang/*.lng | awk '{print $$5-10}' | sort -n | tail -1`" > $@

$(BUILDDIR)/lang/lang_core.o: $(BUILDDIR)/lang/lang.h $(BUILDDIR)/lang/lang_core.c
	$(call PRINTS,CC lang_core.c)$(CC) $(CFLAGS) -c $(BUILDDIR)/lang/lang_core.c -o $@

# genlang -p writes lang/lang.h, lang/lang_core.c *and* lang_enum.h in one
# run. A pattern rule with several targets is the one way make (3.81
# included, which has no grouped "&:" targets) knows that one run of the
# recipe makes all of them: genlang runs once, never twice or in parallel,
# and every target counts as remade. The stem is the "g" of "lang" (so the
# pattern matches these three files only). lang_enum.h used to be a
# recipe-less rule depending on lang.h, which make 3.81 did not treat as
# remade: an object that includes only lang_enum.h (firmware/usb.c,
# hes.c, jpeg_load.c) and lang-enum.txt kept the old string enum until a
# second make (p090-locale, ledger 3.19).
$(BUILDDIR)/lan%/lang.h $(BUILDDIR)/lan%/lang_core.c $(BUILDDIR)/lan%_enum.h: $(APPSDIR)/lang/$(ENGLISH).lang $(BUILDDIR)/apps/genlang-features $(TOOLSDIR)/genlang
	$(call PRINTS,GEN lang.h)
	$(SILENT)$(TOOLSDIR)/genlang -e=$(APPSDIR)/lang/$(ENGLISH).lang -p=$(BUILDDIR)/lang -t=$(MODELNAME):`cat $(BUILDDIR)/apps/genlang-features` $<

# NOTE: for some weird reasons in GNU make, multi targets rules WITH patterns actually express
# the fact that the two files are created as the result of one invocation of the rule
$(BUILDDIR)/%.lng $(BUILDDIR)/%.vstrings: $(ROOTDIR)/%.lang $(APPSDIR)/lang/$(ENGLISH).lang $(BUILDDIR)/apps/genlang-features $(TOOLSDIR)/genlang $(TOOLSDIR)/updatelang
	$(call PRINTS,GENLANG $(subst $(ROOTDIR)/,,$<))
	$(SILENT)mkdir -p $(dir $@)
	$(SILENT)$(TOOLSDIR)/updatelang $(APPSDIR)/lang/$(ENGLISH).lang $< $@.tmp
	$(SILENT)$(TOOLSDIR)/genlang -e=$(APPSDIR)/lang/$(ENGLISH).lang -t=$(MODELNAME):`cat $(BUILDDIR)/apps/genlang-features` -i=$(TARGET_ID) -b=$*.lng -c=$*.vstrings $@.tmp
	$(SILENT)rm -f $@.tmp

$(BUILDDIR)/apps/lang/voice-corrections.txt: $(ROOTDIR)/tools/voice-corrections.txt
	$(SILENT)mkdir -p $(dir $@)
	$(call PRINTS,CP $(subst $(ROOTDIR)/,,$<))cp $< $@

$(BUILDDIR)/apps/lang/lang-enum.txt: $(BUILDDIR)/lang_enum.h
	$(SILENT)mkdir -p $(dir $@)
	$(call PRINTS,GEN $(subst $(BUILDDIR)/,,$@))perl -ne 'print if s|\s+(.*), /\* (\w+).*|$$2:$$1|' < $<  |grep -v 'this:' > $@

$(BUILDDIR)/apps/lang/voicestrings.zip: $(VOICEOBJ) $(wildcard $(BUILDDIR)/apps/lang/*.talk) $(BUILDDIR)/apps/lang/voice-corrections.txt $(BUILDDIR)/apps/lang/lang-enum.txt
	$(call PRINTS,ZIP $(subst $(BUILDDIR)/,,$@))
	$(SILENT)zip -X -9 -q $@ $(subst $(BUILDDIR)/,,$^)
ifneq ($(REPRODUCIBLE_ZIP),)
	$(SILENT)strip-nondeterminism $@
endif

#copy any included talk files to the /lang directory
$(BUILDDIR)/apps/lang/%.talk: $(ROOTDIR)/apps/lang/%.talk
	$(call PRINTS,CP $(subst $(ROOTDIR)/,,$<))cp $< $(BUILDDIR)/apps/lang
