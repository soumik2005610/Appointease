const API_BASE = '/api';
const SLOTS = ["09:00", "10:00", "11:00", "12:00", "14:00", "15:00", "16:00"];
let currentRole = 'patient';
let adminPin = '';

document.addEventListener('DOMContentLoaded', () => {
    switchPortalRole('patient');
    initDateInputs();
    loadDashboard();
});

function initDateInputs() {
    const today = new Date().toISOString().split('T')[0];
    ['opd-date', 'svc-date', 'admit-date'].forEach(id => {
        const el = document.getElementById(id);
        if (el) el.value = today;
    });
}

function showAlert(msg, isError = false) {
    const el = document.getElementById('alert-banner');
    el.textContent = msg;
    el.className = `alert-banner ${isError ? 'error' : 'success'}`;
    el.classList.remove('hidden');
    setTimeout(() => el.classList.add('hidden'), 5000);
}

function switchPortalRole(role) {
    if (role === 'hospital') {
        const enteredPin = prompt("Enter Hospital Admin PIN (Default: 1234):");
        if (enteredPin === "1234") {
            currentRole = 'hospital';
            adminPin = "1234";
            document.body.classList.add('role-admin');
            showAlert("Switched to Hospital Admin Mode.");
        } else {
            alert("Incorrect Admin PIN!");
            document.getElementById('portal-role').value = 'patient';
            return;
        }
    } else {
        currentRole = 'patient';
        adminPin = '';
        document.body.classList.remove('role-admin');
        showAlert("Switched to Patient View Mode.");
    }
    switchTab('dashboard');
}

function switchTab(tabId) {
    document.querySelectorAll('.tab-btn').forEach(b => b.classList.remove('active'));
    document.querySelectorAll('.tab-content').forEach(c => c.classList.remove('active'));

    const activeBtn = document.querySelector(`.tab-btn[onclick*="${tabId}"]`);
    if (activeBtn) activeBtn.classList.add('active');

    const targetTab = document.getElementById(`tab-${tabId}`);
    if (targetTab) targetTab.classList.add('active');

    if (tabId === 'dashboard') loadDashboard();
    if (tabId === 'opd') loadOPDTab();
    if (tabId === 'services') loadServicesTab();
    if (tabId === 'wards') loadWardsTab();
    if (tabId === 'doctors') loadDoctorsTab();
    if (tabId === 'patients') loadPatientsTab();
}

async function apiFetch(endpoint, method = 'GET', body = null) {
    try {
        let url = `${API_BASE}/${endpoint}`;
        const headers = {};
        let bodyData = null;

        if (body) {
            if (adminPin) body.admin_pin = adminPin;
            bodyData = new URLSearchParams(body).toString();
            if (method === 'GET') {
                url += `?${bodyData}`;
                bodyData = null;
            } else {
                headers['Content-Type'] = 'application/x-www-form-urlencoded';
            }
        } else if (adminPin && (method === 'DELETE' || method === 'POST')) {
            bodyData = `admin_pin=${encodeURIComponent(adminPin)}`;
            headers['Content-Type'] = 'application/x-www-form-urlencoded';
        }

        const res = await fetch(url, { method, headers, body: bodyData });
        const data = await res.json();
        if (!res.ok) throw new Error(data.error || 'Server request failed');
        return data;
    } catch (err) {
        showAlert(err.message, true);
        return null;
    }
}

async function loadDashboard() {
    const data = await apiFetch('dashboard');
    if (!data || !data.length) return;
    const d = data[0];
    document.getElementById('dash-opd').textContent = d.opd || 0;
    document.getElementById('dash-xray').textContent = d.xray || 0;
    document.getElementById('dash-ct').textContent = d.ct || 0;
    document.getElementById('dash-mri').textContent = d.mri || 0;
    document.getElementById('dash-docs').textContent = d.doctors || 0;
    document.getElementById('dash-pts').textContent = d.patients || 0;
    document.getElementById('dash-admitted').textContent = d.admitted || 0;
    document.getElementById('dash-beds').textContent = d.beds_free || 0;
}

async function loadOPDTab() {
    const doctors = await apiFetch('doctors');
    const patients = await apiFetch('patients');
    
    populateSelect('opd-doctor', doctors, 'id', 'name', 'Select Doctor');
    populateSelect('opd-patient', patients, 'id', 'name', 'Select Patient');
    
    loadDoctorSlotAvailability();
    loadOPDAppointments();
}

