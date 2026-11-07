// AppointEase frontend: talks to the C backend through /api/...

// Hourly slots from 09:00 to 16:00. Change these to suit the hospital.
const SLOTS = ["09:00", "10:00", "11:00", "12:00", "14:00", "15:00", "16:00"];

const $ = (id) => document.getElementById(id);

// ---------- helpers ----------
function showMessage(text, type) {
  const box = $("message");
  box.textContent = text;
  box.className = type || "";
  clearTimeout(showMessage.timer);
  showMessage.timer = setTimeout(() => { box.textContent = ""; }, 4000);
}

// Calls the backend. Throws an Error with the server's message on failure.
async function api(path, method = "GET", data = null) {
  const options = { method };
  if (data) {
    options.headers = { "Content-Type": "application/x-www-form-urlencoded" };
    options.body = new URLSearchParams(data).toString();
  }
  const res = await fetch(path, options);
  const json = await res.json();
  if (!res.ok) throw new Error(json.error || "Something went wrong.");
  return json;
}

function formData(form) {
  return Object.fromEntries(new FormData(form).entries());
}

function option(value, label, disabled = false) {
  const o = document.createElement("option");
  o.value = value;
  o.textContent = label;
  o.disabled = disabled;
  return o;
}

// ---------- doctors & patients ----------
async function loadDoctors() {
  const doctors = await api("/api/doctors");

  const list = $("doctor-list");
  list.replaceChildren();
  for (const d of doctors) {
    const li = document.createElement("li");
    li.textContent = d.name + " ";
    const small = document.createElement("small");
    small.textContent = d.specialization;
    li.appendChild(small);
    list.appendChild(li);
  }

  const select = $("book-doctor");
  select.replaceChildren(option("", "Select doctor"));
  for (const d of doctors) {
    select.appendChild(option(d.id, d.specialization ? `${d.name} (${d.specialization})` : d.name));
  }
}

async function loadPatients() {
  const patients = await api("/api/patients");

  const list = $("patient-list");
  list.replaceChildren();
  for (const p of patients) {
    const li = document.createElement("li");
    li.textContent = p.name + " ";
    const small = document.createElement("small");
    small.textContent = p.phone;
    li.appendChild(small);
    list.appendChild(li);
  }

  const select = $("book-patient");
  select.replaceChildren(option("", "Select patient"));
  for (const p of patients) select.appendChild(option(p.id, p.name));
}

// ---------- availability: grey out booked slots ----------
async function refreshSlots() {
  const doctorId = $("book-doctor").value;
  const date = $("book-date").value;
  const select = $("book-time");
  select.replaceChildren(option("", "Select time"));

  let booked = [];
  if (doctorId && date) {
    try {
      booked = (await api(`/api/availability?doctor_id=${doctorId}&date=${date}`)).booked;
    } catch (err) {
      showMessage(err.message, "error");
    }
  }
  for (const slot of SLOTS) {
    const taken = booked.includes(slot);
    select.appendChild(option(slot, taken ? `${slot} (booked)` : slot, taken));
  }
}

// ---------- schedule ----------
async function loadSchedule() {
  const rows = await api("/api/appointments");
  const body = $("schedule");
  body.replaceChildren();

  if (rows.length === 0) {
    const tr = document.createElement("tr");
    const td = document.createElement("td");
    td.colSpan = 5;
    td.textContent = "No appointments yet.";
    tr.appendChild(td);
    body.appendChild(tr);
    return;
  }

  for (const a of rows) {
    const tr = document.createElement("tr");
    for (const text of [a.date, a.time, a.doctor, a.patient]) {
      const td = document.createElement("td");
      td.textContent = text;
      tr.appendChild(td);
    }
    const last = document.createElement("td");
    const btn = document.createElement("button");
    btn.textContent = "Cancel";
    btn.className = "danger";
    btn.addEventListener("click", () => cancelAppointment(a.id));
    last.appendChild(btn);
    tr.appendChild(last);
    body.appendChild(tr);
  }
}

async function cancelAppointment(id) {
  if (!confirm("Cancel this appointment?")) return;
  try {
    await api(`/api/appointments?id=${id}`, "DELETE");
    showMessage("Appointment cancelled.", "success");
    await loadSchedule();
    await refreshSlots();
  } catch (err) {
    showMessage(err.message, "error");
  }
}

// ---------- form handlers ----------
$("doctor-form").addEventListener("submit", async (e) => {
  e.preventDefault();
  try {
    await api("/api/doctors", "POST", formData(e.target));
    e.target.reset();
    showMessage("Doctor added.", "success");
    await loadDoctors();
  } catch (err) {
    showMessage(err.message, "error");
  }
});

$("patient-form").addEventListener("submit", async (e) => {
  e.preventDefault();
  try {
    await api("/api/patients", "POST", formData(e.target));
    e.target.reset();
    showMessage("Patient added.", "success");
    await loadPatients();
  } catch (err) {
    showMessage(err.message, "error");
  }
});

$("book-form").addEventListener("submit", async (e) => {
  e.preventDefault();
  try {
    await api("/api/appointments", "POST", formData(e.target));
    showMessage("Appointment booked!", "success");
    await loadSchedule();
    await refreshSlots();
  } catch (err) {
    showMessage(err.message, "error"); // e.g. "This doctor is already booked at that time."
    await refreshSlots();
  }
});

$("book-doctor").addEventListener("change", refreshSlots);
$("book-date").addEventListener("change", refreshSlots);

// ---------- start ----------
(async function init() {
  $("book-date").min = new Date().toISOString().slice(0, 10);
  try {
    await Promise.all([loadDoctors(), loadPatients(), loadSchedule()]);
    await refreshSlots();
  } catch (err) {
    showMessage("Cannot reach the server. Is the C backend running?", "error");
  }
})();
