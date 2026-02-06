CUDA_VER?=
ifeq ($(CUDA_VER),)
	$(error "CUDA_VER is not set")
endif

CFLAGS:= -Wall -Wfatal-errors -fPIC -Wno-deprecated-declarations

APP:= deepstream

DS_SDK_ROOT:= /opt/nvidia/deepstream/deepstream

LIB_INSTALL_DIR?= $(DS_SDK_ROOT)/lib/

TARGET_DEVICE= $(shell gcc -dumpmachine | cut -f1 -d -)
ifeq ($(TARGET_DEVICE), aarch64)
	CFLAGS+= -DPLATFORM_TEGRA
endif

# Kafka support (optional) - build with: make KAFKA=1
KAFKA?=1
ifeq ($(KAFKA), 1)
	CFLAGS+= -DKAFKA_ENABLED_BUILD
	KAFKA_LIBS:= -lrdkafka
else
	KAFKA_LIBS:=
endif

SRCS:= $(wildcard *.c)
SRCS+= $(wildcard modules/*.c)

INCS:= $(wildcard *.h)
INCS+= $(wildcard modules/*.h)

PKGS:= gstreamer-1.0

OBJS:= $(addsuffix .o, $(basename $(SRCS)))

CFLAGS+= -I$(DS_SDK_ROOT)/sources/apps/apps-common/includes -I$(DS_SDK_ROOT)/sources/includes \
 	     -I/usr/local/cuda-$(CUDA_VER)/include

CFLAGS+= `pkg-config --cflags $(PKGS)`
LIBS:= `pkg-config --libs $(PKGS)`

LIBS+= -L$(LIB_INSTALL_DIR) -lnvdsgst_meta -lnvds_meta -lnvdsgst_helper -lnvbufsurftransform -lnvbufsurface -L/usr/local/cuda-$(CUDA_VER)/lib64/ -lcudart \
       -lcuda -Wl,-rpath,$(LIB_INSTALL_DIR) $(KAFKA_LIBS) -lm -ljpeg

all: $(APP)

%.o: %.c $(INCS) Makefile
	$(CC) -c -o $@ $(CFLAGS) $<

$(APP): $(OBJS) Makefile
	$(CC) -o $(APP) $(OBJS) $(LIBS)

clean: clean_images
	rm -rf $(OBJS) $(APP)

clean_images:
	rm ./outputs/frames/*

run: clean_images $(APP)
	GST_DEBUG=deepstream:4 ./$(APP) -s file:///app/videos/faces_tracking.mp4  \
	   	-c config_infer_primary_yoloV8_face.txt  \
		--kafka-broker kafka:29092  --kafka-topic face-detections

.PHONY: all clean help clean_images

help:
	@echo "DeepStream Face Detection Application"
	@echo ""
	@echo "Usage:"
	@echo "  make CUDA_VER=<version>           Build without Kafka support"
	@echo "  make CUDA_VER=<version> KAFKA=1   Build with Kafka support (requires librdkafka)"
	@echo "  make clean                        Clean build files"
	@echo ""
	@echo "Example:"
	@echo "  make CUDA_VER=12.6"
	@echo "  make CUDA_VER=12.6 KAFKA=1"
	@echo ""
	@echo "Prerequisites for Kafka support:"
	@echo "  sudo apt-get install librdkafka-dev"