async function loadDoctorSlotAvailability() {
    const docId = document.getElementById('opd-doctor').value;
    const date = document.getElementById('opd-date').value;
    const select = document.getElementById('opd-time');
    select.innerHTML = '';

    if (!docId || !date) return;
    const res = await apiFetch('availability', 'GET', { doctor_id: docId, date });
    const booked = res ? res.booked : [];

    SLOTS.forEach(s => {
        const opt = document.createElement('option');
        opt.value = s;
        opt.textContent = booked.includes(s) ? `${s} (Booked)` : s;
        if (booked.includes(s)) opt.disabled = true;
        select.appendChild(opt);
    });
}

async function handleBookOPD(e) {
    e.preventDefault();
    const res = await apiFetch('appointments', 'POST', {
        doctor_id: document.getElementById('opd-doctor').value,
        patient_id: document.getElementById('opd-patient').value,
        date: document.getElementById('opd-date').value,
        time: document.getElementById('opd-time').value
    });
    if (res) {
        showAlert("OPD Appointment Booked Successfully!");
        loadOPDTab();
    }
}

async function loadOPDAppointments() {
    const list = await apiFetch('appointments');
    const tbody = document.getElementById('table-opd');
    tbody.innerHTML = '';
    (list || []).forEach(row => {
        tbody.innerHTML += `
            <tr>
                <td>${row.date}</td>
                <td>${row.time}</td>
                <td>${row.doctor}</td>
                <td>${row.patient}</td>
                <td class="admin-only"><button class="btn-danger" onclick="cancelAppointment(${row.id})">Cancel</button></td>
            </tr>`;
    });
}

async function cancelAppointment(id) {
    if (confirm("Cancel this appointment?")) {
        const res = await apiFetch('appointments', 'DELETE', { id });
        if (res) { showAlert("Appointment cancelled"); loadOPDAppointments(); }
    }
}

async function loadServicesTab() {
    const services = await apiFetch('services');
    const patients = await apiFetch('patients');
    
    populateSelect('svc-service', services, 'id', 'name', 'Select Service');
    populateSelect('svc-patient', patients, 'id', 'name', 'Select Patient');
    
    loadServiceSlotAvailability();
    loadServiceBookings();
}

async function loadServiceSlotAvailability() {
    const svcId = document.getElementById('svc-service').value;
    const date = document.getElementById('svc-date').value;
    const select = document.getElementById('svc-time');
    select.innerHTML = '';

    if (!svcId || !date) return;
    const res = await apiFetch('service-availability', 'GET', { service_id: svcId, date });
    const booked = res ? res.booked : [];

    SLOTS.forEach(s => {
        const opt = document.createElement('option');
        opt.value = s;
        opt.textContent = booked.includes(s) ? `${s} (Booked)` : s;
        if (booked.includes(s)) opt.disabled = true;
        select.appendChild(opt);
    });
}

async function handleBookService(e) {
    e.preventDefault();
    const res = await apiFetch('service-bookings', 'POST', {
        service_id: document.getElementById('svc-service').value,
        patient_id: document.getElementById('svc-patient').value,
        date: document.getElementById('svc-date').value,
        time: document.getElementById('svc-time').value
    });
        if (res && res.success) {
        // 1. Backend theke asa data diye text build koro
        const letterText = 
`===================================================
         APPOINTEASE APPOINTMENT CONFIRMATION      
===================================================

 Patient Name : ${res.patient}
 Doctor/Service: ${res.doctor}
 Date & Time  : ${res.date} at ${res.time}
---------------------------------------------------
 Status       : Confirmed
===================================================`;

        // 2. Safari automatic file download trigger
        const blob = new Blob([letterText], { type: 'text/plain' });
        const link = document.createElement('a');
        link.href = URL.createObjectURL(blob);
        link.download = `OPD_Slip_${res.patient.replace(/\s+/g, '_')}.txt`;
        document.body.appendChild(link);
        link.click();
        document.body.removeChild(link);
        URL.revokeObjectURL(link.href);

        showAlert("Slot Booked & Letter Downloaded!");
        loadServicesTab(); // or loadOPDTab()
    }

}

async function loadServiceBookings() {
    const list = await apiFetch('service-bookings');
    const tbody = document.getElementById('table-services');
    tbody.innerHTML = '';
    (list || []).forEach(row => {
        tbody.innerHTML += `
            <tr>
                <td>${row.date}</td>
                <td>${row.time}</td>
                <td>${row.service}</td>
                <td>${row.patient}</td>
                <td class="admin-only"><button class="btn-danger" onclick="cancelServiceBooking(${row.id})">Cancel</button></td>
            </tr>`;
    });
}

