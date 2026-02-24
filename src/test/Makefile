# ─────────────────────────────────────────────────────────────────
#  Makefile for deepstream_app
#  Tested with DeepStream 6.x / 7.x on Jetson and dGPU platforms
# ─────────────────────────────────────────────────────────────────

APP       := main
SRCS      := main.c
TEST_APP  := test
TEST_SRCS := test.c
DEBUG	?= 1

# ── DeepStream & SDK paths (override if installed elsewhere) ─────
DS_SDK_ROOT   ?= /opt/nvidia/deepstream/deepstream
CUDA_VER      ?= 12.6
CUDA_ROOT     ?= /usr/local/cuda-$(CUDA_VER)

# ── pkg-config supplied flags ────────────────────────────────────
PKGS := gstreamer-1.0 gstreamer-video-1.0 glib-2.0

PKG_CFLAGS  := $(shell pkg-config --cflags $(PKGS))
PKG_LIBS    := $(shell pkg-config --libs   $(PKGS))

# ── Include paths ────────────────────────────────────────────────
CFLAGS := -Wall -O2 \
           $(PKG_CFLAGS) \
           -I$(DS_SDK_ROOT)/sources/includes \
           -I$(CUDA_ROOT)/include

# add debug flags
ifeq ($(DEBUG), 1)
	CFLAGS += -g -DDEBUG
endif

# ── Library paths & libraries ────────────────────────────────────
LDFLAGS := -L$(DS_SDK_ROOT)/lib \
            -L$(CUDA_ROOT)/lib64 \
            -L/usr/lib64 \
            $(PKG_LIBS) \
            -lnvdsgst_meta \
            -lnvds_meta \
            -lnvdsgst_helper \
            -lnvbufsurface \
            -lnvbufsurftransform \
            -lnvds_batch_jpegenc \
            -lcudart \
            -lcuda \
            -lm \
            -Wl,-rpath,$(DS_SDK_ROOT)/lib

# ── Build rules ──────────────────────────────────────────────────
.PHONY: all clean run clean_images debug run_test debug_test

all: $(APP) $(TEST_APP)

$(APP): $(SRCS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)
	@echo "Build successful → $(APP)"

$(TEST_APP): $(TEST_SRCS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)
	@echo "Build successful → $(TEST_APP)"

run: $(APP)
	./$(APP) /app/videos/friends_short.mp4

debug: clean $(APP)
	gdb --args ./$(APP) /app/videos/friends_short.mp4
# run
# khi bị segmentation fault, nhập: bt

run_test: $(TEST_APP)
	./$(TEST_APP) 0 file:///app/videos/friends_short.mp4

debug_test: $(TEST_APP)
	gdb --args ./$(TEST_APP) 0 file:///app/videos/friends_short.mp4

clean_images: $(TEST_APP)
	- rm -r /app/outputs/crops/*

clean: clean_images
	$(RM) $(APP)
	$(RM) $(TEST_APP)
	@echo "Cleaned."