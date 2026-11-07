# AppointEase
Hospital Appointment Management System. It manages doctors, patients and appointments, and blocks double-bookings.

**Tech:** C (backend server) · SQLite · HTML · CSS · JavaScript

## Structure
- `backend/` - main.c and Makefile (C web server + SQLite database)
- `frontend/` - index.html, style.css, app.js

## Run (Linux, WSL or GitHub Codespaces)
    sudo apt install libsqlite3-dev
    cd backend
    make
    ./appointease

Then open http://localhost:8080

## How conflicts are blocked
Booking the same doctor at the same date and time returns an error (HTTP 409). The database also has a UNIQUE rule on (doctor, date, time).
