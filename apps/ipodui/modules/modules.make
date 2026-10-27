#             __________               __   ___.
#   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
#   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
#   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
#   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
#                     \/            \/     \/    \/            \/
#
# ProkBox shell modules (#106, apps/ipodui/modload.h): compile each module's
# sources Thumb with IPODUI_IN_MODULE, link them twice against rockbox.elf
# (--just-symbols), keep every core symbol they call alive in the core link,
# write modules.pak and the build ID into rockbox.elf (tools/modpak.py)
# before rockbox.bin is made. Only in a target build with IPODUI_MODULES.
#
# Copyright (C) 2026 prokopto-dev
#
# This program is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# as published by the Free Software Foundation; either version 2
# of the License, or (at your option) any later version.
#
# This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
# KIND, either express or implied.

MOD_ON := $(call preprocess, $(APPSDIR)/ipodui/modules/enabled.txt)

ifneq ($(MOD_ON),)
MOD_DIR := $(BUILDDIR)/modules
MOD_LDS := $(APPSDIR)/ipodui/modules/module.lds
MOD_BASE_A := 0x03000000
MOD_BASE_B := 0x03100000
# the arena's size, from modload.h (one definition): modpak.py refuses a
# set of modules that would not fit side by side
MOD_ARENA := $(notdir $(call preprocess, $(APPSDIR)/ipodui/modules/arena.txt,-I$(APPSDIR) -imacros $(APPSDIR)/ipodui/modload.h))

# Each module: its enum mod_id (modload.h) and its sources. Reset
# Settings, Volume Limit (+ roller) and About stay resident: they must
# never depend on a file loading (About is also the developer chord's
# entry). Resident halves stay in apps/SOURCES: the alarm engine and ring,
# the lock gate, the stopwatch timing, tzpick, the xindex driver and core.
MOD_settings_ID := 0
MOD_settings_SRC := ipodui/modules/settings_mod.c ipodui/screens/legal.c \
	ipodui/screens/datetime.c ipodui/screens/checklist.c
MOD_devloader_ID := 1
MOD_devloader_SRC := ipodui/modules/devloader_mod.c ipodui/modules/devloader.c
# P12 wave 1 (docs/phase12-brief.md section 6)
MOD_clocks_ID := 2
MOD_clocks_SRC := ipodui/modules/clocks_mod.c ipodui/screens/clocks.c
MOD_alarms_ID := 3
MOD_alarms_SRC := ipodui/modules/alarms_mod.c ipodui/screens/alarms.c
MOD_stopwatch_ID := 4
MOD_stopwatch_SRC := ipodui/modules/stopwatch_mod.c ipodui/screens/stopwatch.c
MOD_lockset_ID := 5
MOD_lockset_SRC := ipodui/modules/lockset_mod.c ipodui/screens/lockset.c
MOD_notes_ID := 6
MOD_notes_SRC := ipodui/modules/notes_mod.c ipodui/screens/notes.c
MOD_contacts_ID := 7
MOD_contacts_SRC := ipodui/modules/contacts_mod.c ipodui/screens/contacts.c
MOD_calendars_ID := 8
MOD_calendars_SRC := ipodui/modules/calendars_mod.c ipodui/screens/calendars.c ipodui/xical_read.c
MOD_xindex_ID := 9
MOD_xindex_SRC := ipodui/modules/xindex_mod.c ipodui/screens/xindex.c \
	ipodui/xindex_build.c ipodui/xcontact_core.c
MOD_xindex_SRC += ipodui/screens/xical_build.c ipodui/xical_core.c ipodui/xical_read.c

MOD_NAMES := settings clocks alarms stopwatch lockset notes contacts calendars xindex
ifneq ($(findstring HAVE_IPOD_UI_DEVMODE,$(EXTRA_DEFINES) $(DEFINES)),)
MOD_NAMES += devloader
endif

# P12 W4-0 (docs/design/sdk.md): SDK modules, the games. Same compile;
# linked against the SDK's stub library only (libsdk.a, sdk.sym) and
# libgcc, never rockbox.elf; manifests in ipodui/games/<name>/.
MOD_solitaire_ID := 10
MOD_solitaire_SRC := ipodui/games/solitaire/solitaire.c \
	ipodui/games/solitaire/klondike.c
MOD_badgame_ID := 11
MOD_badgame_SRC := ipodui/games/badgame/badgame.c
MOD_quadrus_ID := 12
MOD_quadrus_SRC := ipodui/games/quadrus/quadrus.c \
	ipodui/games/quadrus/quadrus_core.c
MOD_bricks_ID := 13
MOD_bricks_SRC := ipodui/games/bricks/bricks.c \
	ipodui/games/bricks/bricks_core.c
