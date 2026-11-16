# tini built from source, as in ArmoniK.Core: the release binary buffers its own output,
# so its -vv logs are delayed, or lost when the container is SIGKILLed
FROM alpine:3.24.3 AS tini-build
RUN apk add gcc cmake make musl-dev
ADD https://github.com/krallin/tini.git .

# By default, tini uses buffered IO. Disable buffering to have logs in real-time.
# The check fails the build if the anchor matched no line, or several, in a future tini.c
RUN sed -i '/int main/a setvbuf(stdout, NULL, _IOLBF, BUFSIZ);setvbuf(stderr, NULL, _IOLBF, BUFSIZ);' src/tini.c && \
    [ "$(grep -c 'setvbuf(stdout' src/tini.c)" -eq 1 ]
RUN cmake . && make

FROM registry.access.redhat.com/ubi8 AS builder

ARG PARALLEL_JOBS=4

RUN rpm -ivh https://dl.fedoraproject.org/pub/epel/epel-release-latest-8.noarch.rpm

RUN yum --disableplugin=subscription-manager update -y && \
    yum --disableplugin=subscription-manager install -y \
    make \
    cmake \
    git \
    gcc \
    gcc-c++ \
    wget \
    rpm-build \
    libcurl-devel \
    simdjson-devel \
    re2-devel \
    zlib-devel \
    openssl-devel

RUN yum --disableplugin=subscription-manager clean all

RUN git clone --depth 1 https://github.com/nlohmann/json.git -b v3.11.3 /tmp/nlohmann-json && \
    cmake -S /tmp/nlohmann-json -B /tmp/nlohmann-json/build \
        -DCMAKE_INSTALL_PREFIX=/usr/local \
        -DJSON_BuildTests=OFF && \
    cmake --build /tmp/nlohmann-json/build --target install && \
    rm -rf /tmp/nlohmann-json

WORKDIR /tmp

# Fetch aneo's grpc rpm packages, in our repo we append an extra .x to the grpc version to distinguish several builds of the
# same version.
ARG GRPC_VERSION=1.62.2
ARG GRPC_BUILD=${GRPC_VERSION}.0

RUN wget "https://github.com/aneoconsulting/grpc-rpm/releases/download/${GRPC_BUILD}/grpc-${GRPC_BUILD%.*}-1.el8.x86_64.rpm" && \
    rpm -ivh "grpc-${GRPC_BUILD%.*}-1.el8.x86_64.rpm"

RUN wget "https://github.com/aneoconsulting/grpc-rpm/releases/download/${GRPC_BUILD}/grpc-devel-${GRPC_BUILD%.*}-1.el8.x86_64.rpm" && \
    rpm -ivh "grpc-devel-${GRPC_BUILD%.*}-1.el8.x86_64.rpm"

# Default value read from tools/common.sh file
ARG API_VERSION
RUN wget "https://github.com/aneoconsulting/ArmoniK.Api/releases/download/${API_VERSION}/libarmonik-${API_VERSION}-Linux.rpm" \
         "https://github.com/aneoconsulting/ArmoniK.Api/releases/download/${API_VERSION}/libarmonik-devel-${API_VERSION}-Linux.rpm" && \
    rpm -ivh "libarmonik-${API_VERSION}-Linux.rpm" "libarmonik-devel-${API_VERSION}-Linux.rpm"

RUN rm -rf *.rpm

# Copy the application source files into the image
WORKDIR /app/source
COPY tools/packaging/common/. ./tools/packaging/common/
COPY ./ArmoniK.SDK.Common ./ArmoniK.SDK.Common
COPY ./ArmoniK.SDK.Worker ./ArmoniK.SDK.Worker
COPY ./ArmoniK.SDK.DynamicWorker ./ArmoniK.SDK.DynamicWorker
COPY ./CMakeLists.txt ./
COPY ./Utils.cmake ./
COPY ./Packaging.cmake ./

WORKDIR /app/builder/worker
ARG WORKER_VERSION
RUN cmake -DCMAKE_INSTALL_PREFIX=/app/install \
    -DCMAKE_PREFIX_PATH=/usr/local/grpc \
    -DARMONIK_API_DIR=/armonik/api \
    -DBUILD_CLIENT=OFF \
    -DBUILD_DYNAMICWORKER=ON \
    -DBUILD_END2END=OFF \
    -DVERSION="${WORKER_VERSION}" \
    /app/source/ && make -j ${PARALLEL_JOBS} && make install && make clean

# Start with the latest Alpine base image for the final stage
FROM registry.access.redhat.com/ubi8 AS runner
# tini runs as PID 1 and forwards SIGTERM to the worker: the kernel drops a signal sent to a
# namespace init that has no handler for it, so the worker would otherwise only stop on SIGKILL
COPY --from=tini-build /tini-static /tini

RUN rpm -ivh https://dl.fedoraproject.org/pub/epel/epel-release-latest-8.noarch.rpm

RUN yum --disableplugin=subscription-manager update -y && \
    yum --disableplugin=subscription-manager install -y \
    re2 \
    zlib \
    wget \
    openssl

RUN yum --disableplugin=subscription-manager clean all

WORKDIR /tmp
RUN wget https://github.com/aneoconsulting/grpc-rpm/releases/download/1.62.2.0/grpc-1.62.2-1.el8.x86_64.rpm && \
    rpm -ivh grpc-1.62.2-1.el8.x86_64.rpm && rm grpc-1.62.2-1.el8.x86_64.rpm

# ArmoniK.Api ships shared libraries (libArmoniK.Api.*.so), so the runtime rpm must be installed here too,
# not just in the builder stage, or the worker fails to start with a missing shared library error.
ARG API_VERSION
RUN wget "https://github.com/aneoconsulting/ArmoniK.Api/releases/download/${API_VERSION}/libarmonik-${API_VERSION}-Linux.rpm" && \
    rpm -ivh "libarmonik-${API_VERSION}-Linux.rpm" && rm "libarmonik-${API_VERSION}-Linux.rpm"

RUN adduser -d /home/armonikuser -u 5000 -U --shell /bin/sh armonikuser
USER armonikuser

# Copy the application files, libraries, and binaries from the builder image to the final image
COPY --from=builder /app/install/. /app/install/.

# Update the PATH environment variable to include the application libraries and binaries
ENV LD_LIBRARY_PATH="/app/install/lib:$LD_LIBRARY_PATH"
ENV PATH="/app/install/bin:$PATH"

# Set the entrypoint for the application's test executable
# This is the command that will be executed when the container is run
WORKDIR /app/install/bin
ENTRYPOINT ["/tini", "-s", "-vv", "--", "./ArmoniK.SDK.DynamicWorker"]
