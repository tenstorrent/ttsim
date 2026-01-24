# Force AMD64 platform
FROM --platform=linux/amd64 ghcr.io/tenstorrent/tt-metal/tt-metalium-ubuntu-22.04-release-models-amd64:latest

ENV TT_METAL_SIMULATOR_HOME=/work/sim \
    TT_METAL_SIMULATOR=/work/sim/libttsim.so \
    TT_METAL_SLOW_DISPATCH_MODE=1 \
    TT_METAL_HOME=/tt-metal

# TTSim configuration arguments
ARG TTSIM_VERSION=v1.3.1
ARG BUILD_SUFFIX=release_bh
ARG ARCH_NAME=blackhole

ENV ARCH_NAME=${ARCH_NAME}

RUN apt-get update && apt-get install -y --no-install-recommends \
    wget \
    nano \
    vim \
    && rm -rf /var/lib/apt/lists/*

# Set working directory
WORKDIR /tt-metal

# Download TTSim, copy SOC descriptor, and create directories in one layer
RUN mkdir -p ${TT_METAL_SIMULATOR_HOME} \
    && ASSET_SUFFIX="${BUILD_SUFFIX#release_}" \
    && if [ "${BUILD_SUFFIX}" = "release_wh" ]; then SOC_DESC="wormhole_b0_80_arch.yaml"; \
    elif [ "${BUILD_SUFFIX}" = "release_bh" ]; then SOC_DESC="blackhole_140_arch.yaml"; \
    else SOC_DESC="wormhole_b0_80_arch.yaml"; fi \
    && echo "Downloading TTSim for architecture: ${ASSET_SUFFIX}" \
    && wget -q https://github.com/tenstorrent/ttsim/releases/download/${TTSIM_VERSION}/libttsim_${ASSET_SUFFIX}.so \
    -O ${TT_METAL_SIMULATOR_HOME}/libttsim.so \
    && chmod +x ${TT_METAL_SIMULATOR_HOME}/libttsim.so \
    && echo "Copying SOC descriptor: ${SOC_DESC}" \
    && cp ${TT_METAL_HOME}/tt_metal/soc_descriptors/${SOC_DESC} \
    ${TT_METAL_SIMULATOR_HOME}/soc_descriptor.yaml

# Currently, this doesn't work, added for future reference
RUN wget -q https://github.com/tenstorrent/tt-kmd/releases/download/ttkmd-2.6.0/tenstorrent-dkms_2.6.0_all.deb \
    && dpkg -i tenstorrent-dkms_2.6.0_all.deb || apt-get install -f -y \
    && rm tenstorrent-dkms_2.6.0_all.deb

RUN ./build_metal.sh

# Install the debugger
RUN scripts/install_debugger.sh && \
    pip install -r tools/triage/requirements.txt && \
    apt-get update -y && \
    apt-get install gdb -y

CMD ["/bin/bash"]