MOD_chute_ID := 14
MOD_chute_SRC := ipodui/games/chute/chute.c \
	ipodui/games/chute/chute_core.c

MOD_chess_ID := 15
MOD_chess_SRC := ipodui/games/chess/chess.c \
	ipodui/games/chess/chess_core.c ipodui/games/chess/chess_engine.c \
	ipodui/games/chess/chess_pad.c

MOD_SDK_NAMES := solitaire quadrus bricks chute chess
ifneq ($(findstring HAVE_IPOD_UI_DEVMODE,$(EXTRA_DEFINES) $(DEFINES)),)
MOD_SDK_NAMES += badgame
endif
MOD_SDK_SYM := $(APPSDIR)/ipodui/sdk/sdk.sym
MOD_SDK_LIB := $(MOD_DIR)/libsdk.a

# P12 (W1-F): modpak.py check's specs, name:a.elf:b.elf:objects
MOD_COMMA := ,
MOD_EMPTY :=
MOD_SPACE := $(MOD_EMPTY) $(MOD_EMPTY)

mod_link = $(CC) $(GCCOPTS) -mthumb -nostdlib -Wl,--gc-sections \
	-Wl,--no-warn-rwx-segments -Wl,--defsym=__mod_base=$(2) -Wl,--just-symbols=$(BUILDDIR)/rockbox.elf \
	-T$(MOD_LDS) -Wl,-Map,$(basename $@).map -o $@ $(1)

# an SDK module: no core symbols at all; the stubs and libgcc, statically
mod_sdk_link = $(CC) $(GCCOPTS) -mthumb -nostdlib -Wl,--gc-sections \
	-Wl,--no-warn-rwx-segments -Wl,--defsym=__mod_base=$(2) \
	-T$(MOD_LDS) -Wl,-Map,$(basename $@).map -o $@ $(1) $(MOD_SDK_LIB) -lgcc

# one module: its objects, both links, its pack and check specs
# ($(2): mod_link for shell modules, mod_sdk_link for SDK modules)
define MOD_RULES
MOD_$(1)_OBJ := $$(patsubst %.c,$$(MOD_DIR)/obj/%.o,$$(MOD_$(1)_SRC))
MOD_ALL_OBJ += $$(MOD_$(1)_OBJ)
MOD_ELFS += $$(MOD_DIR)/$(1).a.elf $$(MOD_DIR)/$(1).b.elf
MOD_SPECS += $$(MOD_$(1)_ID):$(1):$$(MOD_DIR)/$(1).a.elf:$$(MOD_DIR)/$(1).b.elf
MOD_CHECKS += $(1):$$(MOD_DIR)/$(1).a.elf:$$(MOD_DIR)/$(1).b.elf:$$(subst $$(MOD_SPACE),$$(MOD_COMMA),$$(strip $$(MOD_$(1)_OBJ)))
$$(MOD_DIR)/$(1).a.elf: $$(MOD_$(1)_OBJ) $$(MOD_LDS) $(3)
	$$(call PRINTS,LD [module] $$(@F))$$(call $(2),$$(MOD_$(1)_OBJ),$$(MOD_BASE_A))
$$(MOD_DIR)/$(1).b.elf: $$(MOD_$(1)_OBJ) $$(MOD_LDS) $(3)
	$$(call PRINTS,LD [module] $$(@F))$$(call $(2),$$(MOD_$(1)_OBJ),$$(MOD_BASE_B))
endef

MOD_ALL_OBJ :=
MOD_ELFS :=
MOD_SPECS :=
MOD_CHECKS :=
$(foreach m,$(MOD_NAMES),$(eval $(call MOD_RULES,$(m),mod_link,$$(BUILDDIR)/rockbox.elf)))
# keep.txt lists the core symbols of the shell modules only: a game calls
# the core through the SDK table, which the core keeps on its own; its
# libgcc helpers are its own copies
MOD_SHELL_OBJ := $(MOD_ALL_OBJ)
$(foreach m,$(MOD_SDK_NAMES),$(eval $(call MOD_RULES,$(m),mod_sdk_link,$$(MOD_SDK_LIB))))

# the stub library: one ARM stub per sdk.sym entry (sdk_stubs.S)
$(MOD_DIR)/obj/ipodui/sdk/sdk_stubs.o: $(APPSDIR)/ipodui/sdk/sdk_stubs.S $(MOD_SDK_SYM)
	$(SILENT)mkdir -p $(dir $@)
	$(call PRINTS,AS [module] $(subst $(ROOTDIR)/,,$<))$(CC) $(CFLAGS) -c $< -o $@
$(MOD_SDK_LIB): $(MOD_DIR)/obj/ipodui/sdk/sdk_stubs.o
	$(call PRINTS,AR [module] $(@F))rm -f $@ && $(AR) rcs $@ $^

