# =====================================================================
# Dockerfile to compile app_eval_tracer and app_hot_tracer
# Matches system configuration: Ubuntu 24.04 (Noble Numbat), GCC 13.3, Make 4.3
# =====================================================================

FROM ubuntu:24.04

# Prevent interactive prompts during installation
ENV DEBIAN_FRONTEND=noninteractive

# Install compilation tools matching the current host configuration
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    g++ \
    make \
    && rm -rf /var/lib/apt/lists/*

# Set up working directory inside the container
WORKDIR /workspace/tracer

# Define environment variable for Pin tool compilation
# This points to the mounted/copied Pin kit inside the container
ENV PIN_ROOT=/workspace/pin_kit

# Build instructions:
# To build the docker image:
#   docker build -t pin-tracer-compiler .
#
# To run the compiler using system paths via mounts:
#   docker run --rm \
#     -v /media/pravesh/Storage/code/sims/artHermes/tracer:/workspace/tracer \
#     -v /media/pravesh/Storage/code/sims/artVictima/sniper/pin_kit:/workspace/pin_kit \
#     pin-tracer-compiler
#
# This will output:
#   - obj-intel64/app_eval_tracer.so
#   - obj-intel64/app_hot_tracer.so
# in your local /media/pravesh/Storage/code/sims/artHermes/tracer/ directory.

CMD ["make", "obj-intel64/app_eval_tracer.so", "obj-intel64/app_hot_tracer.so"]
