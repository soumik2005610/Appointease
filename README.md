# AppointEase

Hospital management system with separate logins for staff, doctors and patients.

Tech: C (backend server) · SQLite · HTML · CSS · JavaScript

## What each person can do

| Role | Can do |
| --- | --- |
| Patient | Register and log in, book their own OPD appointments and X-Ray / CT / MRI slots, see and cancel only their own bookings, view bed availability and their own admission |
| Doctor | See only their own appointments (with patient name and phone), mark them completed, view admitted patients and bed availability |
| Staff | Everything: dashboard, add and remove doctors (each gets a login), manage patients, book for anyone, admit and discharge |

## Demo logins (loaded automatically)

| Role | Username | Password |
| --- | --- | --- |
| Staff | `admin` | `admin123` |
| Doctor | `dr.rao`, `dr.das`, `dr.biswas` | `doctor123` |
| Patient | `riya`, `amit` | `patient123` |

New patients can create their own account from the first screen.

## Rules enforced by the C server
- Every API call checks who is logged in; the page only hides what a user may not use.
- A doctor or a service slot cannot be booked twice at the same date and time (HTTP 409), also guarded by a UNIQUE rule in the database.
- A patient cannot be in two places at once or admitted twice; a full ward refuses admissions.
- Passwords are stored salted and hashed (SHA-256, 10,000 rounds), never in plain text. Logins last 12 hours and use an HttpOnly cookie.
- Only the three frontend files can be downloaded from the server.

## Structure
- `backend/` - main.c and Makefile (C web server + SQLite database)
- `frontend/` - index.html, style.css, app.js
- `Dockerfile` - used to deploy on Render

## Run (Linux, WSL or GitHub Codespaces)
```bash
sudo apt install libsqlite3-dev
cd backend
rm -f appointease.db    # only needed if you ran an older version
make
./appointease