async function cancelServiceBooking(id) {
    if (confirm("Cancel this booking?")) {
        const res = await apiFetch('service-bookings', 'DELETE', { id });
        if (res) { showAlert("Booking cancelled"); loadServiceBookings(); }
    }
}

async function loadWardsTab() {
    const wards = await apiFetch('wards');
    const patients = await apiFetch('patients');
    const admissions = await apiFetch('admissions');

    const tbodyWards = document.getElementById('table-wards');
    tbodyWards.innerHTML = '';
    (wards || []).forEach(w => {
        tbodyWards.innerHTML += `
            <tr>
                <td>${w.name}</td>
                <td>${w.total}</td>
                <td>${w.occupied}</td>
                <td>${w.available}</td>
            </tr>`;
    });

    populateSelect('admit-ward', wards, 'id', 'name', 'Select Ward');
    populateSelect('admit-patient', patients, 'id', 'name', 'Select Patient');

    const tbodyAdm = document.getElementById('table-admissions');
    tbodyAdm.innerHTML = '';
    (admissions || []).forEach(a => {
        tbodyAdm.innerHTML += `
            <tr>
                <td>${a.ward}</td>
                <td>${a.patient}</td>
                <td>${a.admitted_on}</td>
                <td class="admin-only"><button class="btn-danger" onclick="dischargePatient(${a.id})">Discharge</button></td>
            </tr>`;
    });
}

async function handleAdmitPatient(e) {
    e.preventDefault();
    const res = await apiFetch('admissions', 'POST', {
        ward_id: document.getElementById('admit-ward').value,
        patient_id: document.getElementById('admit-patient').value,
        date: document.getElementById('admit-date').value
    });
    if (res) {
        showAlert("Patient Admitted!");
        loadWardsTab();
    }
}

async function dischargePatient(id) {
    if (confirm("Discharge patient?")) {
        const res = await apiFetch('admissions', 'DELETE', { id });
        if (res) { showAlert("Patient Discharged"); loadWardsTab(); }
    }
}

async function loadDoctorsTab() {
    const doctors = await apiFetch('doctors');
    const tbody = document.getElementById('table-doctors');
    tbody.innerHTML = '';
    (doctors || []).forEach(d => {
        tbody.innerHTML += `
            <tr>
                <td>${d.id}</td>
                <td>${d.name}</td>
                <td>${d.specialization}</td>
                <td class="admin-only"><button class="btn-danger" onclick="deleteDoctor(${d.id})">Remove</button></td>
            </tr>`;
    });
}

async function handleAddDoctor(e) {
    e.preventDefault();
    const res = await apiFetch('doctors', 'POST', {
        name: document.getElementById('doc-name').value,
        specialization: document.getElementById('doc-spec').value
    });
    if (res) {
        showAlert("Doctor Added!");
        document.getElementById('form-add-doctor').reset();
        loadDoctorsTab();
    }
}

async function deleteDoctor(id) {
    if (confirm("Delete doctor?")) {
        const res = await apiFetch('doctors', 'DELETE', { id });
        if (res) { showAlert("Doctor Removed"); loadDoctorsTab(); }
    }
}

async function loadPatientsTab() {
    const patients = await apiFetch('patients');
    const tbody = document.getElementById('table-patients');
    tbody.innerHTML = '';
    (patients || []).forEach(p => {
        tbody.innerHTML += `
            <tr>
                <td>${p.id}</td>
                <td>${p.name}</td>
                <td>${p.phone}</td>
                <td class="admin-only"><button class="btn-danger" onclick="deletePatient(${p.id})">Remove</button></td>
            </tr>`;
    });
}

async function handleAddPatient(e) {
    e.preventDefault();
    const res = await apiFetch('patients', 'POST', {
        name: document.getElementById('pat-name').value,
        phone: document.getElementById('pat-phone').value
    });
    if (res) {
        showAlert("Patient Registered!");
        document.getElementById('form-add-patient').reset();
        loadPatientsTab();
    }
}

async function deletePatient(id) {
    if (confirm("Delete patient?")) {
        const res = await apiFetch('patients', 'DELETE', { id });
        if (res) { showAlert("Patient Removed"); loadPatientsTab(); }
    }
}

function populateSelect(elemId, items, valKey, textKey, placeholder) {
    const select = document.getElementById(elemId);
    if (!select) return;
    select.innerHTML = `<option value="">${placeholder}</option>`;
    (items || []).forEach(item => {
        const opt = document.createElement('option');
        opt.value = item[valKey];
        opt.textContent = item[textKey];
        select.appendChild(opt);
    });
}
