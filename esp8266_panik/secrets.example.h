// Copy this file to secrets.h (same folder) and fill in your own values.
// secrets.h is listed in .gitignore so Wi-Fi passwords never go to GitHub.
// The board connects to whichever of these networks it can find (the strongest one).
// For each network, put the LAPTOP's IP on that network (run `ip a` on the laptop while connected to it),
// or an online server address such as "https://yourname.pythonanywhere.com" (no trailing slash).
#pragma once

// This board's vehicle plate: it is what the police console shows. Every board needs its own.
const char* DEVICE_ID = "KDA-123X";

const WifiNet WIFI_NETWORKS[] = {
  // Wi-Fi name          password               Panik server on that network
  {"your-wifi-name",     "your-wifi-password",  "http://192.168.1.226:5000"},
  {"your-phone-hotspot", "hotspot-password",    "http://LAPTOP-IP-ON-HOTSPOT:5000"},
};