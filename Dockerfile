# Stage 1: Build the C binary
FROM gcc:13 AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    libsqlite3-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Copy full repository
COPY . .

# Compile appointease binary
RUN cd backend && make

# Stage 2: Runtime Container
FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
    libsqlite3-0 \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Create necessary folders
RUN mkdir -p /app/backend /app/frontend

# Copy binary into backend/
COPY --from=builder /app/backend/appointease /app/backend/appointease

# Copy frontend static files into frontend/
COPY frontend/ /app/frontend/

# Set working directory to /app/backend (so ../frontend/ points to /app/frontend/)
WORKDIR /app/backend

ENV PORT=8080
EXPOSE 8080

CMD ["./appointease"]
