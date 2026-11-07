// AppointEase frontend: talks to the C backend through /api/...

// Time slots. Change these to suit the hospital.
const OPD_SLOTS  = ["09:00", "10:00", "11:00", "12:00", "14:00", "15:00", "16:00"];
const DIAG_SLOTS = ["08:00", "09:00", "10:00", "11:00", "12:00", "14:00", "15:00", "16:00", "17:00"];

// Which elements and API each booking form uses
const OPD  = { resource: "book-doctor", date: "book-date", time: "book-time",
               url: "/api/availability?doctor_id=", slots: OPD_SLOTS };
const DIAG = { resource: "diag-service", date: "diag-date", time: "diag-time",
               url: "/api/service-availability?service_id=", slots: DIAG_SLOTS };

const $ = (id) => document.getElementById(id);

// ---------- helpers ----------
function el(tag, text, className) {
  const e = document.createElement(tag);
  if (text !== undefined && text !== null) e.textContent = text;   // textContent keeps names safe
  if (className) e.className = className;
  return e;
}

function today() {   // local date as YYYY-MM-DD
  const d = new Date();
  d.setMinutes(d.getMinutes() - d.getTimezoneOffset());
  return d.toISOString().slice(0, 10);
}

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

// Fill a dropdown, keeping the current choice if it still exists
function fillSelect(select, placeholder, items, labelFn) {
  const old = select.value;
  select.replaceChildren(option("", placeholder));
  for (const item of items) select.appendChild(option(item.id, labelFn(item)));
  if ([...select.options].some((o) => o.value === old)) select.value = old;
}

function actionButton(label, onClick) {
  const b = el("button", label, "danger");
  b.addEventListener("click", onClick);
  return b;
}

// Draw table rows from a list of objects
function renderRows(tbody, rows, fields, emptyText, actionLabel, onAction) {
  tbody.replaceChildren();
  if (rows.length === 0) {
    const tr = el("tr");
    const td = el("td", emptyText);
    td.colSpan = fields.length + 1;
    tr.appendChild(td);
    tbody.appendChild(tr);
    return;
  }
  for (const row of rows) {
    const tr = el("tr");
    for (const f of fields) tr.appendChild(el("td", row[f]));
    const last = el("td");
    last.appendChild(actionButton(actionLabel, () => onAction(row)));
    tr.appendChild(last);
    tbody.appendChild(tr);
  }
}

// ---------- tabs ----------
function showTab(name) {
  document.querySelectorAll(".tab").forEach((s) => { s.hidden = s.id !== "tab-" + name; });
  document.querySelectorAll("#tabs button").forEach((b) => {
    b.classList.toggle("active", b.dataset.tab === name);
  });
}
document.querySelectorAll("#tabs button").forEach((b) => {
  b.addEventListener("click", () => showTab(b.dataset.tab));
});

// ---------- loaders ----------
async function loadDoctors() {
  const doctors = await api("/api/doctors");
  const list = $("doctor-list");
  list.replaceChildren();
  for (const d of doctors) {
    const li = el("li", d.name + " ");
    li.appendChild(el("small", d.specialization));
    li.appendChild(actionButton("Delete", () => removeItem(`/api/doctors?id=${d.id}`, `Delete ${d.name}?`, "Doctor deleted.")));
    list.appendChild(li);
  }
  fillSelect($("book-doctor"), "Select doctor", doctors,
    (d) => (d.specialization ? `${d.name} (${d.specialization})` : d.name));
}

async function loadPatients() {
  const patients = await api("/api/patients");
  const list = $("patient-list");
  list.replaceChildren();
  for (const p of patients) {
    const li = el("li", p.name + " ");
    li.appendChild(el("small", p.phone));
    li.appendChild(actionButton("Delete", () => removeItem(`/api/patients?id=${p.id}`, `Delete ${p.name}?`, "Patient deleted.")));
    list.appendChild(li);
  }
  for (const id of ["book-patient", "diag-patient", "admit-patient"]) {
    fillSelect($(id), "Select patient", patients, (p) => p.name);
  }
}

async function loadServices() {
  const services = await api("/api/services");
  fillSelect($("diag-service"), "Select service", services, (s) => s.name);
}

async function loadSchedule() {
  const rows = await api("/api/appointments");
  renderRows($("schedule"), rows, ["date", "time", "doctor", "patient"], "No appointments yet.",
    "Cancel", (r) => removeItem(`/api/appointments?id=${r.id}`, "Cancel this appointment?", "Appointment cancelled."));
}

