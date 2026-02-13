ARG DEEPSTREAM_VERSION=7.1



ARG BASE_IMAGE=nvcr.io/nvidia/deepstream:${DEEPSTREAM_VERSION}-gc-triton-devel

FROM ${BASE_IMAGE}

# Metadata labels following OCI image spec
LABEL org.opencontainers.image.title="DeepStream-Yolo-Face" \
      org.opencontainers.image.description="NVIDIA DeepStream SDK application for YOLO-Face models" \
      org.opencontainers.image.source="https://github.com/your-repo/DeepStream-Yolo-Face" \
      org.opencontainers.image.licenses="MIT"


# Set environment variables
ENV DEBIAN_FRONTEND=noninteractive \
    DS_SDK_ROOT=/opt/nvidia/deepstream/deepstream \
    CUDA_VER=${CUDA_VER} \
    # Prevent Python from writing pyc files and buffering stdout/stderr
    PYTHONDONTWRITEBYTECODE=1 \
    PYTHONUNBUFFERED=1

# Install additional dependencies in a single layer
# Sort packages alphabetically for better readability and cache efficiency
# Note: librivermax.so.1 (NVIDIA Rivermax SDK) is optional for UDP streaming
# Install from https://developer.nvidia.com/networking/rivermax if needed
RUN --mount=type=cache,target=/var/cache/apt,sharing=locked \
    --mount=type=cache,target=/var/lib/apt,sharing=locked \
    apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    git \
    gstreamer1.0-libav \
    gstreamer1.0-plugins-bad \
    gstreamer1.0-plugins-base \
    gstreamer1.0-plugins-good \
    gstreamer1.0-plugins-ugly \
    gstreamer1.0-tools \
    libgstreamer1.0-dev \
    libgstreamer-plugins-base1.0-dev \
    libgstrtspserver-1.0-dev \
    libmpg123-0 \
    pkg-config \
    python3-dev \
    python3-gi \
    python3-gst-1.0 \
    python3-pip \
    wget

# Install other apt dependencies 
RUN --mount=type=cache,target=/var/cache/apt,sharing=locked \
    --mount=type=cache,target=/var/lib/apt,sharing=locked \
    apt-get update && apt install -y \
        libvpx-dev \
        libx264-dev \
        libx265-dev \
        libflac-dev \
        libmpg123-dev \
        libmp3lame-dev \
        libdca-dev \
        libdvdread-dev \
        libdvdnav-dev \
        libmjpegtools-dev

# Set working directory
WORKDIR /app/DeepStream-Yolo-Face

# Create directory for models early (can be used as mount point)
RUN mkdir -p /app/models

# Re-declare ARGs after FROM (they go out of scope)
ARG PYDS_VERSION=1.2.0
ARG PYTHON_VERSION=cp310
ARG CUDA_VER=12.6
ENV CUDA_VER=${CUDA_VER}

RUN --mount=type=cache,target=/root/.cache/pip \
    echo "Attempting to install pyds from pre-built wheel..." && \
    (wget -q https://github.com/NVIDIA-AI-IOT/deepstream_python_apps/releases/download/v${PYDS_VERSION}/pyds-${PYDS_VERSION}-${PYTHON_VERSION}-${PYTHON_VERSION}-linux_x86_64.whl -O /tmp/pyds-${PYDS_VERSION}-${PYTHON_VERSION}-${PYTHON_VERSION}-linux_x86_64.whl && \
     pip3 install /tmp/pyds-${PYDS_VERSION}-${PYTHON_VERSION}-${PYTHON_VERSION}-linux_x86_64.whl && \
     echo "✓ pyds installed successfully from wheel") 
#https://github.com/NVIDIA-AI-IOT/deepstream_python_apps/releases/download/v1.2.0/pyds-1.2.0-cp310-cp310-linux_x86_64.whl

RUN pip3 install kafka-python==2.3.0 numpy==1.26.4 opencv-python==4.11.0.86

# Copy only build files first for better layer caching
COPY src/Makefile ./
COPY src/nvdsinfer_custom_impl_Yolo_face/ ./nvdsinfer_custom_impl_Yolo_face/

# Build the custom parser library
RUN make -C nvdsinfer_custom_impl_Yolo_face clean && \
    make -C nvdsinfer_custom_impl_Yolo_face CUDA_VER=${CUDA_VER}

# Copy remaining project files
COPY configs/config_infer_primary_*.txt configs/labels.txt ./
COPY src/*.c ./
COPY src/*.h ./
COPY src/*.py ./
COPY src/modules/ ./modules/

# Build the main application
RUN make clean && make CUDA_VER=${CUDA_VER}

# Set library path and GStreamer plugin paths
ENV LD_LIBRARY_PATH=/app/DeepStream-Yolo-Face/nvdsinfer_custom_impl_Yolo_face:${DS_SDK_ROOT}/lib:${LD_LIBRARY_PATH} \
    GST_PLUGIN_PATH=${DS_SDK_ROOT}/lib/gst-plugins:/usr/lib/x86_64-linux-gnu/gstreamer-1.0:${GST_PLUGIN_PATH} \
    GST_PLUGIN_SCANNER=/usr/lib/x86_64-linux-gnu/gstreamer1.0/gstreamer-1.0/gst-plugin-scanner

# Create non-root user for security (optional, comment out if GPU access requires root)
# RUN useradd --create-home --shell /bin/bash appuser && \
#     chown -R appuser:appuser /app
# USER appuser

# Health check to verify the application is working
HEALTHCHECK --interval=30s --timeout=10s --start-period=5s --retries=3 \
    CMD python3 -c "import pyds" || exit 1

# Document exposed ports (if using RTSP streaming)
# EXPOSE 8554

# Use exec form for proper signal handling
ENTRYPOINT ["python3", "deepstream.py"]
CMD ["--help"]
