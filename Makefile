#---------------------------------------------------------------------------------
# PS3 M3U Player - PSL1GHT makefile
#---------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(PSL1GHT)),)
$(error "Please set PSL1GHT in your environment. export PSL1GHT=<path>")
endif

#---------------------------------------------------------------------------------
# Package settings (must come before ppu_rules)
#---------------------------------------------------------------------------------
TITLE		:=	M3U Player
APPID		:=	M3UP00001
CONTENTID	:=	UP0001-$(APPID)_00-0000000000000000
ICON0		:=	pkgfiles/ICON0.PNG
SFOXML		:=	sfo.xml

include $(PSL1GHT)/ppu_rules

#---------------------------------------------------------------------------------
TARGET		:=	M3UPlayer
BUILD		:=	build
SOURCES		:=	source
DATA		:=	data
INCLUDES	:=	include
PKGFILES	:=	$(CURDIR)/pkgfiles

#---------------------------------------------------------------------------------
# libraries (order matters for static linking)
#---------------------------------------------------------------------------------
LIBS		:=	-lavformat -lavcodec -lswscale -lswresample -lavutil \
				-lcurl -lpolarssl -lfreetype -lz \
				-ltiny3d -lrsx -lsimdmath -lgcm_sys \
				-lnetctl -lnet -lio -lsysutil -lsysmodule -lsysfs -laudio \
				-lrt -llv2 -lm

CFLAGS		=	-O2 -Wall -mcpu=cell -std=gnu99 $(MACHDEP) $(INCLUDE)
CXXFLAGS	=	$(CFLAGS)
LDFLAGS		=	$(MACHDEP) -Wl,-Map,$(notdir $@).map

LIBDIRS		:=

#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT	:=	$(CURDIR)/$(TARGET)
export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
					$(foreach dir,$(DATA),$(CURDIR)/$(dir))
export DEPSDIR	:=	$(CURDIR)/$(BUILD)
export BUILDDIR	:=	$(CURDIR)/$(BUILD)

CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
BINFILES	:=	$(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.bin)))

export LD	:=	$(CC)

export OFILES	:=	$(addsuffix .o,$(BINFILES)) $(CFILES:.c=.o)

export INCLUDE	:=	$(foreach dir,$(INCLUDES), -I$(CURDIR)/$(dir)) \
					$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
					$(LIBPSL1GHT_INC) \
					-I$(PORTLIBS)/include/freetype2 \
					-I$(CURDIR)/$(BUILD) -I$(PORTLIBS)/include

export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib) \
					$(LIBPSL1GHT_LIB) -L$(PORTLIBS)/lib

.PHONY: $(BUILD) clean pkg

#---------------------------------------------------------------------------------
$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(OUTPUT).elf $(OUTPUT).self $(OUTPUT).pkg $(OUTPUT).gnpdrm.pkg

run:
	ps3load $(OUTPUT).self

pkg:	$(BUILD) $(OUTPUT).pkg

#---------------------------------------------------------------------------------
else

DEPENDS	:=	$(OFILES:.o=.d)

$(OUTPUT).self: $(OUTPUT).elf
$(OUTPUT).elf:	$(OFILES)

%.bin.o	:	%.bin
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPENDS)

endif
#---------------------------------------------------------------------------------
