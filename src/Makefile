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

SRCS_C:= detection_manager.c $(wildcard modules/*.c)
SRCS_CPP:= $(wildcard *.cpp)
SRCS:= $(SRCS_C) $(SRCS_CPP)

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

ifeq ($(DEBUG), 1)
	CFLAGS += -g -DDEBUG
endif

LOG_APP?=
ifeq ($(LOG_APP),)
	LOG_APP=deepstream
endif
LOG?=
ifeq ($(LOG),)
	LOG=4
endif

all: $(APP)

%.o: %.c $(INCS) Makefile
	$(CC) -c -o $@ $(CFLAGS) $<

%.o: %.cpp $(INCS) Makefile
	$(CXX) -c -o $@ $(CFLAGS) $<

$(APP): $(OBJS) Makefile
	$(CXX) -o $(APP) $(OBJS) $(LIBS)
	@echo "Build successful → $(APP) with debug=$(DEBUG) and Kafka support=$(KAFKA)"

clean: clean_images
	rm -rf $(OBJS) $(APP)

clean_images:
	- rm -r /app/outputs/frames/*
	- rm -r /app/outputs/faces/*
	- rm -r /app/outputs/annotated_frames/*

run: clean_images $(APP)
	GST_DEBUG=$(LOG_APP):$(LOG) ./$(APP) --config /app/configs/app.ini \
	-s file:///app/videos/faces_tracking.mp4

run_2infer: clean_images $(APP)
	GST_DEBUG=$(LOG_APP):$(LOG) ./$(APP) --config /app/configs/app2.ini \
	-s file:///app/videos/myface.mp4

run_my: clean_images $(APP)
	GST_DEBUG=$(LOG_APP):$(LOG) ./$(APP) --config /app/configs/app.ini \
	-s file:///app/videos/myface.mp4

debug: clean_images $(APP)
	GST_DEBUG=$(LOG_APP):$(LOG) gdb --args ./$(APP) --config /app/configs/app.ini \
	-s file:///app/videos/faces_tracking.mp4
	
debug2: clean_images $(APP)
	GST_DEBUG=$(LOG_APP):$(LOG) gdb --args ./$(APP) --config /app/configs/app.ini \
	-s file:///app/videos/faces_tracking.mp4 -s file:///app/videos/friends_short.mp4 --disable-display

run_triton: clean_images $(APP)
	GST_DEBUG=deepstream:$(LOG) ./$(APP) --config /app/configs/app_triton.ini

run2: clean_images $(APP)
	GST_DEBUG=deepstream:$(LOG) ./$(APP) --config /app/configs/app_multi.ini

run1: clean_images $(APP)
	GST_DEBUG=deepstream:$(LOG) ./$(APP) -s file:///app/videos/faces_tracking.mp4  \
	   	-c /app/configs/config_infer_primary_yoloV8_face.txt  \
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
