#pragma once

#include <array>
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace LocalCheckpointDetail {
template<class T> auto Copy(const T& value) { return value; }
template<class T, size_t N> auto Copy(const T (&value)[N]) {
    std::array<T, N> result{};
    std::copy_n(value, N, result.begin());
    return result;
}
template<class T> T Copy(const std::atomic<T>& value) { return value.load(std::memory_order_relaxed); }
template<class T, size_t N> auto Copy(const std::array<T, N>& value) {
    std::array<decltype(Copy(value[0])), N> result{};
    for (size_t i = 0; i < N; ++i) result[i] = Copy(value[i]);
    return result;
}
template<class T, class V> void Assign(T& destination, const V& value) { destination = value; }
template<class T, size_t N> void Assign(T (&destination)[N], const std::array<T, N>& value) {
    std::copy(value.begin(), value.end(), destination);
}
template<class T> void Assign(std::atomic<T>& destination, T value) { destination.store(value, std::memory_order_relaxed); }
template<class T, class V, size_t N> void Assign(std::array<T, N>& destination, const std::array<V, N>& value) {
    for (size_t i = 0; i < N; ++i) Assign(destination[i], value[i]);
}
template<class T> size_t Bytes(const T&) { return sizeof(T); }
template<class T> size_t Bytes(const std::vector<T>& value) { return sizeof(value) + value.size() * sizeof(T); }
template<class T> size_t Bytes(const std::deque<T>& value) { return sizeof(value) + value.size() * sizeof(T); }
template<class K, class V, class H, class E, class A>
size_t Bytes(const std::unordered_map<K, V, H, E, A>& value) {
    return sizeof(value) + value.bucket_count() * sizeof(void*) +
        value.size() * (sizeof(typename std::remove_reference_t<decltype(value)>::value_type) + sizeof(void*));
}
template<class K, class H, class E, class A>
size_t Bytes(const std::unordered_set<K, H, E, A>& value) {
    return sizeof(value) + value.bucket_count() * sizeof(void*) + value.size() * (sizeof(K) + sizeof(void*));
}
}

// An immutable in-process memento, with no byte-import/deserialization API.
// Captured fields must outlive it. Call capture/restore only while all writers
// are quiescent. Each subsystem supplies its own lifetime/quiescence guard.
class LocalCheckpoint {
public:
    size_t ByteSize() const { return bytes_; }
    explicit operator bool() const { return bool(restore_); }
    bool CanRestore() const { return guard_ && (*guard_)(); }
    bool Restore() const { return restore_ && (*restore_)(); }
    static LocalCheckpoint FromCallbacks(size_t bytes, std::function<bool()> guard, std::function<bool()> restore) {
        LocalCheckpoint result;
        result.bytes_ = bytes;
        result.guard_ = std::make_shared<const std::function<bool()>>(std::move(guard));
        result.restore_ = std::make_shared<const std::function<bool()>>(std::move(restore));
        return result;
    }

    template<class... T>
    static LocalCheckpoint Capture(std::function<bool()> guard, T&... fields) {
        if (!guard) throw std::invalid_argument("Checkpoint requires a lifetime guard");
        auto saved = std::make_tuple(LocalCheckpointDetail::Copy(fields)...);
        LocalCheckpoint result;
        result.guard_ = std::make_shared<const std::function<bool()>>(guard);
        result.bytes_ = std::apply([](const auto&... value) {
            return (size_t(0) + ... + LocalCheckpointDetail::Bytes(value));
        }, saved);
        result.restore_ = std::make_shared<const std::function<bool()>>(
            [guard = std::move(guard), destination = std::tie(fields...), saved = std::move(saved)] {
                if (!guard()) return false;
                [&]<size_t... I>(std::index_sequence<I...>) {
                    (LocalCheckpointDetail::Assign(std::get<I>(destination), std::get<I>(saved)), ...);
                }(std::index_sequence_for<T...>{});
                return true;
            });
        return result;
    }
private:
    size_t bytes_ = 0;
    std::shared_ptr<const std::function<bool()>> restore_;
    std::shared_ptr<const std::function<bool()>> guard_;
};
