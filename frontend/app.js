// AppointEase frontend: login for staff, doctors and patients.
// What each person can see and do is decided by the C server; this page only shows or hides things.

// Time slots. Change these to suit the hospital.
const OPD_SLOTS  = ["09:00", "10:00", "11:00", "12:00", "14:00", "15:00", "16:00"];
const DIAG_SLOTS = ["08:00", "09:00", "10:00", "11:00", "12:00", "14:00", "15:00"];

// Which elements and API each booking form uses
const OPD  = { resource: "book-doctor", date: "book-date", time: "book-time",
               url: "/api/availability?doctor_id=", slots: OPD_SLOTS };
const DIAG = { resource: "diag-service", date: "diag-date", time: "diag-time",
               url: "/api/service-availability?service_id=", slots: DIAG_SLOTS };

const $ = (id) => document.getElementById(id);
const state = { role: "", name: "" };
const can = (...roles) => roles.includes(state.role);

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
  if (res.status === 401 && state.role) {   // the login expired
    showLogin();
    throw new Error("Please log in again.");
  }
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

function actionButton(label, onClick, cls = "danger") {
  const b = el("button", label, cls);
  b.type = "button";
  b.addEventListener("click", onClick);
  return b;
}

// Build a whole table from a list of objects.
// actionsFor(row) returns the buttons for that row: [{ label, cls, run }]
function renderTable(table, columns, rows, emptyText, actionsFor) {
  const head = el("tr");
  for (const c of columns) head.appendChild(el("th", c.label));
  head.appendChild(el("th"));
  const thead = el("thead");
  thead.appendChild(head);

  const tbody = el("tbody");
  if (rows.length === 0) {
    const tr = el("tr");
    const td = el("td", emptyText);
    td.colSpan = columns.length + 1;
    tr.appendChild(td);
    tbody.appendChild(tr);
  }
  for (const row of rows) {
    const tr = el("tr");
    for (const c of columns) tr.appendChild(el("td", row[c.field]));
    const last = el("td");
    for (const a of actionsFor(row)) last.appendChild(actionButton(a.label, a.run, a.cls));
    tr.appendChild(last);
    tbody.appendChild(tr);
  }
  table.replaceChildren(thead, tbody);
}

// ---------- tabs and roles ----------
function showTab(name) {
  document.querySelectorAll(".tab").forEach((s) => { s.hidden = s.id !== "tab-" + name; });
  document.querySelectorAll("#tabs button").forEach((b) => {
    b.classList.toggle("active", b.dataset.tab === name);
  });
}
document.querySelectorAll("#tabs button").forEach((b) => {
  b.addEventListener("click", () => showTab(b.dataset.tab));
});

// Show only what this role may use (the server enforces the same rules)
function applyRole() {
  document.querySelectorAll("[data-roles]").forEach((e) => {
    const hide = !e.dataset.roles.split(" ").includes(state.role);
    e.classList.toggle("role-hidden", hide);
    if (e.matches("select, input")) e.disabled = hide;   // hidden fields are not sent
  });
  document.querySelectorAll("#tabs button").forEach((b) => {
    if (!b.dataset.base) b.dataset.base = b.textContent;
    b.textContent = b.getAttribute("data-label-" + state.role) || b.dataset.base;
  });
  $("opd-title").textContent = can("admin") ? "OPD Schedule" : "My Appointments";
  $("admit-title").textContent = can("patient") ? "My Admission" : "Admitted Patients Log";
}

function showLogin() {
  state.role = "";
  state.name = "";
  $("app").hidden = true;
  $("userbox").hidden = true;
  $("login-screen").hidden = false;
}

function showApp(who) {
  state.role = who.role;
  state.name = who.name;
  $("login-screen").hidden = true;
  $("app").hidden = false;
  $("userbox").hidden = false;
  $("whoami").textContent = `${who.name} (${who.role === "admin" ? "staff" : who.role})`;
  applyRole();
  showTab(can("admin") ? "dashboard" : "opd");
  reloadAll();
}

// ---------- loaders ----------
async function loadDoctors() {
  const doctors = await api("/api/doctors");
  const list = $("doctor-list");
  list.replaceChildren();
  for (const d of doctors) {
    const li = el("li", d.name + " ");
    li.appendChild(el("small", d.specialization));
    li.appendChild(actionButton("Delete", () =>
      removeItem(`/api/doctors?id=${d.id}`, `Delete doctor ${d.name}?`, "Doctor removed.")));
    list.appendChild(li);
  }
  fillSelect($("book-doctor"), "Select doctor", doctors,
    (d) => (d.specialization ? `${d.name} (${d.specialization})` : d.name));
}

