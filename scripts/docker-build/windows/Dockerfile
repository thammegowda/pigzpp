FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build nasm git ca-certificates python3 \
        mingw-w64 \
    && rm -rf /var/lib/apt/lists/* \
    && update-alternatives --set x86_64-w64-mingw32-gcc /usr/bin/x86_64-w64-mingw32-gcc-posix \
    && update-alternatives --set x86_64-w64-mingw32-g++ /usr/bin/x86_64-w64-mingw32-g++-posix
WORKDIR /src
COPY build-in-container.sh /usr/local/bin/build-in-container.sh
RUN sed -i 's/\r$//' /usr/local/bin/build-in-container.sh && chmod +x /usr/local/bin/build-in-container.sh
ENTRYPOINT ["/usr/local/bin/build-in-container.sh"]
