################################################################################
#
# ilrdos-vosk — libvosk pré-compilada + modelo PT-BR pequeno (Vosk, Apache-2.0)
#
################################################################################

ILRDOS_VOSK_VERSION = 0.3.45
ILRDOS_VOSK_SITE = https://github.com/alphacep/vosk-api/releases/download/v$(ILRDOS_VOSK_VERSION)
ILRDOS_VOSK_SOURCE = vosk-linux-aarch64-$(ILRDOS_VOSK_VERSION).zip
ILRDOS_VOSK_EXTRA_DOWNLOADS = https://alphacephei.com/vosk/models/vosk-model-small-pt-0.3.zip
ILRDOS_VOSK_LICENSE = Apache-2.0
ILRDOS_VOSK_INSTALL_STAGING = YES

define ILRDOS_VOSK_INSTALL_STAGING_CMDS
	$(INSTALL) -D -m 0644 $(@D)/vosk_api.h $(STAGING_DIR)/usr/include/vosk_api.h
	$(INSTALL) -D -m 0755 $(@D)/libvosk.so $(STAGING_DIR)/usr/lib/libvosk.so
endef

define ILRDOS_VOSK_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/libvosk.so $(TARGET_DIR)/usr/lib/libvosk.so
	mkdir -p $(TARGET_DIR)/usr/share/ilrdos/models
	cd $(TARGET_DIR)/usr/share/ilrdos/models && \
		unzip -oq $(ILRDOS_VOSK_DL_DIR)/vosk-model-small-pt-0.3.zip && \
		rm -rf vosk-pt && mv vosk-model-small-pt-0.3 vosk-pt
endef

$(eval $(generic-package))
