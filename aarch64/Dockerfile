ARG ARCH=aarch64
ARG VERSION=12.10.0
ARG UBUNTU_VERSION=24.04
ARG REPO=axisecp
ARG SDK=acap-native-sdk

FROM ${REPO}/${SDK}:${VERSION}-${ARCH}-ubuntu${UBUNTU_VERSION}

RUN apt-get update && DEBIAN_FRONTEND=noninteractive \
    apt-get install -y --no-install-recommends \
        git cmake curl ca-certificates && \
    rm -rf /var/lib/apt/lists/*

# --- whisper.cpp: static CPU-only libs cross-compiled with the SDK toolchain
ARG WHISPER_VERSION=v1.7.5
WORKDIR /opt/build
RUN git clone --depth 1 --branch ${WHISPER_VERSION} \
        https://github.com/ggml-org/whisper.cpp.git
RUN . /opt/axis/acapsdk/environment-setup* && \
    cd whisper.cpp && \
    cmake -B build \
        -DCMAKE_SYSTEM_NAME=Linux \
        -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_SHARED_LIBS=OFF \
        -DGGML_NATIVE=OFF \
        -DGGML_OPENMP=OFF \
        -DWHISPER_BUILD_EXAMPLES=OFF \
        -DWHISPER_BUILD_TESTS=OFF && \
    cmake --build build -j"$(nproc)"

# --- Whisper model (bundled into the .eap)
# Whisper model bundled into the .eap. base.en gives clearly better accuracy
# than tiny.en on hard content (names, numbers, phonetic alphabet) at ~2x the
# CPU cost and still streams acceptably on ARTPEC-8/9. Swap to tiny.en for
# lower latency, or small.en for best accuracy (too slow for live captions).
# The app auto-detects whichever ggml-*.bin is bundled.
ARG MODEL=ggml-base.en-q5_1.bin
RUN mkdir -p /opt/model && \
    curl -fsSL -o /opt/model/${MODEL} \
        "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/${MODEL}"

# --- Building the ACAP application
COPY common/app /opt/app/
COPY aarch64/app/manifest.json /opt/app/manifest.json
WORKDIR /opt/app
RUN mkdir -p whisper/lib whisper/include && \
    cp $(find /opt/build/whisper.cpp/build -name 'lib*.a') whisper/lib/ && \
    cp /opt/build/whisper.cpp/include/whisper.h whisper/include/ && \
    cp /opt/build/whisper.cpp/ggml/include/*.h whisper/include/ && \
    cp /opt/model/${MODEL} .
RUN . /opt/axis/acapsdk/environment-setup* && \
    acap-build . -a ${MODEL}
