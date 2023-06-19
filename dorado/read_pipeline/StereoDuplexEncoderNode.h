#pragma once
#include "ReadPipeline.h"
#include "utils/stats.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <vector>

namespace dorado {

class StereoDuplexEncoderNode : public MessageSink {
public:
    StereoDuplexEncoderNode(int input_signal_stride);

    std::shared_ptr<dorado::Read> stereo_encode(std::shared_ptr<dorado::Read> template_read,
                                                std::shared_ptr<dorado::Read> complement_read);

    ~StereoDuplexEncoderNode();
    std::string get_name() const override { return "StereoDuplexEncoderNode"; }
    stats::NamedStats sample_stats() const override;

private:
    // Consume reads from input queue
    void worker_thread();

    std::vector<std::unique_ptr<std::thread>> worker_threads;

    // Time when Basecaller Node is initialised. Used for benchmarking and debugging
    std::chrono::time_point<std::chrono::system_clock> initialization_time;

    // Time when Basecaller Node terminates. Used for benchmarking and debugging
    std::chrono::time_point<std::chrono::system_clock> termination_time;

    // The stride which was used to simplex call the data
    int m_input_signal_stride;

    // Performance monitoring stats.
    std::atomic<int64_t> m_num_discarded_pairs = 0;
};

}  // namespace dorado