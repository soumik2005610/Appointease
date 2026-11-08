FROM debian:bookworm-slim

# Prevent interactive prompts during apt-get package installation
ENV DEBIAN_FRONTEND=noninteractive

# Install compiler toolchain and sqlite3 development headers
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    gcc \
    make \
    libsqlite3-dev \
 && rm -rf /var/lib/apt/lists/*

# Set working directory inside container
WORKDIR /app

# Copy repository files into container
COPY . .

# Compile backend binary inside the backend folder
RUN cd backend && make clean && make

# Copy backend binary and static frontend assets into app root
RUN cp backend/appointease . && cp -r frontend/* .

EXPOSE 8080

CMD ["./appointease"]
