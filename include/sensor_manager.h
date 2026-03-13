#pragma once
#include "type_definitions.h"
#include "circular_buffer.h"
#include "system_constants.h"

// ================================================================
// sensor_manager.h
// Intelligent Aquarium v4.0
//
// Trách nhiệm DUY NHẤT: đọc phần cứng → push vào rawSensorBuffer.
// KHÔNG có bất kỳ logic lọc nào ở đây.
// ※ Đã bỏ HC-SR04 / water_level_cm
// ================================================================

// ----------------------------------------------------------------
// Raw sensor buffer — global, các module khác đọc qua extern
// DataPipeline sẽ consume từ đây.
// ----------------------------------------------------------------
extern CircularBuffer<SensorReading, SENSOR_HISTORY_SIZE> rawSensorBuffer;

// ----------------------------------------------------------------
// API
// ----------------------------------------------------------------

// Gọi trong setup(): khởi tạo OneWire, DS18B20, pinMode ADC pins
void sensor_manager_init();

// Gọi trong loop() — kiểm tra interval, nếu đến lúc thì đọc
// phần cứng và push vào rawSensorBuffer.
// Trả về true nếu vừa có mẫu mới được push.
bool readSensors();

// Kiểm tra xem rawSensorBuffer có ít nhất 1 mẫu chưa
bool isSensorDataReady();
