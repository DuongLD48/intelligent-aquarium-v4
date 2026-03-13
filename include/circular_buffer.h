#pragma once
#include <stdint.h>
#include <stddef.h>

// ================================================================
// circular_buffer.h
// Intelligent Aquarium v4.0
// Template circular buffer dùng chung cho SensorReading, float, v.v.
// ================================================================

template <typename T, size_t Capacity>
class CircularBuffer {
public:
    CircularBuffer() : _head(0), _size(0) {}

    // Thêm phần tử mới. Nếu đầy, ghi đè phần tử cũ nhất.
    void push(const T& item) {
        _data[_head] = item;
        _head = (_head + 1) % Capacity;
        if (_size < Capacity) {
            _size++;
        }
    }

    // Lấy phần tử từ đầu (oldest). Trả false nếu rỗng.
    bool shift(T& out) {
        if (_size == 0) return false;
        size_t tail = (_head + Capacity - _size) % Capacity;
        out = _data[tail];
        _size--;
        return true;
    }

    // Phần tử mới nhất (last pushed)
    const T& last() const {
        size_t idx = (_head + Capacity - 1) % Capacity;
        return _data[idx];
    }

    // Truy cập theo index (0 = oldest)
    const T& operator[](size_t index) const {
        size_t tail = (_head + Capacity - _size) % Capacity;
        return _data[(tail + index) % Capacity];
    }

    T& operator[](size_t index) {
        size_t tail = (_head + Capacity - _size) % Capacity;
        return _data[(tail + index) % Capacity];
    }

    // Số phần tử hiện có
    size_t size() const { return _size; }

    // Kiểm tra rỗng / đầy
    bool isEmpty() const { return _size == 0; }
    bool isFull()  const { return _size == Capacity; }

    // Xóa buffer
    void clear() {
        _head = 0;
        _size = 0;
    }

    // Capacity tối đa
    static constexpr size_t capacity() { return Capacity; }

private:
    T      _data[Capacity];
    size_t _head;   // Vị trí sẽ ghi tiếp theo
    size_t _size;   // Số phần tử hiện có
};