async function loadDiagSchedule() {
  const rows = await api("/api/service-bookings");
  renderRows($("diag-schedule"), rows, ["date", "time", "service", "patient"], "No bookings yet.",
    "Cancel", (r) => removeItem(`/api/service-bookings?id=${r.id}`, "Cancel this booking?", "Booking cancelled."));
}

async function loadWards() {
  const wards = await api("/api/wards");
  for (const box of [$("ward-list"), $("dash-wards")]) {
    box.replaceChildren();
    for (const w of wards) {
      const row = el("div", null, "ward");
      row.appendChild(el("strong", w.name));
      row.appendChild(el("span", `${w.available} free of ${w.total}`));
      const bar = el("div", null, "bar");
      const fill = el("div", null, w.available === 0 ? "full" : (w.occupied / w.total >= 0.8 ? "warn" : ""));
      fill.style.width = (w.total ? (w.occupied / w.total) * 100 : 0) + "%";
      bar.appendChild(fill);
      row.appendChild(bar);
      box.appendChild(row);
    }
  }
  fillSelect($("admit-ward"), "Select ward", wards, (w) => `${w.name} (${w.available} free)`);
}

async function loadAdmissions() {
  const rows = await api("/api/admissions");
  renderRows($("admissions"), rows, ["ward", "patient", "admitted_on"], "No patients admitted.",
    "Discharge", (r) => removeItem(`/api/admissions?id=${r.id}`, `Discharge ${r.patient}?`, "Patient discharged."));
}

async function loadDashboard() {
  const [s] = await api("/api/dashboard?date=" + today());
  for (const key of ["opd", "xray", "ct", "mri", "doctors", "patients", "admitted", "beds_free"]) {
    $("stat-" + key).textContent = s[key];
  }
}

// Grey out slots that are already booked
async function refreshSlots(cfg) {
  const resourceId = $(cfg.resource).value;
  const date = $(cfg.date).value;
  const select = $(cfg.time);
  const old = select.value;
  select.replaceChildren(option("", "Select time"));

  let booked = [];
  if (resourceId && date) {
    try {
      booked = (await api(`${cfg.url}${resourceId}&date=${date}`)).booked;
    } catch (err) {
      showMessage(err.message, "error");
    }
  }
  for (const slot of cfg.slots) {
    const taken = booked.includes(slot);
    select.appendChild(option(slot, taken ? `${slot} (booked)` : slot, taken));
  }
  if (old && !booked.includes(old)) select.value = old;
}

async function reloadAll() {
  try {
    await Promise.all([loadDoctors(), loadPatients(), loadServices(), loadSchedule(),
                       loadDiagSchedule(), loadWards(), loadAdmissions(), loadDashboard()]);
    await Promise.all([refreshSlots(OPD), refreshSlots(DIAG)]);
  } catch (err) {
    showMessage("Cannot reach the server. Is the C backend running?", "error");
  }
}

// ---------- actions ----------
async function removeItem(path, question, doneText) {
  if (!confirm(question)) return;
  try {
    await api(path, "DELETE");
    showMessage(doneText, "success");
  } catch (err) {
    showMessage(err.message, "error"); // e.g. "Cannot delete: they still have bookings..."
  }
  await reloadAll();
}

function onSubmit(formId, path, doneText, resetAfter) {
  $(formId).addEventListener("submit", async (e) => {
    e.preventDefault();
    try {
      await api(path, "POST", formData(e.target));
      if (resetAfter) e.target.reset();
      showMessage(doneText, "success");
    } catch (err) {
      showMessage(err.message, "error"); // e.g. "This doctor is already booked at that time."
    }
    await reloadAll();
  });
}

onSubmit("doctor-form", "/api/doctors", "Doctor added.", true);
onSubmit("patient-form", "/api/patients", "Patient added.", true);
onSubmit("book-form", "/api/appointments", "Appointment booked!", false);
onSubmit("diag-form", "/api/service-bookings", "Booking confirmed!", false);
onSubmit("admit-form", "/api/admissions", "Patient admitted.", false);

for (const cfg of [OPD, DIAG]) {
  $(cfg.resource).addEventListener("change", () => refreshSlots(cfg));
  $(cfg.date).addEventListener("change", () => refreshSlots(cfg));
}

// ---------- start ----------
$("book-date").min = today();
$("diag-date").min = today();
$("admit-date").value = today();
reloadAll();
