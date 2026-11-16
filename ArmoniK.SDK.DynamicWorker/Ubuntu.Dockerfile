# tini built from source, as in ArmoniK.Core: the release binary buffers its own output,
# so its -vv logs are delayed, or lost when the container is SIGKILLed
FROM alpine:3.24.3 AS tini-build
RUN apk add gcc cmake make musl-dev
ADD https://github.com/krallin/tini.git .

# By default, tini uses buffered IO. Disable buffering to have logs in real-time
RUN sed -i '/int main/a setvbuf(stdout, NULL, _IOLBF, BUFSIZ);setvbuf(stderr, NULL, _IOLBF, BUFSIZ);' src/tini.c
RUN cmake . && make

# Start with the latest Alpine base image for the build stage
FROM ubuntu:24.04 AS builder

# Install all the necessary dependencies required for the build process
# These include tools and libraries for building and compiling the source code
RUN apt-get update && DEBIAN_FRONTEND="noninteractive" TZ="Europe/London" apt-get install -y \
    gcc \
    g++ \
    gdb \
    make \
    ninja-build \
    cmake \
    autoconf \
    automake \
    locales-all \
    build-essential \
    libc-ares-dev \
    protobuf-compiler-grpc \
    libgrpc-dev \
    libgrpc++-dev \
    libprotobuf-dev \
    nlohmann-json3-dev \
    git \
    && apt-get clean

# Update the PATH environment variable to include the gRPC libraries and binaries
ENV LD_LIBRARY_PATH="/app/install/lib:$LD_LIBRARY_PATH"
ENV PATH="/app/install/bin:$PATH"

# Display the updated PATH environment variable
RUN echo $PATH

# Get and install ArmoniK api into the image
WORKDIR /tmp
ARG API_VERSION
RUN git clone https://github.com/aneoconsulting/ArmoniK.Api.git -b "${API_VERSION}" && \
    cd ArmoniK.Api/packages/cpp && \
    mkdir -p /app/proto && \
    mkdir -p /armonik/api && \
    cp -r ../../Protos/V1/* /app/proto && \
    mkdir -p build/ && \
    cd build/ && \
    cmake "-DCMAKE_INSTALL_PREFIX=/armonik/api" "-DBUILD_TEST=OFF" "-DBUILD_CLIENT=OFF" "-DBUILD_WORKER=ON" .. && \
    make -j $(nproc) install && \
    make clean


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
RUN cmake "-DCMAKE_INSTALL_PREFIX=/app/install" "-DBUILD_CLIENT=OFF" "-DBUILD_DYNAMICWORKER=ON" "-DBUILD_END2END=OFF" -DVERSION="${WORKER_VERSION}" /app/source/ && make -j $(nproc) install && make clean

# Start with the latest Alpine base image for the final stage
FROM ubuntu:24.04 AS runner
# tini runs as PID 1 and forwards SIGTERM to the worker: the kernel drops a signal sent to a
# namespace init that has no handler for it, so the worker would otherwise only stop on SIGKILL
COPY --from=tini-build /tini-static /tini
# Install all the necessary dependencies required for the build process
# These include tools and libraries for building and compiling the source code
RUN apt-get update && DEBIAN_FRONTEND="noninteractive" TZ="Europe/London" apt-get install -y \
    libc-ares-dev \
    libgrpc-dev \
    libgrpc++-dev \
    libprotobuf-dev \
    && apt-get clean
	
# Create a non-root user and group for running the application
# This is a security best practice to avoid running applications as the root user
RUN groupadd --gid 5000 armonikuser && useradd --home-dir /home/armonikuser --create-home --uid 5000 --gid 5000 --shell /bin/sh --skel /dev/null armonikuser && mkdir /cache && chown armonikuser: /cache
USER armonikuser

# Copy the application files, libraries, and binaries from the builder image to the final image
COPY --from=builder /app/install/bin /app/install/bin
COPY --from=builder /app/install/lib /app/install/lib
COPY --from=builder /app/install/include /app/install/include

# Update the PATH environment variable to include the application libraries and binaries
ENV LD_LIBRARY_PATH="/app/install/lib:$LD_LIBRARY_PATH"
ENV PATH="/app/install/bin:$PATH"

# Set the entrypoint for the application's test executable
# This is the command that will be executed when the container is run
WORKDIR /app/install/bin
ENTRYPOINT ["/tini", "-s", "-vv", "--", "./ArmoniK.SDK.DynamicWorker"]