-include $(MOD_ALL_OBJ:.o=.d)

# generated headers first (a clean "make bin" reaches modules early).
# Module objects are not in $(SRC), so make.dep never lists them: every
# generated header a module can include is named here (font.h pulls in
# sysfont.h, made by convbdf after firmware/common/config.o).
$(MOD_ALL_OBJ): | $(BUILDDIR)/lang/lang.h $(BUILDDIR)/lang_enum.h \
	$(BUILDDIR)/rbversion.h $(BUILDDIR)/sysfont.h

$(MOD_DIR)/obj/%.o: $(APPSDIR)/%.c
	$(SILENT)mkdir -p $(dir $@)
	$(call PRINTS,CC [module] $(subst $(ROOTDIR)/,,$<))$(CC) $(CFLAGS) \
		$(CORE_SECTIONS_CFLAGS) -mthumb -DIPODUI_IN_MODULE -MMD -MP -c $< -o $@
	$(SILENT)$(fix_dep_target)

# every core symbol a module calls stays in the core's --gc-sections link
$(MOD_DIR)/keep.txt: $(MOD_SHELL_OBJ) $(ROOTDIR)/tools/modpak.py
	$(call PRINTS,MODKEEP $(@F))python3 $(ROOTDIR)/tools/modpak.py keep \
		--nm $(subst objcopy,nm,$(OC)) -o $@ $(MOD_SHELL_OBJ)

$(BUILDDIR)/rockbox.elf: $(MOD_DIR)/keep.txt
CORE_LDOPTS += -Wl,@$(MOD_DIR)/keep.txt

# The pack, and the build ID patched into rockbox.elf (symbols unchanged)
# before rockbox.bin is made from it
# the pack v2 labels of each module: apps/ipodui/modules/<name>.mod (P12)
MOD_MANIFESTS := $(wildcard $(APPSDIR)/ipodui/modules/*.mod \
	$(APPSDIR)/ipodui/games/*/*.mod)
MOD_MDIRS := $(APPSDIR)/ipodui/modules:$(APPSDIR)/ipodui/games

# P12 (W1-F): the class rules (deny lists, sizes, the module header
# against the manifest) refuse the build before the pack is written
MOD_DENY := $(wildcard $(APPSDIR)/ipodui/modules/*.deny)
$(MOD_DIR)/check.ok: $(MOD_ELFS) $(ROOTDIR)/tools/modpak.py $(MOD_MANIFESTS) $(MOD_DENY) $(MOD_SDK_SYM)
	$(call PRINTS,MODCHECK $(@F))python3 $(ROOTDIR)/tools/modpak.py check \
		--nm $(subst objcopy,nm,$(OC)) --objcopy $(OC) \
		--manifests $(MOD_MDIRS) --sdk-sym $(MOD_SDK_SYM) --base $(MOD_BASE_A) \
		--delta 0x100000 --arena $(MOD_ARENA) $(MOD_CHECKS) >$@.log \
		&& mv $@.log $@ || { cat $@.log; exit 1; }

$(MOD_DIR)/modules.pak: $(MOD_ELFS) $(ROOTDIR)/tools/modpak.py $(MOD_MANIFESTS) $(MOD_DIR)/check.ok
	$(call PRINTS,MODPAK $(@F))python3 $(ROOTDIR)/tools/modpak.py pack \
		--nm $(subst objcopy,nm,$(OC)) --objcopy $(OC) \
		--manifests $(MOD_MDIRS) \
		--core $(BUILDDIR)/rockbox.elf --base $(MOD_BASE_A) \
		--delta 0x100000 --arena $(MOD_ARENA) -o $@ $(MOD_SPECS)

# root.make's rockbox.bin recipe runs this after its objcopy
$(BUILDDIR)/rockbox.bin: $(MOD_DIR)/modules.pak
MOD_PATCH_BIN = $(SILENT)python3 $(ROOTDIR)/tools/modpak.py patchbin \
	--bin $@ --pak $(MOD_DIR)/modules.pak
endif

# The shell's own objects in $(SRC) (apps/SOURCES, every --ipodui build,
# modules or not): make.dep orders them after the generated headers only
# when its "-MG" scan saw the include, so name the headers here too and a
# fresh "make -j" never compiles a screen before genlang or convbdf ran
IPODUI_APP_OBJ := $(patsubst $(ROOTDIR)/%.c,$(BUILDDIR)/%.o, \
	$(filter $(APPSDIR)/ipodui/%.c,$(SRC)))
$(IPODUI_APP_OBJ): | $(BUILDDIR)/lang/lang.h $(BUILDDIR)/lang_enum.h \
	$(BUILDDIR)/sysfont.h
