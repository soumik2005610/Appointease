FROM debian:bookworm-slim

# Install C compiler, make, and SQLite header dependencies
RUN apt-get update && apt-get install -y --no-install-recommends gcc make libc6-dev libsqlite3-dev && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Copy all project files into /app
COPY . .

# Compile the C backend inside the backend folder
RUN cd backend && make

# Move compiled executable and frontend assets into the root /app folder
RUN cp backend/appointease . && cp frontend/* .

# Start the application from the root folder /app
CMD ["./appointease"]
