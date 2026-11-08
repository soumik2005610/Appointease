# Stage 1: Build the C binary
FROM gcc:13 AS builder

# Install SQLite development headers
RUN apt-get update && apt-get install -y --no-install-recommends \
    libsqlite3-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Copy the entire repository
COPY . .

# Build the C executable from backend directory
RUN cd backend && make

# Stage 2: Minimal runtime image
FROM debian:bookworm-slim

# Install runtime sqlite3 library
RUN apt-get update && apt-get install -y --no-install-recommends \
    libsqlite3-0 \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Recreate directory structure
RUN mkdir -p backend frontend

# Copy compiled executable and frontend files
COPY --from=builder /app/backend/appointease /app/backend/appointease
COPY frontend/ /app/frontend/

# Set working directory to backend (main.c looks for static files at ../frontend/)
WORKDIR /app/backend

ENV PORT=8080
EXPOSE 8080

CMD ["./appointease"]
