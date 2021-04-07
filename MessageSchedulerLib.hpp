#pragma once
#include <chrono>
#include <deque>
#include <iostream>
#include <unordered_map>
#include <vector>
#include <functional>

using SchedulerClock = std::chrono::high_resolution_clock;

template <typename T> struct WaitingMessage {
  // the message data
  T message;

  // is this message unsent and waiting?
  bool message_ready = false;

  double priority = 0;

  // when a message was last sent on this topic
  SchedulerClock::time_point last_send_time = SchedulerClock::now();

  // updated as now() - last_send_time, only when a message is ready
  std::chrono::duration<double> time_waiting =
      std::chrono::duration<double>::zero();
};

/**
 * @brief Queues messages and schedules them on demand.
 *
 * Messages are enqueued, and then later scheduled (via the scheduled signal)
 * when schedule() is called.
 */
template <typename T> class MessageSchedulerLib {
  std::function<void(const T&)> sc_;
  uint64_t network_backpressure_counter_ = 0;
  uint64_t max_queue_before_waiting_ = 1;

  std::deque<T> no_drop_queue;
  std::unordered_map<std::string, WaitingMessage<T>> topic_queue;

  public:
    MessageSchedulerLib(uint64_t mq, std::function<void(const T&)> sc) {
      max_queue_before_waiting_ = mq;
      sc_ = sc;
    }

    void enqueue(
        const std::string& topic, const T& data, double priority,
        bool no_drop) {
      if (no_drop) {
        no_drop_queue.push_back(data);
      } else {
        topic_queue[topic].message = data;
        topic_queue[topic].message_ready = true;
        topic_queue[topic].priority = priority;
      }
      schedule();
    }

    /**
     * @brief Fire this to indicate that the network is free
     * Updates the counter for network backpressure
     */
    void backpressure_update(uint64_t message_index, uint64_t last_ponged_index) {
      network_backpressure_counter_ = message_index - last_ponged_index;
      schedule();
    }

    /**
     * @brief Schedule messages now.
     * Messages flagged as no_drop are sent first, in FIFO fashion.
     * Then, messages are sent by topic priority.
     */
    void schedule() {
      if (network_backpressure_counter_ >= max_queue_before_waiting_) {
        return;
      }

      // flush no-drop queue
      if (!no_drop_queue.empty()) {
        while (!no_drop_queue.empty()) {
          const auto& next = no_drop_queue.front();
          sc_(next);
          no_drop_queue.pop_front();
          network_backpressure_counter_++;
        }
        if (network_backpressure_counter_ >= max_queue_before_waiting_) {
          return;
        }
      }

      if (topic_queue.empty()) {
        return;
      }

      const auto now = SchedulerClock::now();

      // Collect candidates
      std::vector<WaitingMessage<T>*> candidates;
      for (auto it = topic_queue.begin(); it != topic_queue.end(); ++it) {
        const std::string& candidate_topic = it->first;
        WaitingMessage<T>& candidate = it->second;

        if (candidate.message_ready) {
          candidate.time_waiting =
              (now - candidate.last_send_time) * candidate.priority;
          candidates.push_back(&candidate);
        }
      }

      // Determine how many messages we are allowed to send
      int messages_to_send = std::min(
          (max_queue_before_waiting_ - network_backpressure_counter_),
          candidates.size());

      // Sort candidates
      auto compare = [](WaitingMessage<T>* lhs, WaitingMessage<T>* rhs) {
        return lhs->time_waiting < rhs->time_waiting;
      };
      std::sort(candidates.begin(), candidates.end(), compare);

      // attempt to publish top-K candidates
      for (int cand_idx = 0; cand_idx < messages_to_send; cand_idx++) {
        sc_(candidates[cand_idx]->message);
        candidates[cand_idx]->message_ready = false;
        candidates[cand_idx]->last_send_time = now;
        network_backpressure_counter_++;
      }
    }
};
