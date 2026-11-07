FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends gcc make libc6-dev libsqlite3-dev && rm -rf /var/lib/apt/lists/*
WORKDIR /app
COPY . .
WORKDIR /app/backend
RUN make
CMD ["./appointease"]
