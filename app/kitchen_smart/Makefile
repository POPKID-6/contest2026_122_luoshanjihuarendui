############################################################################
# vendor/allwinnertech/apps/kitchen_smart/Makefile  (v2)
############################################################################

include $(APPDIR)/Make.defs
include $(APPDIR)/vendor/allwinnertech/Common.mk

MAINSRC  = kitchen_smart.c
CSRCS   += kitchen_wifi.c kitchen_mqtt.c kitchen_audio.c

PROGNAME = $(CONFIG_KITCHEN_SMART_PROGNAME)
PRIORITY = $(CONFIG_KITCHEN_SMART_PRIORITY)
STACKSIZE = $(CONFIG_KITCHEN_SMART_STACKSIZE)
MODULE   = $(CONFIG_KITCHEN_SMART)

$(eval $(AW_ADD_HAL_INCLUDES))

include $(APPDIR)/Application.mk
