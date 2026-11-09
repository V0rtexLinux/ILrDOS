################################################################################
#
# ilrdos
#
################################################################################

ILRDOS_VERSION = 2.0.0
ILRDOS_SITE = $(BR2_EXTERNAL_ILRDOS_PATH)/..
ILRDOS_SITE_METHOD = local
ILRDOS_LICENSE = Proprietary
ILRDOS_DEPENDENCIES = host-pkgconf openssl opencv4 alsa-lib

ILRDOS_CXXFLAGS = -std=c++20 -O2 -Wall -Wextra -fstack-protector-strong \
	-pthread -DILRDOS_HAVE_OPENCV=1 -DILRDOS_HAVE_ALSA=1

ifeq ($(BR2_PACKAGE_ILRDOS_VOSK),y)
ILRDOS_DEPENDENCIES += ilrdos-vosk
ILRDOS_CXXFLAGS += -DILRDOS_HAVE_VOSK=1
ILRDOS_VOSK_LIBS = -lvosk
endif

define ILRDOS_BUILD_CMDS
	mkdir -p $(@D)/build
	$(TARGET_CXX) $(TARGET_CXXFLAGS) $(ILRDOS_CXXFLAGS) \
		$$($(PKG_CONFIG_HOST_BINARY) --cflags opencv4) \
		$(@D)/src/robotd.cpp \
		$(TARGET_LDFLAGS) -lcrypto -lasound $(ILRDOS_VOSK_LIBS) \
		$$($(PKG_CONFIG_HOST_BINARY) --libs opencv4) \
		-o $(@D)/build/ilrdosd
endef

define ILRDOS_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/build/ilrdosd $(TARGET_DIR)/usr/sbin/ilrdosd
	$(INSTALL) -D -m 0644 $(@D)/models/face_detection_yunet_2023mar.onnx \
		$(TARGET_DIR)/usr/share/ilrdos/models/face_detection_yunet_2023mar.onnx
	$(INSTALL) -D -m 0644 $(@D)/models/LICENSE \
		$(TARGET_DIR)/usr/share/ilrdos/models/LICENSE
	if [ -f $(@D)/models/face_recognition_sface_2021dec.onnx ]; then \
		$(INSTALL) -D -m 0644 $(@D)/models/face_recognition_sface_2021dec.onnx \
			$(TARGET_DIR)/usr/share/ilrdos/models/face_recognition_sface_2021dec.onnx; \
	else \
		echo "AVISO: face_recognition_sface_2021dec.onnx ausente; rode scripts/fetch-models.sh"; \
	fi
endef

$(eval $(generic-package))
