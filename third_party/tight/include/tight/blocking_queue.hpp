#pragma once

// 有界阻塞队列（单链表 + 节点回收池实现）。
// 出队节点进入队列内自由链表复用（复用既有互斥锁，零额外竞争），
// 不像 std::queue(deque) 那样在排空后仍保留内部块，
// 同时消除了高频入出队时的节点 new/delete 抖动。

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <optional>
#include <utility>

namespace tight {

template <typename T>
class BlockingQueue {
public:
    explicit BlockingQueue(std::size_t capacity = 0)
        : m_capacity(capacity), m_closed(false) {}

    ~BlockingQueue() {
        close();
        drain_list(m_head);
        drain_list(m_free_nodes);
    }

    BlockingQueue(const BlockingQueue&) = delete;
    BlockingQueue& operator=(const BlockingQueue&) = delete;
    BlockingQueue(BlockingQueue&&) = delete;
    BlockingQueue& operator=(BlockingQueue&&) = delete;

    bool push(T item) {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (m_closed) return false;
        if (m_capacity > 0 && m_size >= m_capacity) {
            m_not_full.wait(lock, [this] {
                return m_size < m_capacity || m_closed;
            });
        }
        if (m_closed) return false;
        Node* node = obtain_node_locked();
        node->item.emplace(std::move(item));
        link_tail(node);
        m_not_empty.notify_one();
        return true;
    }

    bool try_push(T item) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_closed || (m_capacity > 0 && m_size >= m_capacity)) return false;
        Node* node = obtain_node_locked();
        node->item.emplace(std::move(item));
        link_tail(node);
        m_not_empty.notify_one();
        return true;
    }

    std::optional<T> take() {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_not_empty.wait(lock, [this] {
            return m_size > 0 || m_closed;
        });
        if (m_closed && m_size == 0) return std::nullopt;
        return unlink_head_locked();
    }

    std::optional<T> take_for(std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(m_mutex);
        bool ok = m_not_empty.wait_for(lock, timeout, [this] {
            return m_size > 0 || m_closed;
        });
        if (!ok || (m_closed && m_size == 0)) return std::nullopt;
        return unlink_head_locked();
    }

    std::optional<T> poll() {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_size == 0) return std::nullopt;
        return unlink_head_locked();
    }

    void close() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_closed = true;
        m_not_empty.notify_all();
        m_not_full.notify_all();
    }

    bool is_closed() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_closed;
    }

    std::size_t size() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_size;
    }

    std::size_t capacity() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_capacity;
    }

private:
    struct Node {
        std::optional<T> item;   // 回收时 reset，节点本体复用
        Node* next{nullptr};
    };

    static void drain_list(Node* node) {
        while (node) {
            Node* next = node->next;
            delete node;
            node = next;
        }
    }

    // 从自由链表取节点（无则新建）；调用方需已持锁
    Node* obtain_node_locked() {
        if (m_free_nodes) {
            Node* node = m_free_nodes;
            m_free_nodes = node->next;
            node->next = nullptr;
            --m_free_count;
            return node;
        }
        return new Node();
    }

    // 节点回收到自由链表（上限 64 个）；调用方需已持锁
    void recycle_node_locked(Node* node) {
        node->item.reset();
        if (m_free_count < 64) {
            node->next = m_free_nodes;
            m_free_nodes = node;
            ++m_free_count;
        } else {
            delete node;
        }
    }

    // 入队到尾部（调用方需已持锁且完成关闭/容量检查）
    void link_tail(Node* node) {
        if (m_tail) {
            m_tail->next = node;
            m_tail = node;
        } else {
            m_head = m_tail = node;
        }
        ++m_size;
    }

    // 出队头部节点：元素移出后节点进入回收链表（调用方需已持锁）
    std::optional<T> unlink_head_locked() {
        Node* node = m_head;
        m_head = node->next;
        if (!m_head) m_tail = nullptr;
        --m_size;
        std::optional<T> item{std::move(*node->item)};
        recycle_node_locked(node);
        m_not_full.notify_one();
        return item;
    }

    std::size_t                m_capacity;
    std::size_t                m_size{0};
    Node*                      m_head{nullptr};
    Node*                      m_tail{nullptr};
    Node*                      m_free_nodes{nullptr};
    std::size_t                m_free_count{0};
    mutable std::mutex         m_mutex;
    std::condition_variable    m_not_empty;
    std::condition_variable    m_not_full;
    bool                       m_closed;
};

} // namespace tight
