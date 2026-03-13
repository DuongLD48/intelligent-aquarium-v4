#pragma once

// ================================================================
// credentials.h
// Intelligent Aquarium v4.0
// ⚠️  KHÔNG commit file này lên Git!
// Điền thông tin thực vào đây trước khi flash firmware.
// ================================================================

// ----------------------------------------------------------------
// WiFi
// ----------------------------------------------------------------
// #define WIFI_SSID       "Nha Tro"
// #define WIFI_PASSWORD   "0972667827"

#define WIFI_SSID       "LE THI HUE"
#define WIFI_PASSWORD   "0909963646"

// ----------------------------------------------------------------
// Firebase Realtime Database
// ----------------------------------------------------------------
#define FIREBASE_URL    "link"
#define FIREBASE_DEVICE "aquarium_1"         // Device ID trong Firebase path
#define FIREBASE_TOKEN  "token"

// ----------------------------------------------------------------
// Firebase path convention:
//   /devices/{FIREBASE_DEVICE}/telemetry/    ← gửi lên
//   /devices/{FIREBASE_DEVICE}/config/       ← nhận config
//   /devices/{FIREBASE_DEVICE}/water_change/ ← lịch + trigger
// ----------------------------------------------------------------
