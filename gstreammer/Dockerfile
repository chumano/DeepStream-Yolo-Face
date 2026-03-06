# Use Ubuntu 24.04 as base image
FROM ubuntu:24.04 as ubuntu24

# Set environment variables to avoid user interaction
ENV DEBIAN_FRONTEND=noninteractive

# Install system dependencies and GStreamer
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
    wget \
	&& rm -rf /var/lib/apt/lists/*
# print gst version
RUN gst-launch-1.0 --version


WORKDIR /app

# Set default command
CMD ["python3"]


FROM ubuntu:22.04 as ubuntu22


# Set environment variables to avoid user interaction
ENV DEBIAN_FRONTEND=noninteractive

# Install system dependencies and GStreamer
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
    wget \
	&& rm -rf /var/lib/apt/lists/*
# print gst version
RUN gst-launch-1.0 --version


WORKDIR /app

# Set default command
CMD ["python3"]
