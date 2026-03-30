#pragma once
#include "type_definitions.h"
#include "circular_buffer.h"
#include "system_constants.h"

// ================================================================
// sensor_manager.h
// Intelligent Aquarium v4.1
//
// Trách nhiệm: đọc phần cứng → push vào rawSensorBuffer.
//
// THAY ĐỔI v4.1:
//   - readSensors(): chỉ đọc Temp + TDS. pH = NAN luôn.
//   - readPhOnce():  đọc pH 1 lần, chỉ cho PhSessionManager dùng
//                   khi relay đã tắt (COLLECTING phase).
//
// Lý do: relay gây nhiễu ADC pH nặng → không đọc pH khi relay chạy.
// ================================================================

extern CircularBuffer<SensorReading, SENSOR_HISTORY_SIZE> rawSensorBuffer;

// Gọi trong setup(): khởi tạo OneWire, DS18B20, analogSetAttenuation
void sensor_manager_init();

// Gọi trong loop() mỗi 5 giây — đọc Temp + TDS, pH = NAN
// Trả về true nếu vừa có mẫu mới được push.
bool readSensors();

// Kiểm tra rawSensorBuffer có dữ liệu chưa
bool isSensorDataReady();

// Cập nhật calibration runtime từ ConfigManager/Firebase
void sensorManagerSetCalibration(float phSlope, float phOffset, float tdsFactor);

// Đọc 1 mẫu pH trực tiếp từ ADC (16 lần average).
// CHỈ gọi từ PhSessionManager khi đang COLLECTING (safe mode on).
// Không push vào rawSensorBuffer — trả thẳng giá trị pH thô.
float readPhOnce();