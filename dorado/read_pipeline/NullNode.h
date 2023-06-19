#pragma once

#include "ReadPipeline.h"

namespace dorado {

class NullNode : public MessageSink {
public:
    // NullNode has no sink - input messages go nowhere
    NullNode();
    ~NullNode();
    std::string get_name() const override { return "NullNode"; }

private:
    void worker_thread();
    std::vector<std::unique_ptr<std::thread>> m_workers;
};

}  // namespace dorado
