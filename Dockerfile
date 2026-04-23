# Build/dev image for the T5 Video Processing server.
# Usage (from project root):
#   docker build -t vps-build .
#   docker run --rm -v /c/Users/adria/Documents/skeleton:/app -w /app vps-build make
FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive
ARG APT_OPT="-o Acquire::Check-Valid-Until=false -o Acquire::Check-Date=false"

RUN apt-get $APT_OPT update -qq && \
    apt-get $APT_OPT install -y -qq --no-install-recommends \
        build-essential gcc clang-tidy pkg-config make \
        libconfig-dev \
        libavformat-dev libavcodec-dev libavfilter-dev libavutil-dev \
        ffmpeg ca-certificates && \
    rm -rf /var/lib/apt/lists/*

WORKDIR /app
CMD ["bash"]
