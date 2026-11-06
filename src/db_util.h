// SQLite and threading helpers shared by the database builders (build-db,
// build-assets): a connection, a prepared statement with chained binds, one
// SQL value, and the bounded queue that carries rows from the decode threads
// to the one writer thread.
#pragma once

#include <windows.h>

#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "sqlite3.h"

namespace skqe {

class Db {
public:
    ~Db() {
        if (db_) sqlite3_close(db_);
    }
    bool Open(const std::wstring& path, std::string* error) {
        if (sqlite3_open16(path.c_str(), &db_) != SQLITE_OK) {
            *error = db_ ? sqlite3_errmsg(db_) : "cannot create the database";
            return false;
        }
        return true;
    }
    bool Exec(const std::string& sql, std::string* error) {
        char* msg = nullptr;
        if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &msg) != SQLITE_OK) {
            *error = std::string(msg ? msg : "sqlite error") + " in: " + sql.substr(0, 200);
            sqlite3_free(msg);
            return false;
        }
        return true;
    }
    sqlite3_stmt* Prepare(const char* sql, std::string* error) {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db_, sql, -1, &st, nullptr) != SQLITE_OK) *error = sqlite3_errmsg(db_);
        return st;
    }
    const char* Message() const { return sqlite3_errmsg(db_); }
    sqlite3* raw() const { return db_; }

private:
    sqlite3* db_ = nullptr;
};

struct Stmt {
    sqlite3_stmt* st = nullptr;
    ~Stmt() {
        if (st) sqlite3_finalize(st);
    }
    Stmt& Int(int i, int64_t v) {
        sqlite3_bind_int64(st, i, v);
        return *this;
    }
    Stmt& Real(int i, double v) {
        sqlite3_bind_double(st, i, v);
        return *this;
    }
    Stmt& Text(int i, const std::string& v) {
        sqlite3_bind_text(st, i, v.data(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
        return *this;
    }
    Stmt& Blob(int i, const std::string& v) {
        sqlite3_bind_blob(st, i, v.data(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
        return *this;
    }
    Stmt& Null(int i) {
        sqlite3_bind_null(st, i);
        return *this;
    }
    bool Run() {
        int rc = sqlite3_step(st);
        sqlite3_reset(st);
        return rc == SQLITE_DONE;
    }
};

// One SQL value.
struct Cell {
    enum class T : uint8_t { Null, Int, Real, Text, Blob } t = T::Null;
    int64_t i = 0;
    double d = 0;
    std::string s;
    static Cell Int(int64_t v) {
        Cell c;
        c.t = T::Int;
        c.i = v;
        return c;
    }
    static Cell Real(double v) {
        Cell c;
        c.t = T::Real;
        c.d = v;
        return c;
    }
    static Cell Text(std::string v) {
        Cell c;
        c.t = T::Text;
        c.s = std::move(v);
        return c;
    }
    // Hex text (as the decoder writes byte runs) as the bytes themselves.
    static Cell Blob(const std::string& hex) {
        Cell c;
        c.t = T::Blob;
        c.s.reserve(hex.size() / 2);
        auto nib = [](char h) { return h <= '9' ? h - '0' : (h & ~0x20) - 'A' + 10; };
        for (size_t i = 0; i + 1 < hex.size(); i += 2) c.s.push_back(static_cast<char>(nib(hex[i]) << 4 | nib(hex[i + 1])));
        return c;
    }
    // Bytes as they are.
    static Cell Bytes(std::string raw) {
        Cell c;
        c.t = T::Blob;
        c.s = std::move(raw);
        return c;
    }
};

inline bool BindRun(sqlite3_stmt* st, const std::vector<Cell>& cells) {
    for (size_t i = 0; i < cells.size(); ++i) {
        const Cell& c = cells[i];
        int k = static_cast<int>(i) + 1;
        switch (c.t) {
            case Cell::T::Null: sqlite3_bind_null(st, k); break;
            case Cell::T::Int: sqlite3_bind_int64(st, k, c.i); break;
            case Cell::T::Real: sqlite3_bind_double(st, k, c.d); break;
            case Cell::T::Text: sqlite3_bind_text(st, k, c.s.data(), static_cast<int>(c.s.size()), SQLITE_STATIC); break;
            case Cell::T::Blob: sqlite3_bind_blob(st, k, c.s.data(), static_cast<int>(c.s.size()), SQLITE_STATIC); break;
        }
    }
    int rc = sqlite3_step(st);
    sqlite3_reset(st);
    return rc == SQLITE_DONE;
}

// Bounded multi-producer queue: decode threads push batches, the writer pops
// until every producer is done.
template <class T>
class BoundedQueue {
public:
    void Push(T b) {
        std::unique_lock<std::mutex> lk(m_);
        notFull_.wait(lk, [this] { return q_.size() < 32; });
        q_.push_back(std::move(b));
        notEmpty_.notify_one();
    }
    bool Pop(T* b) {
        std::unique_lock<std::mutex> lk(m_);
        notEmpty_.wait(lk, [this] { return !q_.empty() || producers_ == 0; });
        if (q_.empty()) return false;
        *b = std::move(q_.front());
        q_.pop_front();
        notFull_.notify_one();
        return true;
    }
    void SetProducers(int n) { producers_ = n; }
    void ProducerDone() {
        std::lock_guard<std::mutex> lk(m_);
        --producers_;
        notEmpty_.notify_all();
    }

private:
    std::mutex m_;
    std::condition_variable notEmpty_, notFull_;
    std::deque<T> q_;
    int producers_ = 0;
};

inline std::string NowIso() {
    SYSTEMTIME t;
    GetSystemTime(&t);
    char buf[32];
    snprintf(buf, sizeof(buf), "%04u-%02u-%02uT%02u:%02u:%02uZ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
             t.wSecond);
    return buf;
}

}  // namespace skqe
