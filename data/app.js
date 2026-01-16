const rumorList = document.getElementById("rumorList");
const rumorCount = document.getElementById("rumorCount");
const nameFilter = document.getElementById("nameFilter");
const resetAllBtn = document.getElementById("resetAllBtn");
const cancelEditBtn = document.getElementById("cancelEditBtn");
const formTitle = document.getElementById("formTitle");

const form = document.getElementById("rumorForm");
const titleInput = document.getElementById("titleInput");
const textNlInput = document.getElementById("textNlInput");
const textEnInput = document.getElementById("textEnInput");
const peopleInput = document.getElementById("peopleInput");
const maxPrintsInput = document.getElementById("maxPrintsInput");
const activeInput = document.getElementById("activeInput");

let rumors = [];
let editingId = null;
let filterTimer = null;

function setEditing(rumor) {
  if (!rumor) {
    editingId = null;
    formTitle.textContent = "Add a Rumor";
    cancelEditBtn.hidden = true;
    form.reset();
    maxPrintsInput.value = 5;
    activeInput.checked = true;
    return;
  }
  editingId = rumor.id;
  formTitle.textContent = "Edit Rumor";
  cancelEditBtn.hidden = false;
  titleInput.value = rumor.title || "";
  textNlInput.value = rumor.text_nl || "";
  textEnInput.value = rumor.text_en || "";
  peopleInput.value = rumor.people || "";
  maxPrintsInput.value = rumor.max_prints || 5;
  activeInput.checked = !!rumor.active;
}

function createTag(text, className) {
  const tag = document.createElement("span");
  tag.textContent = text;
  tag.className = className;
  return tag;
}

function renderRumors() {
  rumorList.innerHTML = "";
  const activeCount = rumors.filter((r) => r.active).length;
  rumorCount.textContent = `${activeCount}/${rumors.length}`;

  rumors.forEach((rumor, index) => {
    const card = document.createElement("div");
    card.className = "rumor-card";
    card.style.animationDelay = `${index * 40}ms`;

    const title = document.createElement("h3");
    title.className = "rumor-title";
    title.textContent = rumor.title || "Untitled";

    const meta = document.createElement("div");
    meta.className = "rumor-meta";
    meta.appendChild(
      createTag(rumor.active ? "Active" : "Inactive", `pill ${rumor.active ? "active" : "inactive"}`)
    );
    meta.appendChild(
      createTag(`${rumor.printed_count}/${rumor.max_prints} prints`, "pill")
    );
    if (rumor.people) {
      const people = document.createElement("span");
      people.textContent = `People: ${rumor.people}`;
      meta.appendChild(people);
    }

    const actions = document.createElement("div");
    actions.className = "rumor-actions";

    const toggleBtn = document.createElement("button");
    toggleBtn.textContent = rumor.active ? "Deactivate" : "Activate";
    toggleBtn.onclick = () => updateRumor(rumor.id, { active: !rumor.active });

    const editBtn = document.createElement("button");
    editBtn.textContent = "Edit";
    editBtn.onclick = () => setEditing(rumor);

    const resetBtn = document.createElement("button");
    resetBtn.textContent = "Reset Count";
    resetBtn.onclick = () => resetRumor(rumor.id);

    const deleteBtn = document.createElement("button");
    deleteBtn.textContent = "Delete";
    deleteBtn.onclick = () => deleteRumor(rumor.id);

    actions.append(toggleBtn, editBtn, resetBtn, deleteBtn);
    card.append(title, meta, actions);
    rumorList.appendChild(card);
  });
}

async function fetchRumors() {
  const query = nameFilter.value.trim();
  const url = query ? `/api/rumors?name=${encodeURIComponent(query)}` : "/api/rumors";
  const response = await fetch(url);
  if (!response.ok) {
    return;
  }
  rumors = await response.json();
  renderRumors();
}

async function createRumor(payload) {
  const response = await fetch("/api/rumors", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(payload),
  });
  if (response.ok) {
    await fetchRumors();
    setEditing(null);
  }
}

async function updateRumor(id, payload) {
  const response = await fetch(`/api/rumors/${id}`, {
    method: "PUT",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(payload),
  });
  if (response.ok) {
    await fetchRumors();
  }
}

async function deleteRumor(id) {
  const response = await fetch(`/api/rumors/${id}`, { method: "DELETE" });
  if (response.ok) {
    await fetchRumors();
    if (editingId === id) {
      setEditing(null);
    }
  }
}

async function resetRumor(id) {
  const response = await fetch(`/api/rumors/${id}/reset`, { method: "POST" });
  if (response.ok) {
    await fetchRumors();
  }
}

resetAllBtn.addEventListener("click", async () => {
  const response = await fetch("/api/rumors/resetAll", { method: "POST" });
  if (response.ok) {
    await fetchRumors();
  }
});

cancelEditBtn.addEventListener("click", () => setEditing(null));

nameFilter.addEventListener("input", () => {
  clearTimeout(filterTimer);
  filterTimer = setTimeout(fetchRumors, 250);
});

form.addEventListener("submit", async (event) => {
  event.preventDefault();
  const payload = {
    title: titleInput.value.trim(),
    text_nl: textNlInput.value.trim(),
    text_en: textEnInput.value.trim(),
    people: peopleInput.value.trim(),
    max_prints: Number(maxPrintsInput.value) || 5,
    active: activeInput.checked,
  };

  if (editingId) {
    await updateRumor(editingId, payload);
    setEditing(null);
  } else {
    await createRumor(payload);
  }
});

fetchRumors();
