# Stage 1: Build the C application
FROM gcc:13 AS builder

# Install SQLite development headers
RUN apt-get update && apt-get install -y --no-install-recommends \
    libsqlite3-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Copy project files
COPY . .

# Compile main.c into executable using Makefile or direct GCC command
# (Handles source files located in root or inside backend/ frontend/ directories)
RUN if [ -f Makefile ]; then make; else gcc -Wall -Wextra -O2 backend/main.c -o appointease -lsqlite3; fi

# Stage 2: Runtime image (minimal footprint)
FROM debian:bookworm-slim

# Install runtime sqlite3 library
RUN apt-get update && apt-get install -y --no-install-recommends \
    libsqlite3-0 \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Create backend & frontend structural folders expected by serve_static()
RUN mkdir -p backend frontend data

# Copy executable to backend directory so relative paths work
COPY --from=builder /app/appointease /app/backend/appointease

# Copy frontend assets to frontend directory
COPY index.html style.css app.js* app\ 2.js* /app/frontend/
RUN if [ -f /app/frontend/app\ 2.js ]; then mv /app/frontend/app\ 2.js /app/frontend/app.js; fi

# Set runtime working directory to backend/ (main.c expects to run from backend/)
WORKDIR /app/backend

# Render sets PORT env variable automatically (defaults to 8080)
ENV PORT=8080
EXPOSE 8080

CMD ["./appointease"]