async function loadPatients() {   // staff only
  const patients = await api("/api/patients");
  const list = $("patient-list");
  list.replaceChildren();
  for (const p of patients) {
    const li = el("li", p.name + " ");
    li.appendChild(el("small", p.phone));
    li.appendChild(actionButton("Delete", () =>
      removeItem(`/api/patients?id=${p.id}`, `Delete patient ${p.name}?`, "Patient removed.")));
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
  const columns = [{ label: "Date", field: "date" }, { label: "Time", field: "time" },
                   { label: "Doctor", field: "doctor" }];
  if (!can("patient")) columns.push({ label: "Patient", field: "patient" }, { label: "Phone", field: "phone" });
  columns.push({ label: "Status", field: "status" });

  renderTable($("schedule"), columns, rows, "No appointments booked.", (r) => {
    const actions = [];
    if (r.status === "Booked") {
      if (can("admin", "doctor")) {
        actions.push({ label: "Mark completed", cls: "ok", run: () => completeAppointment(r) });
      }
      actions.push({ label: "Cancel", cls: "danger", run: () =>
        removeItem(`/api/appointments?id=${r.id}`, "Cancel this appointment?", "Appointment cancelled.") });
    }
    return actions;
  });
}

async function loadDiagSchedule() {
  const rows = await api("/api/service-bookings");
  const columns = [{ label: "Date", field: "date" }, { label: "Time", field: "time" },
                   { label: "Service", field: "service" }];
  if (!can("patient")) columns.push({ label: "Patient", field: "patient" });

  renderTable($("diag-schedule"), columns, rows, "No diagnostic bookings.", (r) => [
    { label: "Cancel", cls: "danger", run: () =>
      removeItem(`/api/service-bookings?id=${r.id}`, "Cancel this booking?", "Booking cancelled.") },
  ]);
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
  const columns = [{ label: "Ward", field: "ward" }, { label: "Patient", field: "patient" },
                   { label: "Admitted on", field: "admitted_on" }];
  renderTable($("admissions"), columns, rows, "No patients admitted.", (r) =>
    can("admin")
      ? [{ label: "Discharge", cls: "danger", run: () =>
          removeItem(`/api/admissions?id=${r.id}`, `Discharge ${r.patient}?`, "Patient discharged.") }]
      : []);
}

async function loadDashboard() {   // staff only
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

// Load only the data this role is allowed to see
async function reloadAll() {
  if (!state.role) return;
  try {
    const jobs = [loadDoctors(), loadSchedule(), loadWards(), loadAdmissions()];
    if (can("admin")) jobs.push(loadPatients(), loadDashboard());
    if (can("admin", "patient")) jobs.push(loadServices(), loadDiagSchedule());
    await Promise.all(jobs);
    if (can("admin", "patient")) await Promise.all([refreshSlots(OPD), refreshSlots(DIAG)]);
  } catch (err) {
    if (state.role) showMessage(err.message || "Cannot reach the server.", "error");
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

async function completeAppointment(r) {
  try {
    await api("/api/complete", "POST", { id: r.id });
    showMessage("Appointment marked as completed.", "success");
  } catch (err) {
    showMessage(err.message, "error");
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

// ---------- login, register, logout ----------
let loginRole = "patient";

document.querySelectorAll("#login-roles button").forEach((b) => {
  b.addEventListener("click", () => {
    loginRole = b.dataset.login;
    document.querySelectorAll("#login-roles button").forEach((x) => x.classList.toggle("active", x === b));
    $("register-box").classList.toggle("role-hidden", loginRole !== "patient");
    $("register-form").hidden = true;
  });
});

$("show-register").addEventListener("click", (e) => {
  e.preventDefault();
  $("register-form").hidden = !$("register-form").hidden;
});

$("login-form").addEventListener("submit", async (e) => {
  e.preventDefault();
  try {
    const who = await api("/api/login", "POST", { ...formData(e.target), role: loginRole });
    e.target.reset();
    showApp(who);
  } catch (err) {
    showMessage(err.message, "error");
  }
});

$("register-form").addEventListener("submit", async (e) => {
  e.preventDefault();
  try {
    const who = await api("/api/register", "POST", formData(e.target));
    e.target.reset();
    $("register-form").hidden = true;
    showApp(who);
    showMessage("Account created. Welcome!", "success");
  } catch (err) {
    showMessage(err.message, "error");
  }
});

$("logout-btn").addEventListener("click", async () => {
  try { await api("/api/logout", "POST", {}); } catch (err) { /* already logged out */ }
  showLogin();
});

// ---------- start ----------
$("book-date").min = today();
$("diag-date").min = today();
$("admit-date").value = today();

(async function init() {
  try {
    const who = await api("/api/me");
    if (who.role) showApp(who); else showLogin();
  } catch (err) {
    showLogin();
    showMessage("Cannot reach the server. Is the C backend running?", "error");
  }
})();
