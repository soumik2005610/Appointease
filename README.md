# AppointEase - Hospital & Patient Management System

AppointEase is a lightweight C/SQLite hospital management solution featuring OPD appointment scheduling, lab service bookings (X-Ray, CT, MRI), ward bed management, and doctor rosters.

---

## Features
- **Dual Portal Views**: Patient Portal and Hospital Admin Portal protected by PIN (`1234`).
- **Dynamic Slot Lock**: Real-time checking to block already-booked OPD & Service slots.
- **Embedded C Web Server**: Fast custom socket HTTP engine with dynamic `PORT` support for Render.

---

## File Structure
```text
.
├── Dockerfile
├── Makefile
├── README.md
├── backend/
│   ├── main.c
│   └── Makefile
└── frontend/
    ├── app.js
    ├── index.html
    └── style.css
