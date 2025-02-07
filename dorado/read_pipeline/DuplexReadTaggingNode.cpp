#include "DuplexReadTaggingNode.h"

#include <spdlog/spdlog.h>

namespace dorado {

void DuplexReadTaggingNode::worker_thread() {
    torch::InferenceMode inference_mode_guard;

    Message message;
    while (get_input_message(message)) {
        // If this message isn't a read, just forward it to the sink.
        if (!std::holds_alternative<std::shared_ptr<Read>>(message)) {
            send_message_to_sink(std::move(message));
            continue;
        }

        // If this message isn't a read, we'll get a bad_variant_access exception.
        auto read = std::get<std::shared_ptr<Read>>(message);

        // The algorithm is as follows -
        // There's no inherent ordering between when a duplex or its parent
        // simplex reads are expected in the pipeline (yet). So both cases need
        // to be handled, where the simplex parents come first or the duplex
        // offspring comes first.
        // 1. When a duplex read comes by, we derive its parent reads
        // from the name and check:
        // * If the parent read has already been processed and sent downstream, do
        // nothing. This can happen because 2 duplex parents can share the same
        // read (nothing in our pipeline prevents that right now even though
        // biologically that's not possible).
        // * If the parent has been seen but not processed yet, then the parent
        // read is added to the processed list and sent downstream.
        // * Lastly, if the parent has not been seen yet, then the parent is
        // added to a set of parents to look for.
        // 2. When a simplex parent comes by:
        // * Check if it's already being asked for by a duplex offspring. If so,
        // we process the parent and pass it down, while removing it from
        // the set of duplex parents being looked for.
        // * If no duplex child for this parent has been seen, then add it to
        // the map of available parents.
        //
        // Once all reads have been processed, any leftover parent simplex reads are
        // the ones whose duplex offsprings never came. They are retagged to not be
        // duplex parents and then sent downstream.
        if (!read->is_duplex && !read->is_duplex_parent) {
            send_message_to_sink(read);
        } else if (read->is_duplex) {
            std::string template_read_id = read->read_id.substr(0, read->read_id.find(';'));
            std::string complement_read_id =
                    read->read_id.substr(read->read_id.find(';') + 1, read->read_id.length());

            send_message_to_sink(read);

            for (auto& rid : {template_read_id, complement_read_id}) {
                if (m_parents_processed.find(rid) != m_parents_processed.end()) {
                    // Parent read has already been processed. Do nothing.
                    continue;
                }
                auto find_parent = m_duplex_parents.find(rid);
                if (find_parent != m_duplex_parents.end()) {
                    // Parent read has been seen. Process it and send it
                    // downstream.
                    send_message_to_sink(find_parent->second);
                    m_parents_processed.insert(rid);
                    m_duplex_parents.erase(find_parent);
                } else {
                    // Parent read hasn't been seen. So add it to list of
                    // parents to look for.
                    m_parents_wanted.insert(rid);
                }
            }
        } else {
            auto find_read = m_parents_wanted.find(read->read_id);
            if (find_read != m_parents_wanted.end()) {
                // If a read is in the parents wanted list, then sent it downstream
                // and add it to the set of processed reads. It will also be removed
                // from the parent reads being looked for.
                send_message_to_sink(read);
                m_parents_processed.insert(read->read_id);
                m_parents_wanted.erase(find_read);
            } else {
                // No duplex offspring is seen so far, so hold it and track
                // it as available parents.
                m_duplex_parents[read->read_id] = std::move(read);
            }
        }
    }

    for (auto& [k, v] : m_duplex_parents) {
        v->is_duplex_parent = false;
        send_message_to_sink(v);
    }
}

DuplexReadTaggingNode::DuplexReadTaggingNode() : MessageSink(1000) { start_threads(); }

void DuplexReadTaggingNode::start_threads() {
    m_worker =
            std::make_unique<std::thread>(std::thread(&DuplexReadTaggingNode::worker_thread, this));
}

void DuplexReadTaggingNode::terminate_impl() {
    terminate_input_queue();
    if (m_worker->joinable()) {
        m_worker->join();
    }
}

void DuplexReadTaggingNode::restart() {
    restart_input_queue();
    m_duplex_parents.clear();
    m_parents_processed.clear();
    m_parents_wanted.clear();
    start_threads();
}

stats::NamedStats DuplexReadTaggingNode::sample_stats() const {
    stats::NamedStats stats = stats::from_obj(m_work_queue);
    return stats;
}

}  // namespace dorado
