# syntax=docker/dockerfile:1.7
FROM debian:bookworm-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake ca-certificates \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt ./
COPY include ./include
COPY src ./src
COPY tests ./tests
RUN cmake -B build -DCMAKE_BUILD_TYPE=Release \
 && cmake --build build -j$(nproc) \
 && ctest --test-dir build --output-on-failure

FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates libstdc++6 \
 && rm -rf /var/lib/apt/lists/* \
 && useradd -u 10001 -m ig
USER ig
WORKDIR /srv
COPY --from=build /src/build/inferenceg   /usr/local/bin/inferenceg
COPY --from=build /src/build/fakebackend  /usr/local/bin/fakebackend
COPY --from=build /src/build/loadgen      /usr/local/bin/loadgen
COPY --chown=ig:ig web /srv/web
EXPOSE 9090
HEALTHCHECK --interval=10s --timeout=2s --retries=3 \
  CMD wget -qO- http://127.0.0.1:9090/healthz || exit 1
ENTRYPOINT ["/usr/local/bin/inferenceg"]
CMD ["--port", "9090"]
