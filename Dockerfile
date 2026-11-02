# ---- build ----
FROM ubuntu:24.04 AS build
RUN apt-get update && apt-get install -y --no-install-recommends g++ cmake make && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt .
COPY include include
COPY src src
COPY tests tests
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j2 && ./build/FunctionalTests

# ---- run ----
FROM ubuntu:24.04
RUN useradd -m minidb
WORKDIR /app
COPY --from=build /src/build/MiniDB /app/MiniDB
COPY web /app/web
RUN mkdir -p /app/data && chown -R minidb /app
USER minidb
ENV MINIDB_DATA_DIR=/app/data MINIDB_WEB_DIR=/app/web MINIDB_TCP_PORT=0
EXPOSE 8081
CMD ["/app/MiniDB"]
