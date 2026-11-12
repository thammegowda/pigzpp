FROM ubuntu:22.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build nasm git ca-certificates python3 \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY build-in-container.sh /usr/local/bin/build-in-container.sh
RUN sed -i 's/\r$//' /usr/local/bin/build-in-container.sh && chmod +x /usr/local/bin/build-in-container.sh
ENTRYPOINT ["/usr/local/bin/build-in-container.sh"]
