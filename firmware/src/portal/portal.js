const q = (id) => document.getElementById(id);
async function json(url, options) {
  const response = await fetch(url, options);
  if (response.status === 401) {
    alert("Open the authenticated settings URL shown on the device.");
    throw new Error("authentication required");
  }
  return response.json();
}
async function status() {
  try {
    q("status").textContent = JSON.stringify(
      await json("/api/status"),
      null,
      2,
    );
  } catch (error) {
    q("status").textContent = error.message;
  }
}
async function scan() {
  const data = await json("/api/wifi/scan");
  const select = q("ssid");
  select.replaceChildren();
  for (const network of data.networks) {
    const option = document.createElement("option");
    option.textContent = network.ssid;
    option.value = network.ssid;
    select.appendChild(option);
  }
}
async function saveWifi() {
  const ssid = q("manual").value || q("ssid").value;
  const body = new URLSearchParams({ ssid, password: q("wifiPass").value });
  await json("/api/wifi", {
    method: "POST",
    headers: { "Content-Type": "application/x-www-form-urlencoded" },
    body,
  });
  q("wifiPass").value = "";
  await status();
}
scan();
status();
setInterval(status, 5000);
