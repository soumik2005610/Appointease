:root {
  --bg: #f4f7fa;
  --card: #ffffff;
  --text: #1c2b36;
  --muted: #6b7c88;
  --primary: #0a7ea4;
  --danger: #c0392b;
  --ok: #1e8449;
  --border: #dde4ea;
}

* { box-sizing: border-box; }
[hidden] { display: none !important; }
.role-hidden { display: none !important; }

body {
  margin: 0;
  font-family: system-ui, -apple-system, "Segoe UI", Roboto, sans-serif;
  background: var(--bg);
  color: var(--text);
}

header { background: var(--primary); color: #fff; padding: 20px 24px 14px; }
header h1 { margin: 0; font-size: 1.6rem; }
header p  { margin: 4px 0 0; opacity: 0.9; }
.header-row { display: flex; justify-content: space-between; align-items: center; gap: 12px; flex-wrap: wrap; }
#userbox { display: flex; align-items: center; gap: 10px; }
#userbox button { background: #fff; color: var(--primary); font-weight: 600; padding: 6px 12px; }

/* tabs */
nav {
  background: var(--primary);
  display: flex;
  gap: 4px;
  padding: 0 16px;
  overflow-x: auto;
}
nav button {
  background: transparent;
  color: #d6eef7;
  border: none;
  border-radius: 8px 8px 0 0;
  padding: 10px 16px;
  white-space: nowrap;
}
nav button.active { background: var(--bg); color: var(--primary); font-weight: 600; }

main { max-width: 1000px; margin: 20px auto; padding: 0 16px 40px; }
.tab { display: grid; grid-template-columns: 1fr 1fr; gap: 16px; }

.card {
  background: var(--card);
  border: 1px solid var(--border);
  border-radius: 10px;
  padding: 16px;
}
.card.wide { grid-column: 1 / -1; }
.card h2 { margin: 0 0 12px; font-size: 1.1rem; }

form { display: flex; flex-wrap: wrap; gap: 8px; }

input, select, button {
  font: inherit;
  padding: 9px 10px;
  border: 1px solid var(--border);
  border-radius: 6px;
}
input, select { flex: 1 1 160px; background: #fff; }

button { background: var(--primary); color: #fff; border: none; cursor: pointer; }
button:hover { filter: brightness(1.1); }
button.danger { background: var(--danger); padding: 6px 10px; }
button.ok { background: var(--ok); padding: 6px 10px; }

.plain-list { list-style: none; margin: 12px 0 0; padding: 0; max-height: 220px; overflow-y: auto; }
.plain-list.tall { max-height: 420px; }
.plain-list li { display: flex; align-items: center; gap: 6px; padding: 6px 0; border-bottom: 1px solid var(--border); }
.plain-list small { color: var(--muted); }
.plain-list li button { margin-left: auto; padding: 3px 8px; font-size: 0.8rem; }

.hint { color: var(--muted); font-size: 0.85rem; margin: 8px 0 0; }
.hint a { color: var(--primary); }

.table-wrap { overflow-x: auto; }
table { width: 100%; border-collapse: collapse; }
th, td { text-align: left; padding: 8px; border-bottom: 1px solid var(--border); white-space: nowrap; }
th { color: var(--muted); font-weight: 600; font-size: 0.85rem; }
td button { margin-right: 6px; }

/* login */
#login-screen { display: flex; justify-content: center; padding: 28px 16px; }
.login-card { width: 100%; max-width: 420px; }
.role-tabs { display: flex; gap: 6px; margin-bottom: 14px; }
.role-tabs button { flex: 1; background: var(--bg); color: var(--text); border: 1px solid var(--border); }
.role-tabs button.active { background: var(--primary); color: #fff; border-color: var(--primary); }
.login-card form input { flex: 1 1 100%; }
#register-box { margin-top: 14px; border-top: 1px solid var(--border); padding-top: 10px; }
#register-form { margin-top: 10px; }

/* dashboard */
.stats { display: grid; grid-template-columns: repeat(4, 1fr); gap: 10px; }
.stat { background: var(--bg); border-radius: 8px; padding: 12px; text-align: center; }
.stat b { display: block; font-size: 1.7rem; color: var(--primary); }
.stat span { color: var(--muted); font-size: 0.85rem; }

/* wards */
.ward { margin-bottom: 12px; }
.ward span { float: right; color: var(--muted); font-size: 0.9rem; }
.bar { background: var(--border); border-radius: 6px; height: 10px; margin-top: 4px; overflow: hidden; }
.bar div { background: var(--ok); height: 100%; }
.bar div.full { background: var(--danger); }
.bar div.warn { background: #e67e22; }

#message {
  max-width: 1000px;
  margin: 12px auto 0;
  padding: 0 16px;
  min-height: 1.4em;
  font-weight: 600;
}
#message.error { color: var(--danger); }
#message.success { color: var(--ok); }

@media (max-width: 700px) {
  .tab { grid-template-columns: 1fr; }
  .stats { grid-template-columns: repeat(2, 1fr); }
}
