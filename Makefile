#---------------------------------------------------------------------------------
# SwitchMX — Nintendo Switch homebrew build (devkitA64 + libnx)
#
#   make            -> SwitchMX.nro
#   make -f Makefile.desktop   -> desktop test build (macOS / Linux)
#---------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

TARGET      := SwitchMX
BUILD       := build
SOURCES     := source third_party/nanovg third_party/mpv039/demux third_party/libarchive372
DATA        :=
INCLUDES    := source third_party/nanovg third_party/libusbhsfs/include third_party/mupdf/include third_party/libsmb2/include
APP_TITLE   := SwitchMX
APP_AUTHOR  := Farzin Abdi
APP_VERSION := 1.0.0

ifneq (,$(wildcard $(TOPDIR)/icon.jpg))
ICON := icon.jpg
endif

PKGCONF := $(DEVKITPRO)/portlibs/switch/bin/aarch64-none-elf-pkg-config

#---------------------------------------------------------------------------------
# options for code generation
#---------------------------------------------------------------------------------
ARCH     := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

CFLAGS   := -g -Wall -O2 -ffunction-sections -fno-omit-frame-pointer $(ARCH) $(DEFINES)
CFLAGS   += $(INCLUDE) -D__SWITCH__ $(shell $(PKGCONF) --cflags mpv sdl2)
CXXFLAGS := $(CFLAGS) -std=gnu++17
ASFLAGS  := -g $(ARCH)
LDFLAGS   = -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map) \
            -Wl,--wrap=__syscall_thread_create -Wl,--wrap=sslConnectionSetVerifyOption

# USB drives: GPL build of libusbhsfs reads FAT, exFAT, NTFS and EXT2/3/4 (needs switch-ntfs-3g, switch-lwext4).
USBHSFS  := third_party/libusbhsfs
# Documents: MuPDF (AGPL-3.0), built by tools/build_mupdf.sh. SMB shares: libsmb2 (LGPL-2.1), tools/build_libsmb2.sh.
LIBS     := -L$(TOPDIR)/$(USBHSFS)/lib -lusbhsfs -lntfs-3g -llwext4 \
            -L$(TOPDIR)/third_party/mupdf/lib -lmupdf -lmupdf-third \
            -L$(TOPDIR)/third_party/libsmb2/lib -lsmb2 \
            $(shell $(PKGCONF) --libs --static mpv sdl2) -lglad -lEGL -lglapi -ldrm_nouveau -lnx -lm
LIBDIRS  := $(PORTLIBS) $(LIBNX)

#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT  := $(CURDIR)/$(TARGET)
export TOPDIR  := $(CURDIR)
export VPATH   := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) $(foreach dir,$(DATA),$(CURDIR)/$(dir))
export DEPSDIR := $(CURDIR)/$(BUILD)

CFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
BINFILES := $(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

export LD := $(CXX)

export OFILES_BIN  := $(addsuffix .o,$(BINFILES))
export OFILES_SRC  := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES      := $(OFILES_BIN) $(OFILES_SRC)
export HFILES_BIN  := $(addsuffix .h,$(subst .,_,$(BINFILES)))

export INCLUDE := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                  $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                  -I$(CURDIR)/$(BUILD)

export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

ifeq ($(strip $(ICON)),)
    icons := $(wildcard *.jpg)
    ifneq (,$(findstring $(TARGET).jpg,$(icons)))
        export APP_ICON := $(TOPDIR)/$(TARGET).jpg
    else
        ifneq (,$(findstring icon.jpg,$(icons)))
            export APP_ICON := $(TOPDIR)/icon.jpg
        endif
    endif
else
    export APP_ICON := $(TOPDIR)/$(ICON)
endif

ifeq ($(strip $(NO_ICON)),)
    export NROFLAGS += --icon=$(APP_ICON)
endif

ifeq ($(strip $(NO_NACP)),)
    export NROFLAGS += --nacp=$(CURDIR)/$(TARGET).nacp
endif

.PHONY: $(BUILD) clean all

all: $(BUILD)

# source/font_data.c and source/codepages.c are generated from the typefaces in third_party/ and
# from Python's own tables, so they are built rather than kept in the repository.
GENERATED := source/font_data.c source/codepages.c
$(GENERATED): tools/gen_data.py $(wildcard third_party/*/*.ttf)
	@echo "generating embedded data ..."
	@python3 tools/gen_data.py

$(BUILD): $(GENERATED)
	@$(MAKE) --no-print-directory -C $(USBHSFS) BUILD_TYPE=GPL release
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).nro $(TARGET).nacp $(TARGET).elf $(GENERATED)

#---------------------------------------------------------------------------------
else
.PHONY: all

DEPENDS := $(OFILES:.o=.d)

# Patched mpv demuxer: replaces the same object in libmpv.a (see third_party/mpv039/README.md).
demux_mkv.o: CFLAGS += -std=c11 -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64 -w -I$(TOPDIR)/third_party/mpv039

# Patched libarchive readers with AES support, replacing the objects in libarchive.a.
ARCHIVE_READERS := archive_read_support_format_7zip.o archive_read_support_format_rar.o archive_read_support_format_rar5.o
$(ARCHIVE_READERS): CFLAGS += -std=gnu11 -DHAVE_CONFIG_H -D_FILE_OFFSET_BITS=64 -w -I$(TOPDIR)/third_party/libarchive372

all: $(OUTPUT).nro

$(OUTPUT).nro: $(OUTPUT).elf $(OUTPUT).nacp

$(OUTPUT).elf: $(OFILES)

$(OFILES_SRC): $(HFILES_BIN)

%.bin.o %_bin.h : %.bin
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPENDS)

#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------
